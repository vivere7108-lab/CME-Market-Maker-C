// End to end: the pipeline over a generated tape, and the live runner
// against a fake gateway fed the same records.
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "fakes.hpp"
#include "harvester/live/journal.hpp"
#include "harvester/live/pipeline.hpp"
#include "harvester/live/runner.hpp"
#include "harvester/replay/runner.hpp"
#include "harvester/replay/synthetic.hpp"
#include "helpers.hpp"

using namespace test;

TEST_SUITE("replay and live") {

TEST_CASE("synthetic records build a valid book and a tape") {
    RecordFeed feed("mbp-10", 10, [] { return 0.0; });
    SyntheticMarket market(es(), 30.0, 5000.0, 3);
    Mbp10Record record;
    int trades = 0;
    while (market.next(record)) {
        if (feed.push(record)) ++trades;
        const BookSnapshot s = feed.snapshot();
        REQUIRE(s.two_sided());
        REQUIRE(*s.spread() >= 0.25 - 1e-9);
    }
    CHECK(trades > 100);
    CHECK(feed.snapshot().best_ask()->price - feed.snapshot().best_bid()->price == doctest::Approx(0.25));
}

TEST_CASE("a toxic fraction of zero has no informed trades") {
    SyntheticMarket quiet(es(), 30.0, 5000.0, 3, 0.0);
    Mbp10Record record;
    while (quiet.next(record)) {
    }
    CHECK(quiet.informed_trades == 0);
    SyntheticMarket loud(es(), 60.0, 5000.0, 3, 0.5);
    while (loud.next(record)) {
    }
    CHECK(loud.informed_trades > 0);
}

TEST_CASE("the synthetic tape is the Python generator's, record for record") {
    // Pinned against the Python implementation: seed 7, ES, 5000.0.
    SyntheticMarket market(es(), 1800.0, 5000.0, 7, 0.2);
    Mbp10Record r;
    REQUIRE(market.next(r));
    CHECK(r.ts_event == 1'700'000'000'000'000'000LL);
    CHECK(r.action == 'A');
    CHECK(r.levels[0].bid_px == P(5000.0));
    CHECK(r.levels[0].ask_px == P(5000.25));
    CHECK(r.levels[0].bid_sz == 102);
    std::int64_t records = 1;
    while (market.next(r)) ++records;
    CHECK(records == 59767);
    CHECK(market.trades == 29159);
}

TEST_CASE("replay runs, fills and ends flat of quotes") {
    Config cfg = test_config();
    cfg.replay.synthetic_toxic_fraction = 0.5;
    cfg.risk.daily_loss_limit_usd = 100'000.0;
    ReplayRunner runner(cfg);
    const ReplayResult result = runner.run();
    CHECK(result.cycles > 100);
    CHECK(result.records > 1000);
    CHECK(result.fills > 0);
    CHECK_FALSE(runner.pipeline.quotes.any_working());
    CHECK(runner.pipeline.gate.warmed_up());
    CHECK(result.transitions > 0);
    for (const auto& [level, _] : result.markouts) {
        CHECK((level == "calm" || level == "elevated" || level == "toxic" || level == "extreme"));
    }
    CHECK(result.summary().find("markouts") != std::string::npos);
}

TEST_CASE("the replay applies the quoting hours") {
    Config cfg = test_config();
    cfg.replay.synthetic_toxic_fraction = 0.5;
    cfg.risk.daily_loss_limit_usd = 100'000.0;
    // The generated tape is stamped at ``quote_start`` on a weekday, so it
    // is inside the hours from its first record.
    ReplayRunner quoting(cfg);
    const ReplayResult inside = quoting.run();
    CHECK(inside.fills > 0);

    // The same tape stamped an hour and three quarters earlier: every
    // decision cycle still runs, and none of them quotes. Before the
    // replay read the hours at all this was indistinguishable from the run
    // above, so no replay number was the configuration the live walk runs.
    ReplayRunner early(cfg);
    const std::int64_t start = session_start_ns(early.tz, SYNTHETIC_DATE, TimeOfDay{7, 0, 0});
    SyntheticSource source(SyntheticMarket(es(), cfg.replay.synthetic_seconds, cfg.replay.synthetic_start_price,
                                           static_cast<std::uint64_t>(cfg.replay.synthetic_seed),
                                           cfg.replay.synthetic_toxic_fraction, start));
    const ReplayResult outside = early.run(&source);
    CHECK_FALSE(early.in_hours(static_cast<double>(start) / 1e9));
    CHECK(outside.cycles == inside.cycles);
    CHECK(outside.fills == 0);
}

TEST_CASE("the message rate stays inside the budget") {
    Config cfg = test_config();
    cfg.execution.max_messages_per_second = 5.0;
    cfg.execution.burst = 5;
    cfg.execution.requote_min_interval_ms = 0.0;
    const ReplayResult result = ReplayRunner(cfg).run();
    CHECK(result.messages <= 5 + 5.0 * cfg.replay.synthetic_seconds + 2);
}

TEST_CASE("the journal is written") {
    Config cfg = test_config();
    cfg.replay.synthetic_toxic_fraction = 0.5;
    cfg.risk.daily_loss_limit_usd = 100'000.0;
    const std::string dir = temp_dir("journal");
    {
        SessionJournal journal(dir);
        ReplayRunner(cfg, &journal).run();
        const auto counts = journal.counts();
        CHECK(counts.at("snapshots") > 50);
        CHECK(counts.count("fills") == 1);
        CHECK(counts.at("fills") > 0);
    }
    const auto fills = read_journal(dir, "fills");
    REQUIRE_FALSE(fills.empty());
    for (const char* key : {"side", "price", "size", "level", "position"}) CHECK(fills[0].get(key) != nullptr);
    std::string first_snapshot;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().filename().string().starts_with("snapshots-")) {
            std::ifstream in(entry.path());
            std::getline(in, first_snapshot);
        }
    }
    const json::Value row = json::parse(first_snapshot);
    CHECK(row.get("vpin") != nullptr);
    CHECK(row.get("quote_bid") != nullptr);
    CHECK(row.get("tokens") != nullptr);
    CHECK(row.get("ts") != nullptr);
}

