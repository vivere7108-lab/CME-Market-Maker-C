// The latency benchmark: what each stage of the path costs, on this box.
//
//     harvester_bench [seconds-of-tape]
//
// Numbers are wall-clock nanoseconds per call from a steady clock, taken
// over a generated tape, so they say what the code costs and nothing about
// the network.  The stages, in the order data moves through them:
//
//   mbp10 apply     one MBP-10 record into the book (the common live case)
//   mbo apply       one MBO record into the book (the high-rate case)
//   feed push       the record through the feed's lock, trades kept
//   snapshot        a locked copy of the top ten levels a side
//   engine decide   one Avellaneda-Stoikov decision
//   pipeline step   one whole decision cycle: signals, risk, engine,
//                   reconcile, throttle, simulated fills
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "harvester/book/feed.hpp"
#include "harvester/config.hpp"
#include "harvester/execution/simulated.hpp"
#include "harvester/live/pipeline.hpp"
#include "harvester/quoting/engine.hpp"
#include "harvester/replay/pyrandom.hpp"
#include "harvester/replay/synthetic.hpp"
#include "harvester/util/log.hpp"

using namespace harvester;

namespace {

using Clock = std::chrono::steady_clock;

struct Stats {
    double mean;
    double p50;
    double p99;
    double max;
};

Stats stats(std::vector<double>& samples) {
    std::sort(samples.begin(), samples.end());
    double total = 0;
    for (const double s : samples) total += s;
    const auto at = [&](double q) { return samples[std::min(samples.size() - 1, static_cast<std::size_t>(q * samples.size()))]; };
    return {total / samples.size(), at(0.5), at(0.99), samples.back()};
}

void report(const char* name, Stats s, const char* unit = "ns") {
    std::printf("  %-16s mean %9.0f %s   p50 %9.0f   p99 %9.0f   max %9.0f\n", name, s.mean, unit, s.p50, s.p99, s.max);
}

template <typename F>
std::vector<double> time_each(std::size_t n, F&& f) {
    std::vector<double> samples;
    samples.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto t0 = Clock::now();
        f(i);
        const auto t1 = Clock::now();
        samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }
    return samples;
}

// A plausible MBO stream: a few thousand resting orders, adds, cancels,
// modifies and fills concentrated near the touch.
std::vector<MboRecord> mbo_tape(std::size_t n) {
    PyRandom rng(11);
    std::vector<MboRecord> tape;
    tape.reserve(n);
    std::vector<std::pair<std::uint64_t, std::pair<int, std::int64_t>>> live;  // id -> (side, price)
    std::uint64_t next_id = 1;
    const std::int64_t tick = ES.tick_int();
    const std::int64_t mid = ES.fixed(5000.0);
    std::int64_t ts = 1'700'000'000'000'000'000LL;
    for (std::size_t i = 0; i < n; ++i) {
        ts += 20'000;
        MboRecord r;
        r.ts_event = ts;
        r.sequence = static_cast<std::uint32_t>(i);
        const double roll = rng.random();
        if (live.size() < 2000 || roll < 0.45) {
            const int side = rng.choice(std::array<int, 2>{1, -1});
            const auto depth = static_cast<std::int64_t>(std::min<std::int64_t>(static_cast<std::int64_t>(rng.expovariate(0.5)), 40));
            r.action = 'A';
            r.side = side > 0 ? 'B' : 'A';
            r.price = side > 0 ? mid - depth * tick : mid + (depth + 1) * tick;
            r.size = static_cast<std::uint32_t>(rng.randint(1, 20));
            r.order_id = next_id++;
            live.emplace_back(r.order_id, std::make_pair(side, r.price));
        } else {
            const std::size_t at = static_cast<std::size_t>(rng.randbelow(live.size()));
            const auto [id, where] = live[at];
            r.order_id = id;
            r.side = where.first > 0 ? 'B' : 'A';
            r.price = where.second;
            if (roll < 0.75) {
                r.action = 'C';
                r.size = 0;
                live[at] = live.back();
                live.pop_back();
            } else if (roll < 0.9) {
                r.action = 'M';
                r.size = static_cast<std::uint32_t>(rng.randint(1, 20));
            } else {
                r.action = 'F';
                r.size = 1;
            }
        }
        tape.push_back(r);
    }
    return tape;
}

}  // namespace

