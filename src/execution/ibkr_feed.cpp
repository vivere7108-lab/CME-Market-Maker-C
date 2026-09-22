#include "harvester/execution/ibkr_feed.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>

#include "harvester/util/clock.hpp"
#include "harvester/util/log.hpp"

namespace harvester {

namespace {
constexpr const char* kLog = "harvester.book.ibkr";

// IBKR's own encodings, kept at the edge.
constexpr int OP_INSERT = 0;
constexpr int OP_UPDATE = 1;
constexpr int OP_DELETE = 2;
constexpr int SIDE_ASK = 0;
constexpr int SIDE_BID = 1;

std::int64_t now_ns() { return static_cast<std::int64_t>(wall_now() * 1e9); }

}  // namespace

IbkrBookFeed::IbkrBookFeed(std::unique_ptr<IbMarketData> session, const IBKRConfig& cfg, const Product& product,
                           int depth, int rows)
    : RecordFeed(Schema::Mbp10, depth), session_(std::move(session)), cfg_(cfg), product_(product),
      rows_(std::max(1, rows)) {
    session_->set_listener(this);
}

IbkrBookFeed::~IbkrBookFeed() { IbkrBookFeed::close(); }

void IbkrBookFeed::start() {
    const int client_id = cfg_.market_data_client_id > 0 ? cfg_.market_data_client_id : cfg_.client_id + 1;
    session_->connect(cfg_.host, cfg_.port, client_id, cfg_.connect_timeout);
    // The same resolution the router uses, so the contract the book is
    // built from is the contract the orders go to.
    const IbContract contract = qualify_front_contract(
        product_, cfg_.local_symbol, [this](const IbContract& query) { return session_->qualify(query); });
    if (!contract.local_symbol.empty()) set_symbol(contract.local_symbol);
    session_->subscribe(contract, rows_);
    started_ = true;
    HLOG_INFO(kLog, "IBKR market data: {} (conId {}) on client id {}, {} rows of depth",
              contract.local_symbol.empty() ? "?" : contract.local_symbol, contract.con_id, client_id, rows_);
}

void IbkrBookFeed::close() {
    if (!started_) return;
    started_ = false;
    try {
        session_->unsubscribe();
    } catch (const std::exception& exc) {  // shutting down
        HLOG_WARNING(kLog, "IBKR market data did not unsubscribe cleanly ({})", exc.what());
    }
    session_->disconnect();
}

std::optional<std::string> IbkrBookFeed::wait_for_symbol(double /*timeout_seconds*/) {
    // ``start()`` qualifies synchronously, so there is nothing to wait for.
    return raw_symbol();
}

double IbkrBookFeed::unknown() const {
    const std::int64_t seen = trades_seen_.load();
    return seen > 0 ? static_cast<double>(unclassified_.load()) / static_cast<double>(seen) : 0.0;
}

bool IbkrBookFeed::quotable() const { return !bids_.empty() && !asks_.empty(); }

void IbkrBookFeed::on_depth(int position, int operation, int side, double price, double size) {
    if (position < 0) return;
    bool push_it = false;
    Mbp10Record record;
    {
        const std::lock_guard<std::mutex> guard(ladder_mutex_);
        // The touch as it stood *before* this change, so a trade that
        // arrives after the level it hit has already been removed can
        // still be classified against the book it actually crossed.
        if (!bids_.empty()) prev_bid_ = product_.price(bids_.front().price);
        if (!asks_.empty()) prev_ask_ = product_.price(asks_.front().price);

        std::vector<Row>& rows = side == SIDE_BID ? bids_ : asks_;
        const auto at = static_cast<std::size_t>(position);
        const Row row{product_.fixed(price), static_cast<std::int32_t>(size)};
        switch (operation) {
            case OP_INSERT:
                if (at > rows.size()) return;  // a row we never saw; the ladder is out of step
                rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(at), row);
                break;
            case OP_UPDATE:
                if (at >= rows.size()) {
                    if (at > rows.size()) return;
                    rows.push_back(row);  // an update to the row past the end is an insert
                } else {
                    rows[at] = row;
                }
                break;
            case OP_DELETE:
                if (at >= rows.size()) return;
                rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(at));
                break;
            default: return;
        }
        if (rows.size() > static_cast<std::size_t>(rows_)) rows.resize(static_cast<std::size_t>(rows_));
        // A zero-size row is a level that is gone; IBKR usually deletes it
        // but does not always.
        std::erase_if(rows, [](const Row& r) { return r.size <= 0; });
        if (quotable()) {
            record = build(nullptr);
            push_it = true;
        }
    }
    if (push_it) push(record);
}

