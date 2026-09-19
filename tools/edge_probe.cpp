// The informational-edge probe.
//
// Not a backtest.  A backtest answers "what would this configuration have
// earned"; this answers the two questions underneath it, so that they can
// be asked of a product whose tape the strategy has never seen:
//
//   1. How much is a maker *allowed* to charge here?  The exchange sets a
//      floor on the spread (one tick) and takes a fee out of it; the tape
//      sets how far behind the touch an aggressor will still reach.  The
//      probe rests a notional quote 0..5 ticks behind the touch on every
//      sweep and records, for each distance, whether it was reached and
//      what it captured against the pre-sweep mid.  That is the revenue
//      curve: capture rises linearly in the distance, fill probability
//      falls, and where the product of the two peaks is what the market
//      will pay a maker who never competes for the touch.
//
//   2. How much of that does the aggressor take back?  Every notional fill
//      is marked out against the mid and the microprice at 0, 0.1, 1, 5,
//      30 and 60 seconds.  Markout minus fee is the whole P&L of a passive
//      fill; the question of whether the strategy is viable on a product
//      is the question of whether that number is positive, and of whether
//      its *signals* can tell the positive fills from the negative ones
//      before the sweep arrives.
//
// So each row is one notional fill, carrying the signal state as it stood
// **before the first trade of the sweep** -- OFI, queue depletion, the
// aggressor run, VPIN and its percentile rank, the gate's level, realised
// vol, the spread and the touch imbalance -- and the markouts that
// followed.  Nothing in a row's signal columns is contemporaneous with the
// sweep; the scoring in the aggregator can therefore be read as prediction
// rather than as description.
//
// Sweeps, not trades
// ------------------
// CME sends one trade record per price level, so one aggressor clearing
// three levels is three records.  A quote fills once.  Trades are
// therefore grouped into sweeps -- same aggressor, no more than
// ``--sweep-gap-ms`` apart -- the quote prices are fixed from the book as
// it stood before the sweep's first trade, and each distance fills at most
// once per sweep.  The sweep is also the unit of the fill-probability
// denominator.
//
//   harvester_edge --dbn tape.dbn.zst --config configs/es_paper.yaml
//                  --product ES --out events.csv --summary summary.json

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "harvester/book/feed.hpp"
#include "harvester/config.hpp"
#include "harvester/instruments.hpp"
#include "harvester/replay/source.hpp"
#include "harvester/signals/flow.hpp"
#include "harvester/signals/vol.hpp"
#include "harvester/signals/vpin.hpp"

using namespace harvester;

