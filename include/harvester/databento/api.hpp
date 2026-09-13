// The Databento-backed pieces: the live feed, the historical fetch, and
// (declared in ``replay/source.hpp``) the DBN file reader.  All three need
// databento-cpp and are built with ``HARVESTER_WITH_DATABENTO``; without
// it these calls throw a message saying so, and the rest of the system --
// the synthetic replay, the tests, the tooling -- is unaffected.
#pragma once

#include <memory>
#include <string>

#include "harvester/book/feed.hpp"
#include "harvester/config.hpp"
#include "harvester/instruments.hpp"

namespace harvester {

bool databento_supported();

// One ``databento`` live session feeding the book on its own thread.
// ``start()`` subscribes; the API key is read from ``cfg.api_key_env``.
std::shared_ptr<RecordFeed> make_databento_feed(const DatabentoConfig& cfg, const Product& product, int depth);

// Download a tape from Databento's historical API for replay. Returns the
// file's size in bytes.
std::uint64_t fetch_dbn(const Config& cfg, const std::string& api_key, const std::string& start,
                        const std::string& end, const std::string& out_path);

}  // namespace harvester