TEST_CASE("the loss limit halts and flattens") {
    Config cfg = test_config();
    cfg.replay.synthetic_toxic_fraction = 0.8;
    cfg.risk.daily_loss_limit_usd = 30.0;
    cfg.replay.synthetic_seconds = 300.0;
    ReplayRunner runner(cfg);
    const ReplayResult result = runner.run();
    CHECK(result.halted);
    CHECK(runner.pipeline.risk.halted);
    CHECK(runner.pipeline.inventory.position == 0);
    CHECK_FALSE(runner.pipeline.quotes.any_working());
}

TEST_CASE("a disabled gate and no skew is the control") {
    Config cfg = test_config();
    cfg.toxicity.enabled = false;
    cfg.quoting.skew_ofi_ticks = cfg.quoting.skew_depletion_ticks = cfg.quoting.skew_run_ticks = 0.0;
    const ReplayResult result = ReplayRunner(cfg).run();
    CHECK(result.transitions == 0);
    REQUIRE(result.time_in_level.size() == 1);
    CHECK(result.time_in_level[0].first == "calm");
}

TEST_CASE("a DBN schema mismatch is refused") {
    Config cfg = test_config();
    cfg.replay.source = "dbn";
    cfg.databento.schema = "mbo";
    const char* tape = std::getenv("HARVESTER_TEST_MBP10_DBN");
    if (dbn_supported() && tape != nullptr) {
        cfg.replay.path = tape;
        CHECK_THROWS_WITH(ReplayRunner(cfg).records(), doctest::Contains("schema"));
    } else {
        cfg.replay.path = "/nonexistent/x.dbn";
        CHECK_THROWS(ReplayRunner(cfg).records());
    }
}

