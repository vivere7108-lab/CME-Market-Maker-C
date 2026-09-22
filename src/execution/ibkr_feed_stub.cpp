// The IBKR feed factory in a build without the TWS client. The ladder in
// ``ibkr_feed.cpp`` is always compiled and always tested; only the socket
// behind it is conditional.
#include <stdexcept>

#include "harvester/execution/ibkr_feed.hpp"

namespace harvester {

bool ibkr_market_data_supported() { return false; }

std::shared_ptr<RecordFeed> make_ibkr_feed(const IBKRConfig&, const Product&, int) {
    throw std::runtime_error("ibkr.market_data needs a build with -DHARVESTER_WITH_IBKR=ON and TWS_API_DIR set; "
                             "this build has no TWS client");
}

}  // namespace harvester