namespace {

// Seconds after the fill at which the mid is read back.
constexpr double HORIZONS[] = {0.0, 0.1, 1.0, 5.0, 30.0, 60.0};
constexpr int NH = static_cast<int>(std::size(HORIZONS));
// Ticks behind the touch at which a notional quote is rested.
constexpr int MAX_BEHIND = 5;
constexpr int NB = MAX_BEHIND + 1;

// The signal state, frozen at the instant before a sweep's first trade.
struct SignalSnap {
    double ofi = 0.0;
    double depletion = 0.0;
    double run = 0.0;
    std::int64_t run_length = 0;
    double vpin = std::nan("");
    double vpin_pct = std::nan("");
    int tox = 0;
    bool warm = false;
    double sigma = 0.0;
    double spread_ticks = std::nan("");
    double imbalance = std::nan("");
    std::int64_t bid_depth = 0;
    std::int64_t ask_depth = 0;
    std::int32_t bid_top = 0;
    std::int32_t ask_top = 0;
};

// One notional fill, waiting for its horizons to elapse.
struct Pending {
    std::int64_t ts = 0;
    int behind = 0;
    int side = 0;  // +1 we bought (our bid was swept), -1 we sold
    double price = 0.0;
    double mid_pre = 0.0;
    double micro_pre = 0.0;
    std::int64_t sweep_ts = 0;
    std::int32_t sweep_contracts = 0;
    int sweep_trades = 0;
    bool through = false;  // the sweep printed past our price, so no queue doubt
    // What was already resting at our price before the sweep, and what the
    // sweep ended up trading there.  A quote that joined the level rather
    // than created it is behind the first number, so it only fills once the
    // second exceeds it -- which is the ``queue_position: back`` the
    // replay's own fill model assumes.
    std::int32_t queue_ahead = 0;
    std::int32_t volume_at_price = 0;
    SignalSnap sig;
    double mid_h[NH];
    double micro_h[NH];
    bool have[NH];
};

// One periodic observation of the whole state, with what the mid did next.
// The fill rows answer "was this fill informed"; these answer the prior
// question, "does the state predict the next move at all", on a sample
// that is not conditioned on a sweep having happened.
struct Sample {
    std::int64_t ts = 0;
    double mid = 0.0;
    double micro_off = 0.0;   // (microprice - mid) in ticks
    double spread_ticks = 0.0;
    double imb1 = 0.0, imb5 = 0.0, imb10 = 0.0;
    std::int64_t bid1 = 0, ask1 = 0, bid5 = 0, ask5 = 0;
    double ofi = 0.0, depletion = 0.0, run = 0.0;
    std::int64_t run_length = 0;
    double vpin = std::nan(""), vpin_pct = std::nan("");
    int tox = 0;
    bool warm = false;
    double sigma = 0.0;
    // The tape over the last second: how busy, how one-sided.
    int trades_1s = 0;
    std::int64_t volume_1s = 0, signed_volume_1s = 0;
    // Where the mid has just been, in ticks: the bounce and the drift.
    double r_prev_100ms = 0.0, r_prev_1s = 0.0;
    // The self-exciting arrival intensity of aggressive market orders --
    // a Hawkes kernel sum, exp(-(t - t_i)/tau) over sweep starts, kept
    // exactly at trade resolution rather than counted in the sample
    // window.  Two timescales because ES's kernel is not one.
    double mo_fast = 0.0, mo_slow = 0.0;      // per sweep
    double mv_fast = 0.0, mv_slow = 0.0;      // weighted by contracts
    // What is resting 0..5 ticks behind each touch: the level size a quote
    // would have to queue behind, which with `queue_position: back` IS its
    // position in the queue.
    std::int32_t bid_sz[NB] = {0};
    std::int32_t ask_sz[NB] = {0};
    double seconds_into_tape = 0.0;
    double fwd_mid[NH];
    double fwd_micro[NH];
    bool have[NH];
};

struct Sweep {
    int aggressor = 0;
    std::int64_t start_ts = 0;
    std::int64_t last_ts = 0;
    double quote_px[NB];
    bool filled[NB];
    std::int32_t queue_ahead[NB];
    std::int32_t volume_at[NB];
    bool valid = false;
    double mid_pre = 0.0;
    double micro_pre = 0.0;
    std::int32_t contracts = 0;
    int trades = 0;
    SignalSnap sig;
};

struct Args {
    std::string dbn;
    std::string config;
    std::string product;
    std::string out;
    std::string summary;
    double sweep_gap_ms = 50.0;
    double max_seconds = 0.0;
    // For a product the registry does not know: its contract arithmetic.
    double tick = 0.0;
    double multiplier = 0.0;
    double fee = -1.0;
    // VPIN's bucket, in contracts. Scaled per product so that a bucket
    // closes as often on a thin tape as on a thick one; a gate whose
    // buckets close once a minute on ZC and every four seconds on ES is
    // not the same gate, and the comparison would be of cadence rather
    // than of information.
    double bucket = 0.0;
    std::string samples;
    double sample_ms = 200.0;
};

[[noreturn]] void die(const std::string& message) {
    std::cerr << "harvester_edge: " << message << "\n";
    std::exit(2);
}

class Probe : public RecordSink {
public:
    Probe(const Config& cfg, const Product& product, const Args& args, std::FILE* out)
        : cfg_(cfg),
          product_(product),
          feed_(cfg.databento.schema, cfg.book.depth, [this] { return static_cast<double>(now_) / 1e9; }),
          flow_(cfg.flow),
          gate_(cfg.toxicity),
          vol_(cfg.quoting.vol_halflife_seconds, cfg.quoting.vol_sample_ms, cfg.quoting.vol_floor,
               cfg.quoting.vol_ceiling),
          gap_ns_(static_cast<std::int64_t>(args.sweep_gap_ms * 1e6)),
          max_ns_(static_cast<std::int64_t>(args.max_seconds * 1e9)),
          out_(out) {
        sample_ns_ = static_cast<std::int64_t>(args.sample_ms * 1e6);
    }

    void set_samples(std::FILE* f) { samples_out_ = f; }

    bool on_mbo(const MboRecord& record) override { return step(record); }
    bool on_mbp10(const Mbp10Record& record) override { return step(record); }

