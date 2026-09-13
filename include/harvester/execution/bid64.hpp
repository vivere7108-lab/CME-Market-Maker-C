// IEEE 754-2008 decimal64 in the binary-integer-significand encoding
// (BID64), which is what the TWS API's ``Decimal`` is.
//
// The TWS client converts sizes to and from this type through Intel's
// decimal library, which IBKR does not ship and which has to be built
// from source.  The client itself only ever needs a string parsed and a
// value printed, and the adapter only ever needs a double in and out, so
// this is that: encode, decode, parse, print, on finite values with up to
// sixteen digits, rounding to nearest-even where sixteen do not suffice.
// ``src/execution/bid64_shim.cpp`` exposes it under the names the client
// links against.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace harvester::bid64 {

using Bits = std::uint64_t;

inline constexpr int kBias = 398;
inline constexpr int kMaxExponent = 369;
inline constexpr int kMinExponent = -398;
inline constexpr std::uint64_t kMaxCoefficient = 9'999'999'999'999'999ULL;  // 10^16 - 1
inline constexpr Bits kQuietNaN = 0x7c00000000000000ULL;
inline constexpr Bits kInfinity = 0x7800000000000000ULL;

struct Decoded {
    bool negative = false;
    bool nan = false;
    bool inf = false;
    std::uint64_t coefficient = 0;
    int exponent = 0;  // unbiased: value = coefficient * 10^exponent
};

inline Decoded decode(Bits bits) {
    Decoded d;
    d.negative = (bits >> 63) & 1;
    const unsigned top5 = static_cast<unsigned>((bits >> 58) & 0x1f);
    if (top5 == 0x1f) {
        d.nan = true;
        return d;
    }
    if (top5 == 0x1e) {
        d.inf = true;
        return d;
    }
    if (((bits >> 61) & 0x3) == 0x3) {
        d.exponent = static_cast<int>((bits >> 51) & 0x3ff) - kBias;
        d.coefficient = (bits & ((1ULL << 51) - 1)) | (1ULL << 53);
        if (d.coefficient > kMaxCoefficient) d.coefficient = 0;  // non-canonical: zero
    } else {
        d.exponent = static_cast<int>((bits >> 53) & 0x3ff) - kBias;
        d.coefficient = bits & ((1ULL << 53) - 1);
    }
    return d;
}

// Rounds a coefficient with more than sixteen digits down to sixteen,
// nearest-even, adjusting the exponent.
inline void normalise(unsigned __int128& coefficient, int& exponent) {
    while (coefficient > kMaxCoefficient) {
        const unsigned digit = static_cast<unsigned>(coefficient % 10);
        coefficient /= 10;
        ++exponent;
        if (digit > 5 || (digit == 5 && (coefficient % 2) == 1)) ++coefficient;
    }
}

inline Bits encode(bool negative, unsigned __int128 coefficient, int exponent) {
    normalise(coefficient, exponent);
    // Bring the exponent into range by trading zeros.
    while (exponent > kMaxExponent && coefficient * 10 <= kMaxCoefficient) {
        coefficient *= 10;
        --exponent;
    }
    while (exponent < kMinExponent) {
        const unsigned digit = static_cast<unsigned>(coefficient % 10);
        coefficient /= 10;
        ++exponent;
        if (digit > 5 || (digit == 5 && (coefficient % 2) == 1)) ++coefficient;
        if (coefficient == 0) {
            exponent = kMinExponent;
            break;
        }
    }
    if (exponent > kMaxExponent) return (negative ? (1ULL << 63) : 0) | kInfinity;
    const auto c = static_cast<std::uint64_t>(coefficient);
    const auto biased = static_cast<std::uint64_t>(exponent + kBias);
    Bits bits = negative ? (1ULL << 63) : 0;
    if (c < (1ULL << 53)) {
        bits |= (biased << 53) | c;
    } else {
        bits |= (3ULL << 61) | (biased << 51) | (c & ((1ULL << 51) - 1));
    }
    return bits;
}

inline Bits nan() { return kQuietNaN; }

