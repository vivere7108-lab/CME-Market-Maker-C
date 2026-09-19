#include <filesystem>
#include <fstream>

#include "harvester/signals/external.hpp"
#include "harvester/signals/flow.hpp"
#include "harvester/signals/vol.hpp"
#include "harvester/signals/vpin.hpp"
#include "helpers.hpp"

using namespace test;

namespace {

Trade trade(int size, int aggressor, std::int64_t ts = 0) { return Trade{ts, 5000.0, size, aggressor}; }

ToxicityGate gate(const std::function<void(ToxicityConfig&)>& tweak = {}) {
    ToxicityConfig c;
    c.bucket_contracts = 20;
    c.window_buckets = 1;
    c.history_buckets = 100;
    c.warmup_buckets = 10;
    c.elevated_enter = 0.7;
    c.elevated_exit = 0.6;
    c.toxic_enter = 0.9;
    c.toxic_exit = 0.8;
    c.extreme_enter = 0.97;
    c.extreme_exit = 0.93;
    c.min_dwell_buckets = 2;
    if (tweak) tweak(c);
    return ToxicityGate(c);
}

// One bucket per entry, with the given |B-S|/V.
void feed(ToxicityGate& g, const std::vector<double>& imbalances) {
    for (const double imb : imbalances) {
        const int buy = static_cast<int>(std::nearbyint(10 + 10 * imb));
        const int sell = 20 - buy;
        if (buy) g.update(trade(buy, 1));
        if (sell) g.update(trade(sell, -1));
    }
}

std::vector<double> repeat(const std::vector<double>& pattern, int times) {
    std::vector<double> out;
    for (int i = 0; i < times; ++i) out.insert(out.end(), pattern.begin(), pattern.end());
    return out;
}

}  // namespace

