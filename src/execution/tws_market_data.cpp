// The IBKR market-data session: depth and tick-by-tick trades off their
// own socket.
//
// A second TWS client, on its own client id, so that a broker reconnect
// does not take the book down and a stalled book does not take the orders
// down. Same thread shape as ``TwsGateway``: the API's ``EReader`` reads
// the socket, a pump thread dispatches, and every callback below runs on
// that pump thread.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "harvester/execution/ibkr_feed.hpp"
#include "harvester/execution/market_data.hpp"
#include "harvester/util/log.hpp"

#include "Contract.h"
#include "Decimal.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"

namespace harvester {

namespace {
constexpr const char* kLog = "harvester.book.ibkr.tws";
constexpr int kDepthReqId = 9001;
constexpr int kTradesReqId = 9002;
constexpr int kContractReqId = 9003;
constexpr double kRequestTimeout = 15.0;

Contract to_tws(const IbContract& c) {
    Contract out;
    out.conId = c.con_id;
    out.symbol = c.symbol;
    out.secType = c.sec_type;
    out.lastTradeDateOrContractMonth = c.last_trade_date;
    out.exchange = c.exchange;
    out.currency = c.currency;
    out.localSymbol = c.local_symbol;
    out.multiplier = c.multiplier;
    return out;
}

IbContract from_tws(const Contract& c) {
    IbContract out;
    out.con_id = c.conId;
    out.symbol = c.symbol;
    out.sec_type = c.secType;
    out.last_trade_date = c.lastTradeDateOrContractMonth;
    out.exchange = c.exchange;
    out.currency = c.currency;
    out.local_symbol = c.localSymbol;
    out.multiplier = c.multiplier;
    return out;
}

double from_decimal(Decimal value) {
    if (value == UNSET_DECIMAL) return 0.0;
    return DecimalFunctions::decimalToDouble(value);
}

// Depth resets and dead subscriptions, as opposed to the informational
// codes IBKR sends on every connect.
bool is_fatal(int code) {
    switch (code) {
        case 317:   // market depth data has been reset
        case 309:   // max number of market depth requests exceeded
        case 10197: // no market data during competing live session
        case 1100:  // connectivity lost
            return true;
        default:
            return false;
    }
}

// The depth callbacks' request-id type, which changed spelling between
// API lines: ``TickerId`` (long) up to 10.37, plain ``int`` from 10.45.
// Getting it wrong does not fail loudly -- the method is simply not an
// override -- so it is detected in TwsApi.cmake rather than assumed. The
// tick-by-tick callbacks took ``int`` on both lines and are left alone.
#if HARVESTER_TWS_TICKER_ID
using DepthReqId = TickerId;
#else
using DepthReqId = int;
#endif

}  // namespace

class TwsMarketData final : public IbMarketData, public DefaultEWrapper {
public:
    TwsMarketData() : signal_(2000), client_(std::make_unique<EClientSocket>(this, &signal_)) {}
    ~TwsMarketData() override { TwsMarketData::disconnect(); }

    void set_listener(IbMarketDataListener* listener) override {
        const std::lock_guard<std::mutex> guard(state_mutex_);
        listener_ = listener;
    }

    void connect(const std::string& host, int port, int client_id, double timeout_seconds) override {
        if (client_->isConnected()) return;
        {
            const std::lock_guard<std::mutex> guard(state_mutex_);
            handshake_ = false;
        }
        if (!client_->eConnect(host.c_str(), port, client_id, false)) {
            throw ExecutionError(std::format("could not connect the market-data session to IBKR at {}:{} (clientId {})",
                                             host, port, client_id));
        }
        reader_ = std::make_unique<EReader>(client_.get(), &signal_);
        reader_->start();
        running_.store(true);
        pump_ = std::thread([this] { pump_loop(); });
        std::unique_lock<std::mutex> lock(state_mutex_);
        const bool ready = cv_.wait_for(lock, std::chrono::duration<double>(timeout_seconds),
                                        [this] { return handshake_; });
        lock.unlock();
        if (!ready) {
            disconnect();
            throw ExecutionError(std::format(
                "IBKR at {}:{} accepted the market-data socket but did not complete the handshake within {:.0f}s "
                "(is the client id free? the router uses ibkr.client_id and the feed uses its own)",
                host, port, timeout_seconds));
        }
    }

    void disconnect() override {
        running_.store(false);
        signal_.issueSignal();
        if (pump_.joinable()) pump_.join();
        if (client_->isConnected()) client_->eDisconnect();
        reader_.reset();
        cv_.notify_all();
    }

    bool is_connected() const override { return client_->isConnected(); }

    std::vector<IbContract> qualify(const IbContract& query) override {
        std::unique_lock<std::mutex> lock(state_mutex_);
        contracts_.clear();
        contracts_done_ = false;
        lock.unlock();
        {
            const std::lock_guard<std::mutex> send(send_mutex_);
            client_->reqContractDetails(kContractReqId, to_tws(query));
        }
        lock.lock();
        if (!cv_.wait_for(lock, std::chrono::duration<double>(kRequestTimeout), [this] { return contracts_done_; })) {
            throw ExecutionError(std::format("IBKR did not answer the market-data contract request within {:.0f}s",
                                             kRequestTimeout));
        }
        return std::move(contracts_);
    }

