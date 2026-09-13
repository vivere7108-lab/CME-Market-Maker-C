// The book itself, and what is read off it.
//
// ``OrderBook`` is the mutable state the builders write into; a
// ``BookSnapshot`` is the immutable, double-priced view handed to
// everything downstream.  Prices inside the book are fixed-point integers
// (see ``instruments``) so that levels are exact keys; sizes are integers
// in contracts.
//
// The book keeps each side as one flat vector sorted best-first, which is
// what makes it fast: the touch is element zero, a snapshot is a copy of
// the first ``depth`` elements, an MBP-10 record that replaces all ten
// levels is twenty appends into cleared storage that is never freed, and
// an MBO update near the touch -- where nearly all of them land -- moves
// a few bytes.  The Python system's dictionaries were the same structure
// with a heap sort on every snapshot.
//
// The microprice
// --------------
// The naive mid, ``(bid + ask) / 2``, says where the spread is centred.
// The microprice says where the *next* trade is likely to be::
//
//     microprice = (bid * ask_size + ask * bid_size) / (bid_size + ask_size)
//
// A bid queue ten times the ask queue means the ask is about to be lifted
// and the microprice sits near the ask.  It is the fair-value anchor every
// quote here is centred on.  Where a side is missing there is no
// microprice, and downstream reads that as "no book", not as a price.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "harvester/instruments.hpp"

namespace harvester {

// One price level: price in points, size in contracts, order count.
struct Level {
    double price = 0.0;
    std::int32_t size = 0;
    std::int32_t count = 0;

    bool operator==(const Level&) const = default;
};

// One execution off the tape.
//
// ``aggressor`` is ``+1`` for a buyer-initiated trade, ``-1`` for a
// seller-initiated one, ``0`` where CME named no aggressor (an implied or
// administrative match).  It is read straight off the feed; nothing here
// infers it.
struct Trade {
    std::int64_t ts_event = 0;  // nanoseconds since the epoch, exchange timestamp
    double price = 0.0;
    std::int32_t size = 0;
    std::int32_t aggressor = 0;

    std::int64_t signed_size() const { return static_cast<std::int64_t>(size) * aggressor; }
};

double microprice(double bid, double bid_size, double ask, double ask_size);

// The top ``depth`` levels a side, at one moment. A value type: copying it
// is what the feed does under its lock, so nothing downstream ever holds a
// reference into the live book.
class BookSnapshot {
public:
    static constexpr int kMaxDepth = 32;

    std::int64_t ts_event = 0;
    std::int64_t sequence = 0;
    // True until the builder has seen a complete book (a snapshot, or the
    // first full MBP-10 message).
    bool complete = true;

    BookSnapshot() = default;
    BookSnapshot(std::int64_t ts, std::int64_t seq, std::span<const Level> bid_levels,
                 std::span<const Level> ask_levels, bool is_complete = true);

    std::span<const Level> bids() const { return {bids_.data(), n_bids_}; }
    std::span<const Level> asks() const { return {asks_.data(), n_asks_}; }
    void push_bid(const Level& level) { bids_[n_bids_++] = level; }
    void push_ask(const Level& level) { asks_[n_asks_++] = level; }

    const Level* best_bid() const { return n_bids_ ? &bids_[0] : nullptr; }
    const Level* best_ask() const { return n_asks_ ? &asks_[0] : nullptr; }
    bool two_sided() const { return n_bids_ > 0 && n_asks_ > 0 && complete; }
    std::optional<double> mid() const;
    std::optional<double> spread() const;
    std::optional<double> microprice() const;
    // ``(bid_size - ask_size) / (bid_size + ask_size)`` at the touch.
    std::optional<double> top_imbalance() const;
    // Contracts resting on ``side`` (+1 bid, -1 ask) over the top ``levels``
    // (all kept levels when ``levels`` is negative).
    std::int64_t depth(int side, int levels = -1) const;

private:
    std::array<Level, kMaxDepth> bids_{};
    std::array<Level, kMaxDepth> asks_{};
    std::size_t n_bids_ = 0;
    std::size_t n_asks_ = 0;
};

// Aggregated price levels a side. Fixed-point prices, integer sizes.
class OrderBook {
public:
    struct PriceLevel {
        std::int64_t price;
        std::int32_t size;
        std::int32_t count;
    };

    std::int64_t ts_event = 0;
    std::int64_t sequence = 0;
    bool complete = false;

    OrderBook();

    void clear();
    // The levels of one side, best first.
    const std::vector<PriceLevel>& side(int side) const { return side > 0 ? bids_ : asks_; }
    void add(int side, std::int64_t price, std::int32_t size, std::int32_t count = 1);
    void remove(int side, std::int64_t price, std::int32_t size, std::int32_t count = 1);
    void set_level(int side, std::int64_t price, std::int32_t size, std::int32_t count);
    std::optional<std::int64_t> best(int side) const;
    // The level at ``price`` on ``side``, or nullptr.
    const PriceLevel* find(int side, std::int64_t price) const;
    BookSnapshot snapshot(int depth) const;
    // Fills ``out`` in place (no allocation, no temporaries).
    void snapshot_into(int depth, BookSnapshot& out) const;

private:
    std::vector<PriceLevel>& levels(int side) { return side > 0 ? bids_ : asks_; }
    // Where ``price`` sits (or would sit) in the sorted vector of ``side``.
    std::size_t position(int side, std::int64_t price, bool& found) const;

    std::vector<PriceLevel> bids_;  // descending price
    std::vector<PriceLevel> asks_;  // ascending price
};

}  // namespace harvester