    // Totals the summary is written from.
    std::int64_t records = 0, trades = 0, sweeps = 0, events = 0;
    std::int64_t first_ts = 0, last_ts = 0;
    std::int64_t sweep_contracts = 0;
    double time_ns = 0.0, spread_time = 0.0, one_tick_time = 0.0, mid_time = 0.0;
    double bid_top_time = 0.0, ask_top_time = 0.0;
    std::int64_t traded_contracts = 0;
    std::int64_t fills_at[NB] = {0};
    double sigma_time = 0.0;

    // At the end of the tape the rows still in flight are written out with
    // whichever horizons elapsed; the aggregator drops a row with a gap.
    void flush_all() {
        flush_pending();
        if (samples_out_ != nullptr) flush_samples();
    }

    void flush_pending() {
        while (!pending_.empty()) {
            emit(pending_.front());
            pending_.pop_front();
            ++base_;
        }
    }

private:
    template <typename Record>
    bool step(const Record& record) {
        now_ = record.ts_event;
        if (!first_ts) first_ts = now_;
        if (max_ns_ > 0 && now_ - first_ts > max_ns_) return false;
        last_ts = now_;
        ++records;

        // The book as it stood *before* this record: the swap keeps it
        // without a second copy per record.
        std::swap(prev_, cur_);
        const std::optional<Trade> trade = feed_.push(record);
        feed_.snapshot_into(cur_);

        accumulate_time();
        resolve(cur_);
        if (samples_out_ != nullptr) {
            trim_windows();
            resolve_samples(cur_);
            maybe_sample();
        }

        if (trade && trade->aggressor != 0) {
            ++trades;
            traded_contracts += trade->size;
            if (samples_out_ != nullptr) tape_.emplace_back(trade->ts_event, trade->size, trade->aggressor);
            on_sweep_trade(*trade);
        }

        // The signals see the book after the record and the trade after the
        // book, which is the order the live pipeline feeds them in.
        flow_.on_book(cur_);
        if (trade) {
            flow_.on_trade(*trade);
            gate_.update(*trade);
        }
        if (const auto anchor = cfg_.book.anchor == "mid" ? cur_.mid() : cur_.microprice()) {
            vol_.update(*anchor, now_);
        }
        return true;
    }

    void accumulate_time() {
        if (last_acc_ && cur_.two_sided()) {
            const double dt = static_cast<double>(now_ - *last_acc_);
            if (dt > 0 && dt < 5e9) {  // a gap longer than five seconds is a halt, not a book
                const double spread_ticks = *cur_.spread() / product_.tick_size;
                time_ns += dt;
                spread_time += dt * spread_ticks;
                if (spread_ticks < 1.5) one_tick_time += dt;
                mid_time += dt * *cur_.mid();
                bid_top_time += dt * cur_.best_bid()->size;
                ask_top_time += dt * cur_.best_ask()->size;
                sigma_time += dt * vol_.sigma();
            }
        }
        last_acc_ = now_;
    }

    SignalSnap capture_signals() {
        SignalSnap s;
        const FlowState f = flow_.state(now_);
        s.ofi = f.ofi;
        s.depletion = f.depletion;
        s.run = f.run;
        s.run_length = f.run_length;
        const ToxicityState t = gate_.state();
        if (t.vpin) s.vpin = *t.vpin;
        if (t.percentile) s.vpin_pct = *t.percentile;
        s.tox = t.index();
        s.warm = t.warmed_up;
        s.sigma = vol_.sigma();
        if (prev_.two_sided()) {
            s.spread_ticks = *prev_.spread() / product_.tick_size;
            if (const auto imb = prev_.top_imbalance()) s.imbalance = *imb;
            s.bid_depth = prev_.depth(1);
            s.ask_depth = prev_.depth(-1);
            s.bid_top = prev_.best_bid()->size;
            s.ask_top = prev_.best_ask()->size;
        }
        return s;
    }