TEST_SUITE("signals") {

TEST_CASE("vpin: a bucket closes only when full") {
    Vpin v(100, 5);
    CHECK(v.update(trade(60, 1)) == 0);
    CHECK_FALSE(v.value().has_value());
    CHECK(v.update(trade(40, 1)) == 1);
    CHECK(*v.value() == doctest::Approx(1.0));
}

TEST_CASE("vpin: a balanced bucket reads zero") {
    Vpin v(100, 5);
    v.update(trade(50, 1));
    v.update(trade(50, -1));
    CHECK(*v.value() == doctest::Approx(0.0));
}

TEST_CASE("vpin: a block spills into the next bucket") {
    Vpin v(100, 5);
    v.update(trade(30, -1));
    const int closed = v.update(trade(250, 1));  // 70 closes bucket 1; 100 closes 2; 80 left
    CHECK(closed == 2);
    CHECK(v.buckets_closed == 2);
    CHECK(v.fill_fraction() == doctest::Approx(0.8));
    // Bucket 1: 30 sell + 70 buy -> |40|/100; bucket 2 all buy -> 1.0
    CHECK(*v.value() == doctest::Approx((0.4 + 1.0) / 2));
}

TEST_CASE("vpin: an unknown side is split or ignored") {
    Vpin split(100, 5, "split");
    split.update(trade(100, 0));
    CHECK(*split.value() == doctest::Approx(0.0));
    Vpin ignore(100, 5, "ignore");
    CHECK(ignore.update(trade(100, 0)) == 0);
    CHECK_FALSE(ignore.value().has_value());
}

TEST_CASE("gate: calm and inert until warmed up") {
    ToxicityGate g = gate();
    feed(g, std::vector<double>(5, 1.0));
    CHECK(g.state().level == ToxicityLevel::Calm);
    CHECK_FALSE(g.state().warmed_up);
    CHECK(g.describe().find("warming up") != std::string::npos);
}

TEST_CASE("gate: steps up immediately and down after the dwell") {
    ToxicityGate g = gate();
    // A history of low readings, then one high one.
    feed(g, repeat({0.0, 0.2}, 10));
    CHECK(g.warmed_up());
    feed(g, {1.0});
    const ToxicityLevel before = g.state().level;
    CHECK((before == ToxicityLevel::Toxic || before == ToxicityLevel::Extreme));
    // One quiet bucket is not enough to step down: the dwell is 2.
    feed(g, {0.0});
    CHECK(g.state().level == before);
    feed(g, {0.0, 0.0});
    CHECK(g.state().index() <= level_index(before));
    REQUIRE_FALSE(g.transitions.empty());
    CHECK(g.transitions[0].from == ToxicityLevel::Calm);
}

TEST_CASE("gate: hysteresis holds a level between exit and enter") {
    ToxicityGate g = gate([](ToxicityConfig& c) { c.min_dwell_buckets = 0; });
    feed(g, repeat({0.0, 0.2, 0.4, 0.6, 0.8, 1.0}, 5));
    // Percentile of 0.8 among a spread of readings is around 0.75:
    // above elevated_enter, below toxic_enter.
    feed(g, {0.8});
    CHECK(g.state().level == ToxicityLevel::Elevated);
    feed(g, {0.7});
    CHECK(g.state().level == ToxicityLevel::Elevated);
    feed(g, {0.4});
    CHECK(g.state().level == ToxicityLevel::Calm);
}

TEST_CASE("gate: a disabled gate always reads calm") {
    ToxicityGate g = gate([](ToxicityConfig& c) { c.enabled = false; });
    std::vector<double> readings(15, 0.0);
    readings.insert(readings.end(), 5, 1.0);
    feed(g, readings);
    CHECK(g.state().level == ToxicityLevel::Calm);
    CHECK(g.state().spread_multiplier == 1.0);
}

TEST_CASE("gate: the multipliers follow the level") {
    ToxicityGate g = gate();
    feed(g, std::vector<double>(15, 0.0));
    feed(g, {1.0});
    const ToxicityState s = g.state();
    CHECK(s.spread_multiplier == g.cfg().spread_multiplier.at(to_string(s.level)));
    CHECK(s.size_multiplier == g.cfg().size_multiplier.at(to_string(s.level)));
}

TEST_CASE("ofi: a growing bid queue is buying pressure") {
    OrderFlowImbalance ofi(1000.0);
    ofi.update(snapshot(5000.0, 10, 5000.25, 10, 0));
    ofi.update(snapshot(5000.0, 40, 5000.25, 10, 1'000'000));
    CHECK(ofi.value(1'000'000) > 0);
}

TEST_CASE("ofi: a cancelled bid queue is selling pressure") {
    OrderFlowImbalance ofi(1000.0);
    ofi.update(snapshot(5000.0, 40, 5000.25, 10, 0));
    ofi.update(snapshot(5000.0, 5, 5000.25, 10, 1'000'000));
    CHECK(ofi.value(1'000'000) < 0);
}

TEST_CASE("ofi: a bid stepping up is buying pressure") {
    OrderFlowImbalance ofi(1000.0);
    ofi.update(snapshot(5000.0, 10, 5000.5, 10, 0));
    ofi.update(snapshot(5000.25, 10, 5000.5, 10, 1'000'000));
    CHECK(ofi.value(1'000'000) > 0);
}

TEST_CASE("ofi: events expire out of the window") {
    OrderFlowImbalance ofi(100.0);
    ofi.update(snapshot(5000.0, 10, 5000.25, 10, 0));
    ofi.update(snapshot(5000.0, 40, 5000.25, 10, 1'000'000));
    CHECK(ofi.value(1'000'000) > 0);
    CHECK(ofi.value(500'000'000) == 0.0);
}

TEST_CASE("ofi: bounded") {
    OrderFlowImbalance ofi(1000.0);
    ofi.update(snapshot(5000.0, 10, 5000.25, 10, 0));
    ofi.update(snapshot(5000.0, 100'000, 5000.25, 10, 1));
    CHECK(ofi.value(1) <= 1.0);
}

TEST_CASE("depletion: a bid being eaten is selling pressure") {
    QueueDepletion d(500.0);
    d.update(snapshot(5000.0, 100, 5000.25, 100, 0));
    d.update(snapshot(5000.0, 40, 5000.25, 100, 200'000'000));
    CHECK(d.value() < 0);
    CHECK(d.side_rate(1) > 0);
}

TEST_CASE("depletion: a vanished ask level is buying pressure") {
    QueueDepletion d(500.0);
    d.update(snapshot(5000.0, 100, 5000.25, 100, 0));
    d.update(snapshot(5000.0, 100, 5000.5, 100, 200'000'000));
    CHECK(d.value() > 0);
}

TEST_CASE("run: same-side trades lengthen the run") {
    AggressorRun r(4, 2.0);
    for (int i = 0; i < 3; ++i) r.update(trade(1, 1, i));
    CHECK(r.run() == 3);
    CHECK(r.value(3) == doctest::Approx(0.75));
}

TEST_CASE("run: an opposite trade restarts it") {
    AggressorRun r(4, 2.0);
    r.update(trade(1, 1));
    r.update(trade(1, 1));
    r.update(trade(1, -1));
    CHECK(r.run() == -1);
}

TEST_CASE("run: saturates and decays") {
    AggressorRun r(4, 2.0);
    for (int i = 0; i < 10; ++i) r.update(trade(1, -1, 0));
    CHECK(r.value(0) == -1.0);
    CHECK(r.value(1'000'000'000) == doctest::Approx(-0.5));
    CHECK(r.value(3'000'000'000) == 0.0);
}

TEST_CASE("run: unflagged trades are ignored") {
    AggressorRun r(4, 2.0);
    r.update(trade(1, 1));
    r.update(trade(1, 0));
    CHECK(r.run() == 1);
}

TEST_CASE("vol: the floor before any sample") {
    RealisedVol v(30.0, 250.0, 0.02, 2.0);
    CHECK(v.sigma() == 0.02);
}

TEST_CASE("vol: measures a steady move") {
    RealisedVol v(30.0, 250.0, 0.001, 2.0);
    double price = 5000.0;
    for (int i = 0; i < 200; ++i) {
        price += i % 2 ? 0.25 : -0.25;
        v.update(price, static_cast<std::int64_t>(i) * 250'000'000);
    }
    // 0.25 points every 0.25s -> 0.5 points per root-second.
    CHECK(v.sigma() == doctest::Approx(0.5).epsilon(0.05));
}

TEST_CASE("vol: clamped to the ceiling") {
    RealisedVol v(30.0, 250.0, 0.02, 1.0);
    v.update(5000.0, 0);
    v.update(5100.0, 250'000'000);
    CHECK(v.sigma() == 1.0);
}

TEST_CASE("vol: ignores samples inside the interval") {
    RealisedVol v(30.0, 250.0, 0.02, 2.0);
    v.update(5000.0, 0);
    v.update(5010.0, 1'000'000);
    CHECK(v.samples == 0);
}

TEST_CASE("flow signals update together") {
    FlowConfig cfg;
    FlowSignals flow(cfg);
    flow.on_book(snapshot(5000.0, 10, 5000.25, 10, 0));
    flow.on_book(snapshot(5000.0, 40, 5000.25, 10, 1'000'000));
    flow.on_trade(trade(1, 1, 1'000'000));
    const FlowState s = flow.state(1'000'000);
    CHECK(s.ofi > 0);
    CHECK(s.run > 0);
    CHECK(s.run_length == 1);
    CHECK(s.composite() == doctest::Approx((s.ofi + s.depletion + s.run) / 3.0));
}

}

// -- an offline rule read beside the tape --------------------------------

TEST_CASE("an external series reads as it stood at or before the clock") {
    const std::string path = "external_series_test.csv";
    {
        std::ofstream out(path);
        out << "ts,value\n1000,0.5\n2000,1.5\n3000,2.5\n";
    }
    const ExternalSeries series(path);
    CHECK(series.size() == 3);
    CHECK_FALSE(series.at(999).has_value());   // before the first row
    CHECK(series.at(1000).value() == doctest::Approx(0.5));
    CHECK(series.at(1999).value() == doctest::Approx(0.5));  // never ahead of the tape
    CHECK(series.at(2000).value() == doctest::Approx(1.5));
    CHECK(series.at(9999).value() == doctest::Approx(2.5));  // and never expires
    std::filesystem::remove(path);
}
