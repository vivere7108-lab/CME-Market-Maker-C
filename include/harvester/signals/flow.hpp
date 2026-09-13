// The fast signals: what the book and the tape are doing *right now*.
//
// Each is a signed number, positive for buying pressure, and each is
// squashed into ``[-1, 1]`` so the quoting engine can turn "full
// deflection" into a configured number of ticks of skew without knowing
// the units.  None of them is gated: they are the per-quote reflexes that
// VPIN, by construction, cannot be.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>

#include "harvester/book/book.hpp"
#include "harvester/config.hpp"

namespace harvester {

// Cont, Kukanov & Stoikov (2014) order flow imbalance at the touch.
//
// For consecutive top-of-book states ``n-1`` and ``n``::
//
//     e_n =  1{Pb_n >= Pb_{n-1}} * qb_n  -  1{Pb_n <= Pb_{n-1}} * qb_{n-1}
//          - 1{Pa_n <= Pa_{n-1}} * qa_n  +  1{Pa_n >= Pa_{n-1}} * qa_{n-1}
//
// summed over a trailing window and normalised by the average depth at
// the touch, so a reading of ``+1`` means "a full touch-depth of net
// buying pressure in the window".
class OrderFlowImbalance {
public:
    explicit OrderFlowImbalance(double window_ms);
    void update(const BookSnapshot& snapshot);
    double value(std::int64_t now_ns);

private:
    struct Top {
        double bid_price;
        std::int32_t bid_size;
        double ask_price;
        std::int32_t ask_size;
    };
    void expire(std::int64_t now_ns);

    std::int64_t window_ns_;
    std::deque<std::pair<std::int64_t, double>> events_;
    double sum_ = 0.0;
    std::optional<Top> last_;
    double depth_ = 1.0;
};

// How fast the best level on each side is being consumed.
//
// Signed: a bid being eaten is selling pressure (negative), an ask being
// eaten is buying pressure (positive).  A level that vanishes is full
// depletion; a growing queue is a mild opposite.
class QueueDepletion {
public:
    explicit QueueDepletion(double window_ms);
    void update(const BookSnapshot& snapshot);
    // Ask depletion minus bid depletion: positive is buying pressure.
    double value() const;
    double side_rate(int side) const { return side > 0 ? rate_bid_ : rate_ask_; }

private:
    struct Held {
        double price;
        std::int32_t size;
        std::int64_t ts;
    };
    void update_side(int side, const Level& level, std::int64_t ts, std::optional<Held>& held, double& rate);

    double window_s_;
    std::optional<Held> bid_;
    std::optional<Held> ask_;
    double rate_bid_ = 0.0;
    double rate_ask_ = 0.0;
};

// The signed length of the current run of same-side aggressor trades,
// decayed towards zero over ``decay_seconds`` and saturating at
// ``saturation`` trades.
class AggressorRun {
public:
    AggressorRun(int saturation, double decay_seconds);
    void update(const Trade& trade);
    double value(std::int64_t now_ns) const;
    std::int64_t run() const { return run_; }
    std::int64_t volume() const { return volume_; }

private:
    int saturation_;
    double decay_ns_;
    std::int64_t run_ = 0;
    std::int64_t volume_ = 0;
    std::optional<std::int64_t> last_ts_;
};

struct FlowState {
    double ofi = 0.0;
    double depletion = 0.0;
    double run = 0.0;
    std::int64_t run_length = 0;

    // A single pressure number for the log; the engine weights each.
    double composite() const { return (ofi + depletion + run) / 3.0; }
};

// The three fast signals, updated together.
class FlowSignals {
public:
    explicit FlowSignals(const FlowConfig& cfg);
    void on_book(const BookSnapshot& snapshot);
    void on_trade(const Trade& trade);
    FlowState state(std::int64_t now_ns);

    OrderFlowImbalance ofi;
    QueueDepletion depletion;
    AggressorRun run;
};

}  // namespace harvester
