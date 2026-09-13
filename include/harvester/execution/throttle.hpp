// IBKR's message budget, and the queue that spends it.
//
// IBKR disconnects an API client that sends more than 50 messages a
// second.  Every order action -- place, modify, cancel -- is one message,
// and so is every request for positions or account values.
// ``MessageBudget`` is a token bucket refilled at the configured rate;
// nothing in the system talks to the gateway without taking a token from
// it first.
//
// ``ActionQueue`` sits in front of the bucket and decides what the next
// token buys.  Actions carry a priority (lower is sooner) and a key; a
// newer action with the same key **replaces** the older one still waiting,
// so a burst of decisions for the bid side collapses to the latest one.
// The priorities:
//
//     0  cancel      a quote that should not be there is a risk
//     1  replace     a quote at the wrong price is a smaller risk
//     2  place       a missing quote is not a risk at all
//     3  poll        positions and account values -- the polls never
//                    starve a quote, and a quote never waits on a poll
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "harvester/util/clock.hpp"

namespace harvester {

inline constexpr int PRIORITY_CANCEL = 0;
inline constexpr int PRIORITY_REPLACE = 1;
inline constexpr int PRIORITY_PLACE = 2;
inline constexpr int PRIORITY_POLL = 3;

class MessageBudget {
public:
    MessageBudget(double rate_per_second, int burst, ClockFn clock = mono_now);

    double available(std::optional<double> now = std::nullopt);
    // Take one token if more than ``headroom`` would remain.
    bool try_acquire(std::optional<double> now = std::nullopt, double headroom = 0.0);

    double rate;
    double burst;
    std::int64_t spent = 0;
    std::int64_t refused = 0;

private:
    void refill(double now);

    ClockFn clock_;
    double tokens_;
    double last_;
};

class ActionQueue {
public:
    using Action = std::function<void()>;

    explicit ActionQueue(MessageBudget& budget);

    // Queue ``action`` under ``key``, replacing whatever waited there.
    void submit(std::string key, int priority, Action action, std::string label = "");
    bool withdraw(std::string_view key);
    std::size_t size() const { return pending_.size(); }
    bool empty() const { return pending_.empty(); }
    std::vector<std::string> pending_keys() const;
    // Run as many queued actions as the budget allows, best first.
    // Returns the labels of what ran.
    std::vector<std::string> pump(std::optional<double> now = std::nullopt);

    MessageBudget& budget;
    std::int64_t coalesced = 0;
    std::int64_t executed = 0;

private:
    struct Queued {
        int priority;
        std::int64_t sequence;
        std::string key;
        Action action;
        std::string label;
    };
    // A handful of entries at most (one per side, the polls); a flat
    // vector scanned linearly beats any map here.
    std::vector<Queued> pending_;
    std::int64_t sequence_ = 0;
};

}  // namespace harvester
