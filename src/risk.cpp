#include "harvester/risk.hpp"

#include <cmath>
#include <filesystem>
#include <format>

#include "harvester/util/format.hpp"
#include "harvester/util/log.hpp"

namespace harvester {

namespace {
constexpr const char* kLog = "harvester.risk";
}

void RiskMonitor::halt(const std::string& reason) {
    if (!halted) HLOG_ERROR(kLog, "HALT: {}", reason);
    halted = true;
    halt_reason = reason;
}

Verdict RiskMonitor::evaluate(const Inventory& inventory, std::optional<double> mark, double feed_age_seconds,
                              bool in_hours, const AccountValues* account, std::optional<int> broker_position,
                              std::optional<double> sigma) {
    const RiskConfig& cfg = cfg_;
    if (halted) return Verdict{false, true, cfg.flatten_on_halt && inventory.position != 0, halt_reason};

    std::error_code ec;
    if (std::filesystem::exists(cfg.kill_file, ec)) {
        return Verdict{false, true, false, std::format("kill file {} exists", cfg.kill_file)};
    }

    const double pnl = inventory.total_pnl(mark);
    if (pnl <= -cfg.daily_loss_limit_usd) {
        halt(std::format("daily loss ${} is past the ${} limit", fmt::commas(-pnl), fmt::commas(cfg.daily_loss_limit_usd)));
        return Verdict{false, true, cfg.flatten_on_halt && inventory.position != 0, halt_reason};
    }

    if (broker_position && *broker_position != inventory.position) {
        halt(std::format("the broker reports a position of {:+d} and the book holds {:+d}; something filled outside "
                         "this process's record",
                         *broker_position, inventory.position));
        return Verdict{false, true, false, halt_reason};
    }

    if (std::abs(inventory.position) > cfg.max_position) {
        halt(std::format("position {:+d} is outside the cap of {}", inventory.position, cfg.max_position));
        return Verdict{false, true, cfg.flatten_on_halt, halt_reason};
    }

    if (account && !account->empty()) {
        const auto held_it = account->find("FullInitMarginReq");
        const auto nav_it = account->find("NetLiquidation");
        const double held = held_it != account->end() ? held_it->second : 0.0;
        const double nav = nav_it != account->end() ? nav_it->second : 0.0;
        if (held != 0.0 && nav != 0.0 && nav > 0 && held / nav > cfg.max_margin_utilisation) {
            halt(std::format("initial margin ${} is {} of the ${} account, past the {} utilisation cap",
                             fmt::commas(held), fmt::pct0(held / nav), fmt::commas(nav),
                             fmt::pct0(cfg.max_margin_utilisation)));
            return Verdict{false, true, false, halt_reason};
        }
    }

    if (cfg.max_sigma > 0.0 && sigma && *sigma > cfg.max_sigma) {
        return Verdict{false, true, false,
                       std::format("realised vol {:.3f} is above the {:.3f} ceiling", *sigma, cfg.max_sigma)};
    }
    if (feed_age_seconds > cfg.stale_book_cancel_seconds) {
        return Verdict{false, true, false, std::format("book is {:.1f}s stale", feed_age_seconds)};
    }
    if (!in_hours) {
        const bool flatten = cfg.flatten_outside_hours && inventory.position != 0;
        return Verdict{false, true, flatten, "outside quoting hours"};
    }
    return Verdict{true, false, false, ""};
}

}  // namespace harvester
