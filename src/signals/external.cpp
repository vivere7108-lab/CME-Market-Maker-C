#include "harvester/signals/external.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <stdexcept>

namespace harvester {

ExternalSeries::ExternalSeries(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::invalid_argument(std::format("cannot read risk.external_file {}", path));
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t comma = line.find(',');
        if (comma == std::string::npos) continue;
        try {
            const std::int64_t ts = std::stoll(line.substr(0, comma));
            const double value = std::stod(line.substr(comma + 1));
            if (!ts_.empty() && ts < ts_.back()) {
                throw std::invalid_argument(std::format("{} is not sorted by timestamp", path));
            }
            ts_.push_back(ts);
            value_.push_back(value);
        } catch (const std::invalid_argument&) {
            if (ts_.empty()) continue;  // a header
            throw;
        } catch (const std::out_of_range&) {
            continue;
        }
    }
    if (ts_.empty()) throw std::invalid_argument(std::format("{} holds no ts,value rows", path));
}

std::optional<double> ExternalSeries::at(std::int64_t ts_ns) const {
    const auto it = std::upper_bound(ts_.begin(), ts_.end(), ts_ns);
    if (it == ts_.begin()) return std::nullopt;
    return value_[static_cast<std::size_t>(it - ts_.begin()) - 1];
}

}  // namespace harvester
