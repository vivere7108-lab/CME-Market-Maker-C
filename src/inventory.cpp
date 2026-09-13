#include "harvester/inventory.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "harvester/util/format.hpp"

namespace harvester {

const char* to_string(OrderEvent::Kind kind) {
    switch (kind) {
        case OrderEvent::Kind::Ack: return "ack";
        case OrderEvent::Kind::Fill: return "fill";
        case OrderEvent::Kind::Cancelled: return "cancelled";
        case OrderEvent::Kind::Rejected: return "rejected";
    }
    return "?";
}

double Inventory::apply(const Fill& fill) {
    double realised_now = 0.0;
    const auto q = static_cast<double>(fill.signed_size());
    const double m = product_.multiplier;
    if (position == 0 || (position > 0) == (q > 0)) {
        const double total = position + q;
        avg_price = total != 0 ? (avg_price * position + fill.price * q) / total : 0.0;
        position = static_cast<int>(total);
    } else {
        const double closing = std::min(std::fabs(q), std::fabs(static_cast<double>(position)));
        const int direction = position > 0 ? 1 : -1;
        realised_now = closing * direction * (fill.price - avg_price) * m;
        realised += realised_now;
        position += static_cast<int>(q);
        if (position == 0) {
            avg_price = 0.0;
        } else if ((position > 0) != (direction > 0)) {
            // Flipped through zero: the excess opens at the fill price.
            avg_price = fill.price;
        }
    }
    fees += fill.fees;
    fills.push_back(fill);
    contracts_traded += fill.size;
    return realised_now;
}

void Inventory::adopt(int new_position, double new_avg_price) {
    position = new_position;
    avg_price = new_position != 0 ? new_avg_price : 0.0;
}

double Inventory::unrealised(std::optional<double> mark) const {
    if (!mark || position == 0) return 0.0;
    return position * (*mark - avg_price) * product_.multiplier;
}

double Inventory::total_pnl(std::optional<double> mark) const { return realised + unrealised(mark) - fees; }

std::string Inventory::describe(std::optional<double> mark) const {
    return std::format("pos {:+d} @ {:.2f} | realised ${} unrealised ${} fees ${} net ${} | {} fills", position,
                       avg_price, fmt::commas(realised), fmt::commas(unrealised(mark)), fmt::commas(fees),
                       fmt::commas(total_pnl(mark)), fills.size());
}

bool MarkoutRow::complete() const {
    return std::all_of(markouts.begin(), markouts.end(), [](const auto& m) { return m.has_value(); });
}

MarkoutTracker::MarkoutTracker(const Product& product, std::vector<double> horizons)
    : product_(product), horizons_(std::move(horizons)) {}

std::string MarkoutTracker::horizon_key(std::size_t index) const { return std::format("{:g}s", horizons_[index]); }

MarkoutRow& MarkoutTracker::record(const Fill& fill, std::optional<double> anchor, ToxicityLevel level, double now) {
    const double anchor_value = anchor ? *anchor : fill.price;
    MarkoutRow row;
    row.ts = now;
    row.side = fill.side;
    row.price = fill.price;
    row.size = fill.size;
    row.level = level;
    row.anchor_at_fill = anchor_value;
    row.markouts.assign(horizons_.size(), std::nullopt);
    row.edge_at_fill = fill.side * (anchor_value - fill.price) * product_.multiplier;
    rows.push_back(std::move(row));
    pending_.push_back(rows.size() - 1);
    return rows.back();
}

std::vector<const MarkoutRow*> MarkoutTracker::update(double now, std::optional<double> anchor) {
    std::vector<const MarkoutRow*> completed;
    if (!anchor) return completed;
    std::vector<std::size_t> still_pending;
    still_pending.reserve(pending_.size());
    for (const std::size_t index : pending_) {
        MarkoutRow& row = rows[index];
        for (std::size_t h = 0; h < horizons_.size(); ++h) {
            if (!row.markouts[h] && now - row.ts >= horizons_[h]) {
                row.markouts[h] = row.side * (*anchor - row.price) * product_.multiplier;
            }
        }
        if (row.complete()) {
            completed.push_back(&row);
        } else {
            still_pending.push_back(index);
        }
    }
    pending_.swap(still_pending);
    return completed;
}

std::map<std::string, MarkoutSummary> MarkoutTracker::summary() const {
    std::map<std::string, MarkoutSummary> out;
    std::map<std::string, std::map<std::string, double>> counts;
    for (const auto& row : rows) {
        const std::string level = to_string(row.level);
        MarkoutSummary& slot = out[level];
        slot.fills += 1;
        slot.contracts += row.size;
        slot.edge += row.edge_at_fill * row.size;
        for (std::size_t h = 0; h < horizons_.size(); ++h) {
            if (!row.markouts[h]) continue;
            const std::string key = horizon_key(h);
            slot.markouts[key] += *row.markouts[h] * row.size;
            counts[level][key] += row.size;
        }
    }
    for (auto& [level, slot] : out) {
        const double n = slot.contracts != 0 ? slot.contracts : 1;
        slot.edge /= n;
        for (auto& [key, total] : slot.markouts) {
            const double count = counts[level][key] != 0 ? counts[level][key] : 1;
            total /= count;
        }
    }
    return out;
}

std::string MarkoutTracker::describe() const {
    const auto s = summary();
    if (s.empty()) return "no fills";
    std::string out = "markouts, $ per contract (positive is in the quote's favour):";
    for (const char* level : TOXICITY_LEVELS) {
        const auto it = s.find(level);
        if (it == s.end()) continue;
        const MarkoutSummary& slot = it->second;
        std::string marks;
        for (std::size_t h = 0; h < horizons_.size(); ++h) {
            const std::string key = horizon_key(h);
            const auto m = slot.markouts.find(key);
            const double value = m == slot.markouts.end() ? std::nan("") : m->second;
            if (!marks.empty()) marks += "  ";
            marks += std::format("{} {:+.2f}", key, value);
        }
        out += std::format("\n  {:<9} {:4d} fills {:5d} ct  edge {:+.2f}  {}", level, static_cast<int>(slot.fills),
                           static_cast<int>(slot.contracts), slot.edge, marks);
    }
    return out;
}

}  // namespace harvester
