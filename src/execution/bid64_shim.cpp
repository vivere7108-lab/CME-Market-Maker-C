// The eight Intel decimal-library entry points the TWS API client links
// against, implemented over ``bid64.hpp``.  Compiled only into the
// ``twsapi`` target, and only when ``HARVESTER_TWS_BID_SHIM`` is on.
#include <cmath>

#include "harvester/execution/bid64.hpp"

namespace {

using harvester::bid64::Bits;

Bits arithmetic(Bits a, Bits b, char op) {
    const long double x = harvester::bid64::to_double(a);
    const long double y = harvester::bid64::to_double(b);
    long double r = 0;
    switch (op) {
        case '+': r = x + y; break;
        case '-': r = x - y; break;
        case '*': r = x * y; break;
        case '/': r = x / y; break;
    }
    if (std::isnan(r)) return harvester::bid64::nan();
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.15Le", r);
    const Bits raw = harvester::bid64::from_string(buffer);
    harvester::bid64::Decoded d = harvester::bid64::decode(raw);
    while (d.coefficient != 0 && d.coefficient % 10 == 0 && d.exponent < harvester::bid64::kMaxExponent) {
        d.coefficient /= 10;
        ++d.exponent;
    }
    return harvester::bid64::encode(d.negative, d.coefficient, d.exponent);
}

}  // namespace

extern "C" {

unsigned long long __bid64_add(unsigned long long a, unsigned long long b, unsigned int, unsigned int* flags) {
    if (flags) *flags = 0;
    return arithmetic(a, b, '+');
}

unsigned long long __bid64_sub(unsigned long long a, unsigned long long b, unsigned int, unsigned int* flags) {
    if (flags) *flags = 0;
    return arithmetic(a, b, '-');
}

unsigned long long __bid64_mul(unsigned long long a, unsigned long long b, unsigned int, unsigned int* flags) {
    if (flags) *flags = 0;
    return arithmetic(a, b, '*');
}

unsigned long long __bid64_div(unsigned long long a, unsigned long long b, unsigned int, unsigned int* flags) {
    if (flags) *flags = 0;
    return arithmetic(a, b, '/');
}

unsigned long long __bid64_from_string(char* text, unsigned int, unsigned int* flags) {
    if (flags) *flags = 0;
    return harvester::bid64::from_string(text);
}

void __bid64_to_string(char* out, unsigned long long value, unsigned int* flags) {
    if (flags) *flags = 0;
    const std::string text = harvester::bid64::to_string(value);
    std::snprintf(out, 64, "%s", text.c_str());
}

double __bid64_to_binary64(unsigned long long value, unsigned int, unsigned int* flags) {
    if (flags) *flags = 0;
    return harvester::bid64::to_double(value);
}

unsigned long long __binary64_to_bid64(double value, unsigned int, unsigned int* flags) {
    if (flags) *flags = 0;
    return harvester::bid64::from_double(value);
}

}  // extern "C"
