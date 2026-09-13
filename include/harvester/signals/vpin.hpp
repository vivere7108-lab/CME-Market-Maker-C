// VPIN, and the regime gate on top of it.
//
// Volume-synchronised probability of informed trading (Easley, Lopez de
// Prado & O'Hara 2012).  The tape is cut into buckets of ``V`` contracts;
// in each bucket the buyer-initiated volume ``B`` and the seller-initiated
// volume ``S`` are counted, and::
//
//     VPIN = mean over the last n buckets of |B - S| / V
//
// MDP 3.0 states the aggressor in every match event and Databento passes
// it through, so ``B`` and ``S`` are read off the flag directly.  Trades
// with no aggressor named (implied and administrative matches) are split
// evenly or ignored, per config.
//
// It is a statement about the *last n buckets*: a bucket only closes when
// ``V`` contracts have traded, so VPIN is exactly as current as the tape
// is busy and updates only then.  That makes it a **regime** signal by
// construction, and the gate built on it treats it as one: percentile-
// ranked against its own recent history, with hysteresis between levels
// and a minimum dwell, so it moves the spread in steps and never tick by
// tick.  The per-quote signals that do move tick by tick are in ``flow``.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "harvester/book/book.hpp"
#include "harvester/config.hpp"

namespace harvester {

// The bucketed imbalance series.
class Vpin {
public:
    Vpin(double bucket_contracts, int window_buckets, const std::string& unknown_side = "split");

    // Mean absolute imbalance over the window; none before one bucket.
    std::optional<double> value() const;
    // How full the current bucket is, 0-1.
    double fill_fraction() const;
    // Fold one trade in. Returns how many buckets it closed (0, 1, ...).
    //
    // A trade larger than what the current bucket has room for spills into
    // the next -- and a block bigger than a whole bucket closes several,
    // each carrying its share of the one-sided volume.
    int update(const Trade& trade);

    double bucket() const { return bucket_; }
    int window() const { return window_; }
    std::int64_t buckets_closed = 0;

private:
    double bucket_;
    int window_;
    bool split_;
    double buy_ = 0.0;
    double sell_ = 0.0;
    std::deque<double> imbalances_;
};

struct ToxicityState {
    ToxicityLevel level = ToxicityLevel::Calm;
    std::optional<double> vpin;
    std::optional<double> percentile;
    bool warmed_up = false;
    double spread_multiplier = 1.0;
    double size_multiplier = 1.0;
    std::int64_t buckets = 0;

    int index() const { return level_index(level); }
    const char* level_name() const { return to_string(level); }
};

struct ToxicityTransition {
    std::int64_t bucket;
    ToxicityLevel from;
    ToxicityLevel to;
};

// VPIN ranked against its own history, with hysteresis and dwell.
//
// Each closed bucket produces one VPIN reading, which is ranked against
// the last ``history_buckets`` readings.  The level steps *up* the moment
// the rank crosses an ``enter`` threshold and steps *down* only once the
// rank is below the ``exit`` threshold of the level being left **and** the
// current level has held for ``min_dwell_buckets``.
//
// Before ``warmup_buckets`` readings the rank is not meaningful and the
// gate reports ``calm`` with ``warmed_up`` false.
class ToxicityGate {
public:
    explicit ToxicityGate(const ToxicityConfig& cfg);

    const ToxicityConfig& cfg() const { return cfg_; }
    bool enabled() const { return cfg_.enabled; }
    // Fold a trade in. Returns the new state if a bucket closed.
    std::optional<ToxicityState> update(const Trade& trade);
    bool warmed_up() const { return static_cast<int>(history_.size()) >= cfg_.warmup_buckets; }
    std::size_t history_size() const { return history_.size(); }
    ToxicityState state() const;
    std::string describe() const;

    Vpin vpin;
    std::vector<ToxicityTransition> transitions;

private:
    void on_bucket();
    void step(double rank);

    ToxicityConfig cfg_;
    std::deque<double> history_;
    std::vector<double> sorted_;
    int level_ = 0;
    int dwell_ = 0;
    std::optional<double> percentile_;
};

}  // namespace harvester
