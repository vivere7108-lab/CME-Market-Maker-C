// A value per timestamp, read beside the tape.
//
// This exists for measurement, not for production.  A rule fitted offline
// -- a latent state, a price-impact estimate, anything that needs an hour
// of history and a matrix to produce -- can be written out as a CSV of
// ``ts_ns,value`` and then gated on by the real engine, with the real
// queue, the real inventory cap and the real loss limit, before anyone
// writes a line of it in C++.  If the rule does not earn its keep through
// this hook it will not earn it implemented properly either.
//
// The lookup is "as it stood at or before the book's clock", so a gate
// read this way is never ahead of the tape.  The series' own sampling
// interval is therefore the staleness of the gate, and a fair comparison
// between two rules reads both of them through here.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace harvester {

class ExternalSeries {
public:
    ExternalSeries() = default;
    explicit ExternalSeries(const std::string& path);

    bool empty() const { return ts_.empty(); }
    std::size_t size() const { return ts_.size(); }
    // The last value stamped at or before ``ts_ns``; none before the first.
    std::optional<double> at(std::int64_t ts_ns) const;

private:
    std::vector<std::int64_t> ts_;
    std::vector<double> value_;
};

}  // namespace harvester
