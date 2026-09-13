// The forward walk: the real feed, the real router, the shared pipeline.
//
// Two connections with two lifetimes.  The Databento session is started
// once and outlives everything else: it has its own reconnect policy and
// the book it keeps is not IBKR's business.  The IBKR connection is made
// inside a reconnect loop, because the gateway restarts itself once a day
// and drops every API client with it -- and on every reconnection the
// working orders on the contract are reconciled (anything this process
// did not place is cancelled) and the position is adopted from the
// account.
//
// The decision loop runs on a steady-clock timer at
// ``live.decision_interval_ms``; the gateway's events land on its own
// thread between cycles and are drained at the top of each one.  Databento
// writes the book from its own thread; every read of it is a copy taken
// under the feed's lock.
//
// A dry run wires the *same* feed into a ``SimulatedBroker``: every
// decision is taken, every quote is "placed" and filled off the real book
// and the real tape, and nothing reaches IBKR.  Unlike the Python runner,
// a dry run here needs no gateway at all: there is no account to adopt a
// position from and no order to route, so it runs on a box with only a
// Databento key.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "harvester/book/feed.hpp"
#include "harvester/config.hpp"
#include "harvester/execution/base.hpp"
#include "harvester/execution/ibkr.hpp"
#include "harvester/live/journal.hpp"
#include "harvester/live/pipeline.hpp"

namespace harvester {

// Tokens that must remain in the budget after a poll takes one, so a fill
// arriving mid-poll can still be answered with a cancel.
inline constexpr double POLL_HEADROOM = 3.0;

// The account holds something the quoter cannot manage.
struct ReconciliationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The gateway went away.
struct ConnectionLost : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using ConnectionFactory = std::function<std::unique_ptr<IbkrConnection>()>;

class LiveRunner {
public:
    // Injection seams for the tests: a ``RecordFeed`` in place of the
    // Databento session, a fake gateway in place of the TWS API.
    LiveRunner(const Config& cfg, bool dry_run = false, std::shared_ptr<RecordFeed> feed = nullptr,
               ConnectionFactory connection_factory = nullptr);

    void request_stop();
    // Runs until stopped (or ``max_cycles``), reconnecting as configured.
    // Returns the pipeline of the last session, which the runner owns.
    Pipeline& run(std::optional<std::int64_t> max_cycles = std::nullopt);

    Pipeline* pipeline() { return pipeline_.get(); }
    std::int64_t cycles() const { return cycles_; }
    bool in_hours(double now) const;

private:
    void run_connected(std::optional<std::int64_t> max_cycles);
    void reconcile(IbkrConnection& conn, IbkrBroker* broker, Pipeline& pipeline);
    std::optional<int> poll_position(IbkrConnection& conn, Pipeline& pipeline);
    void sleep(double seconds);

    Config cfg_;
    bool dry_run_;
    const Product& product_;
    const std::chrono::time_zone* tz_;
    std::shared_ptr<RecordFeed> feed_;
    ConnectionFactory connection_factory_;
    std::unique_ptr<SessionJournal> journal_;
    std::unique_ptr<Pipeline> pipeline_;
    std::unique_ptr<Broker> broker_;
    std::atomic<bool> stop_{false};
    std::int64_t cycles_ = 0;
    std::string halt_reason_;
};

// Whether a SIGINT/SIGTERM has been received by this process.
bool stop_signalled();
void install_stop_handlers();

Pipeline& run_live(LiveRunner& runner, std::optional<std::int64_t> max_cycles = std::nullopt);

}  // namespace harvester