void IbkrBookFeed::on_trade(double /*epoch_seconds*/, double price, double size) {
    if (size <= 0) return;
    ++trades_seen_;
    Mbp10Record record;
    bool push_it = false;
    {
        const std::lock_guard<std::mutex> guard(ladder_mutex_);
        if (!quotable()) return;  // nothing to classify against yet
        const int aggressor = classify(price);
        if (aggressor == 0) ++unclassified_;
        Trade trade;
        trade.price = price;
        trade.size = static_cast<std::int32_t>(size);
        trade.aggressor = aggressor;
        record = build(&trade);
        push_it = true;
    }
    if (push_it) push(record);
}

// The quote rule, against the touch now and then against the touch before
// the last book change. IBKR often removes the level a sweep took before
// it reports the trade that took it, which lands the price inside the new
// spread; the second look catches exactly that case.
int IbkrBookFeed::classify(double price) const {
    const double eps = product_.tick_size / 4.0;
    const double bid = product_.price(bids_.front().price);
    const double ask = product_.price(asks_.front().price);
    if (price >= ask - eps) return 1;
    if (price <= bid + eps) return -1;
    if (prev_ask_ && price >= *prev_ask_ - eps) return 1;
    if (prev_bid_ && price <= *prev_bid_ + eps) return -1;
    return 0;
}

// Caller holds ``ladder_mutex_``.
Mbp10Record IbkrBookFeed::build(const Trade* trade) {
    Mbp10Record out;
    out.ts_event = now_ns();
    out.sequence = ++sequence_;
    out.flags = F_MBP;
    for (std::size_t i = 0; i < out.levels.size(); ++i) {
        if (i < bids_.size()) {
            out.levels[i].bid_px = bids_[i].price;
            out.levels[i].bid_sz = static_cast<std::uint32_t>(bids_[i].size);
        }
        if (i < asks_.size()) {
            out.levels[i].ask_px = asks_[i].price;
            out.levels[i].ask_sz = static_cast<std::uint32_t>(asks_[i].size);
        }
        // bid_ct / ask_ct stay 0: IBKR does not report order counts.
    }
    if (trade != nullptr) {
        out.action = 'T';
        out.price = product_.fixed(trade->price);
        out.size = static_cast<std::uint32_t>(trade->size);
        out.side = trade->aggressor > 0 ? 'B' : (trade->aggressor < 0 ? 'A' : 'N');
    }
    return out;
}

void IbkrBookFeed::on_reset(const std::string& reason) {
    HLOG_WARNING(kLog, "IBKR depth reset ({}); the book is dropped until it has been rebuilt", reason);
    Mbp10Record empty;
    {
        const std::lock_guard<std::mutex> guard(ladder_mutex_);
        bids_.clear();
        asks_.clear();
        prev_bid_.reset();
        prev_ask_.reset();
        // Clearing the ladder is not enough: the builder holds its own
        // copy, and a stale two-sided book is worse than no book -- the
        // risk monitor's staleness check would not see it, because records
        // are still arriving.
        empty = build(nullptr);
    }
    push(empty);
}

void IbkrBookFeed::on_error(long id, int code, const std::string& message) {
    ++errors_;
    HLOG_ERROR(kLog, "IBKR market data error {} (id {}): {}", code, id, message);
}

}  // namespace harvester
