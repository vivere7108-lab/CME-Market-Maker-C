// Logging: the same four levels and the same line format as the Python
// system's ``logging.basicConfig`` (``HH:MM:SS LEVEL   name: message``), so
// a journalctl of either implementation reads the same way.
//
// Formatting only happens when the level is enabled, so a ``HLOG_DEBUG`` on
// the quote path costs one integer comparison in production.
#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace harvester::log {

enum class Level : int { Debug = 10, Info = 20, Warning = 30, Error = 40 };

void set_level(Level level);
Level level();
inline bool enabled(Level candidate) { return static_cast<int>(candidate) >= static_cast<int>(level()); }
const char* level_name(Level level);
// Writes one line to stderr, timestamped in local time.
void write(Level level, std::string_view logger, std::string_view message);

template <typename... Args>
void emit(Level lvl, std::string_view logger, std::format_string<Args...> fmt, Args&&... args) {
    if (!enabled(lvl)) return;
    write(lvl, logger, std::format(fmt, std::forward<Args>(args)...));
}

}  // namespace harvester::log

#define HLOG_DEBUG(logger, ...) ::harvester::log::emit(::harvester::log::Level::Debug, logger, __VA_ARGS__)
#define HLOG_INFO(logger, ...) ::harvester::log::emit(::harvester::log::Level::Info, logger, __VA_ARGS__)
#define HLOG_WARNING(logger, ...) ::harvester::log::emit(::harvester::log::Level::Warning, logger, __VA_ARGS__)
#define HLOG_ERROR(logger, ...) ::harvester::log::emit(::harvester::log::Level::Error, logger, __VA_ARGS__)
