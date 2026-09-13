// A simulated exchange: fills resting quotes off the book and the tape.
//
// Used by the replay and by a live dry run, where the real feed drives it
// and the fills it reports are the ones the quotes *would* have had.
// What it models, and what it does not:
//
// * **Queue position.**  A new order at a price joins the back of the
//   queue: everything resting at that level when it was placed is ahead
//   of it (``queue_position: back``), or nothing is (``front``, the
//   optimistic bound).  Trades at the level consume the queue ahead
//   first; cancellations ahead are inferred when the level shrinks by
//   more than the trades explain.
// * **Passive fills only.**  A resting bid fills when a seller-initiated
//   trade prints at or through its price (after the queue ahead), or when
//   the book's best ask crosses it.  It never fills on a buyer-initiated
//   trade at its price: that was someone lifting the ask.
// * **No impact.  No latency.**  The replay's fills are a *floor* on
//   adverse selection, not an estimate of it.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "harvester/book/book.hpp"
#include "harvester/execution/base.hpp"
#include "harvester/instruments.hpp"
#include "harvester/util/clock.hpp"

namespace harvester {

class SimulatedBroker : public Broker {
public:
    struct SimOrder {
        std::int64_t id = 0;
        int side = 0;
        double price = 0.0;
        int size = 0;
        int filled = 0;
        std::int64_t queue_ahead = 0;
        bool active = true;

        int remaining() const { return size - filled; }
    };

    SimulatedBroker(const Product& product, const std::string& queue_position = "back", ClockFn clock = wall_now);

    // -- the three verbs --
    OrderHandle place(int side, double price, int size) override;
    OrderHandle replace(OrderHandle handle, double price, int size) override;
    void cancel(OrderHandle handle) override;
    std::vector<OrderEvent> drain_events() override;
    std::int64_t messages_sent() const override { return sent_; }

    // Active orders, in placement order.
    std::vector<SimOrder> working() const;
    const SimOrder* find(std::int64_t order_id) const;

    // -- the market --
    void on_book(const BookSnapshot& snapshot);
    void on_trade(const Trade& trade);

    std::vector<Fill> fills;

private:
    std::int64_t queue_at(int side, double price) const;
    void fill(SimOrder& order, int size, double price);
    void compact();

    const Product& product_;
    bool front_;
    ClockFn clock_;
    std::vector<SimOrder> orders_;
    std::vector<OrderEvent> events_;
    std::int64_t next_id_ = 1;
    std::optional<BookSnapshot> snapshot_;
    std::int64_t sent_ = 0;
};

}  // namespace harvester
