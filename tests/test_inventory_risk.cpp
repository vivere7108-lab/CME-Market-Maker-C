#include <filesystem>
#include <fstream>

#include "harvester/inventory.hpp"
#include "harvester/risk.hpp"
#include "helpers.hpp"

using namespace test;

namespace {
Fill fill(int side, double price, int size = 1, double ts = 0.0) {
    Fill f;
    f.ts = ts;
    f.side = side;
    f.price = price;
    f.size = size;
    f.fees = 2.3 * size;
    f.order_id = 1;
    return f;
}

struct RiskFixture {
    RiskConfig cfg;
    std::string dir = temp_dir("risk");
    std::unique_ptr<RiskMonitor> monitor;
    RiskFixture() {
        cfg.max_position = 2;
        cfg.reduce_only_position = 1;
        cfg.daily_loss_limit_usd = 100.0;
        cfg.kill_file = dir + "/HALT";
        cfg.stale_book_cancel_seconds = 2.0;
        monitor = std::make_unique<RiskMonitor>(cfg);
    }
};
}  // namespace

TEST_SUITE("inventory and risk") {

TEST_CASE("a round trip realises the spread") {
    Inventory inv(es());
    inv.apply(fill(1, 5000.0));
    inv.apply(fill(-1, 5000.5));
    CHECK(inv.position == 0);
    CHECK(inv.realised == doctest::Approx(0.5 * 50));
    CHECK(inv.fees == doctest::Approx(4.6));
    CHECK(inv.total_pnl(std::nullopt) == doctest::Approx(25 - 4.6));
}

TEST_CASE("the average price accumulates") {
    Inventory inv(es());
    inv.apply(fill(1, 5000.0));
    inv.apply(fill(1, 5001.0));
    CHECK(inv.avg_price == 5000.5);
    CHECK(inv.unrealised(5002.0) == doctest::Approx(2 * 1.5 * 50));
}

TEST_CASE("flipping through zero") {
    Inventory inv(es());
    inv.apply(fill(-1, 5001.0, 2));
    inv.apply(fill(1, 5000.0, 3));
    CHECK(inv.position == 1);
    CHECK(inv.avg_price == 5000.0);
    CHECK(inv.realised == doctest::Approx(2 * 1.0 * 50));
}

TEST_CASE("adopt") {
    Inventory inv(es());
    inv.adopt(-2, 4990.0);
    CHECK(inv.position == -2);
    CHECK(inv.avg_price == 4990.0);
    inv.adopt(0, 123.0);
    CHECK(inv.avg_price == 0.0);
    CHECK(inv.describe(std::nullopt) == "pos +0 @ 0.00 | realised $0 unrealised $0 fees $0 net $0 | 0 fills");
}

TEST_CASE("markouts are signed per contract in dollars") {
    MarkoutTracker m(es(), {1.0, 5.0});
    m.record(fill(1, 5000.0), 5000.1, ToxicityLevel::Calm, 0.0);
    m.record(fill(-1, 5001.0), 5000.9, ToxicityLevel::Toxic, 0.0);
    CHECK(m.rows[0].edge_at_fill == doctest::Approx(0.1 * 50));
    CHECK(m.rows[1].edge_at_fill == doctest::Approx(0.1 * 50));
    CHECK(m.update(0.5, 5000.0).empty());
    CHECK(m.update(1.0, 5000.5).empty());  // 5s not yet elapsed
    CHECK(*m.rows[0].markouts[0] == doctest::Approx(25.0));
    CHECK(*m.rows[1].markouts[0] == doctest::Approx(25.0));
    const auto done = m.update(5.0, 4999.0);
    CHECK(done.size() == 2);
    const auto s = m.summary();
    CHECK(s.at("calm").markouts.at("5s") == doctest::Approx(-50.0));
    CHECK(s.at("toxic").markouts.at("5s") == doctest::Approx(100.0));
    CHECK(m.describe().find("markouts, $ per contract") == 0);
}

TEST_CASE("the summary weights by size") {
    MarkoutTracker m(es(), {1.0});
    m.record(fill(1, 5000.0, 1), 5000.0, ToxicityLevel::Calm, 0.0);
    m.record(fill(1, 5000.0, 3), 5000.0, ToxicityLevel::Calm, 0.0);
    m.update(1.0, 5001.0);
    CHECK(m.summary().at("calm").markouts.at("1s") == doctest::Approx(50.0));
    CHECK(m.summary().at("calm").contracts == 4);
}

TEST_CASE("risk quotes when everything is fine") {
    RiskFixture f;
    const Verdict v = f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true);
    CHECK(v.quote);
    CHECK_FALSE(v.pull);
}

