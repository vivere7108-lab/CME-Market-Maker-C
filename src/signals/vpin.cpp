#include "harvester/signals/vpin.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "harvester/util/format.hpp"
#include "harvester/util/fsum.hpp"

namespace harvester {

Vpin::Vpin(double bucket_contracts, int window_buckets, const std::string& unknown_side)
    : bucket_(bucket_contracts), window_(window_buckets), split_(unknown_side == "split") {}

std::optional<double> Vpin::value() const {
    if (imbalances_.empty()) return std::nullopt;
    // fsum, not a plain loop: this mirrors Python's ``sum(self._imbalances)``,
    // and on 3.12+ that is compensated. See util/fsum.hpp.
    return fsum(imbalances_) / static_cast<double>(imbalances_.size());
}

double Vpin::fill_fraction() const { return std::min((buy_ + sell_) / bucket_, 1.0); }

int Vpin::update(const Trade& trade) {
    double buy = 0.0;
    double sell = 0.0;
    if (trade.aggressor > 0) {
        buy = static_cast<double>(trade.size);
    } else if (trade.aggressor < 0) {
        sell = static_cast<double>(trade.size);
    } else if (split_) {
        buy = sell = static_cast<double>(trade.size) / 2.0;
    } else {
        return 0;
    }
    int closed = 0;
    while (buy + sell > 0) {
        const double room = bucket_ - (buy_ + sell_);
        const double total = buy + sell;
        if (total < room) {
            buy_ += buy;
            sell_ += sell;
            break;
        }
        const double share = room / total;
        buy_ += buy * share;
        sell_ += sell * share;
        if (static_cast<int>(imbalances_.size()) >= window_) imbalances_.pop_front();
        imbalances_.push_back(std::fabs(buy_ - sell_) / bucket_);
        ++buckets_closed;
        ++closed;
        buy -= buy * share;
        sell -= sell * share;
        buy_ = sell_ = 0.0;
        if (buy + sell < 1e-9) break;
    }
    return closed;
}

ToxicityGate::ToxicityGate(const ToxicityConfig& cfg)
    : vpin(cfg.bucket_contracts, cfg.window_buckets, cfg.unknown_side), cfg_(cfg) {
    sorted_.reserve(static_cast<std::size_t>(cfg.history_buckets) + 1);
}

std::optional<ToxicityState> ToxicityGate::update(const Trade& trade) {
    const int closed = vpin.update(trade);
    if (closed == 0) return std::nullopt;
    for (int i = 0; i < closed; ++i) on_bucket();
    return state();
}

void ToxicityGate::on_bucket() {
    const auto value = vpin.value();
    if (!value) return;
    if (static_cast<int>(history_.size()) >= cfg_.history_buckets) {
        const double oldest = history_.front();
        history_.pop_front();
        const auto at = std::lower_bound(sorted_.begin(), sorted_.end(), oldest);
        if (at != sorted_.end()) sorted_.erase(at);
    }
    history_.push_back(*value);
    sorted_.insert(std::upper_bound(sorted_.begin(), sorted_.end(), *value), *value);
    // Rank of the current reading among the history it is now part of.
    const auto below = static_cast<double>(std::lower_bound(sorted_.begin(), sorted_.end(), *value) - sorted_.begin());
    const auto equal = static_cast<double>(std::upper_bound(sorted_.begin(), sorted_.end(), *value) - sorted_.begin()) - below;
    percentile_ = (below + 0.5 * equal) / static_cast<double>(sorted_.size());
    ++dwell_;
    if (!warmed_up() || !cfg_.enabled) return;
    step(*percentile_);
}

void ToxicityGate::step(double rank) {
    const std::array<std::pair<double, double>, 3> thresholds{{
        {cfg_.elevated_enter, cfg_.elevated_exit},
        {cfg_.toxic_enter, cfg_.toxic_exit},
        {cfg_.extreme_enter, cfg_.extreme_exit},
    }};
    const int before = level_;
    // Up: as many levels as the rank has crossed, immediately.
    while (level_ < static_cast<int>(thresholds.size()) && rank >= thresholds[static_cast<std::size_t>(level_)].first) {
        ++level_;
    }
    if (level_ > before) {
        dwell_ = 0;
        transitions.push_back({vpin.buckets_closed, static_cast<ToxicityLevel>(before), static_cast<ToxicityLevel>(level_)});
        return;
    }
    // Down: one level at a time, past the exit, after the dwell.
    if (level_ > 0 && rank < thresholds[static_cast<std::size_t>(level_ - 1)].second && dwell_ >= cfg_.min_dwell_buckets) {
        --level_;
        dwell_ = 0;
        transitions.push_back({vpin.buckets_closed, static_cast<ToxicityLevel>(before), static_cast<ToxicityLevel>(level_)});
    }
}

ToxicityState ToxicityGate::state() const {
    const auto level = cfg_.enabled ? static_cast<ToxicityLevel>(level_) : ToxicityLevel::Calm;
    return ToxicityState{
        level, vpin.value(), percentile_, warmed_up(),
        cfg_.spread_multiplier_for(level), cfg_.size_multiplier_for(level), vpin.buckets_closed,
    };
}

std::string ToxicityGate::describe() const {
    const ToxicityState s = state();
    if (!s.vpin) return "VPIN n/a (no bucket closed yet)";
    const std::string rank = s.percentile ? fmt::pct0(*s.percentile) : "-";
    const std::string warm = s.warmed_up ? "" : std::format(" (warming up: {}/{})", history_.size(), cfg_.warmup_buckets);
    return std::format("VPIN {:.3f} p{} -> {}{}", *s.vpin, rank, to_string(s.level), warm);
}

}  // namespace harvester