    void on_sweep_trade(const Trade& trade) {
        const bool continues = sweep_.valid && sweep_.aggressor == trade.aggressor &&
                               trade.ts_event - sweep_.last_ts <= gap_ns_;
        if (!continues) {
            // A new sweep: the quote prices are fixed from the book as it
            // stood before this trade, which is where a live quote would
            // have been resting.
            if (!prev_.two_sided()) {
                sweep_.valid = false;
                return;
            }
            ++sweeps;
            excite_arrival();
            sweep_ = Sweep{};
            sweep_.valid = true;
            sweep_.aggressor = trade.aggressor;
            sweep_.start_ts = trade.ts_event;
            sweep_.mid_pre = *prev_.mid();
            sweep_.micro_pre = prev_.microprice().value_or(*prev_.mid());
            sweep_.sig = capture_signals();
            // A seller-initiated sweep reaches down to our bid; a
            // buyer-initiated one reaches up to our ask.
            const double touch = trade.aggressor < 0 ? prev_.best_bid()->price : prev_.best_ask()->price;
            const double away = (trade.aggressor < 0 ? -1.0 : 1.0) * product_.tick_size;
            for (int b = 0; b <= MAX_BEHIND; ++b) {
                sweep_.quote_px[b] = touch + away * b;
                sweep_.filled[b] = false;
                sweep_.volume_at[b] = 0;
                sweep_.queue_ahead[b] = resting_at(prev_, -trade.aggressor, sweep_.quote_px[b]);
            }
        }
        if (!sweep_.valid) return;
        sweep_.last_ts = trade.ts_event;
        excite_volume(trade.size);
        sweep_.contracts += trade.size;
        ++sweep_.trades;
        sweep_contracts += trade.size;

        const int our_side = -trade.aggressor;  // they sell, we buy
        for (int b = 0; b <= MAX_BEHIND; ++b) {
            const double px = sweep_.quote_px[b];
            const bool through = our_side > 0 ? trade.price < px - 1e-9 : trade.price > px + 1e-9;
            const bool at = std::fabs(trade.price - px) < 1e-9;
            if (at) sweep_.volume_at[b] += trade.size;
            if (sweep_.filled[b]) continue;
            if (!through && !at) continue;
            sweep_.filled[b] = true;
            ++fills_at[b];
            Pending p;
            p.ts = trade.ts_event;
            p.behind = b;
            p.side = our_side;
            p.price = px;
            p.mid_pre = sweep_.mid_pre;
            p.micro_pre = sweep_.micro_pre;
            p.sweep_ts = sweep_.start_ts;
            p.through = through;
            p.queue_ahead = sweep_.queue_ahead[b];
            p.sig = sweep_.sig;
            for (int h = 0; h < NH; ++h) p.have[h] = false;
            pending_.push_back(p);
            ++events;
        }
        // The sweep's own size is only known once it is over; it is written
        // out with the row, so patch every row this sweep has produced.
        for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) {
            if (it->sweep_ts != sweep_.start_ts) break;
            it->sweep_contracts = sweep_.contracts;
            it->sweep_trades = sweep_.trades;
            it->volume_at_price = sweep_.volume_at[it->behind];
        }
    }

    // Every horizon has its own pointer into the queue.  Events are
    // appended in time order and a horizon elapses in that same order, so
    // each pointer only ever moves forward and each row's horizon is
    // stamped with the first book state at or after it -- rather than with
    // whatever the book happened to be when the row reached the front,
    // which would give every horizon of a row the same price.
    // Contracts resting at ``price`` on ``side`` in ``book``; zero when the
    // level is not there, which is a level we would have had to ourselves.
    static std::int32_t resting_at(const BookSnapshot& book, int side, double price) {
        for (const Level& level : side > 0 ? book.bids() : book.asks()) {
            if (std::fabs(level.price - price) < 1e-9) return level.size;
        }
        return 0;
    }

    void resolve(const BookSnapshot& book) {
        if (!book.two_sided()) return;
        const double mid = *book.mid();
        const double micro = book.microprice().value_or(mid);
        for (int h = 0; h < NH; ++h) {
            const std::int64_t horizon = static_cast<std::int64_t>(HORIZONS[h] * 1e9);
            while (next_[h] < base_ + static_cast<std::int64_t>(pending_.size())) {
                Pending& p = pending_[static_cast<std::size_t>(next_[h] - base_)];
                if (now_ < p.ts + horizon) break;
                p.mid_h[h] = mid;
                p.micro_h[h] = micro;
                p.have[h] = true;
                ++next_[h];
            }
        }
        // The oldest row is the first to have every horizon behind it.
        while (!pending_.empty()) {
            bool complete = true;
            for (int h = 0; h < NH; ++h) {
                if (next_[h] <= base_) {
                    complete = false;
                    break;
                }
            }
            if (!complete) break;
            emit(pending_.front());
            pending_.pop_front();
            ++base_;
        }
    }

