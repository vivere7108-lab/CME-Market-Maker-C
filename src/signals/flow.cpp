#include "harvester/signals/flow.hpp"

#include <algorithm>
#include <cmath>

namespace harvester {

namespace {
double squash(double x) { return std::tanh(x); }
}  // namespace

OrderFlowImbalance::OrderFlowImbalance(double window_ms) : window_ns_(static_cast<std::int64_t>(window_ms * 1e6)) {}

void OrderFlowImbalance::update(const BookSnapshot& snapshot) {
    if (!snapshot.two_sided()) {
        last_.reset();
        return;
    }
    const Level& bid = *snapshot.best_bid();
    const Level& ask = *snapshot.best_ask();
    const Top current{bid.price, bid.size, ask.price, ask.size};
    depth_ = 0.99 * depth_ + 0.01 * ((static_cast<double>(bid.size) + static_cast<double>(ask.size)) / 2.0);
    if (last_) {
        const Top& prev = *last_;
        double e = 0.0;
        if (current.bid_price >= prev.bid_price) e += current.bid_size;
        if (current.bid_price <= prev.bid_price) e -= prev.bid_size;
        if (current.ask_price <= prev.ask_price) e -= current.ask_size;
        if (current.ask_price >= prev.ask_price) e += prev.ask_size;
        if (e != 0.0) {
            events_.emplace_back(snapshot.ts_event, e);
            sum_ += e;
        }
    }
    last_ = current;
    expire(snapshot.ts_event);
}

void OrderFlowImbalance::expire(std::int64_t now_ns) {
    const std::int64_t cutoff = now_ns - window_ns_;
    while (!events_.empty() && events_.front().first < cutoff) {
        sum_ -= events_.front().second;
        events_.pop_front();
    }
}

double OrderFlowImbalance::value(std::int64_t now_ns) {
    expire(now_ns);
    return squash(sum_ / std::max(depth_, 1.0));
}

QueueDepletion::QueueDepletion(double window_ms) : window_s_(window_ms / 1000.0) {}

void QueueDepletion::update(const BookSnapshot& snapshot) {
    if (!snapshot.two_sided()) return;
    update_side(1, *snapshot.best_bid(), snapshot.ts_event, bid_, rate_bid_);
    update_side(-1, *snapshot.best_ask(), snapshot.ts_event, ask_, rate_ask_);
}

void QueueDepletion::update_side(int side, const Level& level, std::int64_t ts, std::optional<Held>& held, double& rate) {
    if (!held) {
        held = Held{level.price, level.size, ts};
        return;
    }
    const double dt = std::max(static_cast<double>(ts - held->ts) / 1e9, 1e-6);
    double instantaneous;
    if (level.price == held->price) {
        // Fraction of the level consumed per second, scaled to the window.
        const double change = static_cast<double>(held->size - level.size) / static_cast<double>(std::max(held->size, 1));
        instantaneous = change / std::max(dt, window_s_) * window_s_;
    } else if ((side > 0 && level.price < held->price) || (side < 0 && level.price > held->price)) {
        instantaneous = 1.0;  // the level is gone
    } else {
        instantaneous = -0.5;  // the touch improved: fresh liquidity
    }
    // Blend in proportion to how much of the window has elapsed.
    const double weight = std::min(dt / window_s_, 1.0);
    rate = (1 - weight) * rate + weight * instantaneous;
    held = Held{level.price, level.size, ts};
}

double QueueDepletion::value() const { return squash(rate_ask_ - rate_bid_); }

AggressorRun::AggressorRun(int saturation, double decay_seconds)
    : saturation_(saturation), decay_ns_(decay_seconds * 1e9) {}

void AggressorRun::update(const Trade& trade) {
    if (trade.aggressor == 0) return;
    if (run_ * trade.aggressor > 0) {
        run_ += trade.aggressor;
        volume_ += trade.size;
    } else {
        run_ = trade.aggressor;
        volume_ = trade.size;
    }
    last_ts_ = trade.ts_event;
}

double AggressorRun::value(std::int64_t now_ns) const {
    if (!last_ts_ || run_ == 0) return 0.0;
    const std::int64_t age = std::max<std::int64_t>(now_ns - *last_ts_, 0);
    const double decay = std::max(1.0 - static_cast<double>(age) / decay_ns_, 0.0);
    const double raw = static_cast<double>(run_) / static_cast<double>(saturation_);
    return std::max(-1.0, std::min(1.0, raw)) * decay;
}

FlowSignals::FlowSignals(const FlowConfig& cfg)
    : ofi(cfg.ofi_window_ms), depletion(cfg.depletion_window_ms), run(cfg.run_saturation, cfg.run_decay_seconds) {}

void FlowSignals::on_book(const BookSnapshot& snapshot) {
    ofi.update(snapshot);
    depletion.update(snapshot);
}

void FlowSignals::on_trade(const Trade& trade) { run.update(trade); }

FlowState FlowSignals::state(std::int64_t now_ns) {
    return FlowState{ofi.value(now_ns), depletion.value(), run.value(now_ns), run.run()};
}

}  // namespace harvester
