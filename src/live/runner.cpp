#include "harvester/live/runner.hpp"

#include <csignal>
#include <format>
#include <thread>

#include "harvester/databento/api.hpp"
#include "harvester/execution/simulated.hpp"
#include "harvester/util/clock.hpp"
#include "harvester/util/hours.hpp"
#include "harvester/util/log.hpp"

namespace harvester {

namespace {
constexpr const char* kLog = "harvester.live.runner";
std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) out += ", ";
        out += p;
    }
    return out;
}

}  // namespace

bool stop_signalled() { return g_stop.load(); }

void install_stop_handlers() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
}

LiveRunner::LiveRunner(const Config& cfg, bool dry_run, std::shared_ptr<RecordFeed> feed,
                       ConnectionFactory connection_factory)
    : cfg_(cfg), dry_run_(dry_run), product_(cfg_.instrument()), tz_(find_zone(product_.timezone)),
      feed_(std::move(feed)), connection_factory_(std::move(connection_factory)) {
    if (!feed_) feed_ = make_databento_feed(cfg_.databento, cfg_.live, product_, cfg_.book.depth);
    if (cfg_.live.journal) journal_ = std::make_unique<SessionJournal>(cfg_.live.journal_dir);
}

void LiveRunner::request_stop() {
    HLOG_INFO(kLog, "stop requested; pulling quotes and finishing the cycle");
    stop_.store(true);
}

bool LiveRunner::in_hours(double now) const {
    return within_quoting_hours(now, tz_, cfg_.live.quote_start, cfg_.live.quote_end);
}

Pipeline& LiveRunner::run(std::optional<std::int64_t> max_cycles) {
    install_stop_handlers();
    if (feed_->is_live()) {
        feed_->start();
        const auto symbol = feed_->wait_for_symbol(30.0);
        if (!symbol) HLOG_WARNING(kLog, "the feed has not named the contract after 30s; IBKR will use its own front month");
    }

    double backoff = cfg_.live.reconnect_backoff_seconds;
    int attempts = 0;
    try {
        while (!stop_.load() && !g_stop.load()) {
            const std::int64_t cycles_before = cycles_;
            try {
                run_connected(max_cycles);
            } catch (const ReconciliationError&) {
                HLOG_ERROR(kLog, "cannot reconcile against the broker; stopping");
                throw;
            } catch (const std::exception& exc) {  // survive the gateway
                if (stop_.load() || g_stop.load() || !cfg_.live.reconnect) throw;
                if (cycles_ > cycles_before) {
                    attempts = 0;
                    backoff = cfg_.live.reconnect_backoff_seconds;
                }
                ++attempts;
                if (cfg_.live.max_reconnect_attempts && attempts >= *cfg_.live.max_reconnect_attempts) {
                    HLOG_ERROR(kLog, "giving up after {} consecutive connection failures: {}", attempts, exc.what());
                    throw;
                }
                HLOG_WARNING(kLog, "connection lost ({}); reconnecting in {:.0f}s (attempt {})", exc.what(), backoff,
                             attempts);
                sleep(backoff);
                backoff = std::min(backoff * 2.0, cfg_.live.max_reconnect_backoff_seconds);
                continue;
            }
            break;
        }
    } catch (...) {
        feed_->close();
        throw;
    }
    feed_->close();
    if (!pipeline_) throw std::runtime_error("the runner never established a session");
    if (journal_) {
        std::string counts;
        for (const auto& [kind, n] : journal_->counts()) {
            if (!counts.empty()) counts += ", ";
            counts += std::format("{} {}", n, kind);
        }
        HLOG_INFO(kLog, "journal written to {} ({})", journal_->directory().string(), counts);
    }
    return *pipeline_;
}

void LiveRunner::sleep(double wait_seconds) {
    using steady = std::chrono::steady_clock;
    const auto deadline = steady::now() + std::chrono::duration_cast<steady::duration>(std::chrono::duration<double>(wait_seconds));
    const steady::duration slice = std::chrono::duration_cast<steady::duration>(std::chrono::seconds(1));
    while (!stop_.load() && !g_stop.load() && steady::now() < deadline) {
        const steady::duration remaining = deadline - steady::now();
        std::this_thread::sleep_for(remaining < slice ? remaining : slice);
    }
}

