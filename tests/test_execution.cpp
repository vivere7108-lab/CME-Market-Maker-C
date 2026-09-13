#include "harvester/execution/quotes.hpp"
#include "harvester/execution/simulated.hpp"
#include "harvester/execution/throttle.hpp"
#include "helpers.hpp"

using namespace test;

namespace {

QuoteDecision decision(std::optional<std::pair<double, int>> bid, std::optional<std::pair<double, int>> ask) {
    QuoteDecision d;
    if (bid) d.bid = Quote{1, bid->first, bid->second};
    if (ask) d.ask = Quote{-1, ask->first, ask->second};
    d.anchor = 5000.0;
    d.reservation = 5000.0;
    d.half_spread = 0.5;
    d.sigma = 0.1;
    d.skew_ticks = 0.0;
    return d;
}

struct ManagerFixture {
    TestClock clock;
    ScriptedBroker broker;
    ExecutionConfig cfg;
    MessageBudget budget;
    ActionQueue queue;
    QuoteManager qm;

    explicit ManagerFixture(double interval_ms = 200.0, double price_ticks = 0.0, double size_fraction = 0.34,
                            double ack_timeout = 2.0, double rate = 100.0, int burst = 20)
        : cfg(make_cfg(interval_ms, price_ticks, size_fraction, ack_timeout, rate, burst)),
          budget(rate, burst, clock.fn()), queue(budget), qm(broker, es(), cfg, queue, clock.fn()) {}

    static ExecutionConfig make_cfg(double interval_ms, double price_ticks, double size_fraction, double ack_timeout,
                                    double rate, int burst) {
        ExecutionConfig c;
        c.requote_min_interval_ms = interval_ms;
        c.requote_price_ticks = price_ticks;
        c.requote_size_fraction = size_fraction;
        c.ack_timeout_seconds = ack_timeout;
        c.max_messages_per_second = rate;
        c.burst = burst;
        return c;
    }

    std::vector<Fill> cycle() {
        qm.reconcile();
        queue.pump();
        return qm.on_events(broker.drain_events());
    }
};

}  // namespace

