#include <format>
#include <functional>

#include "harvester/quoting/engine.hpp"
#include "harvester/signals/flow.hpp"
#include "harvester/signals/vpin.hpp"
#include "helpers.hpp"

using namespace test;

namespace {

const FlowState FLAT{0.0, 0.0, 0.0, 0};

ToxicityState tox(ToxicityLevel level = ToxicityLevel::Calm, double spread = 1.0, double size = 1.0) {
    return ToxicityState{level, 0.3, 0.5, true, spread, size, 100};
}

struct EngineFixture {
    QuotingConfig quoting;
    RiskConfig risk;
    std::unique_ptr<QuoteEngine> engine;

    explicit EngineFixture(const std::string& extreme_action = "reduce_only",
                           const std::function<void(QuotingConfig&)>& tweak = {}) {
        if (tweak) tweak(quoting);
        risk.max_position = 3;
        risk.reduce_only_position = 2;
        engine = std::make_unique<QuoteEngine>(quoting, risk, es(), "microprice", extreme_action);
    }
    QuoteDecision decide(const BookSnapshot& s, double sigma, const FlowState& f, const ToxicityState& t, int inv) {
        return engine->decide(s, sigma, f, t, inv);
    }
};

}  // namespace

TEST_SUITE("engine") {

TEST_CASE("inventory pushes the reservation against the position") {
    EngineFixture e;
    const double r_flat = e.engine->reservation(5000.0, 0, 0.5);
    CHECK(e.engine->reservation(5000.0, 2, 0.5) < r_flat);
    CHECK(e.engine->reservation(5000.0, -2, 0.5) > r_flat);
}

TEST_CASE("spread grows with vol and risk aversion") {
    EngineFixture e;
    CHECK(e.engine->model_spread(1.0) > e.engine->model_spread(0.1));
    EngineFixture averse("reduce_only", [](QuotingConfig& q) { q.gamma = 0.5; });
    EngineFixture calm("reduce_only", [](QuotingConfig& q) { q.gamma = 0.05; });
    CHECK(averse.engine->model_spread(0.5) > calm.engine->model_spread(0.5));
}

TEST_CASE("spread is positive at zero vol") { CHECK(EngineFixture().engine->model_spread(0.0) > 0); }

TEST_CASE("never at or inside the touch") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.gamma = 0.001; q.kappa = 100.0; });  // a tiny model spread
    const auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.02, FLAT, tox(), 0);
    CHECK(d.bid->price <= 5000.0 - 0.25);
    CHECK(d.ask->price >= 5000.25 + 0.25);
    CHECK(d.bid->price < d.ask->price);
}

TEST_CASE("behind_best_ticks is honoured") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.gamma = 0.001; q.kappa = 100.0; q.behind_best_ticks = 2; });
    const auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.02, FLAT, tox(), 0);
    CHECK(d.bid->price == 4999.5);
    CHECK(d.ask->price == 5000.75);
}

TEST_CASE("a wide model spread beats the cooperative cap") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.gamma = 0.5; q.kappa = 0.1; q.max_behind_ticks = 400; });
    const auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 1.0, FLAT, tox(), 0);
    CHECK(d.bid->price < 4999.75);
    CHECK(d.ask->price > 5000.5);
    CHECK(*d.half_spread > 0.25);
}

TEST_CASE("prices are on the tick grid") {
    EngineFixture e;
    const auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.37, FlowState{0.3, -0.2, 0.1, 1}, tox(), 1);
    for (const auto& q : {d.bid, d.ask}) {
        REQUIRE(q.has_value());
        CHECK(std::fabs(q->price / 0.25 - std::round(q->price / 0.25)) < 1e-9);
    }
}

TEST_CASE("too far behind is not quoted") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.gamma = 0.5; q.kappa = 0.1; q.max_behind_ticks = 2; });
    const auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 1.0, FLAT, tox(), 0);
    CHECK_FALSE(d.bid.has_value());
    CHECK_FALSE(d.ask.has_value());
    bool found = false;
    for (const auto& r : d.reasons) found = found || r.find("behind the touch") != std::string::npos;
    CHECK(found);
}

TEST_CASE("no book means no quotes") {
    BookSnapshot one_sided;
    one_sided.push_bid(Level{5000.0, 5, 0});
    const auto d = EngineFixture().decide(one_sided, 0.1, FLAT, tox(), 0);
    CHECK_FALSE(d.quoting());
    CHECK_FALSE(d.anchor.has_value());
    CHECK(d.describe(es()) == "no quotes: no two-sided book");
}

