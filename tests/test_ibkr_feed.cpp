// The IBKR book feed: IBKR's ladder turned into the MBP-10 records the
// rest of the system reads. Everything here runs without the TWS SDK --
// the socket is behind the ``IbMarketData`` seam and the tests drive the
// listener directly, which is what that seam is for.
#include "fakes.hpp"
#include "harvester/execution/ibkr_feed.hpp"
#include "helpers.hpp"

using namespace test;

namespace {

// IBKR's own encodings.
constexpr int INSERT = 0, UPDATE = 1, DELETE = 2;
constexpr int ASK = 0, BID = 1;

struct FeedFixture {
    FakeMarketData* session;
    IBKRConfig cfg;
    std::unique_ptr<IbkrBookFeed> feed;

    explicit FeedFixture(int rows = 10) {
        auto owned = std::make_unique<FakeMarketData>();
        session = owned.get();
        cfg.client_id = 23;
        feed = std::make_unique<IbkrBookFeed>(std::move(owned), cfg, es(), 10, rows);
    }

    IbMarketDataListener& in() { return feed->listener(); }

    // A two-sided ladder: bids 5000.00/4999.75, asks 5000.25/5000.50.
    void build_book() {
        in().on_depth(0, INSERT, BID, 5000.00, 30);
        in().on_depth(1, INSERT, BID, 4999.75, 40);
        in().on_depth(0, INSERT, ASK, 5000.25, 20);
        in().on_depth(1, INSERT, ASK, 5000.50, 50);
    }
};

}  // namespace

TEST_SUITE("ibkr feed") {

TEST_CASE("the ladder becomes a two-sided book") {
    FeedFixture f;
    f.build_book();
    const BookSnapshot s = f.feed->snapshot();
    REQUIRE(s.two_sided());
    CHECK(s.best_bid()->price == doctest::Approx(5000.00));
    CHECK(s.best_bid()->size == 30);
    CHECK(s.best_ask()->price == doctest::Approx(5000.25));
    CHECK(s.best_ask()->size == 20);
    CHECK(s.bids().size() == 2);
    CHECK(s.asks().size() == 2);
}

TEST_CASE("nothing is pushed until both sides have a row") {
    FeedFixture f;
    f.in().on_depth(0, INSERT, BID, 5000.00, 30);
    f.in().on_depth(1, INSERT, BID, 4999.75, 40);
    CHECK(f.feed->records() == 0);
    CHECK_FALSE(f.feed->snapshot().two_sided());
    f.in().on_depth(0, INSERT, ASK, 5000.25, 20);
    CHECK(f.feed->records() == 1);
    CHECK(f.feed->snapshot().two_sided());
}

TEST_CASE("insert shifts the rows below it down, delete shifts them up") {
    FeedFixture f;
    f.in().on_depth(0, INSERT, BID, 5000.00, 30);
    f.in().on_depth(1, INSERT, BID, 4999.50, 40);
    f.in().on_depth(0, INSERT, ASK, 5000.25, 20);
    // A level appears between the two bids: it takes row 1 and pushes the
    // 4999.50 down to row 2.
    f.in().on_depth(1, INSERT, BID, 4999.75, 15);
    const BookSnapshot s = f.feed->snapshot();
    REQUIRE(s.bids().size() == 3);
    CHECK(s.bids()[0].price == doctest::Approx(5000.00));
    CHECK(s.bids()[1].price == doctest::Approx(4999.75));
    CHECK(s.bids()[1].size == 15);
    CHECK(s.bids()[2].price == doctest::Approx(4999.50));

    FeedFixture g;
    g.build_book();
    g.in().on_depth(0, DELETE, BID, 0.0, 0);
    const BookSnapshot after = g.feed->snapshot();
    REQUIRE_FALSE(after.bids().empty());
    CHECK(after.best_bid()->price == doctest::Approx(4999.75));
    CHECK(after.best_bid()->size == 40);
    CHECK(after.bids().size() == 1);
}

TEST_CASE("an update replaces one row and leaves the rest") {
    FeedFixture f;
    f.build_book();
    f.in().on_depth(1, UPDATE, ASK, 5000.50, 7);
    const BookSnapshot s = f.feed->snapshot();
    CHECK(s.best_ask()->price == doctest::Approx(5000.25));
    REQUIRE(s.asks().size() == 2);
    CHECK(s.asks()[1].size == 7);
}

TEST_CASE("a row that goes to zero leaves the book") {
    FeedFixture f;
    f.build_book();
    f.in().on_depth(0, UPDATE, ASK, 5000.25, 0);
    const BookSnapshot s = f.feed->snapshot();
    REQUIRE_FALSE(s.asks().empty());
    CHECK(s.best_ask()->price == doctest::Approx(5000.50));
}

TEST_CASE("the ladder is capped at the rows asked for") {
    FeedFixture f(2);
    f.build_book();
    f.in().on_depth(2, INSERT, BID, 4999.50, 60);
    CHECK(f.feed->snapshot().bids().size() == 2);
}

TEST_CASE("out-of-step rows are dropped rather than guessed at") {
    FeedFixture f;
    f.build_book();
    const std::int64_t before = f.feed->records();
    f.in().on_depth(9, DELETE, BID, 0.0, 0);   // a row that was never sent
    f.in().on_depth(-1, UPDATE, ASK, 1.0, 1);  // not a row at all
    CHECK(f.feed->records() == before);
    CHECK(f.feed->snapshot().bids().size() == 2);
}

TEST_CASE("the quote rule reads the aggressor off the touch") {
    FeedFixture f;
    f.build_book();
    f.feed->take_trades();
    f.in().on_trade(0.0, 5000.25, 3);   // at the ask: bought
    f.in().on_trade(0.0, 5000.00, 4);   // at the bid: sold
    f.in().on_trade(0.0, 5000.50, 1);   // through the ask: bought
    const std::vector<Trade> trades = f.feed->take_trades();
    REQUIRE(trades.size() == 3);
    CHECK(trades[0].aggressor == 1);
    CHECK(trades[0].price == doctest::Approx(5000.25));
    CHECK(trades[0].size == 3);
    CHECK(trades[1].aggressor == -1);
    CHECK(trades[2].aggressor == 1);
    CHECK(f.feed->trades_seen() == 3);
    CHECK(f.feed->trades_unclassified() == 0);
    CHECK(f.feed->unknown() == doctest::Approx(0.0));
}

TEST_CASE("a trade reported after its own level was removed is still called") {
    // IBKR takes the swept level out of the ladder before it reports the
    // print, which leaves the price inside the new spread. The touch as it
    // stood before that change is what the trade actually crossed.
    FeedFixture f;
    f.build_book();
    f.feed->take_trades();
    f.in().on_depth(0, DELETE, ASK, 0.0, 0);  // the 5000.25 ask is swept away
    f.in().on_trade(0.0, 5000.25, 5);
    const std::vector<Trade> trades = f.feed->take_trades();
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].aggressor == 1);
    CHECK(f.feed->trades_unclassified() == 0);
}