TEST_CASE("a stale feed pulls quotes") {
    Config cfg = test_config();
    ScriptedBroker broker;
    Pipeline p(cfg, broker, [] { return 0.0; });
    const BookSnapshot s = snapshot(5000.0, 10, 5000.25, 10);
    p.step(0.0, s, {}, true, 0.1);
    CHECK(broker.kinds() == std::vector<std::string>{"place", "place"});
    p.step(1.0, s, {}, true, 10.0);
    const auto kinds = broker.kinds();
    CHECK(std::vector<std::string>(kinds.end() - 2, kinds.end()) == std::vector<std::string>{"cancel", "cancel"});
    CHECK_FALSE(p.last_decision.has_value());
}

TEST_CASE("a fill updates the inventory and the markouts") {
    Config cfg = test_config();
    ScriptedBroker broker;
    TestClock clock;
    Pipeline p(cfg, broker, clock.fn());
    const BookSnapshot s = snapshot(5000.0, 10, 5000.25, 10);
    p.step(0.0, s, {}, true, 0.1);
    std::int64_t bid_id = 0;
    for (const auto& [id, order] : broker.orders) {
        if (std::get<0>(order) > 0) bid_id = id;
    }
    REQUIRE(bid_id != 0);
    broker.fill(bid_id, 1);
    clock.set(0.3);
    const StepResult r = p.step(0.3, s, {}, true, 0.1);
    CHECK(r.fills == 1);
    CHECK(p.inventory.position == 1);
    CHECK(p.markouts.rows.size() == 1);
    clock.set(40.0);
    p.step(40.0, s, {}, true, 0.1);
    CHECK(p.markouts.rows[0].complete());
}

TEST_CASE("the ceiling can read a faster vol estimate than the spread does") {
    // Same price path into two pipelines. One shares the quoter's 30s
    // estimate with the ceiling, the other gives the ceiling a 1s one.
    // After a burst of movement the fast estimate is higher, so the
    // ceiling set between the two pulls one and not the other.
    const auto drive = [](double risk_halflife) {
        Config cfg = test_config();
        cfg.risk.sigma_halflife_seconds = risk_halflife;
        cfg.risk.max_sigma = 0.0;  // measure sigma first, gate in the next pass
        ScriptedBroker broker;
        auto p = std::make_unique<Pipeline>(cfg, broker, [] { return 0.0; });
        double price = 5000.0;
        std::int64_t ts = 0;
        // One vol sample a second, well inside the estimator's band so
        // neither estimate is clamped: 200s of one-tick steps, then 8s of
        // four-tick ones.
        for (int i = 0; i < 200; ++i) {
            price += (i % 2 ? 0.25 : -0.25);
            ts += 1'000'000'000;
            p->step(static_cast<double>(ts) / 1e9, snapshot(price, 10, price + 0.25, 10, ts), {}, true, 0.1);
        }
        for (int i = 0; i < 8; ++i) {
            price += (i % 2 ? 1.0 : -1.0);
            ts += 1'000'000'000;
            p->step(static_cast<double>(ts) / 1e9, snapshot(price, 10, price + 0.25, 10, ts), {}, true, 0.1);
        }
        return p;
    };
    const auto slow = drive(0.0);
    const auto fast = drive(1.0);
    REQUIRE_FALSE(slow->risk_vol.has_value());
    REQUIRE(fast->risk_vol.has_value());
    // Both saw the same path, so the quoter's estimate is the same; only
    // the ceiling's differs, and the short half-life is further along.
    CHECK(fast->vol.sigma() == doctest::Approx(slow->vol.sigma()));
    CHECK(fast->risk_vol->sigma() > fast->vol.sigma() * 1.5);
}

TEST_CASE("outside hours flattens through the broker") {
    Config cfg = test_config();
    ScriptedBroker broker;
    Pipeline p(cfg, broker, [] { return 0.0; });
    p.inventory.adopt(1, 5000.0);
    const BookSnapshot s = snapshot(5000.0, 10, 5000.25, 10);
    p.step(0.0, s, {}, false, 0.1);
    const auto& last = broker.sent.back();
    CHECK(last.kind == "place");
    CHECK(last.side == -1);
    CHECK(last.price == doctest::Approx(5000.0 - cfg.risk.flatten_cross_ticks * 0.25));
    const std::int64_t flatten_id = broker.n;
    broker.fill(flatten_id, 1);
    p.step(0.5, s, {}, false, 0.1);
    CHECK(p.inventory.position == 0);
}

