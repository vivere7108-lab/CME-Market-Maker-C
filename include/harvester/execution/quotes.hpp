// The quote manager: the working quotes, and the diff against the wanted
// ones.
//
// One working order a side at most.  Each decision cycle the engine says
// what it wants (``set_desired``); the manager compares that with what is
// working and queues the smallest set of messages that reconciles the
// two, through the ``ActionQueue`` so the budget and the priorities
// apply:
//
//     wanted, nothing working           -> place
//     nothing wanted, something working -> cancel
//     both, and different enough        -> replace (one message)
//
// "Different enough" is the dead-band: a price within
// ``requote_price_ticks`` of the working one, or a size within
// ``requote_size_fraction`` of it, is left alone; and a side is not
// re-quoted more often than ``requote_min_interval_ms``.
//
// States
// ------
// A side's order is ``PendingNew``, ``Working``, ``PendingReplace`` or
// ``PendingCancel``.  While it is pending the manager does not send it
// anything else.  A pending state that outlives ``ack_timeout_seconds``
// is logged and treated as ``Unknown``: the manager re-sends a cancel and
// stops quoting that side until the broker's events say what the order
// is.  That cancel is re-sent every ``ack_timeout_seconds`` for as long
// as the side stays ``Unknown``, because the lost message can be the
// cancel itself -- one attempt and then a wait for an event that has no
// reason to arrive is a side dead for the session on one warning.  The
// retries are counted in ``stuck_cancels``; past ``LOUD_AFTER_CANCELS``
// on one order they are logged as errors, since by then the gateway is
// not answering at all.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "harvester/config.hpp"
#include "harvester/execution/base.hpp"
#include "harvester/execution/throttle.hpp"
#include "harvester/instruments.hpp"
#include "harvester/quoting/engine.hpp"
#include "harvester/util/clock.hpp"

namespace harvester {

enum class OrderState { PendingNew, Working, PendingReplace, PendingCancel, Unknown };
const char* to_string(OrderState state);

struct WorkingOrder {
    int side = 0;
    double price = 0.0;
    int size = 0;
    OrderHandle handle;
    OrderState state = OrderState::PendingNew;
    int filled = 0;
    // The price/size a pending replace is taking it to.
    std::optional<double> target_price;
    std::optional<int> target_size;
    double sent_at = 0.0;
    double last_quoted_at = 0.0;
    // Cancels sent while this order has been ``Unknown``.
    int cancel_attempts = 0;

    int remaining() const { return size - filled > 0 ? size - filled : 0; }
    const char* label() const { return side > 0 ? "bid" : "ask"; }
};

class QuoteManager {
public:
    QuoteManager(Broker& broker, const Product& product, const ExecutionConfig& cfg, ActionQueue& queue,
                 ClockFn clock = mono_now);

    // -- what is wanted --
    void set_desired(const QuoteDecision& decision);
    void cancel_all(const std::string& reason = "");
    bool is_quoting(int side) const;

    // -- reconciling --
    // Queue whatever messages turn the working quotes into the wanted ones.
    void reconcile(std::optional<double> now = std::nullopt);

    // -- what the broker says --
    // Apply the broker's events. Returns the fills among them.
    std::vector<Fill> on_events(const std::vector<OrderEvent>& events);

    // -- reporting --
    std::string describe() const;
    const WorkingOrder* working(int side) const { return slot(side) ? &*slot(side) : nullptr; }
    bool any_working() const { return working_[0].has_value() || working_[1].has_value(); }
    const std::optional<Quote>& desired(int side) const { return desired_[index(side)]; }
    const ExecutionConfig& cfg() const { return cfg_; }

    // Retries past this many on one order are logged as errors.
    static constexpr int LOUD_AFTER_CANCELS = 3;

    std::vector<Fill> fills;
    std::int64_t timeouts = 0;
    std::int64_t rejections = 0;
    // Cancels re-sent to sides the broker has never answered about. Non-zero
    // means a side spent time unquotable; it belongs in any session report.
    std::int64_t stuck_cancels = 0;
    bool suspended(int side) const { return suspended_[index(side)]; }

private:
    static std::size_t index(int side) { return side > 0 ? 0 : 1; }
    std::optional<WorkingOrder>& slot(int side) { return working_[index(side)]; }
    const std::optional<WorkingOrder>& slot(int side) const { return working_[index(side)]; }
    WorkingOrder* find_by_order_id(std::int64_t order_id);

    void reconcile_side(int side, double now);
    bool differs(const WorkingOrder& order, const Quote& wanted) const;
    void place(int side, const Quote& quote);
    void maybe_replace(int side);
    void cancel(int side);
    void cancel_order(WorkingOrder& order);
    void on_timeout(WorkingOrder& order, double now);
    void retry_cancel(int side);
    void on_ack(WorkingOrder& order);
    void forget(const WorkingOrder& order);

    Broker& broker_;
    const Product& product_;
    ExecutionConfig cfg_;
    ActionQueue& queue_;
    ClockFn clock_;
    std::array<std::optional<WorkingOrder>, 2> working_;
    std::array<std::optional<Quote>, 2> desired_;
    std::array<bool, 2> suspended_{false, false};
};

}  // namespace harvester