    // Decay the kernel sums to ``now_``.  O(1), and exact for an
    // exponential kernel however irregular the arrivals are.
    void decay_hawkes() {
        if (hawkes_ts_ == 0) { hawkes_ts_ = now_; return; }
        const double dt = static_cast<double>(now_ - hawkes_ts_) / 1e9;
        if (dt <= 0) return;
        hawkes_ts_ = now_;
        const double f = std::exp(-dt / kFastTau), s = std::exp(-dt / kSlowTau);
        mo_fast_ *= f; mv_fast_ *= f;
        mo_slow_ *= s; mv_slow_ *= s;
    }

    void excite_arrival() {
        decay_hawkes();
        mo_fast_ += 1.0; mo_slow_ += 1.0;
    }

    void excite_volume(std::int32_t contracts) {
        decay_hawkes();
        mv_fast_ += contracts; mv_slow_ += contracts;
    }

    // The resting size at each tick offset behind a touch.
    void level_sizes(const BookSnapshot& book, int side, std::int32_t* out) const {
        const double tick = product_.tick_size;
        const double best = side > 0 ? book.best_bid()->price : book.best_ask()->price;
        for (int i = 0; i < NB; ++i) out[i] = 0;
        const auto levels = side > 0 ? book.bids() : book.asks();
        for (const Level& level : levels) {
            const double away = side > 0 ? (best - level.price) : (level.price - best);
            const int k = static_cast<int>(std::lround(away / tick));
            if (k >= 0 && k < NB) out[k] += level.size;
        }
    }

