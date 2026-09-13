#include "harvester/quoting/engine.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace harvester {

namespace {

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) out += sep;
        out += p;
    }
    return out;
}

// Python ``round(x, 10)`` on a value already on the tick grid.
double round10(double x) { return std::nearbyint(x * 1e10) / 1e10; }

}  // namespace

std::string QuoteDecision::describe(const Product& product) const {
    if (!anchor) return "no quotes: " + join(reasons, "; ");
    const std::string b = bid ? std::format("{}@{:.2f}", bid->size, bid->price) : "-";
    const std::string a = ask ? std::format("{}@{:.2f}", ask->size, ask->price) : "-";
    const double half = half_spread ? product.ticks(*half_spread) : 0.0;
    std::string text = std::format("bid {} | ask {} | anchor {:.3f} r {:.3f} half {:.1f}t skew {:+.2f}t sigma {:.3f} [{}]",
                                   b, a, *anchor, *reservation, half, skew_ticks, sigma, to_string(toxicity));
    if (!reasons.empty()) text += " (" + join(reasons, "; ") + ")";
    return text;
}

QuoteEngine::QuoteEngine(const QuotingConfig& cfg_, const RiskConfig& risk_, const Product& product_,
                         const std::string& anchor, const std::string& extreme_action)
    : cfg(cfg_), risk(risk_), product(product_), anchor_is_microprice(anchor == "microprice"),
      extreme_pull(extreme_action == "pull") {}

double QuoteEngine::reservation(double anchor, int inventory, double sigma) const {
    return anchor - inventory * cfg.gamma * sigma * sigma * cfg.horizon_seconds;
}

double QuoteEngine::model_spread(double sigma) const {
    const double g = cfg.gamma;
    const double k = cfg.kappa;
    return g * sigma * sigma * cfg.horizon_seconds + (2.0 / g) * std::log(1.0 + g / k);
}

QuoteDecision QuoteEngine::decide(const BookSnapshot& snapshot, double sigma, const FlowState& flow,
                                  const ToxicityState& toxicity, int inventory) const {
    const double tick = product.tick_size;
    QuoteDecision out;
    out.sigma = sigma;
    out.toxicity = toxicity.level;
    if (!snapshot.two_sided()) {
        out.reasons.emplace_back("no two-sided book");
        return out;
    }
    const double anchor = anchor_is_microprice ? *snapshot.microprice() : *snapshot.mid();
    const double best_bid = snapshot.best_bid()->price;
    const double best_ask = snapshot.best_ask()->price;

    double r = reservation(anchor, inventory, sigma);
    double half = model_spread(sigma) / 2.0;
    half *= toxicity.spread_multiplier;
    half = std::max(half, cfg.min_half_spread_ticks * tick);

    const double skew_ticks = cfg.skew_ofi_ticks * flow.ofi + cfg.skew_depletion_ticks * flow.depletion +
                              cfg.skew_run_ticks * flow.run;
    r += skew_ticks * tick;

    // Cooperative: behind the touch, snapped outwards.
    const double behind = cfg.behind_best_ticks * tick;
    const double raw_bid = std::min(r - half, best_bid - behind);
    const double raw_ask = std::max(r + half, best_ask + behind);
    double bid_price = std::floor(raw_bid / tick + 1e-9) * tick;
    double ask_price = std::ceil(raw_ask / tick - 1e-9) * tick;
    bid_price = round10(bid_price);
    ask_price = round10(ask_price);

    // A non-zero multiplier shrinks the size but never removes the quote:
    // only a multiplier of 0 does that. Ceil, so 1 x 0.5 is 1.
    int size = static_cast<int>(std::ceil(cfg.base_size * toxicity.size_multiplier - 1e-9));
    size = std::min(std::max(size, 0), cfg.max_size);

    bool want_bid = true;
    bool want_ask = true;
    if (toxicity.level == ToxicityLevel::Extreme) {
        if (extreme_pull || inventory == 0) {
            want_bid = want_ask = false;
            out.reasons.emplace_back("extreme toxicity: pulled");
        } else {
            // Reduce only, at least one contract so the side can work.
            size = std::max(size, 1);
            want_bid = inventory < 0;
            want_ask = inventory > 0;
            out.reasons.emplace_back("extreme toxicity: reduce-only");
        }
    }
    if (size <= 0) {
        want_bid = want_ask = false;
        out.reasons.push_back(std::format("size multiplier {:g} leaves nothing to quote", toxicity.size_multiplier));
    }

    // Inventory.
    if (std::abs(inventory) >= risk.reduce_only_position && inventory != 0) {
        if (inventory > 0 && want_bid) {
            want_bid = false;
            out.reasons.push_back(std::format("long {}: reduce-only, no bid", inventory));
        }
        if (inventory < 0 && want_ask) {
            want_ask = false;
            out.reasons.push_back(std::format("short {}: reduce-only, no ask", -inventory));
        }
    }
    const int bid_size = std::min(size, risk.max_position - inventory);
    const int ask_size = std::min(size, risk.max_position + inventory);
    if (want_bid && bid_size <= 0) {
        want_bid = false;
        out.reasons.emplace_back("at the position cap: no bid");
    }
    if (want_ask && ask_size <= 0) {
        want_ask = false;
        out.reasons.emplace_back("at the position cap: no ask");
    }

    // Too far behind the touch to be anything but a bad fill.
    const double max_behind = cfg.max_behind_ticks * tick + 1e-9;
    if (want_bid && best_bid - bid_price > max_behind) {
        want_bid = false;
        out.reasons.push_back(std::format("bid {:.0f}t behind the touch", product.ticks(best_bid - bid_price)));
    }
    if (want_ask && ask_price - best_ask > max_behind) {
        want_ask = false;
        out.reasons.push_back(std::format("ask {:.0f}t behind the touch", product.ticks(ask_price - best_ask)));
    }

    if (want_bid) out.bid = Quote{1, bid_price, bid_size};
    if (want_ask) out.ask = Quote{-1, ask_price, ask_size};
    out.anchor = anchor;
    out.reservation = r;
    out.half_spread = half;
    out.skew_ticks = skew_ticks;
    return out;
}

}  // namespace harvester
