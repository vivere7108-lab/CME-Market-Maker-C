#include "harvester/book/book.hpp"

#include <algorithm>

namespace harvester {

double microprice(double bid, double bid_size, double ask, double ask_size) {
    const double total = bid_size + ask_size;
    if (total <= 0) return (bid + ask) / 2.0;
    return (bid * ask_size + ask * bid_size) / total;
}

BookSnapshot::BookSnapshot(std::int64_t ts, std::int64_t seq, std::span<const Level> bid_levels,
                           std::span<const Level> ask_levels, bool is_complete)
    : ts_event(ts), sequence(seq), complete(is_complete) {
    for (const auto& level : bid_levels.first(std::min<std::size_t>(bid_levels.size(), kMaxDepth))) push_bid(level);
    for (const auto& level : ask_levels.first(std::min<std::size_t>(ask_levels.size(), kMaxDepth))) push_ask(level);
}

std::optional<double> BookSnapshot::mid() const {
    if (!two_sided()) return std::nullopt;
    return (bids_[0].price + asks_[0].price) / 2.0;
}

std::optional<double> BookSnapshot::spread() const {
    if (!two_sided()) return std::nullopt;
    return asks_[0].price - bids_[0].price;
}

std::optional<double> BookSnapshot::microprice() const {
    if (!two_sided()) return std::nullopt;
    return harvester::microprice(bids_[0].price, bids_[0].size, asks_[0].price, asks_[0].size);
}

std::optional<double> BookSnapshot::top_imbalance() const {
    if (!two_sided()) return std::nullopt;
    const double b = bids_[0].size;
    const double a = asks_[0].size;
    return (b + a) != 0 ? (b - a) / (b + a) : 0.0;
}

std::int64_t BookSnapshot::depth(int side, int levels) const {
    const auto rows = side > 0 ? bids() : asks();
    const std::size_t n = levels < 0 ? rows.size() : std::min<std::size_t>(rows.size(), static_cast<std::size_t>(levels));
    std::int64_t total = 0;
    for (std::size_t i = 0; i < n; ++i) total += rows[i].size;
    return total;
}

OrderBook::OrderBook() {
    bids_.reserve(256);
    asks_.reserve(256);
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
}

std::size_t OrderBook::position(int side, std::int64_t price, bool& found) const {
    const auto& rows = this->side(side);
    // Bids are kept descending, asks ascending: "better" is earlier.
    const auto it = side > 0 ? std::lower_bound(rows.begin(), rows.end(), price,
                                                [](const PriceLevel& l, std::int64_t p) { return l.price > p; })
                             : std::lower_bound(rows.begin(), rows.end(), price,
                                                [](const PriceLevel& l, std::int64_t p) { return l.price < p; });
    found = it != rows.end() && it->price == price;
    return static_cast<std::size_t>(it - rows.begin());
}

void OrderBook::add(int side, std::int64_t price, std::int32_t size, std::int32_t count) {
    auto& rows = levels(side);
    bool found = false;
    const std::size_t at = position(side, price, found);
    if (found) {
        rows[at].size += size;
        rows[at].count += count;
    } else {
        rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(at), PriceLevel{price, size, count});
    }
}

void OrderBook::remove(int side, std::int64_t price, std::int32_t size, std::int32_t count) {
    auto& rows = levels(side);
    bool found = false;
    const std::size_t at = position(side, price, found);
    if (!found) return;
    rows[at].size -= size;
    rows[at].count -= count;
    if (rows[at].size <= 0 || rows[at].count <= 0) rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(at));
}

void OrderBook::set_level(int side, std::int64_t price, std::int32_t size, std::int32_t count) {
    auto& rows = levels(side);
    bool found = false;
    const std::size_t at = position(side, price, found);
    if (size <= 0) {
        if (found) rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(at));
        return;
    }
    if (found) {
        rows[at].size = size;
        rows[at].count = count;
    } else {
        rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(at), PriceLevel{price, size, count});
    }
}

std::optional<std::int64_t> OrderBook::best(int side) const {
    const auto& rows = this->side(side);
    if (rows.empty()) return std::nullopt;
    return rows.front().price;
}

const OrderBook::PriceLevel* OrderBook::find(int side, std::int64_t price) const {
    bool found = false;
    const std::size_t at = position(side, price, found);
    return found ? &this->side(side)[at] : nullptr;
}

void OrderBook::snapshot_into(int depth, BookSnapshot& out) const {
    out = BookSnapshot{};
    out.ts_event = ts_event;
    out.sequence = sequence;
    out.complete = complete;
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(std::max(depth, 0)), BookSnapshot::kMaxDepth);
    for (std::size_t i = 0; i < n && i < bids_.size(); ++i) {
        out.push_bid(Level{static_cast<double>(bids_[i].price) / static_cast<double>(PRICE_SCALE), bids_[i].size,
                           bids_[i].count});
    }
    for (std::size_t i = 0; i < n && i < asks_.size(); ++i) {
        out.push_ask(Level{static_cast<double>(asks_[i].price) / static_cast<double>(PRICE_SCALE), asks_[i].size,
                           asks_[i].count});
    }
}

BookSnapshot OrderBook::snapshot(int depth) const {
    BookSnapshot out;
    snapshot_into(depth, out);
    return out;
}

}  // namespace harvester