TEST_CASE("a print with no side to give is counted, not guessed") {
    FeedFixture f;
    f.build_book();
    f.in().on_depth(0, UPDATE, BID, 4999.00, 30);  // a wide book
    f.in().on_depth(0, UPDATE, ASK, 5001.00, 20);
    f.feed->take_trades();
    f.in().on_trade(0.0, 5000.00, 2);  // squarely inside
    const std::vector<Trade> trades = f.feed->take_trades();
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].aggressor == 0);
    CHECK(f.feed->trades_unclassified() == 1);
    CHECK(f.feed->unknown() == doctest::Approx(1.0));
}

TEST_CASE("a trade before the book is two-sided is not classified at all") {
    FeedFixture f;
    f.in().on_depth(0, INSERT, BID, 5000.00, 30);
    f.in().on_trade(0.0, 5000.00, 1);
    CHECK(f.feed->take_trades().empty());
}

TEST_CASE("a reset drops the book until it is rebuilt") {
    FeedFixture f;
    f.build_book();
    REQUIRE(f.feed->snapshot().two_sided());
    f.in().on_reset("IBKR 317: market depth data has been reset");
    CHECK_FALSE(f.feed->snapshot().two_sided());
    // And the half-built book that follows is not quoted against either.
    f.in().on_depth(0, INSERT, BID, 5000.00, 30);
    CHECK_FALSE(f.feed->snapshot().two_sided());
    f.in().on_depth(0, INSERT, ASK, 5000.25, 20);
    CHECK(f.feed->snapshot().two_sided());
}

TEST_CASE("errors are counted") {
    FeedFixture f;
    CHECK(f.feed->errors() == 0);
    f.in().on_error(9001, 309, "max number of market depth requests exceeded");
    CHECK(f.feed->errors() == 1);
}

TEST_CASE("start qualifies the contract the router will route to") {
    FeedFixture f;
    f.feed->start();
    CHECK(f.session->connected);
    CHECK(f.session->client_id == 24);  // ibkr.client_id + 1, its own session
    REQUIRE(f.session->subscription.has_value());
    CHECK(f.session->subscription->contract.local_symbol == "ESZ6");
    CHECK(f.session->subscription->rows == 10);
    CHECK(f.feed->raw_symbol() == std::optional<std::string>("ESZ6"));
    CHECK(f.feed->wait_for_symbol(0.0) == std::optional<std::string>("ESZ6"));
    f.feed->close();
    CHECK_FALSE(f.session->connected);
    CHECK_FALSE(f.session->subscription.has_value());
}

TEST_CASE("the feed's own client id is honoured when set") {
    auto owned = std::make_unique<FakeMarketData>();
    FakeMarketData* session = owned.get();
    IBKRConfig cfg;
    cfg.client_id = 23;
    cfg.market_data_client_id = 77;
    IbkrBookFeed feed(std::move(owned), cfg, es(), 10, 10);
    feed.start();
    CHECK(session->client_id == 77);
    feed.close();
}

TEST_CASE("the book carries no order counts") {
    FeedFixture f;
    f.build_book();
    const BookSnapshot s = f.feed->snapshot();
    REQUIRE_FALSE(s.bids().empty());
    CHECK(s.bids()[0].count == 0);
}

}
