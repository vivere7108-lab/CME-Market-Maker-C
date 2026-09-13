#include "harvester/book/builder.hpp"

#include <algorithm>

namespace harvester {

MboBuilder::MboBuilder() { orders_.reserve(1 << 16); }

std::optional<Trade> MboBuilder::apply(const MboRecord& record) {
    if (record.flags & F_SNAPSHOT) {
        if (!in_snapshot_) begin_snapshot();
    } else if (in_snapshot_) {
        end_snapshot();
    }

    const char action = record.action;
    const int side = side_of(record.side);
    const std::int64_t price = record.price;
    const auto size = static_cast<std::int32_t>(record.size);
    const std::uint64_t order_id = record.order_id;
    book.ts_event = record.ts_event;
    book.sequence = record.sequence;

    if (action == 'T') {
        return Trade{record.ts_event, static_cast<double>(price) / static_cast<double>(PRICE_SCALE), size, side};
    }
    if (action == 'R') {
        book.clear();
        orders_.clear();
        return std::nullopt;
    }
    if (action == 'N' || side == 0) return std::nullopt;
    if (action == 'A') {
        if (price == UNDEF_PRICE || size <= 0) return std::nullopt;
        if (const auto held = orders_.find(order_id); held != orders_.end()) {
            // A replayed add for an order already held: take the new state
            // rather than double-counting the level.
            book.remove(held->second.side, held->second.price, held->second.size);
            held->second = Order{side, price, size};
        } else {
            orders_.emplace(order_id, Order{side, price, size});
        }
        book.add(side, price, size);
    } else if (action == 'C') {
        const auto held = orders_.find(order_id);
        if (held == orders_.end()) {
            ++dropped;
            return std::nullopt;
        }
        book.remove(held->second.side, held->second.price, held->second.size);
        orders_.erase(held);
    } else if (action == 'M') {
        const auto held = orders_.find(order_id);
        if (held == orders_.end()) {
            // A modify for an order we never saw: treat as an add, which is
            // what the exchange's state is after it either way.
            if (price != UNDEF_PRICE && size > 0) {
                orders_.emplace(order_id, Order{side, price, size});
                book.add(side, price, size);
            }
            ++dropped;
            return std::nullopt;
        }
        book.remove(held->second.side, held->second.price, held->second.size);
        if (size <= 0) {
            orders_.erase(held);
        } else {
            held->second.price = price;
            held->second.size = size;
            book.add(side, price, size);
        }
    } else if (action == 'F') {
        const auto held = orders_.find(order_id);
        if (held == orders_.end()) {
            ++dropped;
            return std::nullopt;
        }
        const std::int32_t filled = std::min(size, held->second.size);
        held->second.size -= filled;
        // The level loses size; it loses an order only when this one is
        // fully done.
        book.remove(held->second.side, held->second.price, filled, 0);
        if (held->second.size <= 0) {
            book.remove(held->second.side, held->second.price, 0, 1);
            orders_.erase(held);
        }
    }
    return std::nullopt;
}

void MboBuilder::begin_snapshot() {
    in_snapshot_ = true;
    book.clear();
    orders_.clear();
    book.complete = false;
}

void MboBuilder::end_snapshot() {
    in_snapshot_ = false;
    book.complete = true;
}

std::int64_t MboBuilder::queue_ahead(int side, std::int64_t price) const {
    const auto* level = book.find(side, price);
    return level ? level->size : 0;
}

std::optional<Trade> Mbp10Builder::apply(const Mbp10Record& record) {
    book.ts_event = record.ts_event;
    book.sequence = record.sequence;
    book.clear();
    for (const auto& pair : record.levels) {
        if (pair.bid_px != UNDEF_PRICE && pair.bid_sz > 0) {
            book.set_level(1, pair.bid_px, static_cast<std::int32_t>(pair.bid_sz), static_cast<std::int32_t>(pair.bid_ct));
        }
        if (pair.ask_px != UNDEF_PRICE && pair.ask_sz > 0) {
            book.set_level(-1, pair.ask_px, static_cast<std::int32_t>(pair.ask_sz), static_cast<std::int32_t>(pair.ask_ct));
        }
    }
    book.complete = true;
    if (record.action == 'T') {
        return Trade{record.ts_event, static_cast<double>(record.price) / static_cast<double>(PRICE_SCALE),
                     static_cast<std::int32_t>(record.size), side_of(record.side)};
    }
    return std::nullopt;
}

std::int64_t Mbp10Builder::queue_ahead(int side, std::int64_t price) const {
    const auto* level = book.find(side, price);
    return level ? level->size : 0;
}

}  // namespace harvester
