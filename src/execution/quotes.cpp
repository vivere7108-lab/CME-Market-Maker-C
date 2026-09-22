#include "harvester/execution/quotes.hpp"

#include <cmath>
#include <format>

#include "harvester/util/log.hpp"

namespace harvester {

namespace {
constexpr const char* kLog = "harvester.execution.quotes";
constexpr const char* kKeys[2] = {"bid", "ask"};
}  // namespace

const char* to_string(OrderState state) {
    switch (state) {
        case OrderState::PendingNew: return "pending_new";
        case OrderState::Working: return "working";
        case OrderState::PendingReplace: return "pending_replace";
        case OrderState::PendingCancel: return "pending_cancel";
        case OrderState::Unknown: return "unknown";
    }
    return "?";
}

QuoteManager::QuoteManager(Broker& broker, const Product& product, const ExecutionConfig& cfg, ActionQueue& queue,
                           ClockFn clock)
    : broker_(broker), product_(product), cfg_(cfg), queue_(queue), clock_(std::move(clock)) {}

void QuoteManager::set_desired(const QuoteDecision& decision) {
    desired_[0] = decision.bid;
    desired_[1] = decision.ask;
}

void QuoteManager::cancel_all(const std::string& reason) {
    desired_[0].reset();
    desired_[1].reset();
    if (!reason.empty() && any_working()) HLOG_INFO(kLog, "cancelling all quotes: {}", reason);
}

bool QuoteManager::is_quoting(int side) const {
    const auto& order = slot(side);
    return order && (order->state == OrderState::Working || order->state == OrderState::PendingNew ||
                     order->state == OrderState::PendingReplace);
}

WorkingOrder* QuoteManager::find_by_order_id(std::int64_t order_id) {
    for (auto& order : working_) {
        if (order && order->handle.order_id == order_id) return &*order;
    }
    return nullptr;
}

void QuoteManager::reconcile(std::optional<double> now) {
    const double t = now ? *now : clock_();
    reconcile_side(1, t);
    reconcile_side(-1, t);
}

void QuoteManager::reconcile_side(int side, double now) {
    const std::optional<Quote>& wanted = desired_[index(side)];
    std::optional<WorkingOrder>& order = slot(side);
    const char* key = kKeys[index(side)];

    if (!order) {
        if (!wanted || suspended_[index(side)]) {
            queue_.withdraw(key);
            return;
        }
        const Quote quote = *wanted;
        queue_.submit(key, PRIORITY_PLACE, [this, side, quote] { place(side, quote); },
                      std::format("place {} {}@{:.2f}", key, quote.size, quote.price));
        return;
    }

    if (order->state == OrderState::PendingNew || order->state == OrderState::PendingReplace ||
        order->state == OrderState::PendingCancel) {
        if (now - order->sent_at > cfg_.ack_timeout_seconds) on_timeout(*order, now);
        // Nothing else is sent to a side with a message in flight; the
        // queue is left holding nothing for it either.
        queue_.withdraw(key);
        return;
    }
    if (order->state == OrderState::Unknown) {
        // Suspended until the broker says what this order is. The cancel
        // that put the side here may itself have been the message that was
        // lost, so it is re-sent on the same interval rather than waited
        // on forever -- through the queue, so it takes a token and honours
        // the cancel priority like any other.
        if (now - order->sent_at > cfg_.ack_timeout_seconds) {
            queue_.submit(key, PRIORITY_CANCEL, [this, side] { retry_cancel(side); },
                          std::format("re-cancel {} (unacknowledged)", key));
        } else {
            queue_.withdraw(key);
        }
        return;
    }

    if (!wanted) {
        queue_.submit(key, PRIORITY_CANCEL, [this, side] { cancel(side); }, std::format("cancel {}", key));
        return;
    }

    if (!differs(*order, *wanted)) {
        queue_.withdraw(key);
        return;
    }
    if ((now - order->last_quoted_at) * 1000.0 < cfg_.requote_min_interval_ms) {
        // Too soon after the last message on this side. Nothing is
        // queued; the next reconcile after the interval submits whatever
        // is wanted *then*, which is the coalescing.
        queue_.withdraw(key);
        return;
    }
    queue_.submit(key, PRIORITY_REPLACE, [this, side] { maybe_replace(side); },
                  std::format("replace {} {}@{:.2f}", key, wanted->size, wanted->price));
}

bool QuoteManager::differs(const WorkingOrder& order, const Quote& wanted) const {
    const double ticks = std::fabs(wanted.price - order.price) / product_.tick_size;
    if (ticks > cfg_.requote_price_ticks + 1e-9) return true;
    if (order.remaining() == 0) return true;
    const double size_change = static_cast<double>(std::abs(wanted.size - order.remaining())) /
                               static_cast<double>(std::max(order.remaining(), 1));
    return size_change > cfg_.requote_size_fraction + 1e-9;
}

void QuoteManager::place(int side, const Quote& quote) {
    if (slot(side)) return;  // a fill or an ack changed things while this was queued
    const double now = clock_();
    const OrderHandle handle = broker_.place(side, quote.price, quote.size);
    WorkingOrder order;
    order.side = side;
    order.price = quote.price;
    order.size = quote.size;
    order.handle = handle;
    order.state = OrderState::PendingNew;
    order.sent_at = now;
    order.last_quoted_at = now;
    slot(side) = order;
    HLOG_DEBUG(kLog, "placed {} {}@{:.2f} (#{})", order.label(), quote.size, quote.price, handle.order_id);
}

// Send the replace only if it is still the right thing to do.
void QuoteManager::maybe_replace(int side) {
    std::optional<WorkingOrder>& order = slot(side);
    if (!order || order->state != OrderState::Working) return;
    const std::optional<Quote>& latest = desired_[index(side)];
    if (!latest) {
        cancel_order(*order);
        return;
    }
    const double now = clock_();
    if ((now - order->last_quoted_at) * 1000.0 < cfg_.requote_min_interval_ms) return;
    if (!differs(*order, *latest)) return;
    const OrderHandle handle = broker_.replace(order->handle, latest->price, latest->size);
    order->handle = handle;
    order->state = OrderState::PendingReplace;
    order->target_price = latest->price;
    order->target_size = latest->size;
    order->sent_at = order->last_quoted_at = now;
    HLOG_DEBUG(kLog, "replace {} -> {}@{:.2f} (#{})", order->label(), latest->size, latest->price, handle.order_id);
}

void QuoteManager::cancel(int side) {
    if (auto& order = slot(side)) cancel_order(*order);
}

void QuoteManager::cancel_order(WorkingOrder& order) {
    if (order.state == OrderState::PendingCancel) return;
    broker_.cancel(order.handle);
    order.state = OrderState::PendingCancel;
    order.sent_at = clock_();
    HLOG_DEBUG(kLog, "cancel {} (#{})", order.label(), order.handle.order_id);
}

void QuoteManager::on_timeout(WorkingOrder& order, double now) {
    ++timeouts;
    HLOG_WARNING(kLog,
                 "{} order #{} has been {} for {:.1f}s with no acknowledgement; re-sending a cancel and not "
                 "quoting that side until the broker says what it is",
                 order.label(), order.handle.order_id, to_string(order.state), now - order.sent_at);
    suspended_[index(order.side)] = true;
    try {
        broker_.cancel(order.handle);
    } catch (const std::exception& exc) {  // the state is already unknown
        HLOG_ERROR(kLog, "cancel of #{} failed: {}", order.handle.order_id, exc.what());
    }
    order.state = OrderState::Unknown;
    order.sent_at = now;
    order.cancel_attempts = 1;
}

// A cancel re-sent to a side the broker has still said nothing about.
void QuoteManager::retry_cancel(int side) {
    auto& order = slot(side);
    if (!order || order->state != OrderState::Unknown) return;  // an event arrived while this was queued
    const double now = clock_();
    ++order->cancel_attempts;
    ++stuck_cancels;
    order->sent_at = now;
    try {
        broker_.cancel(order->handle);
    } catch (const std::exception& exc) {
        HLOG_ERROR(kLog, "cancel of #{} failed: {}", order->handle.order_id, exc.what());
        return;
    }
    if (order->cancel_attempts > LOUD_AFTER_CANCELS) {
        HLOG_ERROR(kLog, "{} order #{} has ignored {} cancels; the {} side has not quoted since", order->label(),
                   order->handle.order_id, order->cancel_attempts, order->label());
    } else {
        HLOG_WARNING(kLog, "{} order #{}: cancel {} with still no acknowledgement", order->label(),
                     order->handle.order_id, order->cancel_attempts);
    }
}

std::vector<Fill> QuoteManager::on_events(const std::vector<OrderEvent>& events) {
    std::vector<Fill> found;
    for (const OrderEvent& event : events) {
        WorkingOrder* order = find_by_order_id(event.order_id);
        if (order == nullptr) {
            if (event.kind == OrderEvent::Kind::Fill && event.fill) {
                // A fill on an order this manager does not hold: an order
                // from a previous session, or one the timeout gave up on.
                // It is still a fill and must reach the inventory.
                HLOG_WARNING(kLog, "fill on unknown order #{}: {} {} @ {:.2f}", event.order_id,
                             event.fill->side > 0 ? "bought" : "sold", event.fill->size, event.fill->price);
                found.push_back(*event.fill);
            }
            continue;
        }
        switch (event.kind) {
            case OrderEvent::Kind::Ack:
                on_ack(*order);
                break;
            case OrderEvent::Kind::Fill:
                if (!event.fill) break;
                found.push_back(*event.fill);
                order->filled += event.fill->size;
                if (order->state == OrderState::PendingNew) order->state = OrderState::Working;
                if (order->remaining() <= 0) forget(*order);
                break;
            case OrderEvent::Kind::Cancelled: {
                const int side = order->side;
                forget(*order);
                suspended_[index(side)] = false;
                break;
            }
            case OrderEvent::Kind::Rejected:
                ++rejections;
                HLOG_ERROR(kLog, "{} order #{} rejected: {}", order->label(), event.order_id, event.detail);
                forget(*order);
                break;
        }
    }
    fills.insert(fills.end(), found.begin(), found.end());
    return found;
}

void QuoteManager::on_ack(WorkingOrder& order) {
    if (order.state == OrderState::PendingReplace) {
        if (order.target_price) order.price = *order.target_price;
        if (order.target_size) order.size = *order.target_size + order.filled;
        order.target_price.reset();
        order.target_size.reset();
    }
    if (order.state == OrderState::PendingNew || order.state == OrderState::PendingReplace ||
        order.state == OrderState::Unknown) {
        order.state = OrderState::Working;
        suspended_[index(order.side)] = false;
    }
}

void QuoteManager::forget(const WorkingOrder& order) {
    auto& held = slot(order.side);
    if (held && held->handle.order_id == order.handle.order_id) held.reset();
}

std::string QuoteManager::describe() const {
    std::string out;
    for (const int side : {1, -1}) {
        if (!out.empty()) out += " | ";
        const auto& order = slot(side);
        if (!order) {
            out += std::format("{} -", kKeys[index(side)]);
        } else {
            out += std::format("{} {}@{:.2f} {}", order->label(), order->remaining(), order->price, to_string(order->state));
        }
    }
    return out;
}

}  // namespace harvester