int main(int argc, char** argv) {
    log::set_level(log::Level::Error);
    const double seconds = argc > 1 ? std::atof(argv[1]) : 600.0;
    const Product& product = ES;

    std::printf("harvester latency benchmark (%.0fs of generated tape)\n", seconds);

    // The tape, generated up front so generation is not timed.
    std::vector<Mbp10Record> tape;
    {
        SyntheticMarket market(product, seconds, 5000.0, 7, 0.2);
        Mbp10Record r;
        while (market.next(r)) tape.push_back(r);
    }
    std::printf("  %zu MBP-10 records\n", tape.size());

    {
        Mbp10Builder builder;
        auto s = time_each(tape.size(), [&](std::size_t i) { builder.apply(tape[i]); });
        report("mbp10 apply", stats(s));
    }
    {
        const auto mbo = mbo_tape(std::max<std::size_t>(tape.size(), 200'000));
        MboBuilder builder;
        auto s = time_each(mbo.size(), [&](std::size_t i) { builder.apply(mbo[i]); });
        report("mbo apply", stats(s));
        std::printf("  %-16s (%zu orders resting at the end, %zu levels a side)\n", "", builder.orders_held(),
                    builder.book.side(1).size());
    }
    {
        RecordFeed feed("mbp-10", 10, mono_now);
        auto s = time_each(tape.size(), [&](std::size_t i) { feed.push(tape[i]); });
        report("feed push", stats(s));
        BookSnapshot snap;
        auto t = time_each(100'000, [&](std::size_t) { feed.snapshot_into(snap); });
        report("snapshot", stats(t));
    }
    {
        Config cfg;
        QuoteEngine engine(cfg.quoting, cfg.risk, product, "microprice", "reduce_only");
        RecordFeed feed("mbp-10", 10, mono_now);
        feed.push(tape[0]);
        const BookSnapshot snap = feed.snapshot();
        const FlowState flow{0.2, -0.1, 0.3, 2};
        ToxicityState tox;
        tox.spread_multiplier = 1.5;
        tox.size_multiplier = 1.0;
        int inv = 0;
        auto s = time_each(200'000, [&](std::size_t i) {
            const QuoteDecision d = engine.decide(snap, 0.3, flow, tox, inv);
            inv = (d.quoting() ? 1 : 0) - static_cast<int>(i % 2);
        });
        report("engine decide", stats(s));
    }
    {
        Config cfg;
        cfg.toxicity.bucket_contracts = 200;
        cfg.toxicity.window_buckets = 20;
        cfg.toxicity.history_buckets = 400;
        cfg.toxicity.warmup_buckets = 40;
        double now = 0.0;
        RecordFeed feed("mbp-10", 10, [&] { return now; });
        SimulatedBroker broker(product, "back", [&] { return now; });
        Pipeline pipeline(cfg, broker, [&] { return now; }, nullptr, &broker);
        const double interval = cfg.live.decision_interval_ms / 1000.0;
        double next_decision = static_cast<double>(tape[0].ts_event) / 1e9;
        BookSnapshot snap;
        std::vector<Trade> trades;
        std::vector<double> samples;
        for (const auto& r : tape) {
            const double ts = static_cast<double>(r.ts_event) / 1e9;
            now = ts;
            feed.push(r);
            while (ts >= next_decision) {
                now = next_decision;
                feed.snapshot_into(snap);
                feed.take_trades_into(trades);
                const auto t0 = Clock::now();
                pipeline.step(now, snap, trades, true, feed.age_seconds(now));
                const auto t1 = Clock::now();
                samples.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
                next_decision += interval;
            }
        }
        report("pipeline step", stats(samples));
        std::printf("  %-16s (%zu cycles, %zu fills, %lld messages)\n", "", samples.size(),
                    pipeline.inventory.fills.size(), static_cast<long long>(pipeline.budget.spent));
    }
    return 0;
}
