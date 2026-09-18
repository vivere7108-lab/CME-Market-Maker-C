// Stand-ins for the Databento-backed pieces in a build without databento-cpp.
#include <stdexcept>

#include "harvester/databento/api.hpp"
#include "harvester/replay/source.hpp"

namespace harvester {

namespace {
[[noreturn]] void unsupported(const char* what) {
    throw std::runtime_error(std::string(what) +
                             " needs a build with -DHARVESTER_WITH_DATABENTO=ON (databento-cpp); this build has none");
}
}  // namespace

bool databento_supported() { return false; }
bool dbn_supported() { return false; }
std::string dbn_schema(const std::string&) { unsupported("reading DBN files"); }
std::unique_ptr<RecordSource> open_dbn(const std::string&) { unsupported("reading DBN files"); }
std::shared_ptr<RecordFeed> make_databento_feed(const DatabentoConfig&, const LiveConfig&, const Product&, int) {
    unsupported("the live Databento feed");
}
std::uint64_t fetch_dbn(const Config&, const std::string&, const std::string&, const std::string&, const std::string&) {
    unsupported("fetching from Databento");
}

}  // namespace harvester