TEST_CASE("a stale book pulls but does not halt") {
    RiskFixture f;
    const Verdict v = f.monitor->evaluate(Inventory(es()), 5000.0, 5.0, true);
    CHECK(v.pull);
    CHECK_FALSE(f.monitor->halted);
    CHECK(f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true).quote);
}

TEST_CASE("realised vol above the ceiling pulls but does not halt") {
    RiskFixture f;
    f.cfg.max_sigma = 0.25;
    f.monitor = std::make_unique<RiskMonitor>(f.cfg);
    // Below the ceiling, and while the estimator has not warmed up, quoting
    // is unaffected; above it, every quote comes off and comes back.
    CHECK(f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true, nullptr, std::nullopt, 0.20).quote);
    CHECK(f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true, nullptr, std::nullopt, std::nullopt).quote);
    const Verdict v = f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true, nullptr, std::nullopt, 0.30);
    CHECK(v.pull);
    CHECK_FALSE(v.quote);
    CHECK_FALSE(v.flatten);
    CHECK_FALSE(f.monitor->halted);
    CHECK(f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true, nullptr, std::nullopt, 0.20).quote);
}

TEST_CASE("a zero ceiling is off") {
    RiskFixture f;
    f.cfg.max_sigma = 0.0;
    f.monitor = std::make_unique<RiskMonitor>(f.cfg);
    CHECK(f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true, nullptr, std::nullopt, 99.0).quote);
}

TEST_CASE("outside hours pulls and flattens") {
    RiskFixture f;
    Inventory inv(es());
    inv.adopt(1, 5000.0);
    const Verdict v = f.monitor->evaluate(inv, 5000.0, 0.1, false);
    CHECK(v.pull);
    CHECK(v.flatten);
    CHECK_FALSE(f.monitor->halted);
    CHECK_FALSE(f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, false).flatten);
}

TEST_CASE("the kill file") {
    RiskFixture f;
    { std::ofstream(f.cfg.kill_file) << ""; }
    CHECK(f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true).pull);
    std::filesystem::remove(f.cfg.kill_file);
    CHECK(f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true).quote);
}

TEST_CASE("the daily loss halts and stays halted") {
    RiskFixture f;
    Inventory inv(es());
    inv.adopt(1, 5000.0);
    Verdict v = f.monitor->evaluate(inv, 4997.0, 0.1, true);  // -$150
    CHECK(f.monitor->halted);
    CHECK(v.flatten);
    v = f.monitor->evaluate(inv, 5010.0, 0.1, true);  // recovered, still halted
    CHECK_FALSE(v.quote);
    CHECK(f.monitor->halted);
}

TEST_CASE("broker disagreement halts") {
    RiskFixture f;
    const Verdict v = f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true, nullptr, 1);
    CHECK(f.monitor->halted);
    CHECK(v.reason.find("outside this process") != std::string::npos);
}

TEST_CASE("a position past the cap halts") {
    RiskFixture f;
    Inventory inv(es());
    inv.adopt(3, 5000.0);
    CHECK(f.monitor->evaluate(inv, 5000.0, 0.1, true).flatten);
    CHECK(f.monitor->halted);
}

TEST_CASE("margin utilisation halts") {
    RiskFixture f;
    const AccountValues account{{"FullInitMarginReq", 30'000.0}, {"NetLiquidation", 100'000.0}};
    CHECK_FALSE(f.monitor->evaluate(Inventory(es()), 5000.0, 0.1, true, &account).quote);
    CHECK(f.monitor->halted);
}

}