    void trim_windows() {
        const std::int64_t cutoff = now_ - 1'000'000'000;
        while (!tape_.empty() && std::get<0>(tape_.front()) < cutoff) tape_.pop_front();
        while (mids_.size() > 1 && mids_.front().first < cutoff - 1'000'000'000) mids_.pop_front();
        if (cur_.two_sided()) {
            if (mids_.empty() || now_ - mids_.back().first >= 10'000'000) mids_.emplace_back(now_, *cur_.mid());
        }
    }

    // The mid as it stood ``ago`` nanoseconds back, or the oldest we hold.
    double mid_ago(std::int64_t ago) const {
        const std::int64_t want = now_ - ago;
        double out = mids_.empty() ? 0.0 : mids_.front().second;
        for (const auto& [ts, mid] : mids_) {
            if (ts > want) break;
            out = mid;
        }
        return out;
    }

    void maybe_sample() {
        if (!cur_.two_sided()) return;
        if (next_sample_ts_ == 0) next_sample_ts_ = now_;
        if (now_ < next_sample_ts_) return;
        while (next_sample_ts_ <= now_) next_sample_ts_ += sample_ns_;

        const double tick = product_.tick_size;
        const double mid = *cur_.mid();
        Sample s;
        s.ts = now_;
        s.mid = mid;
        s.micro_off = (cur_.microprice().value_or(mid) - mid) / tick;
        s.spread_ticks = *cur_.spread() / tick;
        s.bid1 = cur_.best_bid()->size;
        s.ask1 = cur_.best_ask()->size;
        s.bid5 = cur_.depth(1, 5);
        s.ask5 = cur_.depth(-1, 5);
        const auto ratio = [](double b, double a) { return (b + a) > 0 ? (b - a) / (b + a) : 0.0; };
        s.imb1 = ratio(static_cast<double>(s.bid1), static_cast<double>(s.ask1));
        s.imb5 = ratio(static_cast<double>(s.bid5), static_cast<double>(s.ask5));
        s.imb10 = ratio(static_cast<double>(cur_.depth(1)), static_cast<double>(cur_.depth(-1)));
        const FlowState f = flow_.state(now_);
        s.ofi = f.ofi;
        s.depletion = f.depletion;
        s.run = f.run;
        s.run_length = f.run_length;
        const ToxicityState t = gate_.state();
        if (t.vpin) s.vpin = *t.vpin;
        if (t.percentile) s.vpin_pct = *t.percentile;
        s.tox = t.index();
        s.warm = t.warmed_up;
        s.sigma = vol_.sigma();
        for (const auto& [ts, size, agg] : tape_) {
            (void)ts;
            ++s.trades_1s;
            s.volume_1s += size;
            s.signed_volume_1s += static_cast<std::int64_t>(size) * agg;
        }
        decay_hawkes();
        s.mo_fast = mo_fast_; s.mo_slow = mo_slow_;
        s.mv_fast = mv_fast_; s.mv_slow = mv_slow_;
        level_sizes(cur_, 1, s.bid_sz);
        level_sizes(cur_, -1, s.ask_sz);
        s.r_prev_100ms = (mid - mid_ago(100'000'000)) / tick;
        s.r_prev_1s = (mid - mid_ago(1'000'000'000)) / tick;
        s.seconds_into_tape = static_cast<double>(now_ - first_ts) / 1e9;
        for (int h = 0; h < NH; ++h) s.have[h] = false;
        samples_.push_back(s);
    }

    void resolve_samples(const BookSnapshot& book) {
        if (!book.two_sided()) return;
        const double tick = product_.tick_size;
        const double mid = *book.mid();
        const double micro = book.microprice().value_or(mid);
        for (int h = 0; h < NH; ++h) {
            const std::int64_t horizon = static_cast<std::int64_t>(HORIZONS[h] * 1e9);
            while (sample_next_[h] < sample_base_ + static_cast<std::int64_t>(samples_.size())) {
                Sample& s = samples_[static_cast<std::size_t>(sample_next_[h] - sample_base_)];
                if (now_ < s.ts + horizon) break;
                s.fwd_mid[h] = (mid - s.mid) / tick;
                s.fwd_micro[h] = (micro - s.mid) / tick;
                s.have[h] = true;
                ++sample_next_[h];
            }
        }
        while (!samples_.empty()) {
            bool complete = true;
            for (int h = 0; h < NH; ++h) {
                if (sample_next_[h] <= sample_base_) {
                    complete = false;
                    break;
                }
            }
            if (!complete) break;
            emit_sample(samples_.front());
            samples_.pop_front();
            ++sample_base_;
        }
    }

    void emit_sample(const Sample& s) {
        std::fprintf(samples_out_, "%lld,%.10g,%.6g,%.6g,%.6g,%.6g,%.6g,%lld,%lld,%lld,%lld",
                     static_cast<long long>(s.ts), s.mid, s.micro_off, s.spread_ticks, s.imb1, s.imb5, s.imb10,
                     static_cast<long long>(s.bid1), static_cast<long long>(s.ask1), static_cast<long long>(s.bid5),
                     static_cast<long long>(s.ask5));
        std::fprintf(samples_out_, ",%.6g,%.6g,%.6g,%lld,%.6g,%.6g,%d,%d,%.6g,%d,%lld,%lld,%.6g,%.6g,%.6g", s.ofi,
                     s.depletion, s.run, static_cast<long long>(s.run_length), s.vpin, s.vpin_pct, s.tox,
                     s.warm ? 1 : 0, s.sigma, s.trades_1s, static_cast<long long>(s.volume_1s),
                     static_cast<long long>(s.signed_volume_1s), s.r_prev_100ms, s.r_prev_1s, s.seconds_into_tape);
        std::fprintf(samples_out_, ",%.6g,%.6g,%.6g,%.6g", s.mo_fast, s.mo_slow, s.mv_fast, s.mv_slow);
        for (int i = 0; i < NB; ++i) std::fprintf(samples_out_, ",%d", s.bid_sz[i]);
        for (int i = 0; i < NB; ++i) std::fprintf(samples_out_, ",%d", s.ask_sz[i]);
        for (int h = 0; h < NH; ++h) {
            if (s.have[h]) std::fprintf(samples_out_, ",%.6g", s.fwd_mid[h]);
            else std::fprintf(samples_out_, ",");
        }
        for (int h = 0; h < NH; ++h) {
            if (s.have[h]) std::fprintf(samples_out_, ",%.6g", s.fwd_micro[h]);
            else std::fprintf(samples_out_, ",");
        }
        std::fputc('\n', samples_out_);
    }

    void flush_samples() {
        while (!samples_.empty()) {
            emit_sample(samples_.front());
            samples_.pop_front();
            ++sample_base_;
        }
    }

    void emit(const Pending& p) {
        const double tick = product_.tick_size;
        std::fprintf(out_, "%lld,%d,%d,%.10g,%.6g,%.6g,%d,%d,%d", static_cast<long long>(p.ts), p.behind, p.side,
                     p.price, p.side * (p.mid_pre - p.price) / tick, p.side * (p.micro_pre - p.price) / tick,
                     p.through ? 1 : 0, p.sweep_contracts, p.sweep_trades);
        std::fprintf(out_, ",%d,%d", p.queue_ahead, p.volume_at_price);
        std::fprintf(out_, ",%.6g,%.6g,%.6g,%lld,%.6g,%.6g,%d,%d,%.6g,%.6g,%.6g,%lld,%lld,%d,%d", p.sig.ofi,
                     p.sig.depletion, p.sig.run, static_cast<long long>(p.sig.run_length), p.sig.vpin, p.sig.vpin_pct,
                     p.sig.tox, p.sig.warm ? 1 : 0, p.sig.sigma, p.sig.spread_ticks, p.sig.imbalance,
                     static_cast<long long>(p.sig.bid_depth), static_cast<long long>(p.sig.ask_depth), p.sig.bid_top,
                     p.sig.ask_top);
        for (int h = 0; h < NH; ++h) {
            if (p.have[h]) {
                std::fprintf(out_, ",%.6g", p.side * (p.mid_h[h] - p.price) / tick);
            } else {
                std::fprintf(out_, ",");
            }
        }
        for (int h = 0; h < NH; ++h) {
            if (p.have[h]) {
                std::fprintf(out_, ",%.6g", p.side * (p.micro_h[h] - p.price) / tick);
            } else {
                std::fprintf(out_, ",");
            }
        }
        std::fputc('\n', out_);
    }

    const Config& cfg_;
    const Product& product_;
    RecordFeed feed_;
    FlowSignals flow_;
    ToxicityGate gate_;
    RealisedVol vol_;
    std::int64_t gap_ns_;
    std::int64_t max_ns_;
    std::FILE* out_;
    std::int64_t now_ = 0;
    std::optional<std::int64_t> last_acc_;
    BookSnapshot cur_;
    BookSnapshot prev_;
    Sweep sweep_;
    std::deque<Pending> pending_;
    std::deque<Sample> samples_;
    std::int64_t sample_base_ = 0;
    std::int64_t sample_next_[NH] = {0};
    std::int64_t next_sample_ts_ = 0;
    std::int64_t sample_ns_ = 0;
    std::FILE* samples_out_ = nullptr;
    // The Hawkes kernel sums, and the instant they were last decayed to.
    static constexpr double kFastTau = 0.5, kSlowTau = 10.0;
    double mo_fast_ = 0.0, mo_slow_ = 0.0, mv_fast_ = 0.0, mv_slow_ = 0.0;
    std::int64_t hawkes_ts_ = 0;
    // The tape and the mid over the last second, for the window features.
    std::deque<std::tuple<std::int64_t, std::int32_t, int>> tape_;
    std::deque<std::pair<std::int64_t, double>> mids_;
    // Absolute index of pending_.front(), and of the first row whose
    // horizon h has not been stamped yet.
    std::int64_t base_ = 0;
    std::int64_t next_[NH] = {0};
};

std::string header() {
    std::string h =
        "ts,behind,side,price,cap_mid_ticks,cap_micro_ticks,through,sweep_contracts,sweep_trades,"
        "queue_ahead,volume_at_price,"
        "ofi,depletion,run,run_length,vpin,vpin_pct,tox,warm,sigma,spread_ticks,imbalance,bid_depth,ask_depth,"
        "bid_top,ask_top";
    for (int h_i = 0; h_i < NH; ++h_i) h += ",mid_" + std::to_string(h_i);
    for (int h_i = 0; h_i < NH; ++h_i) h += ",micro_" + std::to_string(h_i);
    return h;
}

std::string sample_header() {
    std::string h =
        "ts,mid,micro_off,spread_ticks,imb1,imb5,imb10,bid1,ask1,bid5,ask5,"
        "ofi,depletion,run,run_length,vpin,vpin_pct,tox,warm,sigma,trades_1s,volume_1s,signed_volume_1s,"
        "r_prev_100ms,r_prev_1s,seconds_into_tape";
    h += ",mo_fast,mo_slow,mv_fast,mv_slow";
    for (int i = 0; i < NB; ++i) h += ",bid_sz_" + std::to_string(i);
    for (int i = 0; i < NB; ++i) h += ",ask_sz_" + std::to_string(i);
    for (int i = 0; i < NH; ++i) h += ",fwd_mid_" + std::to_string(i);
    for (int i = 0; i < NH; ++i) h += ",fwd_micro_" + std::to_string(i);
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) die("missing value for " + a);
            return argv[++i];
        };
        if (a == "--dbn") args.dbn = next();
        else if (a == "--config" || a == "-c") args.config = next();
        else if (a == "--product") args.product = next();
        else if (a == "--out" || a == "-o") args.out = next();
        else if (a == "--summary") args.summary = next();
        else if (a == "--samples") args.samples = next();
        else if (a == "--sample-ms") args.sample_ms = std::stod(next());
        else if (a == "--bucket") args.bucket = std::stod(next());
        else if (a == "--tick") args.tick = std::stod(next());
        else if (a == "--multiplier") args.multiplier = std::stod(next());
        else if (a == "--fee") args.fee = std::stod(next());
        else if (a == "--sweep-gap-ms") args.sweep_gap_ms = std::stod(next());
        else if (a == "--max-seconds") args.max_seconds = std::stod(next());
        else die("unknown argument " + a);
    }
    if (args.dbn.empty() || args.config.empty() || args.out.empty()) {
        die("usage: harvester_edge --dbn TAPE --config CONFIG --out EVENTS.csv [--product P] [--summary S.json]");
    }

