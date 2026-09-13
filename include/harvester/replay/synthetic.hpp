// A generated market, shaped like an MBP-10 stream.
//
// A harness, not a model.  It exists so the whole pipeline -- book,
// signals, engine, throttle, simulated exchange, journal -- can be run end
// to end without a Databento key, and so the tests have a tape with known
// properties.  It is the Python generator's algorithm with the Python
// generator's random numbers, so seed 7 here is seed 7 there.
//
// What it generates: a one-tick-wide book with ten levels a side of random
// depth, resting size that fluctuates, and trades that consume the touch.
// Trades are mostly noise -- either side, small -- with **informed
// episodes** at ``toxic_fraction``: runs of same-side sweeps that exhaust
// the touch, move the mid, and keep going.  A result on this market says
// the machinery reacts to that structure; it says nothing about whether ES
// has it.
#pragma once

#include <cstdint>
#include <vector>

#include "harvester/book/builder.hpp"
#include "harvester/instruments.hpp"
#include "harvester/replay/pyrandom.hpp"

namespace harvester {

class SyntheticMarket {
public:
    static constexpr int DEPTH = 10;

    SyntheticMarket(const Product& product, double seconds, double start_price, std::uint64_t seed = 7,
                    double toxic_fraction = 0.2, std::int64_t start_ts = 1'700'000'000'000'000'000LL);

    // The next record of the tape, or false when it has ended.
    bool next(Mbp10Record& out);

    std::int64_t trades = 0;
    std::int64_t informed_trades = 0;

private:
    std::int32_t depth() { return static_cast<std::int32_t>(rng_.randint(20, 200)); }
    std::int64_t best_ask() const { return best_bid_ + tick_; }
    void record(Mbp10Record& out, std::int64_t ts, char action, char side, std::int64_t price, std::uint32_t size);

    PyRandom rng_;
    std::int64_t tick_;
    std::int64_t best_bid_;
    double toxic_fraction_;
    std::int64_t ts_;
    std::int64_t end_;
    std::vector<std::int32_t> bids_;
    std::vector<std::int32_t> asks_;
    std::uint32_t sequence_ = 0;
    int informed_ = 0;  // trades left in the current informed episode
    int informed_side_ = 0;
    bool started_ = false;
};

}  // namespace harvester
