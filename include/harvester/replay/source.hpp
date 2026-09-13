// Where a replay's records come from.
//
// A ``RecordSource`` pushes its records, in time order, into a
// ``RecordSink``; the sink returns false to stop early.  The generated
// market and the DBN file reader are the two sources; the replay runner
// is the sink.  Records are handed over by reference and never copied
// into an intermediate container, so a 60-million-record MBO tape streams
// through in constant memory.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "harvester/book/builder.hpp"
#include "harvester/replay/synthetic.hpp"

namespace harvester {

class RecordSink {
public:
    virtual ~RecordSink() = default;
    virtual bool on_mbo(const MboRecord& record) = 0;
    virtual bool on_mbp10(const Mbp10Record& record) = 0;
    virtual void on_symbol_mapping(std::string_view /*raw_symbol*/) {}
};

class RecordSource {
public:
    virtual ~RecordSource() = default;
    virtual void replay(RecordSink& sink) = 0;
};

class SyntheticSource : public RecordSource {
public:
    explicit SyntheticSource(SyntheticMarket market) : market_(std::move(market)) {}
    void replay(RecordSink& sink) override {
        Mbp10Record record;
        while (market_.next(record)) {
            if (!sink.on_mbp10(record)) break;
        }
    }
    SyntheticMarket& market() { return market_; }

private:
    SyntheticMarket market_;
};

// The schema a DBN file was written with ("mbo", "mbp-10", ...).
// Requires a build with HARVESTER_WITH_DATABENTO; throws otherwise.
std::string dbn_schema(const std::string& path);
// A source over a DBN file (zstd-compressed or not).
std::unique_ptr<RecordSource> open_dbn(const std::string& path);
// Whether this build can read DBN files at all.
bool dbn_supported();

}  // namespace harvester
