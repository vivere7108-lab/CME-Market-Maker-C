// Record stubs and a scripted broker shared by the tests.
#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "doctest/doctest.h"
#include "harvester/book/book.hpp"
#include "harvester/book/builder.hpp"
#include "harvester/book/feed.hpp"
#include "harvester/config.hpp"
#include "harvester/execution/base.hpp"
#include "harvester/instruments.hpp"
#include "harvester/util/clock.hpp"

namespace test {

using namespace harvester;

inline std::int64_t P(double price) { return static_cast<std::int64_t>(std::llround(price * PRICE_SCALE)); }

inline MboRecord mbo(char action, char side, double price, std::uint32_t size, std::uint64_t order_id,
                     std::int64_t ts = 0, std::uint8_t flags = 0, std::uint32_t sequence = 0) {
    MboRecord r;
    r.action = action;
    r.side = side;
    r.price = P(price);
    r.size = size;
    r.order_id = order_id;
    r.ts_event = ts;
    r.flags = flags;
    r.sequence = sequence;
    return r;
}

using Levels = std::vector<std::pair<double, std::uint32_t>>;

inline Mbp10Record mbp10(const Levels& bids, const Levels& asks, std::int64_t ts = 0, char action = 'A',
                         char side = 'N', double price = 0.0, std::uint32_t size = 0, std::uint32_t sequence = 0) {
    Mbp10Record r;
    r.action = action;
    r.side = side;
    r.price = P(price);
    r.size = size;
    r.ts_event = ts;
    r.flags = 0;
    r.sequence = sequence;
    for (std::size_t i = 0; i < 10; ++i) {
        BidAskPair pair;
        if (i < bids.size()) {
            pair.bid_px = P(bids[i].first);
            pair.bid_sz = bids[i].second;
            pair.bid_ct = 1;
        }
        if (i < asks.size()) {
            pair.ask_px = P(asks[i].first);
            pair.ask_sz = asks[i].second;
            pair.ask_ct = 1;
        }
        r.levels[i] = pair;
    }
    return r;
}

// Five levels a side, the touch as given.
inline BookSnapshot snapshot(double bid, int bid_size, double ask, int ask_size, std::int64_t ts = 0) {
    BookSnapshot s;
    s.ts_event = ts;
    s.sequence = 0;
    s.complete = true;
    s.push_bid(Level{bid, bid_size, 0});
    for (int i = 0; i < 4; ++i) s.push_bid(Level{bid - 0.25 * (i + 1), 50, 0});
    s.push_ask(Level{ask, ask_size, 0});
    for (int i = 0; i < 4; ++i) s.push_ask(Level{ask + 0.25 * (i + 1), 50, 0});
    return s;
}

// A clock the test moves by hand.
struct TestClock {
    std::shared_ptr<double> t = std::make_shared<double>(0.0);
    ClockFn fn() const {
        auto p = t;
        return [p] { return *p; };
    }
    void set(double value) { *t = value; }
};

// Acknowledges everything at once; fills on request.
class ScriptedBroker : public Broker {
public:
    struct Sent {
        std::string kind;
        std::int64_t order_id = 0;  // replace/cancel
        int side = 0;               // place
        double price = 0.0;
        int size = 0;
    };

    explicit ScriptedBroker(bool ack_ = true) : ack(ack_) {}

    OrderHandle place(int side, double price, int size) override {
        ++n;
        sent.push_back({"place", n, side, price, size});
        orders[n] = {side, price, size};
        if (ack) events.push_back(OrderEvent::ack(n));
        return OrderHandle{n};
    }

    OrderHandle replace(OrderHandle handle, double price, int size) override {
        sent.push_back({"replace", handle.order_id, 0, price, size});
        const int side = std::get<0>(orders[handle.order_id]);
        orders[handle.order_id] = {side, price, size};
        if (ack) events.push_back(OrderEvent::ack(handle.order_id));
        return handle;
    }

    void cancel(OrderHandle handle) override {
        sent.push_back({"cancel", handle.order_id});
        if (ack) {
            events.push_back(OrderEvent::cancelled(handle.order_id));
            orders.erase(handle.order_id);
        }
    }

    void fill(std::int64_t order_id, int size, std::optional<double> price = std::nullopt, double ts = 0.0) {
        const auto [side, held_price, _] = orders.at(order_id);
        Fill f;
        f.ts = ts;
        f.side = side;
        f.price = price.value_or(held_price);
        f.size = size;
        f.fees = ES.fee_per_contract * size;
        f.order_id = order_id;
        events.push_back(OrderEvent::filled(order_id, f));
    }

    void reject(std::int64_t order_id) { events.push_back(OrderEvent::rejected(order_id, "201: rejected")); }

    std::vector<OrderEvent> drain_events() override {
        std::vector<OrderEvent> out;
        out.swap(events);
        return out;
    }

    std::int64_t messages_sent() const override { return static_cast<std::int64_t>(sent.size()); }

    std::vector<std::string> kinds() const {
        std::vector<std::string> out;
        for (const auto& s : sent) out.push_back(s.kind);
        return out;
    }

    int count(const std::string& kind) const {
        int c = 0;
        for (const auto& s : sent) c += s.kind == kind;
        return c;
    }

    bool ack;
    std::int64_t n = 0;
    std::vector<Sent> sent;
    std::vector<OrderEvent> events;
    std::map<std::int64_t, std::tuple<int, double, int>> orders;
};

// The ``cfg`` fixture: small buckets so the gate warms up inside a short
// generated tape.
inline Config test_config() {
    Config config;
    config.toxicity.bucket_contracts = 100.0;
    config.toxicity.window_buckets = 10;
    config.toxicity.history_buckets = 200;
    config.toxicity.warmup_buckets = 20;
    config.replay.synthetic_seconds = 120.0;
    return config;
}

inline const Product& es() { return get_product("ES"); }

// A scratch directory unique to this process.
std::string temp_dir(const std::string& tag);

}  // namespace test
