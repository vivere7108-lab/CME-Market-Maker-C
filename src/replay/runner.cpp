#include "harvester/replay/runner.hpp"

#include <format>
#include <stdexcept>

#include "harvester/util/format.hpp"
#include "harvester/util/log.hpp"

namespace harvester {

std::string ReplayResult::summary() const {
    std::string levels;
    for (const auto& [level, share] : time_in_level) {
        if (!levels.empty()) levels += ", ";
        levels += std::format("{} {}", level, fmt::pct0(share));
    }
    std::string out;
    out += std::format("replay: {} records, {} trades, {} decision cycles\n", fmt::commas(static_cast<double>(records)),
                       fmt::commas(static_cast<double>(trades)), fmt::commas(static_cast<double>(cycles)));
    out += std::format("fills: {} ({} contracts); final position {:+d}\n", fills, contracts, final_position);
    out += std::format("P&L: realised ${}  unrealised ${}  fees ${}  net ${}\n", fmt::commas(realised),
                       fmt::commas(unrealised), fmt::commas(fees), fmt::commas(net()));
    out += std::format("messages: {} sent, {} refused by the budget, {} coalesced\n",
                       fmt::commas(static_cast<double>(messages)), fmt::commas(static_cast<double>(refused)),
                       fmt::commas(static_cast<double>(coalesced)));
    out += std::format("toxicity: {} transitions; time in {}\n", transitions, levels);
    out += markouts_text;
    return out;
}

ReplayRunner::ReplayRunner(const Config& cfg_, SessionJournal* journal)
    : cfg(cfg_),
      product(cfg.instrument()),
      tz(find_zone(product.timezone)),
      feed(cfg.databento.schema, cfg.book.depth, [this] { return now_; }),
      broker(product, cfg.replay.queue_position, [this] { return now_; }),
      pipeline(cfg, broker, [this] { return now_; }, journal, &broker),
      interval_(cfg.live.decision_interval_ms / 1000.0) {
    trade_buffer_.reserve(1024);
}

std::unique_ptr<RecordSource> ReplayRunner::records() {
    const ReplayConfig& r = cfg.replay;
    if (r.source == "synthetic") {
        // A generated tape has no date of its own, so it is stamped at
        // ``live.quote_start`` on a fixed weekday: the hours check in ``step``
        // is the live runner's, and a tape stamped at whatever the epoch
        // happens to be would simply never be quoted. Only the stamps
        // move -- the records, and so every fill and every P&L line, are
        // the same tape the Python generator produces.
        const std::int64_t start = session_start_ns(tz, SYNTHETIC_DATE, cfg.live.quote_start);
        return std::make_unique<SyntheticSource>(SyntheticMarket(
            product, r.synthetic_seconds, r.synthetic_start_price, static_cast<std::uint64_t>(r.synthetic_seed),
            r.synthetic_toxic_fraction, start));
    }
    const std::string path = r.path.value_or("");
    const std::string schema = dbn_schema(path);
    if (schema != cfg.databento.schema) {
        throw std::invalid_argument(std::format("{} is a '{}' file but databento.schema is '{}'; set them to match",
                                                path, schema, cfg.databento.schema));
    }
    return open_dbn(path);
}

template <typename Record>
bool ReplayRunner::on_record(const Record& record) {
    const double ts = static_cast<double>(record.ts_event) / 1e9;
    now_ = ts;
    if (!first_ts_) {
        first_ts_ = ts;
        next_decision_ = ts;
    }
    if (max_seconds_ && ts - *first_ts_ > *max_seconds_) return false;
    if (feed.push(record)) ++trades_;
    while (next_decision_ && ts >= *next_decision_) {
        step(*next_decision_);
        *next_decision_ += interval_;
    }
    return true;
}

bool ReplayRunner::on_mbo(const MboRecord& record) { return on_record(record); }

bool ReplayRunner::on_mbp10(const Mbp10Record& record) { return on_record(record); }

void ReplayRunner::on_symbol_mapping(std::string_view raw_symbol) { feed.set_symbol(std::string(raw_symbol)); }

ReplayResult ReplayRunner::run(RecordSource* source, std::optional<double> max_seconds) {
    max_seconds_ = max_seconds;
    std::unique_ptr<RecordSource> owned;
    if (source == nullptr) {
        owned = records();
        source = owned.get();
    }
    source->replay(*this);
    if (next_decision_) step(now_);
    // Pull everything at the end so the working quotes are not counted as fills.
    pipeline.quotes.cancel_all("end of replay");
    pipeline.quotes.reconcile(now_);
    pipeline.queue.pump(now_);
    account_level(now_);
    return result();
}

void ReplayRunner::step(double now) {
    now_ = now;
    feed.snapshot_into(snapshot_);
    feed.take_trades_into(trade_buffer_);
    pipeline.step(now, snapshot_, trade_buffer_, in_hours(now), feed.age_seconds(now));
    account_level(now);
}

bool ReplayRunner::in_hours(double now) const {
    return within_quoting_hours(now, tz, cfg.live.quote_start, cfg.live.quote_end);
}

void ReplayRunner::account_level(double now) {
    const std::string level = to_string(pipeline.gate.state().level);
    if (last_level_ts_) {
        bool found = false;
        for (auto& [name, seconds] : time_in_level_) {
            if (name == last_level_) {
                seconds += now - *last_level_ts_;
                found = true;
                break;
            }
        }
        if (!found) time_in_level_.emplace_back(last_level_, now - *last_level_ts_);
    }
    last_level_ = level;
    last_level_ts_ = now;
}

ReplayResult ReplayRunner::result() const {
    const Pipeline& p = pipeline;
    const std::optional<double> anchor = p.last_snapshot ? p.last_snapshot->microprice() : std::nullopt;
    double total = 0.0;
    for (const auto& [_, seconds] : time_in_level_) total += seconds;
    if (total == 0.0) total = 1.0;
    ReplayResult out;
    out.cycles = p.cycles;
    out.records = feed.records();
    out.trades = trades_;
    out.fills = static_cast<std::int64_t>(p.inventory.fills.size());
    out.contracts = p.inventory.contracts_traded;
    out.realised = p.inventory.realised;
    out.fees = p.inventory.fees;
    out.unrealised = p.inventory.unrealised(anchor);
    out.final_position = p.inventory.position;
    out.messages = p.budget.spent;
    out.refused = p.budget.refused;
    out.coalesced = p.queue.coalesced;
    out.transitions = static_cast<std::int64_t>(p.gate.transitions.size());
    out.markouts = p.markouts.summary();
    for (const auto& [level, seconds] : time_in_level_) out.time_in_level.emplace_back(level, seconds / total);
    out.markouts_text = p.markouts.describe();
    out.halted = p.risk.halted;
    return out;
}

ReplayResult run_replay(const Config& cfg, SessionJournal* journal) { return ReplayRunner(cfg, journal).run(); }

}  // namespace harvester
