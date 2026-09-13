#include "harvester/execution/throttle.hpp"

#include <algorithm>

namespace harvester {

MessageBudget::MessageBudget(double rate_per_second, int burst_, ClockFn clock)
    : rate(rate_per_second), burst(static_cast<double>(burst_)), clock_(std::move(clock)), tokens_(burst),
      last_(clock_()) {}

void MessageBudget::refill(double now) {
    const double elapsed = std::max(now - last_, 0.0);
    tokens_ = std::min(burst, tokens_ + elapsed * rate);
    last_ = now;
}

double MessageBudget::available(std::optional<double> now) {
    refill(now ? *now : clock_());
    return tokens_;
}

bool MessageBudget::try_acquire(std::optional<double> now, double headroom) {
    refill(now ? *now : clock_());
    if (tokens_ - 1.0 >= headroom - 1e-9) {
        tokens_ -= 1.0;
        ++spent;
        return true;
    }
    ++refused;
    return false;
}

ActionQueue::ActionQueue(MessageBudget& budget_) : budget(budget_) { pending_.reserve(8); }

void ActionQueue::submit(std::string key, int priority, Action action, std::string label) {
    ++sequence_;
    for (auto& queued : pending_) {
        if (queued.key == key) {
            ++coalesced;
            queued.priority = priority;
            queued.sequence = sequence_;
            queued.action = std::move(action);
            queued.label = std::move(label);
            return;
        }
    }
    pending_.push_back(Queued{priority, sequence_, std::move(key), std::move(action), std::move(label)});
}

bool ActionQueue::withdraw(std::string_view key) {
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->key == key) {
            pending_.erase(it);
            return true;
        }
    }
    return false;
}

std::vector<std::string> ActionQueue::pending_keys() const {
    std::vector<std::string> keys;
    keys.reserve(pending_.size());
    for (const auto& queued : pending_) keys.push_back(queued.key);
    return keys;
}

std::vector<std::string> ActionQueue::pump(std::optional<double> now) {
    std::vector<std::string> done;
    while (!pending_.empty()) {
        auto best = pending_.begin();
        for (auto it = pending_.begin() + 1; it != pending_.end(); ++it) {
            if (it->priority < best->priority || (it->priority == best->priority && it->sequence < best->sequence)) {
                best = it;
            }
        }
        if (!budget.try_acquire(now)) break;
        Queued chosen = std::move(*best);
        pending_.erase(best);
        chosen.action();
        ++executed;
        done.push_back(chosen.label.empty() ? chosen.key : chosen.label);
    }
    return done;
}

}  // namespace harvester
