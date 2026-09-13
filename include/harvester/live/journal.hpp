// Append-only record of a session, live or replayed.
//
// JSON Lines, one file per kind per session date, flushed per record:
//
// ``snapshots-YYYY-MM-DD.jsonl``  the decision as taken, at most once per
//                                 ``live.snapshot_seconds``
// ``fills-YYYY-MM-DD.jsonl``      every fill, for reconciliation
// ``markouts-YYYY-MM-DD.jsonl``   every fill again once its horizons elapsed
// ``events-YYYY-MM-DD.jsonl``     regime transitions, halts, pulls, reconnects
//
// ``harvester report`` reads them back.  The files are byte-compatible with
// the Python system's, so either tool reads either journal.
#pragma once

#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "harvester/util/json.hpp"

namespace harvester {

class SessionJournal {
public:
    using Payload = std::function<void(json::Writer&)>;

    explicit SessionJournal(std::filesystem::path directory, std::optional<std::string> session_date = std::nullopt);
    ~SessionJournal();
    SessionJournal(const SessionJournal&) = delete;
    SessionJournal& operator=(const SessionJournal&) = delete;

    // Writes ``{"ts": ts, ...}`` where ``payload`` adds the members.
    void record(std::string_view kind, double ts, const Payload& payload);
    std::map<std::string, std::int64_t> counts() const { return written_; }
    const std::filesystem::path& directory() const { return directory_; }

private:
    struct Handle {
        std::string day;
        std::FILE* file = nullptr;
    };
    std::FILE* file_for(const std::string& kind, double ts);

    std::filesystem::path directory_;
    std::optional<std::string> day_;
    std::map<std::string, Handle> handles_;
    std::map<std::string, std::int64_t> written_;
    json::Writer writer_;
};

// The UTC calendar date of a wall-clock timestamp, ``YYYY-MM-DD``.
std::string utc_date(double ts);

// Every row of one journal kind, ``ts``-sorted; malformed lines are
// skipped with a warning.
std::vector<json::Value> read_journal(const std::filesystem::path& directory, std::string_view kind,
                                      std::optional<std::string> day = std::nullopt);

}  // namespace harvester
