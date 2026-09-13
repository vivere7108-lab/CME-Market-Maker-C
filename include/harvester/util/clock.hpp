// Two clocks, the same two the Python system used: wall time for anything
// that is recorded (fills, the journal) and a monotonic clock for anything
// that is measured (the budget, the ack timeout, the feed's age).
#pragma once

#include <chrono>
#include <functional>

namespace harvester {

using ClockFn = std::function<double()>;

inline double wall_now() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

inline double mono_now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace harvester