TEST_CASE("the multiplier widens the spread") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.max_behind_ticks = 100; });
    const auto calm = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.5, FLAT, tox(ToxicityLevel::Calm, 1.0, 1.0), 0);
    const auto wide = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.5, FLAT, tox(ToxicityLevel::Toxic, 3.0, 0.5), 0);
    CHECK(*wide.half_spread == doctest::Approx(*calm.half_spread * 3.0));
    CHECK(wide.ask->price - wide.bid->price >= calm.ask->price - calm.bid->price);
}

TEST_CASE("the size multiplier shrinks and rounds") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.base_size = 2; q.max_size = 3; });
    auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(ToxicityLevel::Elevated, 1.5, 0.75), 0);
    CHECK(d.bid->size == 2);  // ceil(1.5)
    d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(ToxicityLevel::Toxic, 2.5, 0.25), 0);
    CHECK(d.bid->size == 1);  // shrunk, never removed, while the multiplier is non-zero
    d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(ToxicityLevel::Toxic, 2.5, 0.0), 0);
    CHECK_FALSE(d.bid.has_value());
    CHECK(d.reasons[0].find("leaves nothing") != std::string::npos);
}

TEST_CASE("extreme pulls both sides") {
    EngineFixture e("pull");
    const auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(ToxicityLevel::Extreme, 4.0, 0.0), 1);
    CHECK_FALSE(d.quoting());
    CHECK(d.reasons[0].find("pulled") != std::string::npos);
}

TEST_CASE("extreme reduce-only keeps the flattening side") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.max_behind_ticks = 100; });
    auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(ToxicityLevel::Extreme, 4.0, 0.0), 1);
    CHECK_FALSE(d.bid.has_value());
    REQUIRE(d.ask.has_value());
    CHECK(d.ask->size == 1);
    d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(ToxicityLevel::Extreme, 4.0, 0.0), -1);
    CHECK_FALSE(d.ask.has_value());
    CHECK(d.bid.has_value());
    const auto flat = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(ToxicityLevel::Extreme, 4.0, 0.0), 0);
    CHECK_FALSE(flat.quoting());
}

TEST_CASE("buying pressure shifts both quotes up") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.skew_ofi_ticks = 2.0; q.max_behind_ticks = 100; q.gamma = 0.5; q.kappa = 0.1; });
    const auto neutral = e.decide(snapshot(5000.0, 10, 5000.25, 10), 1.0, FLAT, tox(), 0);
    const auto up = e.decide(snapshot(5000.0, 10, 5000.25, 10), 1.0, FlowState{1.0, 0.0, 0.0, 0}, tox(), 0);
    CHECK(*up.reservation == doctest::Approx(*neutral.reservation + 0.5));
    CHECK(up.skew_ticks == 2.0);
    CHECK(up.ask->price >= neutral.ask->price);
    CHECK(up.bid->price >= neutral.bid->price);
}

TEST_CASE("each signal has its own weight") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.skew_ofi_ticks = 1.0; q.skew_depletion_ticks = 2.0; q.skew_run_ticks = 0.5; });
    const auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FlowState{-1.0, 0.5, 1.0, 4}, tox(), 0);
    CHECK(d.skew_ticks == doctest::Approx(-1.0 + 1.0 + 0.5));
}

TEST_CASE("reduce-only drops the growing side") {
    EngineFixture e;
    auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(), 2);
    CHECK_FALSE(d.bid.has_value());
    CHECK(d.ask.has_value());
    d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(), -2);
    CHECK_FALSE(d.ask.has_value());
    CHECK(d.bid.has_value());
}

TEST_CASE("size never breaches the cap") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.base_size = 3; q.max_size = 3; });
    e.engine->risk.max_position = 3;
    e.engine->risk.reduce_only_position = 3;
    const auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(), 1);
    CHECK(d.bid->size == 2);  // 3 - 1
    CHECK(d.ask->size == 3);
}

TEST_CASE("inventory skews the reservation") {
    EngineFixture e("reduce_only", [](QuotingConfig& q) { q.gamma = 0.5; q.max_behind_ticks = 100; });
    const auto long_ = e.decide(snapshot(5000.0, 10, 5000.25, 10), 1.0, FLAT, tox(), 1);
    const auto short_ = e.decide(snapshot(5000.0, 10, 5000.25, 10), 1.0, FLAT, tox(), -1);
    CHECK(*long_.reservation < *short_.reservation);
}

TEST_CASE("describe prints the decision") {
    EngineFixture e;
    const auto d = e.decide(snapshot(5000.0, 10, 5000.25, 10), 0.1, FLAT, tox(), 0);
    const std::string text = d.describe(es());
    REQUIRE(d.bid.has_value());
    REQUIRE(d.ask.has_value());
    CHECK(text.find(std::format("bid 1@{:.2f} | ask 1@{:.2f} | anchor 5000.125", d.bid->price, d.ask->price)) == 0);
    CHECK(text.find("[calm]") != std::string::npos);
}

}
