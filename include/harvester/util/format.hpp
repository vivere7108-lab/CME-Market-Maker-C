// The handful of Python format codes the reports and logs use, reproduced
// so the two implementations print the same numbers the same way.
#pragma once

#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace harvester::fmt {

// Python ``f"{value:,.{decimals}f}"``: fixed decimals, thousands separators.
std::string commas(double value, int decimals = 0);

// Python ``f"{value:+d}"``.
inline std::string signed_int(std::int64_t value) { return std::format("{:+d}", value); }

// Python ``f"{value:.0%}"``.
inline std::string pct0(double value) { return std::format("{:.0f}%", value * 100.0); }

// Python ``f"{value:g}"``.
inline std::string g(double value) { return std::format("{:g}", value); }

// Python ``repr(float)``: the shortest round-trip form, always with a
// fractional part or an exponent so a reader can tell it from an int.
std::string repr(double value);

}  // namespace harvester::fmt
