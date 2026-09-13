// Run the pipeline over a recorded or generated tape.
//
// The replay's clock is the tape's: every record's ``ts_event`` advances
// it, the decision cycle runs each ``live.decision_interval_ms`` of tape
// time, and the message budget refills on that same clock -- so a replay
// also says whether the quoting would have stayed inside IBKR's limit on
// that tape.  Fills come from ``SimulatedBroker``; see its header for what
// that does and does not model, and read every replay P&L with it in
// mind: no latency, no impact, back-of-queue placement.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "harvester/book/feed.hpp"
#include "harvester/config.hpp"
#include "harvester/execution/simulated.hpp"
#include "harvester/inventory.hpp"
#include "harvester/live/journal.hpp"
#include "harvester/live/pipeline.hpp"
#include "harvester/replay/source.hpp"

namespace harvester {

struct ReplayResult {
    std::int64_t cycles = 0;
    std::int64_t records = 0;
    std::int64_t trades = 0;
    std::int64_t fills = 0;
    std::int64_t contracts = 0;
    double realised = 0.0;
    double fees = 0.0;
    double unrealised = 0.0;
    int final_position = 0;
    std::int64_t messages = 0;
    std::int64_t refused = 0;
    std::int64_t coalesced = 0;
    std::int64_t transitions = 0;
    std::map<std::string, MarkoutSummary> markouts;
    // Share of tape time in each level, in the order the levels were seen.
    std::vector<std::pair<std::string, double>> time_in_level;
    std::string markouts_text;
    bool halted = false;

    double net() const { return realised + unrealised - fees; }
    std::string summary() const;
};

class ReplayRunner : public RecordSink {
public:
    explicit ReplayRunner(const Config& cfg, SessionJournal* journal = nullptr);

    // The source the config names: the generated market, or the DBN file
    // (whose schema must match ``databento.schema``).
    std::unique_ptr<RecordSource> records();
    ReplayResult run(RecordSource* records = nullptr, std::optional<double> max_seconds = std::nullopt);

    // RecordSink
    bool on_mbo(const MboRecord& record) override;
    bool on_mbp10(const Mbp10Record& record) override;
    void on_symbol_mapping(std::string_view raw_symbol) override;

    Config cfg;
    const Product& product;
    RecordFeed feed;
    SimulatedBroker broker;
    Pipeline pipeline;

private:
    template <typename Record>
    bool on_record(const Record& record);
    void step(double now);
    void account_level(double now);
    ReplayResult result() const;

    double now_ = 0.0;
    double interval_;
    std::optional<double> next_decision_;
    std::optional<double> first_ts_;
    std::optional<double> max_seconds_;
    std::int64_t trades_ = 0;
    std::vector<std::pair<std::string, double>> time_in_level_;
    std::optional<double> last_level_ts_;
    std::string last_level_ = "calm";
    BookSnapshot snapshot_;
    std::vector<Trade> trade_buffer_;
};

ReplayResult run_replay(const Config& cfg, SessionJournal* journal = nullptr);

}  // namespace harvester
