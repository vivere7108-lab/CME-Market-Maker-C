#include "helpers.hpp"

using namespace test;

namespace {
std::vector<std::pair<double, int>> rows(std::span<const Level> levels) {
    std::vector<std::pair<double, int>> out;
    for (const auto& l : levels) out.emplace_back(l.price, l.size);
    return out;
}
}  // namespace

TEST_SUITE("book") {

TEST_CASE("microprice weights each price by the other side's depth") {
    // Bid queue much deeper than the ask: the next trade is a lift.
    CHECK(microprice(100.0, 90, 101.0, 10) == doctest::Approx(100.9));
    CHECK(microprice(100.0, 10, 101.0, 90) == doctest::Approx(100.1));
    CHECK(microprice(100.0, 5, 101.0, 5) == doctest::Approx(100.5));
}

TEST_CASE("snapshot reads the microprice off the touch") {
    MboBuilder b;
    b.apply(mbo('A', 'B', 5000.0, 30, 1, 0, F_SNAPSHOT));
    b.apply(mbo('A', 'A', 5000.25, 10, 2, 0, F_SNAPSHOT));
    b.apply(mbo('N', 'N', 0, 0, 0));  // first live record: the snapshot is complete
    const BookSnapshot s = b.book.snapshot(5);
    CHECK(*s.microprice() == doctest::Approx(5000.0 + 0.25 * 30 / 40));
    CHECK(*s.mid() == 5000.125);
    CHECK(*s.top_imbalance() == doctest::Approx(0.5));
}

TEST_CASE("a one-sided book has no microprice") {
    MboBuilder b;
    b.apply(mbo('A', 'B', 5000.0, 30, 1));
    CHECK_FALSE(b.book.snapshot(5).microprice().has_value());
    CHECK_FALSE(b.book.snapshot(5).two_sided());
}

TEST_CASE("mbo: add, cancel, modify, fill") {
    MboBuilder b;
    b.apply(mbo('A', 'B', 5000.0, 5, 1));
    b.apply(mbo('A', 'B', 5000.0, 3, 2));
    b.apply(mbo('A', 'A', 5000.25, 4, 3));
    CHECK(b.book.snapshot(5).bids()[0].size == 8);
    CHECK(b.book.snapshot(5).bids()[0].count == 2);
    b.apply(mbo('C', 'B', 5000.0, 3, 2));
    CHECK(b.book.snapshot(5).bids()[0].size == 5);
    b.apply(mbo('M', 'B', 4999.75, 5, 1));  // moved down a tick
    BookSnapshot s = b.book.snapshot(5);
    CHECK(rows(s.bids()) == std::vector<std::pair<double, int>>{{4999.75, 5}});
    b.apply(mbo('F', 'A', 5000.25, 1, 3));
    CHECK(b.book.snapshot(5).asks()[0].size == 3);
    b.apply(mbo('F', 'A', 5000.25, 3, 3));
    CHECK(b.book.snapshot(5).asks().empty());
}

TEST_CASE("mbo: a trade record is the tape and does not touch the book") {
    MboBuilder b;
    b.apply(mbo('A', 'B', 5000.0, 5, 1));
    const auto trade = b.apply(mbo('T', 'A', 5000.0, 2, 0, 99));
    REQUIRE(trade.has_value());
    CHECK(trade->price == 5000.0);
    CHECK(trade->size == 2);
    CHECK(trade->aggressor == -1);
    CHECK(trade->ts_event == 99);
    CHECK(b.book.snapshot(5).bids()[0].size == 5);
}

TEST_CASE("mbo: a trade with no aggressor reads zero") {
    MboBuilder b;
    CHECK(b.apply(mbo('T', 'N', 5000.0, 2, 0))->aggressor == 0);
}

TEST_CASE("mbo: the snapshot flag clears and completes") {
    MboBuilder b;
    b.apply(mbo('A', 'B', 4000.0, 1, 9));  // stale, pre-snapshot
    b.apply(mbo('A', 'B', 5000.0, 5, 1, 0, F_SNAPSHOT));
    CHECK_FALSE(b.book.snapshot(5).complete);
    b.apply(mbo('A', 'A', 5000.25, 5, 2, 0, F_SNAPSHOT));
    CHECK_FALSE(b.book.snapshot(5).complete);
    b.apply(mbo('A', 'B', 4999.75, 1, 3));  // first live record ends the snapshot
    const BookSnapshot s = b.book.snapshot(5);
    CHECK(s.complete);
    CHECK(s.two_sided());
    for (const auto& l : s.bids()) CHECK(l.price != 4000.0);
}

TEST_CASE("mbo: no snapshot means never complete") {
    MboBuilder b;
    b.apply(mbo('A', 'B', 5000.0, 5, 1));
    b.apply(mbo('A', 'A', 5000.25, 5, 2));
    CHECK_FALSE(b.book.snapshot(5).complete);
}

TEST_CASE("mbo: clear empties everything") {
    MboBuilder b;
    b.apply(mbo('A', 'B', 5000.0, 5, 1, 0, F_SNAPSHOT));
    b.apply(mbo('A', 'B', 5000.0, 5, 1));
    b.apply(mbo('R', 'N', 0, 0, 0));
    CHECK(b.book.snapshot(5).bids().empty());
    CHECK(b.orders_held() == 0);
}

TEST_CASE("mbo: unknown order references are counted, not fatal") {
    MboBuilder b;
    b.apply(mbo('C', 'B', 5000.0, 5, 42));
    b.apply(mbo('F', 'B', 5000.0, 5, 43));
    CHECK(b.dropped == 2);
}

TEST_CASE("mbo: queue ahead is the level size") {
    MboBuilder b;
    b.apply(mbo('A', 'B', 5000.0, 5, 1));
    b.apply(mbo('A', 'B', 5000.0, 7, 2));
    CHECK(b.queue_ahead(1, P(5000.0)) == 12);
    CHECK(b.queue_ahead(1, P(4999.0)) == 0);
}

TEST_CASE("mbo: a replayed add for a held order replaces its state") {
    MboBuilder b;
    b.apply(mbo('A', 'B', 5000.0, 5, 1));
    b.apply(mbo('A', 'B', 5000.0, 7, 1));  // the same order id again
    CHECK(b.book.snapshot(5).bids()[0].size == 7);
    CHECK(b.book.snapshot(5).bids()[0].count == 1);
}

TEST_CASE("mbp10: the book is replaced from each record") {
    Mbp10Builder b;
    b.apply(mbp10({{5000.0, 10}, {4999.75, 20}}, {{5000.25, 5}}));
    BookSnapshot s = b.book.snapshot(10);
    CHECK(rows(s.bids()) == std::vector<std::pair<double, int>>{{5000.0, 10}, {4999.75, 20}});
    CHECK(rows(s.asks()) == std::vector<std::pair<double, int>>{{5000.25, 5}});
    CHECK(s.complete);
    b.apply(mbp10({{4999.75, 20}}, {{5000.0, 3}, {5000.25, 5}}));
    s = b.book.snapshot(10);
    CHECK(s.best_bid()->price == 4999.75);
    CHECK(s.best_ask()->price == 5000.0);
}

TEST_CASE("mbp10: a trade record yields the trade") {
    Mbp10Builder b;
    const auto t = b.apply(mbp10({{5000.0, 10}}, {{5000.25, 5}}, 7, 'T', 'B', 5000.25, 3));
    REQUIRE(t.has_value());
    CHECK(t->price == 5000.25);
    CHECK(t->size == 3);
    CHECK(t->aggressor == 1);
    CHECK(t->ts_event == 7);
}

TEST_CASE("mbp10: all ten levels are read, undefined ones skipped") {
    Levels bids, asks;
    for (int i = 0; i < 10; ++i) {
        bids.emplace_back(5000.0 - 0.25 * i, 10 + i);
        asks.emplace_back(5000.25 + 0.25 * i, 20 + i);
    }
    Mbp10Builder b;
    b.apply(mbp10(bids, asks));
    const BookSnapshot s = b.book.snapshot(10);
    CHECK(s.bids().size() == 10);
    CHECK(s.bids()[9].size == 19);
    CHECK(s.asks()[9].size == 29);
    b.apply(mbp10({{5000.0, 1}}, {}));
    CHECK(b.book.snapshot(10).asks().empty());
}

TEST_CASE("the side codes are the DBN one-character enums") {
    CHECK(side_of('B') == 1);
    CHECK(side_of('A') == -1);
    CHECK(side_of('N') == 0);
}

TEST_CASE("the order book keeps each side sorted best first") {
    OrderBook book;
    for (const double p : {5000.0, 4999.5, 5000.5, 4999.75}) book.add(1, P(p), 1);
    for (const double p : {5001.0, 5001.5, 5000.75}) book.add(-1, P(p), 1);
    CHECK(*book.best(1) == P(5000.5));
    CHECK(*book.best(-1) == P(5000.75));
    const BookSnapshot s = book.snapshot(2);
    CHECK(rows(s.bids()) == std::vector<std::pair<double, int>>{{5000.5, 1}, {5000.0, 1}});
    CHECK(rows(s.asks()) == std::vector<std::pair<double, int>>{{5000.75, 1}, {5001.0, 1}});
    book.set_level(1, P(5000.5), 0, 0);
    CHECK(*book.best(1) == P(5000.0));
    CHECK(book.find(1, P(4999.5))->size == 1);
    CHECK(book.find(1, P(4000.0)) == nullptr);
}

TEST_CASE("the feed drains trades and reports its age") {
    RecordFeed feed("mbp-10", 10, [] { return 10.0; });
    CHECK(std::isinf(feed.age_seconds(10.0)));
    feed.push(mbp10({{5000.0, 10}}, {{5000.25, 5}}, 7, 'T', 'B', 5000.25, 3));
    CHECK(feed.age_seconds(12.5) == doctest::Approx(2.5));
    CHECK(feed.take_trades().size() == 1);
    CHECK(feed.take_trades().empty());
    CHECK(feed.snapshot().two_sided());
    CHECK(feed.records() == 1);
    CHECK_THROWS_AS(feed.push(mbo('A', 'B', 5000.0, 1, 1)), std::logic_error);
}

}
