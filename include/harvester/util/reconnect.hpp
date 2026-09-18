// Pacing for reconnection after a session drops or is refused.
//
// The runner applies this shape to the IBKR socket inline, around its own
// loop. The Databento feed cannot: its reconnection happens inside
// databento-cpp, which restarts the session the instant the exception
// callback returns Restart, so the wait has to be carried as a value the
// callback can consult. A session the gateway refuses outright -- an
// unentitled schema, a rejected key -- is otherwise retried as fast as a
// socket can be opened.
#pragma once

#include <algorithm>
#include <optional>

namespace harvester {

class ReconnectPacer {
public:
    struct Step {
        bool retry = false;
        // How long to wait before that retry. Zero when giving up.
        double wait_seconds = 0.0;
        // 1 for the first failure since the last good session.
        int attempt = 0;
    };

    ReconnectPacer(double backoff_seconds, double max_backoff_seconds, std::optional<int> max_attempts)
        : base_(backoff_seconds), max_(max_backoff_seconds), max_attempts_(max_attempts), backoff_(backoff_seconds) {}

    // A session failed: whether to try again, and how long to wait first.
    Step fail() {
        const int attempt = ++attempts_;
        if (max_attempts_ && attempt >= *max_attempts_) return {false, 0.0, attempt};
        const double wait = backoff_;
        backoff_ = std::min(backoff_ * 2.0, max_);
        return {true, wait, attempt};
    }

    // A session came up: the next failure starts its backoff from the bottom.
    void succeeded() {
        attempts_ = 0;
        backoff_ = base_;
    }

    int attempts() const { return attempts_; }

private:
    double base_;
    double max_;
    std::optional<int> max_attempts_;
    double backoff_;
    int attempts_ = 0;
};

}  // namespace harvester
