#include "fakes.hpp"
#include "helpers.hpp"

using namespace test;

namespace {

struct BrokerFixture {
    Config cfg;
    std::shared_ptr<FakeGateway> ib = std::make_shared<FakeGateway>();
    IbkrConnection conn;
    std::unique_ptr<IbkrBroker> broker;

    BrokerFixture() : conn(cfg, es(), ib) {
        conn.connect();
        conn.qualify(std::string("ESZ6"));
        broker = std::make_unique<IbkrBroker>(conn, es(), true);
    }

    std::vector<OrderEvent::Kind> kinds() {
        std::vector<OrderEvent::Kind> out;
        for (const auto& e : broker->drain_events()) out.push_back(e.kind);
        return out;
    }
};

}  // namespace

TEST_SUITE("ibkr") {

TEST_CASE("place is a DAY limit order on the contract") {
    BrokerFixture f;
    const auto h = f.broker->place(1, 4999.75, 2);
    const auto& [contract, order] = f.ib->placed.back();
    CHECK(contract.con_id == 5001);
    CHECK(order.action == "BUY");
    CHECK(order.total_quantity == 2);
    CHECK(order.lmt_price == 4999.75);
    CHECK(order.tif == "DAY");
    CHECK(order.outside_rth);
    CHECK(order.order_type == "LMT");
    const auto events = f.broker->drain_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == OrderEvent::Kind::Ack);
    CHECK(events[0].order_id == h.order_id);
}

TEST_CASE("replace modifies in place with one message") {
    BrokerFixture f;
    const auto h = f.broker->place(-1, 5000.5, 1);
    f.broker->drain_events();
    f.broker->replace(h, 5000.75, 2);
    REQUIRE(f.ib->placed.size() == 2);
    CHECK(f.ib->placed[1].second.order_id == h.order_id);
    CHECK(f.ib->placed[1].second.lmt_price == 5000.75);
    CHECK(f.ib->placed[1].second.total_quantity == 2);
    CHECK(f.kinds() == std::vector<OrderEvent::Kind>{OrderEvent::Kind::Ack});
    CHECK(f.broker->messages_sent() == 2);
}

TEST_CASE("replace keeps the filled part") {
    BrokerFixture f;
    const auto h = f.broker->place(1, 4999.75, 2);
    f.ib->fill(h.order_id, 1, 4999.75);
    f.broker->drain_events();
    f.broker->replace(h, 4999.5, 1);
    CHECK(f.ib->placed[1].second.total_quantity == 2);  // 1 filled + 1 wanted
}

TEST_CASE("cancel") {
    BrokerFixture f;
    const auto h = f.broker->place(1, 4999.75, 1);
    f.broker->drain_events();
    f.broker->cancel(h);
    CHECK(f.ib->cancelled[0] == h.order_id);
    CHECK(f.kinds() == std::vector<OrderEvent::Kind>{OrderEvent::Kind::Cancelled});
}

TEST_CASE("fills come through once with the side and fees") {
    BrokerFixture f;
    const auto h = f.broker->place(-1, 5000.5, 2);
    f.broker->drain_events();
    const IbExecution e = f.ib->fill(h.order_id, 1, 5000.5);
    f.ib->emit_exec(h.order_id, e);  // a duplicate delivery
    const auto events = f.broker->drain_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == OrderEvent::Kind::Fill);
    const Fill& fill = *events[0].fill;
    CHECK(fill.side == -1);
    CHECK(fill.size == 1);
    CHECK(fill.price == 5000.5);
    CHECK(fill.fees == doctest::Approx(es().fee_per_contract));
}

TEST_CASE("a reported commission is used when present") {
    BrokerFixture f;
    const auto h = f.broker->place(1, 4999.75, 1);
    f.broker->drain_events();
    f.ib->fill(h.order_id, 1, 4999.75, 1.99);
    CHECK(f.broker->drain_events()[0].fill->fees == doctest::Approx(1.99));
}

TEST_CASE("a rejection carries the error code") {
    BrokerFixture f;
    const auto h = f.broker->place(1, 4999.75, 1);
    f.broker->drain_events();
    f.ib->reject(h.order_id, 201);
    const auto events = f.broker->drain_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == OrderEvent::Kind::Rejected);
    CHECK(events[0].detail.find("201") != std::string::npos);
}

TEST_CASE("events for orders it did not place are ignored") {
    BrokerFixture f;
    const IbOpenOrder foreign = f.ib->add_foreign_order(contract(5001, "ES"));
    f.ib->emit_status(foreign);
    CHECK(f.broker->drain_events().empty());
}

TEST_CASE("cancel_foreign leaves its own orders alone") {
    BrokerFixture f;
    const auto own = f.broker->place(1, 4999.75, 1);
    f.ib->add_foreign_order(contract(5001, "ES"));
    f.ib->add_foreign_order(contract(7777, "NQ"));  // another contract: not ours to touch
    CHECK(f.broker->cancel_foreign() == 1);
    CHECK(f.ib->cancelled == std::vector<long>{900});
    for (const long id : f.ib->cancelled) CHECK(id != own.order_id);
}

TEST_CASE("paper account detection") {
    CHECK(is_paper_account("DU1234567"));
    CHECK_FALSE(is_paper_account("U1234567"));
}

TEST_CASE("refuses a live account without the flag") {
    Config cfg;
    auto ib = std::make_shared<FakeGateway>();
    ib->accounts = {"U999"};
    IbkrConnection conn(cfg, es(), ib);
    CHECK_THROWS_WITH_AS(conn.connect(), doctest::Contains("allow_live_trading"), ExecutionError);
    CHECK_FALSE(ib->is_connected());
}

TEST_CASE("position on the contract and foreign rows") {
    Config cfg;
    auto ib = std::make_shared<FakeGateway>();
    IbkrConnection conn(cfg, es(), ib);
    conn.connect();
    conn.qualify(std::string("ESZ6"));
    ib->positions_rows = {
        fake_position(contract(5001, "ES", "ESZ6"), 2, 5000.0 * 50),
        fake_position(contract(5001, "ES", "ESZ6"), -1, 5002.0 * 50),
        fake_position(contract(9, "MES", "MESZ6"), 1, 1.0),
    };
    const auto report = conn.position();
    CHECK(report.quantity == 1);
    CHECK(report.avg_price == doctest::Approx((2 * 5000.0 - 5002.0) / 1));
    CHECK(report.foreign == std::vector<std::string>{"MESZ6"});
}

TEST_CASE("the connection reads the average cost per point and the account") {
    Config cfg;
    auto ib = std::make_shared<FakeGateway>();
    IbkrConnection conn(cfg, es(), ib);
    conn.connect();
    conn.qualify(std::string("ESZ6"));
    ib->positions_rows = {fake_position(contract(5001, "ES", "ESZ6"), 1, 5000.0 * 50)};
    const auto report = conn.position();
    CHECK(report.quantity == 1);
    CHECK(report.avg_price == 5000.0);
    CHECK(report.foreign.empty());
    const AccountValues values = conn.account_values();
    CHECK(values.at("NetLiquidation") == 250'000.0);
}

TEST_CASE("the front month is resolved through the continuous contract") {
    Config cfg;
    auto ib = std::make_shared<FakeGateway>();
    IbkrConnection conn(cfg, es(), ib);
    conn.connect();
    const IbContract& c = conn.qualify(std::nullopt);
    CHECK(c.sec_type == "FUT");
    CHECK(c.last_trade_date == "20261218");
    CHECK(ib->qualified == 2);
}

}
