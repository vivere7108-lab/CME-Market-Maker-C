// Short-term realised volatility of the anchor.
//
// Sampled at a fixed interval rather than per tick -- per-tick returns of
// a microprice are dominated by bid-ask bounce and queue noise, and would
// size the spread to the noise -- and averaged with an exponential
// half-life so the last minute counts more than the last hour.  Reported
// in **points per root-second**, which is the unit the Avellaneda-Stoikov
// expression wants alongside a horizon in seconds.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace harvester {

class RealisedVol {
public:
    RealisedVol(double halflife_seconds, double sample_ms, double floor, double ceiling)
        : sample_ns_(static_cast<std::int64_t>(sample_ms * 1e6)), floor_(floor), ceiling_(ceiling) {
        // Decay per sample such that weight halves every halflife.
        const double samples_per_halflife = std::max(halflife_seconds * 1000.0 / sample_ms, 1.0);
        decay_ = std::pow(0.5, 1.0 / samples_per_halflife);
    }

    void update(double price, std::int64_t ts_ns) {
        if (!last_ts_) {
            last_price_ = price;
            last_ts_ = ts_ns;
            return;
        }
        const std::int64_t elapsed = ts_ns - *last_ts_;
        if (elapsed < sample_ns_) return;
        const double dt = static_cast<double>(elapsed) / 1e9;
        const double r = price - last_price_;
        const double variance = r * r / dt;  // per second
        if (!have_variance_) {
            variance_per_s_ = variance;
            have_variance_ = true;
        } else {
            variance_per_s_ = decay_ * variance_per_s_ + (1 - decay_) * variance;
        }
        last_price_ = price;
        last_ts_ = ts_ns;
        ++samples;
    }

    // Points per root-second, clamped to the configured band.
    double sigma() const {
        if (!have_variance_) return floor_;
        return std::min(std::max(std::sqrt(variance_per_s_), floor_), ceiling_);
    }

    bool warmed_up() const { return samples >= 10; }

    std::int64_t samples = 0;

private:
    std::int64_t sample_ns_;
    double floor_;
    double ceiling_;
    double decay_;
    bool have_variance_ = false;
    double variance_per_s_ = 0.0;
    double last_price_ = 0.0;
    std::optional<std::int64_t> last_ts_;
};

}  // namespace harvester
