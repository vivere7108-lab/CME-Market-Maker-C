// Download a tape from Databento's historical API for replay.
#include <databento/datetime.hpp>
#include <databento/enums.hpp>
#include <databento/historical.hpp>
#include <filesystem>

#include "harvester/databento/api.hpp"

namespace harvester {

std::uint64_t fetch_dbn(const Config& cfg, const std::string& api_key, const std::string& start,
                        const std::string& end, const std::string& out_path) {
    databento::Historical client = databento::HistoricalBuilder{}.SetKey(api_key).Build();
    const databento::Schema schema = cfg.databento.schema == "mbo" ? databento::Schema::Mbo : databento::Schema::Mbp10;
    databento::SType stype_in = databento::SType::Continuous;
    if (cfg.databento.stype_in == "raw_symbol") stype_in = databento::SType::RawSymbol;
    if (cfg.databento.stype_in == "parent") stype_in = databento::SType::Parent;
    client.TimeseriesGetRangeToFile(cfg.databento.dataset, databento::DateTimeRange<std::string>{start, end},
                                    {cfg.databento.symbol}, schema, stype_in, databento::SType::InstrumentId, 0,
                                    std::filesystem::path{out_path});
    return std::filesystem::file_size(out_path);
}

}  // namespace harvester