void LiveRunner::run_connected(std::optional<std::int64_t> max_cycles) {
    std::unique_ptr<IbkrConnection> conn;
    IbkrBroker* ibkr = nullptr;
    SimulatedBroker* simulated = nullptr;
    struct Disconnect {
        IbkrConnection* conn;
        ~Disconnect() {
            if (conn != nullptr) conn->disconnect();
        }
    } disconnect_guard{nullptr};

    if (dry_run_) {
        auto sim = std::make_unique<SimulatedBroker>(product_, cfg_.replay.queue_position);
        simulated = sim.get();
        broker_ = std::move(sim);
    } else {
        if (!connection_factory_) throw ExecutionError("no IBKR gateway is available in this build");
        conn = connection_factory_();
        conn->connect();
        disconnect_guard.conn = conn.get();
        conn->qualify(feed_->raw_symbol());
        auto real = std::make_unique<IbkrBroker>(*conn, product_, cfg_.ibkr.outside_rth);
        ibkr = real.get();
        broker_ = std::move(real);
    }
    pipeline_ = std::make_unique<Pipeline>(cfg_, *broker_, wall_now, journal_.get(), simulated);
    Pipeline& pipeline = *pipeline_;
    if (!halt_reason_.empty()) {
        pipeline.risk.halt(std::format("{} (found earlier in this run; a reconnect does not clear it)", halt_reason_));
    }
    if (conn) reconcile(*conn, ibkr, pipeline);

    const std::string contract = conn && conn->contract() ? conn->contract()->local_symbol
                                                          : feed_->raw_symbol().value_or("?");
    HLOG_INFO(kLog, "live runner started on {}, quoting {} every {:.0f}ms{}", conn ? conn->account() : "no account",
              contract, cfg_.live.decision_interval_ms,
              dry_run_ ? " [DRY RUN: simulated fills off the real book]" : "");
    const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(cfg_.live.decision_interval_ms / 1000.0));
    double last_positions = mono_now();
    double last_account = last_positions;
    double last_heartbeat = last_positions;
    std::optional<AccountValues> account;
    std::optional<int> broker_position;
    const std::optional<std::string> quoted_symbol = feed_->raw_symbol();
    BookSnapshot snapshot;
    std::vector<Trade> trades;
    trades.reserve(1024);
    auto next_tick = std::chrono::steady_clock::now();
    while (!stop_.load() && !g_stop.load() && (!max_cycles || cycles_ < *max_cycles)) {
        if (conn && !conn->is_connected()) throw ConnectionLost("the IBKR API connection dropped");
        if (feed_->raw_symbol() != quoted_symbol) {
            HLOG_WARNING(kLog, "the feed rolled to {}; reconnecting to re-qualify", feed_->raw_symbol().value_or("?"));
            pipeline.quotes.cancel_all("contract roll");
            pipeline.quotes.reconcile();
            pipeline.queue.pump();
            throw ConnectionLost("contract roll");
        }

        const double now = wall_now();
        const double mono = mono_now();
        feed_->snapshot_into(snapshot);
        feed_->take_trades_into(trades);
        pipeline.step(now, snapshot, trades, in_hours(now), feed_->age_seconds(mono),
                      account ? &*account : nullptr, broker_position);
        ++cycles_;
        if (pipeline.risk.halted && halt_reason_.empty()) halt_reason_ = pipeline.risk.halt_reason;

        // The polls: lowest priority, only with headroom in the budget.
        if (conn) {
            if (mono - last_positions >= cfg_.execution.poll_positions_seconds &&
                pipeline.budget.try_acquire(std::nullopt, POLL_HEADROOM)) {
                broker_position = poll_position(*conn, pipeline);
                last_positions = mono;
            }
            if (mono - last_account >= cfg_.execution.poll_account_seconds &&
                pipeline.budget.try_acquire(std::nullopt, POLL_HEADROOM)) {
                try {
                    account = conn->account_values();
                } catch (const std::exception& exc) {
                    HLOG_WARNING(kLog, "could not read the account ({})", exc.what());
                }
                last_account = mono;
            }
        }
        if (mono - last_heartbeat >= cfg_.live.heartbeat_seconds) {
            HLOG_INFO(kLog, "alive: {} cycles | {}", cycles_, pipeline.describe(snapshot.microprice()));
            last_heartbeat = mono;
        }
        // A fixed cadence: the next cycle starts one interval after this
        // one was due, not one interval after it finished.
        next_tick += interval;
        const auto now_steady = std::chrono::steady_clock::now();
        if (next_tick < now_steady) next_tick = now_steady + interval;
        const double wait = std::chrono::duration<double>(next_tick - now_steady).count();
        if (conn) {
            conn->gateway().sleep(wait);
        } else {
            std::this_thread::sleep_until(next_tick);
        }
    }

    pipeline.quotes.cancel_all("runner stopping");
    pipeline.quotes.reconcile();
    pipeline.queue.pump();
    if (conn) conn->gateway().sleep(1.0);
    if (ibkr != nullptr) ibkr->close();
    HLOG_INFO(kLog, "live runner stopped after {} cycles", cycles_);
}

// Adopt the broker's position; cancel orders this process did not place.
void LiveRunner::reconcile(IbkrConnection& conn, IbkrBroker* broker, Pipeline& pipeline) {
    const IbkrConnection::PositionReport report = conn.position();
    if (!report.foreign.empty()) {
        throw ReconciliationError(std::format(
            "the account holds {}, which this quoter cannot mark or manage. Close it, then restart.",
            join(report.foreign)));
    }
    if (report.quantity != 0) {
        pipeline.inventory.adopt(report.quantity, report.avg_price);
        HLOG_WARNING(kLog, "adopted an existing position: {:+d} {} @ {:.2f}", report.quantity, product_.name,
                     report.avg_price);
        if (std::abs(report.quantity) > cfg_.risk.max_position) {
            pipeline.risk.halt(std::format("adopted position {:+d} is outside the cap of {}", report.quantity,
                                           cfg_.risk.max_position));
        }
    }
    if (broker != nullptr) {
        const int cancelled = broker->cancel_foreign();
        if (cancelled > 0) {
            HLOG_WARNING(kLog, "cancelled {} working order(s) on the contract that this process did not place",
                         cancelled);
        }
    }
}

std::optional<int> LiveRunner::poll_position(IbkrConnection& conn, Pipeline& pipeline) {
    IbkrConnection::PositionReport report;
    try {
        report = conn.position();
    } catch (const std::exception& exc) {
        HLOG_WARNING(kLog, "could not read positions ({})", exc.what());
        return std::nullopt;
    }
    if (!report.foreign.empty()) {
        pipeline.risk.halt(std::format("the account now holds {}, which the book cannot see", join(report.foreign)));
    }
    return report.quantity;
}

Pipeline& run_live(LiveRunner& runner, std::optional<std::int64_t> max_cycles) { return runner.run(max_cycles); }

}  // namespace harvester