    void subscribe(const IbContract& contract, int rows) override {
        const Contract c = to_tws(contract);
        const std::lock_guard<std::mutex> send(send_mutex_);
        // isSmartDepth is false: CME depth comes from the exchange, and a
        // smart-routed aggregate is not the book a CME order joins.
        client_->reqMktDepth(kDepthReqId, c, rows, false, TagValueListSPtr());
        // "AllLast" also carries combo and block prints, which the tape
        // the strategy was measured on does not have; "Last" is the
        // outright tape and is the closer match.
        client_->reqTickByTickData(kTradesReqId, c, "Last", 0, false);
        subscribed_ = true;
    }

    void unsubscribe() override {
        if (!subscribed_) return;
        subscribed_ = false;
        if (!client_->isConnected()) return;
        const std::lock_guard<std::mutex> send(send_mutex_);
        client_->cancelMktDepth(kDepthReqId, false);
        client_->cancelTickByTickData(kTradesReqId);
    }

private:
    void pump_loop() {
        while (running_.load() && client_->isConnected()) {
            signal_.waitForSignal();
            if (!running_.load()) break;
            reader_->processMsgs();
        }
    }

    IbMarketDataListener* listener() {
        const std::lock_guard<std::mutex> guard(state_mutex_);
        return listener_;
    }

    // -- EWrapper ---------------------------------------------------------

    // The handshake is complete once the server has sent the account list.
    // ``nextValidId`` would do as well, but its signature moved between
    // API versions and a market-data session has no use for an order id.
    void managedAccounts(const std::string&) override {
        {
            const std::lock_guard<std::mutex> guard(state_mutex_);
            handshake_ = true;
        }
        cv_.notify_all();
    }

    void contractDetails(int, const ContractDetails& details) override {
        const std::lock_guard<std::mutex> guard(state_mutex_);
        contracts_.push_back(from_tws(details.contract));
    }

    void contractDetailsEnd(int) override {
        {
            const std::lock_guard<std::mutex> guard(state_mutex_);
            contracts_done_ = true;
        }
        cv_.notify_all();
    }

    void updateMktDepth(DepthReqId, int position, int operation, int side, double price, Decimal size) override {
        if (auto* l = listener()) l->on_depth(position, operation, side, price, from_decimal(size));
    }

    void updateMktDepthL2(DepthReqId, int position, const std::string&, int operation, int side, double price,
                          Decimal size, bool) override {
        if (auto* l = listener()) l->on_depth(position, operation, side, price, from_decimal(size));
    }

    void tickByTickAllLast(int, int, time_t time, double price, Decimal size, const TickAttribLast& attrib,
                           const std::string&, const std::string&) override {
        // ``unreported`` prints are not part of the consolidated tape and
        // are not what the VPIN buckets were measured on.
        if (attrib.unreported) return;
        if (auto* l = listener()) l->on_trade(static_cast<double>(time), price, from_decimal(size));
    }

#if HARVESTER_TWS_ERROR_HAS_TIME
    void error(int id, time_t, int code, const std::string& message, const std::string&) override {
        handle_error(id, code, message);
    }
#else
    void error(int id, int code, const std::string& message, const std::string&) override {
        handle_error(id, code, message);
    }
#endif

    void handle_error(int id, int code, const std::string& message) {
        auto* l = listener();
        if (l == nullptr) return;
        if (is_fatal(code)) l->on_reset(std::format("IBKR {}: {}", code, message));
        // 2100-2200 are IBKR's informational "data farm connected" notes.
        if (code < 2100 || code > 2200) l->on_error(id, code, message);
    }

    void connectionClosed() override {
        if (auto* l = listener()) l->on_reset("the market-data socket closed");
    }

    EReaderOSSignal signal_;
    std::unique_ptr<EClientSocket> client_;
    std::unique_ptr<EReader> reader_;
    std::thread pump_;
    std::atomic<bool> running_{false};
    bool subscribed_ = false;

    mutable std::mutex state_mutex_;
    std::mutex send_mutex_;
    std::condition_variable cv_;
    IbMarketDataListener* listener_ = nullptr;
    bool handshake_ = false;
    bool contracts_done_ = false;
    std::vector<IbContract> contracts_;
};

bool ibkr_market_data_supported() { return true; }

std::shared_ptr<RecordFeed> make_ibkr_feed(const IBKRConfig& cfg, const Product& product, int depth) {
    const int rows = cfg.market_data_rows > 0 ? cfg.market_data_rows : depth;
    return std::make_shared<IbkrBookFeed>(std::make_unique<TwsMarketData>(), cfg, product, depth, rows);
}

}  // namespace harvester
