#include "harvester/util/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace harvester::log {

namespace {
std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::mutex g_mutex;
}  // namespace

void set_level(Level level) { g_level.store(static_cast<int>(level), std::memory_order_relaxed); }

Level level() { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }

const char* level_name(Level level) {
    switch (level) {
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warning: return "WARNING";
        case Level::Error: return "ERROR";
    }
    return "?";
}

void write(Level level, std::string_view logger, std::string_view message) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
    localtime_r(&seconds, &local);
    char stamp[16];
    std::strftime(stamp, sizeof stamp, "%H:%M:%S", &local);
    const std::lock_guard<std::mutex> guard(g_mutex);
    std::fprintf(stderr, "%s %-7s %.*s: %.*s\n", stamp, level_name(level), static_cast<int>(logger.size()),
                 logger.data(), static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
}

}  // namespace harvester::log
