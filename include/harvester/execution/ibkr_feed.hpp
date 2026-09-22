// The IBKR book feed: an ``RecordFeed`` fed by IBKR instead of Databento.
//
// It exists so a forward walk can run on the CME Real-Time (P,L2) add-on
// while the Databento session is unavailable. It maintains IBKR's ladder,
// synthesises the ``Mbp10Record`` the builders already read, and pushes it
// through the same ``RecordFeed`` the Databento session uses -- so the
// book, the signals, the gate, the quoter and the journal are untouched
// and unaware.
//
// What is not the same, and matters
// ---------------------------------
// Every one of these makes an IBKR-fed number *not* comparable with a
// Databento-fed or replayed one. None of them is a bug to be fixed here.
//
// 1. **The tape has no aggressor.** MDP 3.0 stamps every trade with the
//    side that crossed; IBKR's tick-by-tick ``AllLast`` does not. The side
//    is inferred with the quote rule -- at or above the ask is a buy, at
//    or below the bid a sell, inside the spread is unknown -- against the
//    touch as it stood before the trade. VPIN is built on exactly this
//    classification, so its levels are an estimate here in a way they are
//    not on MDP 3.0. ``unknown()`` reports the share it could not call;
//    watch it, and set ``toxicity.unknown_side`` knowing it is doing real
//    work rather than catching the occasional oddity.
// 2. **The book is sampled, not streamed.** IBKR maintains a ladder and
//    tells you when a row changed; it does not relay every exchange
//    message. Order flow imbalance and queue depletion are built from book
//    *deltas*, so they see a coarser series than they were measured on.
//    Expect them to be smaller and slower, not merely noisier.
// 3. **The timestamps are local.** Depth updates carry no time at all and
//    trades carry whole seconds, so every record is stamped with its
//    arrival time here. Markouts therefore include IBKR's transport
//    latency, and ``ts_event`` is not an exchange clock.
// 4. **There are no order counts.** ``bid_ct``/``ask_ct`` are zero. Nothing
//    in the strategy reads them today; a signal that starts to would be
//    reading zeroes on this feed.
// 5. **Ten rows is what the add-on gives.** ``book.depth`` above the rows
//    IBKR returns is simply a shorter book, not an error.
//
// Lifecycle: ``start()`` connects its own TWS session (its own client id,
// so a broker reconnect does not drop the feed), qualifies the contract
// and subscribes. ``wait_for_symbol`` returns the local symbol IBKR
// resolved -- the same contract the runner then routes orders to.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "harvester/book/feed.hpp"
#include "harvester/config.hpp"
#include "harvester/execution/market_data.hpp"
#include "harvester/instruments.hpp"

namespace harvester {

// Whether this build has the TWS client compiled in.
bool ibkr_market_data_supported();

class IbkrBookFeed : public RecordFeed, private IbMarketDataListener {
public:
    // ``session`` is owned; ``rows`` is how deep to ask IBKR for.
    IbkrBookFeed(std::unique_ptr<IbMarketData> session, const IBKRConfig& cfg, const Product& product, int depth,
                 int rows);
    ~IbkrBookFeed() override;

    void start() override;
    void close() override;
    bool is_live() const override { return true; }
    std::int64_t errors() const override { return errors_.load(); }
    std::optional<std::string> wait_for_symbol(double timeout_seconds) override;

    // Trades seen, and the share of them the quote rule could not call.
    std::int64_t trades_seen() const { return trades_seen_.load(); }
    std::int64_t trades_unclassified() const { return unclassified_.load(); }
    double unknown() const;

    // Visible for testing: drive the listener by hand.
    IbMarketDataListener& listener() { return *this; }

private:
    struct Row {
        std::int64_t price = 0;  // fixed-point
        std::int32_t size = 0;
    };

    // IbMarketDataListener
    void on_depth(int position, int operation, int side, double price, double size) override;
    void on_trade(double epoch_seconds, double price, double size) override;
    void on_reset(const std::string& reason) override;
    void on_error(long id, int code, const std::string& message) override;

    // Both sides have a row, so a record is worth pushing.
    bool quotable() const;
    // The ladder as one MBP-10 record. ``trade``, when given, fills in the
    // action, price, size and inferred side. Caller holds the ladder lock.
    Mbp10Record build(const Trade* trade);
    // The quote rule: +1 bought, -1 sold, 0 could not tell.
    int classify(double price) const;

    std::unique_ptr<IbMarketData> session_;
    IBKRConfig cfg_;
    const Product& product_;
    int rows_;

    // The ladder, and the touch as it stood before the last change to it.
    mutable std::mutex ladder_mutex_;
    std::vector<Row> bids_;
    std::vector<Row> asks_;
    std::optional<double> prev_bid_;
    std::optional<double> prev_ask_;

    std::atomic<std::int64_t> errors_{0};
    std::atomic<std::int64_t> trades_seen_{0};
    std::atomic<std::int64_t> unclassified_{0};
    std::atomic<std::uint32_t> sequence_{0};
    bool started_ = false;
};

// The live IBKR feed the runner uses. Throws with a readable message when
// the build has no TWS client.
std::shared_ptr<RecordFeed> make_ibkr_feed(const IBKRConfig& cfg, const Product& product, int depth);

}  // namespace harvester
