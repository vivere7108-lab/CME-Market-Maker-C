#include "harvester/live/journal.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>

#include "harvester/util/log.hpp"

namespace harvester {

namespace {
constexpr const char* kLog = "harvester.live.journal";
}

std::string utc_date(double ts) {
    const auto seconds = static_cast<std::time_t>(std::floor(ts));
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    char buffer[16];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%d", &utc);
    return buffer;
}

SessionJournal::SessionJournal(std::filesystem::path directory, std::optional<std::string> session_date)
    : directory_(std::move(directory)), day_(std::move(session_date)) {
    std::filesystem::create_directories(directory_);
}

SessionJournal::~SessionJournal() {
    for (auto& [_, handle] : handles_) {
        if (handle.file) std::fclose(handle.file);
    }
}

std::FILE* SessionJournal::file_for(const std::string& kind, double ts) {
    const std::string day = day_ ? *day_ : utc_date(ts);
    Handle& handle = handles_[kind];
    if (handle.file && handle.day == day) return handle.file;
    if (handle.file) std::fclose(handle.file);
    const std::filesystem::path path = directory_ / (kind + "-" + day + ".jsonl");
    handle.file = std::fopen(path.c_str(), "a");
    handle.day = day;
    return handle.file;
}

void SessionJournal::record(std::string_view kind, double ts, const Payload& payload) {
    const std::string name(kind);
    std::FILE* file = file_for(name, ts);
    if (file == nullptr) {
        HLOG_ERROR(kLog, "could not write the {} journal ({})", name, std::strerror(errno));
        return;
    }
    writer_ = json::Writer{};
    writer_.begin_object();
    writer_.member("ts", ts);
    payload(writer_);
    writer_.end_object();
    const std::string& line = writer_.str();
    if (std::fwrite(line.data(), 1, line.size(), file) != line.size() || std::fputc('\n', file) == EOF) {
        HLOG_ERROR(kLog, "could not write the {} journal ({})", name, std::strerror(errno));
        return;
    }
    std::fflush(file);
    ++written_[name];
}

std::vector<json::Value> read_journal(const std::filesystem::path& directory, std::string_view kind,
                                      std::optional<std::string> day) {
    std::vector<json::Value> rows;
    std::vector<std::filesystem::path> paths;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return rows;
    const std::string prefix = std::string(kind) + "-";
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(prefix) || !name.ends_with(".jsonl")) continue;
        if (day && name != prefix + *day + ".jsonl") continue;
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        std::ifstream in(path);
        std::string line;
        int number = 0;
        while (std::getline(in, line)) {
            ++number;
            const auto begin = line.find_first_not_of(" \t\r");
            if (begin == std::string::npos) continue;
            try {
                rows.push_back(json::parse(std::string_view(line).substr(begin)));
            } catch (const std::exception&) {
                HLOG_WARNING(kLog, "skipping malformed line {} of {}", number, path.filename().string());
            }
        }
    }
    std::stable_sort(rows.begin(), rows.end(), [](const json::Value& a, const json::Value& b) {
        return a.number("ts").value_or(0.0) < b.number("ts").value_or(0.0);
    });
    return rows;
}

}  // namespace harvester
