#include "harvester/util/hours.hpp"

#include "harvester/util/log.hpp"

namespace harvester {

namespace {
constexpr const char* kLog = "harvester.hours";
}  // namespace

const std::chrono::time_zone* find_zone(const std::string& name) {
    try {
        return std::chrono::locate_zone(name);
    } catch (const std::exception& exc) {
        HLOG_WARNING(kLog, "time zone {} is not available ({}); quoting hours are checked in UTC", name, exc.what());
        return nullptr;
    }
}

namespace {

// The wall clock in ``tz`` at ``now`` seconds since the epoch.
std::chrono::local_time<std::chrono::microseconds> local_at(double now, const std::chrono::time_zone* tz) {
    using namespace std::chrono;
    const sys_time<microseconds> at{microseconds{static_cast<std::int64_t>(now * 1e6)}};
    if (tz != nullptr) return tz->to_local(at);
    return local_time<microseconds>{at.time_since_epoch()};
}

}  // namespace

bool within_quoting_hours(double now, const std::chrono::time_zone* tz, TimeOfDay start, TimeOfDay end) {
    using namespace std::chrono;
    const auto local = local_at(now, tz);
    const auto day = floor<days>(local);
    const weekday wd{sys_days{day.time_since_epoch()}};
    if (wd.iso_encoding() >= 6) return false;  // Saturday, Sunday
    const auto since_midnight = duration_cast<seconds>(local - day).count();
    return start.seconds() <= since_midnight && since_midnight < end.seconds();
}

std::int64_t session_start_ns(const std::chrono::time_zone* tz, std::chrono::year_month_day date, TimeOfDay start) {
    using namespace std::chrono;
    sys_days day{date};
    while (weekday{day}.iso_encoding() >= 6) day += days{1};
    const local_time<seconds> local{day.time_since_epoch() + seconds{start.seconds()}};
    const sys_time<seconds> at = tz != nullptr ? tz->to_sys(local) : sys_time<seconds>{local.time_since_epoch()};
    return at.time_since_epoch().count() * 1'000'000'000LL;
}

}  // namespace harvester
