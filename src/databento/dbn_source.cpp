// Reading a recorded DBN file back as records.
#include <databento/dbn_file_store.hpp>
#include <databento/enums.hpp>
#include <databento/record.hpp>
#include <filesystem>
#include <format>

#include "harvester/databento/records.hpp"
#include "harvester/replay/source.hpp"

namespace harvester {

namespace {

class DbnSource : public RecordSource {
public:
    explicit DbnSource(std::string path) : path_(std::move(path)) {}

    // Every record in the file, in file order (which is time order).
    // Symbol-mapping records come through too; definition and other
    // records are skipped, exactly as the live feed skips them.
    void replay(RecordSink& sink) override {
        databento::DbnFileStore store{std::filesystem::path{path_}};
        while (const databento::Record* record = store.NextRecord()) {
            if (const auto* mbo = record->GetIf<databento::MboMsg>()) {
                if (!sink.on_mbo(to_record(*mbo))) return;
            } else if (const auto* mbp = record->GetIf<databento::Mbp10Msg>()) {
                if (!sink.on_mbp10(to_record(*mbp))) return;
            } else if (const auto* mapping = record->GetIf<databento::SymbolMappingMsg>()) {
                sink.on_symbol_mapping(mapping->STypeOutSymbol());
            }
        }
    }

private:
    std::string path_;
};

}  // namespace

bool dbn_supported() { return true; }

std::string dbn_schema(const std::string& path) {
    databento::DbnFileStore store{std::filesystem::path{path}};
    const databento::Metadata& metadata = store.GetMetadata();
    if (!metadata.schema) throw std::runtime_error(std::format("{} has no single schema in its metadata", path));
    return databento::ToString(*metadata.schema);
}

std::unique_ptr<RecordSource> open_dbn(const std::string& path) {
    if (!std::filesystem::exists(path)) throw std::runtime_error("no such DBN file: " + path);
    return std::make_unique<DbnSource>(path);
}

}  // namespace harvester