inline double to_double(Bits bits) {
    const Decoded d = decode(bits);
    if (d.nan) return std::numeric_limits<double>::quiet_NaN();
    if (d.inf) return d.negative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
    double value;
    if (d.exponent >= 0 && d.exponent <= 22) {
        value = static_cast<double>(d.coefficient) * std::pow(10.0, d.exponent);
    } else if (d.exponent < 0 && d.exponent >= -22) {
        value = static_cast<double>(d.coefficient) / std::pow(10.0, -d.exponent);
    } else {
        value = static_cast<double>(d.coefficient) * std::pow(10.0, d.exponent);
    }
    return d.negative ? -value : value;
}

// Parses ``[+-]digits[.digits][(e|E)[+-]digits]``, ``NaN``, ``Inf``.
// Anything else is NaN, which is what Intel's parser returns and what the
// TWS client relies on for its "unset" sentinels.
inline Bits from_string(const char* text) {
    if (text == nullptr) return kQuietNaN;
    while (*text == ' ' || *text == '\t') ++text;
    bool negative = false;
    if (*text == '+' || *text == '-') {
        negative = *text == '-';
        ++text;
    }
    if (std::strncmp(text, "NaN", 3) == 0 || std::strncmp(text, "nan", 3) == 0 || std::strncmp(text, "SNaN", 4) == 0) {
        return (negative ? (1ULL << 63) : 0) | kQuietNaN;
    }
    if (std::strncmp(text, "Inf", 3) == 0 || std::strncmp(text, "inf", 3) == 0) {
        return (negative ? (1ULL << 63) : 0) | kInfinity;
    }
    unsigned __int128 coefficient = 0;
    int exponent = 0;
    int digits = 0;
    bool seen_point = false;
    bool any = false;
    for (; *text; ++text) {
        const char c = *text;
        if (c >= '0' && c <= '9') {
            any = true;
            if (digits < 34) {
                coefficient = coefficient * 10 + static_cast<unsigned>(c - '0');
                if (coefficient != 0) ++digits;
                if (seen_point) --exponent;
            } else if (!seen_point) {
                ++exponent;  // beyond what we keep: a power of ten
            }
        } else if (c == '.' && !seen_point) {
            seen_point = true;
        } else {
            break;
        }
    }
    if (!any) return kQuietNaN;
    if (*text == 'e' || *text == 'E') {
        ++text;
        bool exp_negative = false;
        if (*text == '+' || *text == '-') {
            exp_negative = *text == '-';
            ++text;
        }
        if (*text < '0' || *text > '9') return kQuietNaN;
        int e = 0;
        for (; *text >= '0' && *text <= '9'; ++text) {
            if (e < 100000) e = e * 10 + (*text - '0');
        }
        exponent += exp_negative ? -e : e;
    }
    while (*text == ' ' || *text == '\t') ++text;
    if (*text != '\0') return kQuietNaN;
    if (coefficient == 0) exponent = 0;
    return encode(negative, coefficient, exponent);
}

// Intel's format: sign, coefficient digits, ``E``, signed exponent
// (``+25E-1``); ``+NaN``, ``+Inf``.
inline std::string to_string(Bits bits) {
    const Decoded d = decode(bits);
    std::string out = d.negative ? "-" : "+";
    if (d.nan) return out + "NaN";
    if (d.inf) return out + "Inf";
    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%lluE%+d", static_cast<unsigned long long>(d.coefficient), d.exponent);
    return out + buffer;
}

// The nearest decimal64 to a double, with trailing zeros dropped so an
// integer size prints as an integer.
inline Bits from_double(double value) {
    if (std::isnan(value)) return kQuietNaN;
    if (std::isinf(value)) return (value < 0 ? (1ULL << 63) : 0) | kInfinity;
    if (value == 0.0) return std::signbit(value) ? (1ULL << 63) | encode(false, 0, 0) : encode(false, 0, 0);
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.15e", value);  // sixteen significant digits
    const Bits raw = from_string(buffer);
    Decoded d = decode(raw);
    while (d.coefficient != 0 && d.coefficient % 10 == 0 && d.exponent < kMaxExponent) {
        d.coefficient /= 10;
        ++d.exponent;
    }
    return encode(d.negative, d.coefficient, d.exponent);
}

}  // namespace harvester::bid64
