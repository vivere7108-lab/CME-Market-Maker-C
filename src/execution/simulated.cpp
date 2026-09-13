#include "harvester/execution/simulated.hpp"

#include <algorithm>
#include <cmath>

namespace harvester {

SimulatedBroker::SimulatedBroker(const Product& product, const std::string& queue_position, ClockFn clock)
    : product_(product), front_(queue_position == "front"), clock_(std::move(clock)) {
    orders_.reserve(8);
    events_.reserve(16);
}

OrderHandle SimulatedBroker::place(int side, double price, int size) {
    const std::int64_t order_id = next_id_++;
    SimOrder order;
    order.id = order_id;
    order.side = side;
    order.price = price;
    order.size = size;
    order.queue_ahead = queue_at(side, price);
    orders_.push_back(order);
    events_.push_back(OrderEvent::ack(order_id));
    ++sent_;
    return OrderHandle{order_id};
}

OrderHandle SimulatedBroker::replace(OrderHandle handle, double price, int size) {
    ++sent_;
    auto it = std::find_if(orders_.begin(), orders_.end(), [&](const SimOrder& o) { return o.id == handle.order_id; });
    if (it == orders_.end() || !it->active) return handle;
    if (price != it->price) {
        it->price = price;
        it->queue_ahead = queue_at(it->side, price);
    } else if (size > it->remaining()) {
        it->queue_ahead = queue_at(it->side, price);
    }
    it->size = size + it->filled;
    events_.push_back(OrderEvent::ack(handle.order_id));
    return handle;
}

void SimulatedBroker::cancel(OrderHandle handle) {
    ++sent_;
    auto it = std::find_if(orders_.begin(), orders_.end(), [&](const SimOrder& o) { return o.id == handle.order_id; });
    if (it == orders_.end() || !it->active) return;
    it->active = false;
    events_.push_back(OrderEvent::cancelled(handle.order_id));
    compact();
}

std::vector<OrderEvent> SimulatedBroker::drain_events() {
    std::vector<OrderEvent> out;
    out.swap(events_);
    events_.reserve(16);
    return out;
}

std::vector<SimulatedBroker::SimOrder> SimulatedBroker::working() const {
    std::vector<SimOrder> out;
    for (const auto& order : orders_) {
        if (order.active) out.push_back(order);
    }
    return out;
}

const SimulatedBroker::SimOrder* SimulatedBroker::find(std::int64_t order_id) const {
    for (const auto& order : orders_) {
        if (order.id == order_id) return &order;
    }
    return nullptr;
}

std::int64_t SimulatedBroker::queue_at(int side, double price) const {
    if (front_ || !snapshot_) return 0;
    for (const Level& level : side > 0 ? snapshot_->bids() : snapshot_->asks()) {
        if (std::fabs(level.price - price) < 1e-9) return level.size;
    }
    return 0;
}

void SimulatedBroker::on_book(const BookSnapshot& snapshot) {
    snapshot_ = snapshot;
    if (!snapshot.two_sided()) return;
    const double best_ask = snapshot.best_ask()->price;
    const double best_bid = snapshot.best_bid()->price;
    for (auto& order : orders_) {
        if (!order.active) continue;
        for (const Level& level : order.side > 0 ? snapshot.bids() : snapshot.asks()) {
            if (std::fabs(level.price - order.price) < 1e-9) {
                // Cancellations ahead of us shrink the queue; fills ahead
                // are handled on the trade. Either way we are never behind
                // more than is resting.
                order.queue_ahead = std::min<std::int64_t>(order.queue_ahead, level.size);
                break;
            }
        }
        const bool crossed = (order.side > 0 && best_ask <= order.price + 1e-9) ||
                             (order.side < 0 && best_bid >= order.price - 1e-9);
        if (crossed) fill(order, order.remaining(), order.price);
    }
    compact();
}

void SimulatedBroker::on_trade(const Trade& trade) {
    if (trade.aggressor == 0) return;
    for (auto& order : orders_) {
        if (!order.active) continue;
        // A seller-initiated trade can hit our bid; a buyer-initiated one our ask.
        if (trade.aggressor != -order.side) continue;
        const bool through = (order.side > 0 && trade.price < order.price - 1e-9) ||
                             (order.side < 0 && trade.price > order.price + 1e-9);
        const bool at = std::fabs(trade.price - order.price) < 1e-9;
        if (through) {
            fill(order, order.remaining(), order.price);
        } else if (at) {
            const std::int64_t take = std::min<std::int64_t>(order.queue_ahead, trade.size);
            order.queue_ahead -= take;
            const std::int64_t left = trade.size - take;
            if (left > 0) fill(order, static_cast<int>(std::min<std::int64_t>(left, order.remaining())), order.price);
        }
    }
    compact();
}

void SimulatedBroker::fill(SimOrder& order, int size, double price) {
    if (size <= 0) return;
    order.filled += size;
    Fill f;
    f.ts = clock_();
    f.side = order.side;
    f.price = price;
    f.size = size;
    f.fees = product_.fee_per_contract * size;
    f.order_id = order.id;
    fills.push_back(f);
    events_.push_back(OrderEvent::filled(order.id, f));
    if (order.remaining() <= 0) order.active = false;
}

void SimulatedBroker::compact() {
    // Finished orders are dropped once nothing can reference them any
    // more; the events they produced are already queued.
    std::erase_if(orders_, [](const SimOrder& o) { return !o.active; });
}

}  // namespace harvester
