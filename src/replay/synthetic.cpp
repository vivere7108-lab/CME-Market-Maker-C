#include "harvester/replay/synthetic.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace harvester {

SyntheticMarket::SyntheticMarket(const Product& product, double seconds, double start_price, std::uint64_t seed,
                                 double toxic_fraction, std::int64_t start_ts)
    : rng_(seed), tick_(product.tick_int()), best_bid_(product.fixed(start_price)), toxic_fraction_(toxic_fraction),
      ts_(start_ts), end_(start_ts + static_cast<std::int64_t>(seconds * 1e9)) {
    bids_.reserve(DEPTH + 1);
    asks_.reserve(DEPTH + 1);
    for (int i = 0; i < DEPTH; ++i) bids_.push_back(depth());
    for (int i = 0; i < DEPTH; ++i) asks_.push_back(depth());
}

void SyntheticMarket::record(Mbp10Record& out, std::int64_t ts, char action, char side, std::int64_t price,
                             std::uint32_t size) {
    ++sequence_;
    out.ts_event = ts;
    out.sequence = sequence_;
    out.action = action;
    out.side = side;
    out.price = price;
    out.size = size;
    out.flags = 0;
    const std::int64_t ask = best_ask();
    for (int i = 0; i < DEPTH; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        BidAskPair& pair = out.levels[idx];
        pair.bid_px = best_bid_ - i * tick_;
        pair.ask_px = ask + i * tick_;
        pair.bid_sz = static_cast<std::uint32_t>(bids_[idx]);
        pair.ask_sz = static_cast<std::uint32_t>(asks_[idx]);
        pair.bid_ct = static_cast<std::uint32_t>(std::max(bids_[idx] / 7, 1));
        pair.ask_ct = static_cast<std::uint32_t>(std::max(asks_[idx] / 7, 1));
    }
}

bool SyntheticMarket::next(Mbp10Record& out) {
    static constexpr std::array<int, 2> kSides{1, -1};
    if (!started_) {
        started_ = true;
        record(out, ts_, 'A', 'B', best_bid_, static_cast<std::uint32_t>(bids_[0]));
        return true;
    }
    if (ts_ >= end_) return false;
    ts_ += static_cast<std::int64_t>(rng_.expovariate(1.0 / 0.03) * 1e9);  // ~30 ms between events
    const double roll = rng_.random();
    if (informed_ == 0 && rng_.random() < toxic_fraction_ * 0.03) {
        informed_ = static_cast<int>(rng_.randint(6, 20));
        informed_side_ = rng_.choice(kSides);
    }
    if (roll < 0.55 && informed_ == 0) {
        // Resting size changes at a level, more often near the touch.
        const int side = rng_.choice(kSides);
        const auto level = static_cast<std::size_t>(
            std::min<std::int64_t>(static_cast<std::int64_t>(rng_.expovariate(0.6)), DEPTH - 1));
        std::vector<std::int32_t>& book = side > 0 ? bids_ : asks_;
        const auto change = static_cast<std::int32_t>(rng_.randint(-30, 30));
        book[level] = std::max(book[level] + change, 1);
        const char action = change > 0 ? 'A' : 'C';
        const std::int64_t price = side > 0 ? (best_bid_ - static_cast<std::int64_t>(level) * tick_)
                                            : (best_ask() + static_cast<std::int64_t>(level) * tick_);
        record(out, ts_, action, side > 0 ? 'B' : 'A', price, static_cast<std::uint32_t>(std::abs(change)));
        return true;
    }
    // A trade.
    int aggressor;
    std::int32_t size;
    if (informed_ != 0) {
        aggressor = informed_side_;
        size = static_cast<std::int32_t>(rng_.randint(10, 60));
        --informed_;
        ++informed_trades;
    } else {
        aggressor = rng_.choice(kSides);
        size = static_cast<std::int32_t>(std::min<std::int64_t>(static_cast<std::int64_t>(rng_.expovariate(0.25)) + 1, 40));
    }
    ++trades;
    if (aggressor > 0) {
        const std::int64_t price = best_ask();
        asks_[0] -= size;
        if (asks_[0] <= 0) {
            asks_.erase(asks_.begin());
            asks_.push_back(depth());
            // The ask stepped up; the bid usually follows to keep the
            // spread one tick, sometimes after a moment.
            if (rng_.random() < 0.8) {
                best_bid_ += tick_;
                bids_.insert(bids_.begin(), static_cast<std::int32_t>(rng_.randint(5, 60)));
                bids_.pop_back();
            } else {
                best_bid_ += tick_;
                bids_.insert(bids_.begin(), static_cast<std::int32_t>(rng_.randint(1, 5)));
                bids_.pop_back();
            }
        }
        record(out, ts_, 'T', 'B', price, static_cast<std::uint32_t>(size));
    } else {
        const std::int64_t price = best_bid_;
        bids_[0] -= size;
        if (bids_[0] <= 0) {
            bids_.erase(bids_.begin());
            bids_.push_back(depth());
            best_bid_ -= tick_;
            if (rng_.random() < 0.8) {
                asks_.insert(asks_.begin(), static_cast<std::int32_t>(rng_.randint(5, 60)));
            } else {
                asks_.insert(asks_.begin(), static_cast<std::int32_t>(rng_.randint(1, 5)));
            }
            asks_.pop_back();
        }
        record(out, ts_, 'T', 'A', price, static_cast<std::uint32_t>(size));
    }
    return true;
}

}  // namespace harvester
