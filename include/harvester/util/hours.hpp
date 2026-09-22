// The quoting hours, in the product's own time zone.
//
// One implementation, used by the live runner and by the replay: a replay
// that quotes through 08:30 when ``quote_start`` is 08:45 is not measuring
// the configuration the live walk runs.  ``session_start`` is the other
// direction -- where to put a generated tape so it lands inside those
// hours rather than wherever the epoch happens to fall.
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "harvester/config.hpp"

namespace harvester {

// The zone by IANA name, or nullptr with one warning if the platform has
// no tzdata; callers then fall back to UTC, which is what the warning says.
const std::chrono::time_zone* find_zone(const std::string& name);

// Monday to Friday, ``start <= local time of day < end``.  ``now`` is
// seconds since the epoch; ``tz`` nullptr means UTC.
bool within_quoting_hours(double now, const std::chrono::time_zone* tz, TimeOfDay start, TimeOfDay end);

// ``ts_event`` in nanoseconds for ``start`` local time on the first
// weekday on or after ``date``, for a generated tape that has to sit
// inside the quoting hours to be quoted at all.
std::int64_t session_start_ns(const std::chrono::time_zone* tz, std::chrono::year_month_day date, TimeOfDay start);

}  // namespace harvester
