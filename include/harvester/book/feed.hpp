// Feeds: where records come from, and the book they keep.
//
// ``RecordFeed`` is pushed records by whoever holds them -- the replay
// loop, a test, or the Databento session's I/O thread (``DatabentoBookFeed``
// derives from it).  The runners read it one way: ``snapshot()`` for the
// book as it stands, ``take_trades()`` for the executions since the last
// call, ``age_seconds`` for how long since the book last moved.  The book
// is the feed's; the runner never touches it directly, and every read is a
// copy taken under the feed's lock.  The lock is held for the few hundred
// nanoseconds an update or a copy takes, so the I/O thread is never
// blocked behind a decision.
//
// The traded contract
// -------------------
// A continuous symbol (``ES.v.0``) resolves to a raw contract (``ESZ6``)
// that Databento reports in a symbol-mapping message at the start of the
// session and again at each roll.  The feed keeps the current one and the
// runner routes IBKR orders to it, so the month quoted is the month the
// book was built from.
#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "harvester/book/book.hpp"
#include "harvester/book/builder.hpp"
#include "harvester/util/clock.hpp"

namespace harvester {

enum class Schema { Mbo, Mbp10 };
Schema schema_from_string(const std::string& name);
const char* to_string(Schema schema);

class RecordFeed {
public:
    // Executions kept between drains. A drain that stops -- the decision
    // loop wedged -- must not become unbounded memory.
    static constexpr std::size_t MAX_TRADES = 100'000;

    // ``clock`` returns the "now" in seconds that ``age_seconds`` is
    // measured against -- monotonic time for a live feed, the replay's own
    // simulated clock for a replay.
    RecordFeed(Schema schema, int depth, ClockFn clock = mono_now);
    RecordFeed(const std::string& schema, int depth, ClockFn clock = mono_now);
    virtual ~RecordFeed() = default;

    Schema schema() const { return schema_; }
    int depth() const { return depth_; }

    std::optional<std::string> raw_symbol() const;
    void set_symbol(std::string raw);

    // The record must match the feed's schema.
    std::optional<Trade> push(const MboRecord& record);
    std::optional<Trade> push(const Mbp10Record& record);

    BookSnapshot snapshot() const;
    void snapshot_into(BookSnapshot& out) const;
    std::vector<Trade> take_trades();
    // Appends into ``out`` (cleared first) without allocating a new vector.
    void take_trades_into(std::vector<Trade>& out);
    // Seconds since the last record; infinite before the first.
    double age_seconds(double now) const;
    std::int64_t queue_ahead(int side, std::int64_t price) const;

    std::int64_t records() const;
    std::int64_t dropped_trades() const;
    // Whether the builder's book has ever been marked complete.
    bool complete() const;

    // Lifecycle hooks a live feed implements; a pushed feed has none.
    virtual void start() {}
    virtual void close() {}
    virtual std::optional<std::string> wait_for_symbol(double /*timeout_seconds*/) { return raw_symbol(); }
    virtual bool is_live() const { return false; }
    // Errors the live session has reported; none for a pushed feed.
    virtual std::int64_t errors() const { return 0; }

protected:
    mutable std::mutex mutex_;

private:
    template <typename Record>
    std::optional<Trade> push_locked(const Record& record);
    const OrderBook& book() const { return schema_ == Schema::Mbo ? mbo_.book : mbp10_.book; }

    Schema schema_;
    int depth_;
    ClockFn clock_;
    MboBuilder mbo_;
    Mbp10Builder mbp10_;
    std::deque<Trade> trades_;
    std::optional<double> last_update_;
    std::optional<std::string> raw_symbol_;
    std::int64_t records_ = 0;
    std::int64_t dropped_trades_ = 0;
};

}  // namespace harvester