namespace {
std::shared_ptr<RecordFeed> fed_feed(double seconds) {
    auto feed = std::make_shared<RecordFeed>("mbp-10", 10);
    SyntheticMarket market(es(), seconds, 5000.0);
    Mbp10Record record;
    while (market.next(record)) feed->push(record);
    return feed;
}
}  // namespace

TEST_CASE("the live runner runs against a fake gateway and reconciles") {
    Config cfg = test_config();
    cfg.live.quote_start = TimeOfDay{0, 0, 0};
    cfg.live.quote_end = TimeOfDay{23, 59, 0};
    cfg.live.journal = false;
    cfg.live.decision_interval_ms = 1.0;
    auto feed = fed_feed(5.0);
    auto ib = std::make_shared<FakeGateway>();
    ib->positions_rows = {fake_position(contract(5001, "ES", "ESZ6"), 1, 5000.0 * 50)};
    ib->add_foreign_order(contract(5001, "ES"));
    LiveRunner runner(cfg, false, feed, [&] { return std::make_unique<IbkrConnection>(cfg, es(), ib); });
    Pipeline& pipeline = runner.run(5);
    CHECK(pipeline.inventory.position == 1);
    REQUIRE_FALSE(ib->cancelled.empty());
    CHECK(ib->cancelled[0] == 900);  // the foreign order
    // Long one: reduce-only, so only an ask was placed, then pulled at stop.
    int own = 0;
    for (const auto& [_, order] : ib->placed) {
        if (order.order_id < 900) {
            ++own;
            CHECK(order.action == "SELL");
        }
    }
    CHECK(own > 0);
    CHECK(runner.cycles() == 5);
}

TEST_CASE("a foreign position is a reconciliation error") {
    Config cfg = test_config();
    cfg.live.journal = false;
    auto feed = std::make_shared<RecordFeed>("mbp-10", 10);
    auto ib = std::make_shared<FakeGateway>();
    ib->positions_rows = {fake_position(contract(1, "CL", "CLZ6"), 1, 1.0)};
    LiveRunner runner(cfg, false, feed, [&] { return std::make_unique<IbkrConnection>(cfg, es(), ib); });
    CHECK_THROWS_AS(runner.run(1), ReconciliationError);
}

TEST_CASE("the runner reconnects after the gateway drops") {
    Config cfg = test_config();
    cfg.live.journal = false;
    cfg.live.reconnect_backoff_seconds = 0.01;
    cfg.live.max_reconnect_backoff_seconds = 0.01;
    cfg.live.decision_interval_ms = 1.0;
    auto feed = fed_feed(2.0);
    auto ib = std::make_shared<FakeGateway>();
    ib->drop_after = 3;
    LiveRunner runner(cfg, false, feed, [&] { return std::make_unique<IbkrConnection>(cfg, es(), ib); });
    runner.run(6);
    CHECK(ib->connects >= 2);
    CHECK(runner.cycles() == 6);
}

TEST_CASE("a dry run needs no gateway") {
    Config cfg = test_config();
    cfg.live.journal = false;
    cfg.live.decision_interval_ms = 1.0;
    auto feed = fed_feed(2.0);
    LiveRunner runner(cfg, true, feed, nullptr);
    Pipeline& pipeline = runner.run(3);
    CHECK(pipeline.cycles == 3);
    CHECK(pipeline.simulated != nullptr);
}

TEST_CASE("quoting hours are checked in the exchange's zone") {
    Config cfg = test_config();
    cfg.live.journal = false;
    LiveRunner runner(cfg, true, std::make_shared<RecordFeed>("mbp-10", 10), nullptr);
    // 2026-09-14 is a Monday. 14:00 UTC is 09:00 Chicago (CDT): inside 08:45-15:00.
    CHECK(runner.in_hours(1'789'394'400.0));
    // 21:00 UTC is 16:00 Chicago: outside.
    CHECK_FALSE(runner.in_hours(1'789'419'600.0));
    // 2026-09-13 (Sunday) at 14:00 UTC: a weekend.
    CHECK_FALSE(runner.in_hours(1'789'308'000.0));
}

}