    Config cfg = Config::from_yaml(args.config);
    if (!args.product.empty()) cfg.product = args.product;
    // A product the registry has never heard of is registered from the
    // command line, so a tape can be probed without a code change.
    if (args.tick > 0.0 && args.multiplier > 0.0) {
        Product p;
        p.name = cfg.product;
        p.databento_root = cfg.product;
        p.ibkr_symbol = cfg.product;
        p.multiplier = args.multiplier;
        p.tick_size = args.tick;
        p.fee_per_contract = args.fee >= 0.0 ? args.fee : 0.0;
        register_product(p);
    }
    const Product& product = cfg.instrument();

    const std::string schema = dbn_schema(args.dbn);
    cfg.databento.schema = schema;
    if (args.bucket > 0.0) cfg.toxicity.bucket_contracts = args.bucket;

    std::FILE* out = std::fopen(args.out.c_str(), "w");
    if (!out) die("cannot write " + args.out);
    std::fprintf(out, "%s\n", header().c_str());

    Probe probe(cfg, product, args, out);
    std::FILE* samples = nullptr;
    if (!args.samples.empty()) {
        samples = std::fopen(args.samples.c_str(), "w");
        if (!samples) die("cannot write " + args.samples);
        std::fprintf(samples, "%s\n", sample_header().c_str());
        probe.set_samples(samples);
    }
    auto source = open_dbn(args.dbn);
    source->replay(probe);
    probe.flush_all();
    std::fclose(out);
    if (samples) std::fclose(samples);

