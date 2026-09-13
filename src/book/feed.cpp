#include "harvester/book/feed.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace harvester {

Schema schema_from_string(const std::string& name) {
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    if (key == "mbo") return Schema::Mbo;
    if (key == "mbp-10") return Schema::Mbp10;
    throw std::invalid_argument("no book builder for schema '" + name + "'");
}

const char* to_string(Schema schema) { return schema == Schema::Mbo ? "mbo" : "mbp-10"; }

RecordFeed::RecordFeed(Schema schema, int depth, ClockFn clock)
    : schema_(schema), depth_(depth), clock_(std::move(clock)) {}

RecordFeed::RecordFeed(const std::string& schema, int depth, ClockFn clock)
    : RecordFeed(schema_from_string(schema), depth, std::move(clock)) {}

std::optional<std::string> RecordFeed::raw_symbol() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return raw_symbol_;
}

void RecordFeed::set_symbol(std::string raw) {
    const std::lock_guard<std::mutex> guard(mutex_);
    raw_symbol_ = std::move(raw);
}

template <typename Record>
std::optional<Trade> RecordFeed::push_locked(const Record& record) {
    std::optional<Trade> trade;
    if constexpr (std::is_same_v<Record, MboRecord>) {
        if (schema_ != Schema::Mbo) throw std::logic_error("an MBO record was pushed into an MBP-10 feed");
        trade = mbo_.apply(record);
    } else {
        if (schema_ != Schema::Mbp10) throw std::logic_error("an MBP-10 record was pushed into an MBO feed");
        trade = mbp10_.apply(record);
    }
    ++records_;
    last_update_ = clock_();
    if (trade) {
        if (trades_.size() >= MAX_TRADES) {
            trades_.pop_front();
            ++dropped_trades_;
        }
        trades_.push_back(*trade);
    }
    return trade;
}

std::optional<Trade> RecordFeed::push(const MboRecord& record) {
    const std::lock_guard<std::mutex> guard(mutex_);
    return push_locked(record);
}

std::optional<Trade> RecordFeed::push(const Mbp10Record& record) {
    const std::lock_guard<std::mutex> guard(mutex_);
    return push_locked(record);
}

BookSnapshot RecordFeed::snapshot() const {
    BookSnapshot out;
    snapshot_into(out);
    return out;
}

void RecordFeed::snapshot_into(BookSnapshot& out) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    book().snapshot_into(depth_, out);
}

std::vector<Trade> RecordFeed::take_trades() {
    std::vector<Trade> rows;
    take_trades_into(rows);
    return rows;
}

void RecordFeed::take_trades_into(std::vector<Trade>& out) {
    out.clear();
    const std::lock_guard<std::mutex> guard(mutex_);
    out.insert(out.end(), trades_.begin(), trades_.end());
    trades_.clear();
}

double RecordFeed::age_seconds(double now) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!last_update_) return std::numeric_limits<double>::infinity();
    return std::max(now - *last_update_, 0.0);
}

std::int64_t RecordFeed::queue_ahead(int side, std::int64_t price) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return schema_ == Schema::Mbo ? mbo_.queue_ahead(side, price) : mbp10_.queue_ahead(side, price);
}

std::int64_t RecordFeed::records() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return records_;
}

std::int64_t RecordFeed::dropped_trades() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return dropped_trades_;
}

bool RecordFeed::complete() const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return book().complete;
}

}  // namespace harvester