TEST_SUITE("execution") {

TEST_CASE("budget refills at the rate") {
    TestClock c;
    MessageBudget b(10.0, 5, c.fn());
    for (int i = 0; i < 5; ++i) CHECK(b.try_acquire());
    CHECK_FALSE(b.try_acquire());
    c.set(0.1);
    CHECK(b.try_acquire());
    CHECK_FALSE(b.try_acquire());
    CHECK(b.refused == 2);
}

TEST_CASE("budget never exceeds the burst") {
    TestClock c;
    MessageBudget b(10.0, 3, c.fn());
    c.set(100.0);
    CHECK(b.available() == 3.0);
}

TEST_CASE("headroom holds tokens back") {
    TestClock c;
    MessageBudget b(10.0, 5, c.fn());
    CHECK(b.try_acquire(std::nullopt, 4.0));
    CHECK_FALSE(b.try_acquire(std::nullopt, 4.0));
    CHECK(b.try_acquire(std::nullopt, 0.0));
}

TEST_CASE("rate is respected over a second") {
    TestClock c;
    MessageBudget b(30.0, 10, c.fn());
    int sent = 0;
    for (int i = 0; i < 1000; ++i) {
        c.set(i / 1000.0);
        sent += b.try_acquire() ? 1 : 0;
    }
    CHECK(sent <= 10 + 30);
}

TEST_CASE("queue: priority then arrival order") {
    TestClock c;
    MessageBudget b(100.0, 10, c.fn());
    ActionQueue q(b);
    std::vector<std::string> out;
    q.submit("place-ask", 2, [&] { out.push_back("place"); });
    q.submit("replace-bid", 1, [&] { out.push_back("replace"); });
    q.submit("cancel-ask", 0, [&] { out.push_back("cancel"); });
    q.pump();
    CHECK(out == std::vector<std::string>{"cancel", "replace", "place"});
}

TEST_CASE("queue: the same key coalesces to the latest") {
    TestClock c;
    MessageBudget b(100.0, 10, c.fn());
    ActionQueue q(b);
    std::vector<int> out;
    for (int i = 0; i < 5; ++i) q.submit("bid", 1, [&out, i] { out.push_back(i); });
    CHECK(q.size() == 1);
    CHECK(q.coalesced == 4);
    q.pump();
    CHECK(out == std::vector<int>{4});
}

TEST_CASE("queue: stops when the budget is empty") {
    TestClock c;
    MessageBudget b(1.0, 2, c.fn());
    ActionQueue q(b);
    std::vector<std::string> out;
    for (const char* k : {"a", "b", "c", "d"}) q.submit(k, 1, [&out, k] { out.emplace_back(k); });
    q.pump();
    CHECK(out == std::vector<std::string>{"a", "b"});
    CHECK(q.size() == 2);
    c.set(2.0);
    q.pump();
    CHECK(out == std::vector<std::string>{"a", "b", "c", "d"});
}

TEST_CASE("manager places both sides and tracks acks") {
    ManagerFixture f;
    f.qm.set_desired(decision({{4999.75, 1}}, {{5000.5, 1}}));
    f.cycle();
    CHECK(f.broker.kinds() == std::vector<std::string>{"place", "place"});
    CHECK(f.qm.working(1)->state == OrderState::Working);
    CHECK(f.qm.working(-1)->state == OrderState::Working);
}

TEST_CASE("replace is one message and waits for the ack") {
    ManagerFixture f;
    f.qm.set_desired(decision({{4999.75, 1}}, std::nullopt));
    f.cycle();
    f.clock.set(1.0);
    f.broker.ack = false;
    f.qm.set_desired(decision({{4999.5, 1}}, std::nullopt));
    f.cycle();
    CHECK(f.broker.kinds().back() == "replace");
    CHECK(f.qm.working(1)->state != OrderState::Working);
    // A further change is held while the replace is unacknowledged.
    f.clock.set(1.5);
    f.qm.set_desired(decision({{4999.25, 1}}, std::nullopt));
    f.cycle();
    CHECK(f.broker.count("replace") == 1);
    f.broker.events.push_back(OrderEvent::ack(1));
    f.cycle();
    CHECK(f.qm.working(1)->price == 4999.5);
    CHECK(f.qm.working(1)->state == OrderState::Working);
    f.clock.set(2.0);
    f.cycle();
    const auto& last = f.broker.sent.back();
    CHECK(last.kind == "replace");
    CHECK(last.order_id == 1);
    CHECK(last.price == 4999.25);
    CHECK(last.size == 1);
}

TEST_CASE("the dead-band swallows small changes") {
    ManagerFixture f(0.0, 1.0, 0.5);
    f.qm.set_desired(decision({{4999.75, 2}}, std::nullopt));
    f.cycle();
    f.qm.set_desired(decision({{4999.5, 2}}, std::nullopt));  // one tick: inside the band
    f.cycle();
    f.qm.set_desired(decision({{4999.75, 3}}, std::nullopt));  // +50% of 2: inside
    f.cycle();
    CHECK(f.broker.kinds() == std::vector<std::string>{"place"});
    f.qm.set_desired(decision({{4999.25, 2}}, std::nullopt));  // two ticks: outside
    f.cycle();
    CHECK(f.broker.kinds() == std::vector<std::string>{"place", "replace"});
}

TEST_CASE("the minimum interval defers and coalesces") {
    ManagerFixture f;
    f.qm.set_desired(decision({{4999.75, 1}}, std::nullopt));
    f.cycle();
    int i = 0;
    for (const double price : {4999.5, 4999.25, 4999.0}) {
        f.clock.set(0.05 * (++i));
        f.qm.set_desired(decision({{price, 1}}, std::nullopt));
        f.cycle();
    }
    CHECK(f.broker.kinds() == std::vector<std::string>{"place"});
    f.clock.set(0.3);
    f.cycle();
    CHECK(f.broker.sent.back().kind == "replace");
    CHECK(f.broker.sent.back().price == 4999.0);
}

TEST_CASE("cancel when nothing is wanted") {
    ManagerFixture f;
    f.qm.set_desired(decision({{4999.75, 1}}, {{5000.5, 1}}));
    f.cycle();
    f.qm.cancel_all("test");
    f.cycle();
    const auto kinds = f.broker.kinds();
    CHECK(std::vector<std::string>(kinds.end() - 2, kinds.end()) == std::vector<std::string>{"cancel", "cancel"});
    CHECK_FALSE(f.qm.any_working());
}

TEST_CASE("fills shrink and finish the order") {
    ManagerFixture f;
    f.qm.set_desired(decision({{4999.75, 2}}, std::nullopt));
    f.cycle();
    f.broker.fill(1, 1);
    const auto fills = f.cycle();
    CHECK(fills.size() == 1);
    CHECK(f.qm.working(1)->remaining() == 1);
    f.broker.fill(1, 1);
    f.cycle();
    CHECK(f.qm.working(1) == nullptr);
}

TEST_CASE("a fill on an unknown order is still reported") {
    ManagerFixture f;
    f.broker.orders[99] = {1, 5000.0, 1};
    f.broker.fill(99, 1);
    const auto fills = f.qm.on_events(f.broker.drain_events());
    CHECK(fills.size() == 1);
    CHECK(fills[0].order_id == 99);
}

TEST_CASE("a rejection forgets the order") {
    ManagerFixture f;
    f.qm.set_desired(decision({{4999.75, 1}}, std::nullopt));
    f.cycle();
    f.broker.reject(1);
    f.cycle();
    CHECK(f.qm.working(1) == nullptr);
    CHECK(f.qm.rejections == 1);
}

TEST_CASE("an ack timeout cancels and suspends the side") {
    ManagerFixture f;
    f.broker.ack = false;
    f.qm.set_desired(decision({{4999.75, 1}}, std::nullopt));
    f.cycle();
    CHECK(f.qm.working(1)->state == OrderState::PendingNew);
    f.clock.set(3.0);
    f.cycle();
    CHECK(f.qm.working(1)->state == OrderState::Unknown);
    CHECK(f.qm.timeouts == 1);
    CHECK(f.broker.kinds().back() == "cancel");
    // Nothing more is sent for that side until the broker speaks.
    f.clock.set(4.0);
    f.qm.set_desired(decision({{4999.5, 1}}, std::nullopt));
    f.cycle();
    CHECK(f.broker.count("place") == 1);
    f.broker.events.push_back(OrderEvent::cancelled(1));
    f.cycle();
    f.clock.set(5.0);
    f.cycle();
    CHECK(f.broker.count("place") == 2);
}

TEST_CASE("stays inside the budget under a storm") {
    ManagerFixture f(0.0, 0.0, 0.34, 5.0, 10.0, 5);
    for (int i = 0; i < 2000; ++i) {
        f.clock.set(i / 1000.0);
        const double price = 4999.75 - 0.25 * (i % 7);
        f.qm.set_desired(decision({{price, 1}}, {{price + 1.0, 1}}));
        f.cycle();
    }
    CHECK(f.broker.sent.size() <= 5 + 10 * 2 + 1);
}

TEST_CASE("manager describes the working quotes") {
    ManagerFixture f;
    CHECK(f.qm.describe() == "bid - | ask -");
    f.qm.set_desired(decision({{4999.75, 1}}, std::nullopt));
    f.cycle();
    CHECK(f.qm.describe() == "bid 1@4999.75 working | ask -");
}

TEST_CASE("simulated: joins the back of the queue") {
    TestClock c;
    SimulatedBroker broker(es(), "back", c.fn());
    broker.on_book(snapshot(5000.0, 30, 5000.25, 10));
    const auto h = broker.place(1, 5000.0, 1);
    CHECK(broker.find(h.order_id)->queue_ahead == 30);
    broker.on_trade(Trade{0, 5000.0, 20, -1});
    CHECK(broker.fills.empty());
    broker.on_trade(Trade{0, 5000.0, 15, -1});
    CHECK(broker.fills.size() == 1);
    CHECK(broker.fills[0].price == 5000.0);
}

TEST_CASE("simulated: the front of the queue is the optimistic bound") {
    SimulatedBroker broker(es(), "front");
    broker.on_book(snapshot(5000.0, 30, 5000.25, 10));
    broker.place(1, 5000.0, 1);
    broker.on_trade(Trade{0, 5000.0, 1, -1});
    CHECK(broker.fills.size() == 1);
}

TEST_CASE("simulated: a lift of the ask does not fill a bid") {
    SimulatedBroker broker(es(), "back");
    broker.on_book(snapshot(5000.0, 30, 5000.25, 10));
    broker.place(1, 5000.0, 1);
    broker.on_trade(Trade{0, 5000.0, 100, 1});
    CHECK(broker.fills.empty());
}

TEST_CASE("simulated: trading through the price fills in full") {
    SimulatedBroker broker(es(), "back");
    broker.on_book(snapshot(5000.0, 30, 5000.25, 10));
    broker.place(-1, 5000.5, 2);
    broker.on_trade(Trade{0, 5000.75, 1, 1});
    CHECK(broker.fills[0].size == 2);
}

TEST_CASE("simulated: a crossing book fills") {
    SimulatedBroker broker(es(), "back");
    broker.on_book(snapshot(5000.0, 30, 5000.25, 10));
    broker.place(1, 4999.75, 1);
    broker.on_book(snapshot(4999.25, 30, 4999.5, 10));
    CHECK(broker.fills.size() == 1);
    CHECK(broker.fills[0].price == 4999.75);
}

TEST_CASE("simulated: cancels ahead shorten the queue") {
    SimulatedBroker broker(es(), "back");
    broker.on_book(snapshot(5000.0, 30, 5000.25, 10));
    const auto h = broker.place(1, 5000.0, 1);
    broker.on_book(snapshot(5000.0, 5, 5000.25, 10));
    CHECK(broker.find(h.order_id)->queue_ahead == 5);
}

TEST_CASE("simulated: a replace to a new price requeues") {
    SimulatedBroker broker(es(), "back");
    broker.on_book(snapshot(5000.0, 30, 5000.25, 10));
    const auto h = broker.place(1, 4999.75, 1);
    broker.on_book(snapshot(5000.0, 30, 5000.25, 10));
    broker.replace(h, 5000.0, 1);
    CHECK(broker.find(h.order_id)->queue_ahead == 30);
    broker.replace(h, 5000.0, 3);  // bigger at the same price: back again
    CHECK(broker.find(h.order_id)->queue_ahead == 30);
}

TEST_CASE("simulated: a cancel stops fills") {
    SimulatedBroker broker(es(), "back");
    broker.on_book(snapshot(5000.0, 30, 5000.25, 10));
    const auto h = broker.place(1, 5000.0, 1);
    broker.cancel(h);
    broker.on_trade(Trade{0, 4999.0, 100, -1});
    CHECK(broker.fills.empty());
    std::vector<OrderEvent::Kind> kinds;
    for (const auto& e : broker.drain_events()) kinds.push_back(e.kind);
    CHECK(kinds == std::vector<OrderEvent::Kind>{OrderEvent::Kind::Ack, OrderEvent::Kind::Cancelled});
}

TEST_CASE("simulated: fees use the product rate") {
    SimulatedBroker broker(es(), "back");
    broker.on_book(snapshot(5000.0, 0, 5000.25, 10));
    broker.place(1, 5000.0, 2);
    broker.on_trade(Trade{0, 5000.0, 2, -1});
    CHECK(broker.fills[0].fees == doctest::Approx(2 * es().fee_per_contract));
}

}