    const double seconds = static_cast<double>(probe.last_ts - probe.first_ts) / 1e9;
    const double t = std::max(probe.time_ns, 1.0);
    std::string json;
    auto add = [&](const std::string& key, double value) {
        if (!json.empty()) json += ",\n";
        char buf[128];
        std::snprintf(buf, sizeof(buf), "  \"%s\": %.10g", key.c_str(), value);
        json += buf;
    };
    json += "  \"product\": \"" + product.name + "\",\n  \"schema\": \"" + schema + "\",\n  \"tape\": \"" + args.dbn +
            "\"";
    add("records", static_cast<double>(probe.records));
    add("trades", static_cast<double>(probe.trades));
    add("sweeps", static_cast<double>(probe.sweeps));
    add("events", static_cast<double>(probe.events));
    add("traded_contracts", static_cast<double>(probe.traded_contracts));
    add("span_seconds", seconds);
    add("book_seconds", probe.time_ns / 1e9);
    add("mean_spread_ticks", probe.spread_time / t);
    add("share_one_tick", probe.one_tick_time / t);
    add("mean_mid", probe.mid_time / t);
    add("mean_bid_top", probe.bid_top_time / t);
    add("mean_ask_top", probe.ask_top_time / t);
    add("mean_sigma_pts_rt_s", probe.sigma_time / t);
    add("tick_size", product.tick_size);
    add("multiplier", product.multiplier);
    add("tick_value", product.tick_value());
    add("fee_per_contract", product.fee_per_contract);
    add("vpin_bucket_contracts", cfg.toxicity.bucket_contracts);
    for (int b = 0; b <= MAX_BEHIND; ++b) add("fills_behind_" + std::to_string(b), static_cast<double>(probe.fills_at[b]));

    const std::string text = "{\n" + json + "\n}\n";
    if (!args.summary.empty()) {
        std::FILE* s = std::fopen(args.summary.c_str(), "w");
        if (!s) die("cannot write " + args.summary);
        std::fputs(text.c_str(), s);
        std::fclose(s);
    }
    std::cout << text;
    return 0;
}
