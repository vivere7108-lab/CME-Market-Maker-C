// What stops the quoter, and what merely pulls it.
//
// * **pull** -- cancel every quote now, resume when the condition clears.
//   A stale book, outside the quoting hours, realised vol above
//   ``max_sigma``, the operator's kill file while it exists.
// * **halt** -- cancel, optionally flatten, and stay stopped until a
//   person restarts the process.  The daily loss limit, margin past the
//   utilisation cap, a position the account holds that the book cannot
//   see.  A halt is sticky because whatever caused it does not go away
//   when the number that reported it moves back inside its limit.
//
// The inventory *cap* is not here.  It is applied in the quoting engine,
// where it decides which side is quoted; this layer only checks that the
// position the broker reports is inside it.
#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "harvester/config.hpp"
#include "harvester/inventory.hpp"

namespace harvester {

using AccountValues = std::unordered_map<std::string, double>;

struct Verdict {
    // Quotes may be placed.
    bool quote = false;
    // Every working quote must be cancelled now.
    bool pull = false;
    // Flatten the position with a marketable order.
    bool flatten = false;
    std::string reason;
};

class RiskMonitor {
public:
    explicit RiskMonitor(const RiskConfig& cfg) : cfg_(cfg) {}

    void halt(const std::string& reason);
    // ``sigma`` is the realised vol of the anchor, in points per
    // root-second, or none while the estimator is still warming up -- the
    // ceiling cannot be applied to a number that is still the floor.
    Verdict evaluate(const Inventory& inventory, std::optional<double> mark, double feed_age_seconds, bool in_hours,
                     const AccountValues* account = nullptr, std::optional<int> broker_position = std::nullopt,
                     std::optional<double> sigma = std::nullopt);

    const RiskConfig& cfg() const { return cfg_; }
    bool halted = false;
    std::string halt_reason;

private:
    RiskConfig cfg_;
};

}  // namespace harvester
