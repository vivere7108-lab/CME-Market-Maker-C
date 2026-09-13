#include "harvester/util/format.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cctype>

namespace harvester::fmt {

std::string commas(double value, int decimals) {
    std::string plain = std::format("{:.{}f}", value, decimals);
    // "-0" is what Python prints for a tiny negative rounded to zero; keep it.
    const std::size_t sign = (!plain.empty() && plain[0] == '-') ? 1 : 0;
    const std::size_t dot = plain.find('.');
    const std::size_t int_end = dot == std::string::npos ? plain.size() : dot;
    std::string out = plain.substr(0, sign);
    const std::size_t digits = int_end - sign;
    for (std::size_t i = 0; i < digits; ++i) {
        if (i > 0 && (digits - i) % 3 == 0) out += ',';
        out += plain[sign + i];
    }
    out += plain.substr(int_end);
    return out;
}

std::string repr(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value > 0 ? "inf" : "-inf";
    // The shortest digits that round-trip, then laid out the way CPython's
    // ``repr`` lays them out: fixed notation for decimal exponents in
    // [-4, 16), scientific with a two-digit exponent otherwise.
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value, std::chars_format::scientific);
    std::string sci(buffer, result.ptr);
    const bool negative = sci[0] == '-';
    if (negative) sci.erase(0, 1);
    const std::size_t e = sci.find('e');
    std::string digits = sci.substr(0, e);
    const int exponent = std::stoi(sci.substr(e + 1));
    if (const auto dot = digits.find('.'); dot != std::string::npos) digits.erase(dot, 1);
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string out = negative ? "-" : "";
    if (exponent >= -4 && exponent < 16) {
        if (exponent < 0) {
            out += "0." + std::string(static_cast<std::size_t>(-exponent - 1), '0') + digits;
        } else if (static_cast<std::size_t>(exponent) + 1 >= digits.size()) {
            out += digits + std::string(static_cast<std::size_t>(exponent) + 1 - digits.size(), '0') + ".0";
        } else {
            out += digits.substr(0, static_cast<std::size_t>(exponent) + 1) + "." + digits.substr(static_cast<std::size_t>(exponent) + 1);
        }
    } else {
        out += digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        out += std::format("e{}{:02d}", exponent < 0 ? "-" : "+", std::abs(exponent));
    }
    return out;
}

}  // namespace harvester::fmt
