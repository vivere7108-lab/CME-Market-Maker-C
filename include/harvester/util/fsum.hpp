// Summation that matches CPython's, for the numbers this system compares
// against the Python implementation.
//
// CPython 3.12 changed the builtin ``sum()`` to Neumaier compensated
// summation for floats (gh-100425). A plain accumulation loop -- the obvious
// translation of ``sum(xs)`` -- stopped being equivalent at that release, and
// the difference is one ulp on ordinary inputs. That is normally beneath
// notice; it is not beneath notice here, because VPIN is ranked against its
// own history and compared to thresholds, so a reading sitting exactly on a
// boundary lands on either side of it depending on the rounding, and the gate
// then reports a regime change that the other implementation does not.
//
// So anywhere a Python ``sum()`` over floats is being mirrored, use this.
// Integer sums need none of it: Python's are exact.
#pragma once

#include <cmath>

namespace harvester {

// Neumaier summation, the variant CPython uses: a running compensation term
// captures the low-order bits lost whenever the running total and the next
// term differ in magnitude, in either direction.
template <typename Range>
double fsum(const Range& values) {
    double total = 0.0;
    double compensation = 0.0;
    for (const double value : values) {
        const double running = total + value;
        if (std::fabs(total) >= std::fabs(value)) {
            compensation += (total - running) + value;
        } else {
            compensation += (value - running) + total;
        }
        total = running;
    }
    return total + compensation;
}

}  // namespace harvester
