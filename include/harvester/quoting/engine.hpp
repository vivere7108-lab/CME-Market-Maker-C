// The quoting engine: where the two quotes go, and how big they are.
//
// One call per decision cycle.  In order:
//
// 1. **Anchor.**  The microprice (or the mid, as the control) off the
//    current snapshot.  No two-sided book, no quotes.
// 2. **Reservation price and spread**, Avellaneda-Stoikov::
//
//        r     = anchor - q * gamma * sigma^2 * tau
//        delta = gamma * sigma^2 * tau + (2 / gamma) * ln(1 + gamma / kappa)
//
// 3. **Toxicity.**  The half-spread is multiplied by the gate's spread
//    multiplier and the size by its size multiplier.  At ``extreme`` the
//    configured action applies.
// 4. **Flow skew.**  The reservation price is shifted by the three fast
//    signals, each worth a configured number of ticks at full deflection.
// 5. **Cooperative placement.**  The bid is capped at ``behind_best_ticks``
//    below the best bid and the ask floored at the same above the best
//    ask, then snapped outwards to the tick.  A side that lands more than
//    ``max_behind_ticks`` away is not quoted -- except the side flattening
//    out of an ``extreme`` regime, which is exempt: that level's own spread
//    multiplier puts it past the cap, and dropping it leaves the position
//    with no passive way out.
// 6. **Inventory.**  Past ``reduce_only_position`` only the flattening side
//    is quoted; no fill may take the position past ``max_position``.
//
// Every dropped side carries a reason, and the decision records the
// intermediate numbers, so the journal can say *why* a quote was where it
// was rather than only where.  The arithmetic is the Python engine's,
// operation for operation, so the two agree to the last bit on a tape.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "harvester/book/book.hpp"
#include "harvester/config.hpp"
#include "harvester/instruments.hpp"
#include "harvester/signals/external.hpp"
#include "harvester/signals/flow.hpp"
#include "harvester/signals/vpin.hpp"

namespace harvester {

struct Quote {
    int side = 0;  // +1 bid, -1 ask
    double price = 0.0;
    int size = 0;

    const char* label() const { return side > 0 ? "bid" : "ask"; }
    bool operator==(const Quote&) const = default;
};

struct QuoteDecision {
    std::optional<Quote> bid;
    std::optional<Quote> ask;
    std::optional<double> anchor;
    std::optional<double> reservation;
    // Model half-spread in points after the toxicity multiplier and floor.
    std::optional<double> half_spread;
    double sigma = 0.0;
    double skew_ticks = 0.0;
    ToxicityLevel toxicity = ToxicityLevel::Calm;
    std::vector<std::string> reasons;

    bool quoting() const { return bid.has_value() || ask.has_value(); }
    std::string describe(const Product& product) const;
};

class QuoteEngine {
public:
    QuoteEngine(const QuotingConfig& cfg, const RiskConfig& risk, const Product& product,
                const std::string& anchor = "microprice", const std::string& extreme_action = "reduce_only");

    // -- the model --
    double reservation(double anchor, int inventory, double sigma) const;
    double model_spread(double sigma) const;

    // -- the decision --
    QuoteDecision decide(const BookSnapshot& snapshot, double sigma, const FlowState& flow,
                         const ToxicityState& toxicity, int inventory) const;

    QuotingConfig cfg;
    RiskConfig risk;
    const Product& product;
    bool anchor_is_microprice;
    bool extreme_pull;
    // Empty unless ``quoting.external_half_file`` names one. See
    // signals/external.hpp: a depth policy measured before it is written.
    ExternalSeries external_half;
    ExternalSeries external_skew;
};

}  // namespace harvester
