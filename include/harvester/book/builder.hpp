// Book builders: one per Databento schema, both producing ``OrderBook``.
//
// Both read plain record structs -- ``MboRecord`` and ``Mbp10Record`` --
// that carry exactly the fields the Python builders read by attribute, with
// ``action`` and ``side`` as the one-character codes DBN uses.  The
// Databento client's ``MboMsg`` / ``Mbp10Msg`` map onto them field for
// field (the adapter in ``src/databento`` does that), the synthetic market
// produces them directly, and a test can build them by hand.
//
// MBO
// ---
// Every order is tracked by ``order_id``; the aggregated levels are derived.
// The actions:
//
//     A  add      a new resting order
//     C  cancel   the order is gone (partial cancels arrive as M)
//     M  modify   new price and/or size
//     F  fill     the resting order traded ``size``; the level shrinks
//     T  trade    the match itself: price, size, and the *aggressor* side.
//                 This is the tape. It does not change the book -- the F
//                 records that follow it do
//     R  clear    the book is reset (start of session, or a recovery)
//     N  none     nothing happens to the book
//
// The first records of a live subscription (with ``snapshot=True``) or of
// an intraday replay carry the ``F_SNAPSHOT`` flag: they are the standing
// book, delivered as adds.  The book is cleared when a snapshot begins and
// marked complete when it ends, so nothing quotes against a book that is
// still being assembled -- an MBO stream joined mid-session with no
// snapshot is a partial book that looks whole, and ``complete`` stays
// false on it forever.
//
// MBP-10
// ------
// Each record carries the full ten levels a side after the change it
// describes, so the book is replaced from the record rather than updated.
// A trade record (``action == 'T'``) carries the levels too, and the same
// tape.  The book is complete from the first record.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "harvester/book/book.hpp"

namespace harvester {

// Databento record flags (``databento_dbn.F_*``).
inline constexpr std::uint8_t F_LAST = 128;
inline constexpr std::uint8_t F_TOB = 64;
inline constexpr std::uint8_t F_SNAPSHOT = 32;
inline constexpr std::uint8_t F_MBP = 16;
inline constexpr std::uint8_t F_BAD_TS_RECV = 8;
inline constexpr std::uint8_t F_MAYBE_BAD_BOOK = 4;

// Databento's "no price" sentinel.
inline constexpr std::int64_t UNDEF_PRICE = 9'223'372'036'854'775'807LL;

struct BidAskPair {
    std::int64_t bid_px = UNDEF_PRICE;
    std::int64_t ask_px = UNDEF_PRICE;
    std::uint32_t bid_sz = 0;
    std::uint32_t ask_sz = 0;
    std::uint32_t bid_ct = 0;
    std::uint32_t ask_ct = 0;
};

struct MboRecord {
    std::int64_t ts_event = 0;
    std::uint64_t order_id = 0;
    std::int64_t price = UNDEF_PRICE;
    std::uint32_t size = 0;
    std::uint32_t sequence = 0;
    std::uint8_t flags = 0;
    char action = 'N';
    char side = 'N';
};

struct Mbp10Record {
    static constexpr int DEPTH = 10;

    std::int64_t ts_event = 0;
    std::int64_t price = UNDEF_PRICE;
    std::uint32_t size = 0;
    std::uint32_t sequence = 0;
    std::uint8_t flags = 0;
    char action = 'N';
    char side = 'N';
    std::array<BidAskPair, DEPTH> levels{};
};

// +1 for 'B', -1 for 'A', 0 otherwise.
inline int side_of(char side) { return side == 'B' ? 1 : (side == 'A' ? -1 : 0); }

// Order-by-order reconstruction.
class MboBuilder {
public:
    MboBuilder();

    // Apply one record. Returns the trade it carried, if any.
    std::optional<Trade> apply(const MboRecord& record);
    // Contracts resting at ``price`` on ``side`` right now: what a new
    // order placed there would have in front of it.
    std::int64_t queue_ahead(int side, std::int64_t price) const;

    OrderBook book;
    // Records that referred to an order not held.
    std::int64_t dropped = 0;
    std::size_t orders_held() const { return orders_.size(); }

private:
    struct Order {
        int side;
        std::int64_t price;
        std::int32_t size;
    };
    void begin_snapshot();
    void end_snapshot();

    std::unordered_map<std::uint64_t, Order> orders_;
    bool in_snapshot_ = false;
};

// Ten aggregated levels a side, replaced from each record.
class Mbp10Builder {
public:
    static constexpr int DEPTH = Mbp10Record::DEPTH;

    std::optional<Trade> apply(const Mbp10Record& record);
    std::int64_t queue_ahead(int side, std::int64_t price) const;

    OrderBook book;
};

}  // namespace harvester
