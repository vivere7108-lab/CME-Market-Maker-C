#include "harvester/execution/tws_gateway.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

// The TWS API.
#include "Contract.h"
#include "Decimal.h"
#include "DefaultEWrapper.h"
#include "EClientSocket.h"
#include "EReader.h"
#include "EReaderOSSignal.h"
#include "Execution.h"
#include "Order.h"
#include "OrderCancel.h"
#include "OrderState.h"
#if HARVESTER_TWS_FEES_REPORT
#include "CommissionAndFeesReport.h"
#else
#include "CommissionReport.h"
#endif

#include "harvester/util/log.hpp"

namespace harvester {

namespace {

constexpr const char* kLog = "harvester.execution.tws";

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

double from_decimal(Decimal value) {
    if (value == UNSET_DECIMAL) return 0.0;
    return DecimalFunctions::decimalToDouble(value);
}

// The order-id type, which changed spelling on the same API version as
// the depth callbacks' request id: ``OrderId`` (long) up to 10.37, plain
// ``int`` from 10.45. See TwsApi.cmake.
#if HARVESTER_TWS_TICKER_ID
using OrderReqId = OrderId;
#else
using OrderReqId = int;
#endif

}  // namespace

struct TwsGateway::Impl : public DefaultEWrapper {
    Impl() : signal(2000), client(std::make_unique<EClientSocket>(this, &signal)) {}
    ~Impl() override { disconnect(); }

    // -- lifecycle ------------------------------------------------------------

    void connect(const std::string& host, int port, int client_id, double timeout_seconds) {
        if (client->isConnected()) return;
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            have_next_id = false;
            have_accounts = false;
        }
        if (!client->eConnect(host.c_str(), port, client_id, false)) {
            throw ExecutionError(std::format("could not connect to IBKR at {}:{} (clientId {})", host, port, client_id));
        }
        reader = std::make_unique<EReader>(client.get(), &signal);
        reader->start();
        running.store(true);
        pump = std::thread([this] { pump_loop(); });
        std::unique_lock<std::mutex> lock(state_mutex);
        const bool ready = cv.wait_for(lock, std::chrono::duration<double>(timeout_seconds),
                                       [this] { return have_next_id && have_accounts; });
        lock.unlock();
        if (!ready) {
            disconnect();
            throw ExecutionError(std::format("IBKR at {}:{} accepted the socket but did not complete the API handshake "
                                             "within {:.0f}s (is the API enabled, and the client id free?)",
                                             host, port, timeout_seconds));
        }
    }

    void pump_loop() {
        while (running.load() && client->isConnected()) {
            signal.waitForSignal();
            if (!running.load()) break;
            reader->processMsgs();
        }
    }

    void disconnect() {
        running.store(false);
        signal.issueSignal();
        if (pump.joinable()) pump.join();
        if (client->isConnected()) client->eDisconnect();
        reader.reset();
        cv.notify_all();
    }

    // -- synchronous requests ---------------------------------------------------

    template <typename Pred>
    void wait_for(std::unique_lock<std::mutex>& lock, Pred pred, const char* what) {
        if (!cv.wait_for(lock, std::chrono::duration<double>(kRequestTimeout), pred)) {
            throw ExecutionError(std::format("IBKR did not answer the {} request within {:.0f}s", what, kRequestTimeout));
        }
        if (!client->isConnected()) throw ExecutionError(std::format("the IBKR connection dropped during the {} request", what));
    }

    std::vector<IbContract> qualify(const IbContract& query) {
        std::unique_lock<std::mutex> lock(state_mutex);
        const int req_id = next_req_id++;
        ContractRequest& req = contract_requests[req_id];
        lock.unlock();
        {
            std::lock_guard<std::mutex> send(send_mutex);
            client->reqContractDetails(req_id, to_tws(query));
        }
        lock.lock();
        wait_for(lock, [&] { return req.done; }, "contract details");
        std::vector<IbContract> out = std::move(req.results);
        contract_requests.erase(req_id);
        return out;
    }

    std::vector<IbPosition> positions() {
        std::unique_lock<std::mutex> lock(state_mutex);
        positions_request = Request<IbPosition>{};
        lock.unlock();
        {
            std::lock_guard<std::mutex> send(send_mutex);
            client->reqPositions();
        }
        lock.lock();
        wait_for(lock, [&] { return positions_request.done; }, "positions");
        std::vector<IbPosition> out = std::move(positions_request.results);
        lock.unlock();
        std::lock_guard<std::mutex> send(send_mutex);
        client->cancelPositions();
        return out;
    }

    std::vector<IbAccountValue> account_values(const std::string& account) {
        std::unique_lock<std::mutex> lock(state_mutex);
        account_request = Request<IbAccountValue>{};
        lock.unlock();
        {
            std::lock_guard<std::mutex> send(send_mutex);
            client->reqAccountUpdates(true, account);
        }
        lock.lock();
        wait_for(lock, [&] { return account_request.done; }, "account values");
        std::vector<IbAccountValue> out = std::move(account_request.results);
        lock.unlock();
        std::lock_guard<std::mutex> send(send_mutex);
        client->reqAccountUpdates(false, account);
        return out;
    }

    std::vector<IbOpenOrder> open_orders() {
        std::unique_lock<std::mutex> lock(state_mutex);
        open_orders_request = Request<IbOpenOrder>{};
        lock.unlock();
        {
            std::lock_guard<std::mutex> send(send_mutex);
            client->reqAllOpenOrders();
        }
        lock.lock();
        wait_for(lock, [&] { return open_orders_request.done; }, "open orders");
        return std::move(open_orders_request.results);
    }

    // -- EWrapper -----------------------------------------------------------------

    void nextValidId(OrderReqId order_id) override {
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            next_order_id_value = std::max<long>(next_order_id_value, order_id);
            have_next_id = true;
        }
        cv.notify_all();
    }

    void managedAccounts(const std::string& list) override {
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            accounts.clear();
            std::stringstream stream(list);
            std::string item;
            while (std::getline(stream, item, ',')) {
                if (!item.empty()) accounts.push_back(item);
            }
            have_accounts = true;
        }
        cv.notify_all();
    }

    void connectionClosed() override {
        HLOG_WARNING(kLog, "the IBKR API connection closed");
        cv.notify_all();
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
        if (id == -1) {
            // Connection and market-data-farm notices, not errors.
            if (code >= 2100 && code < 2200) {
                HLOG_INFO(kLog, "IBKR {}: {}", code, message);
            } else {
                HLOG_WARNING(kLog, "IBKR {}: {}", code, message);
            }
            return;
        }
        bool consumed = false;
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            if (const auto it = contract_requests.find(id); it != contract_requests.end()) {
                // "No security definition has been found": the answer is
                // no contract, and the request is over.
                it->second.done = true;
                consumed = true;
            }
        }
        if (consumed) {
            cv.notify_all();
            HLOG_WARNING(kLog, "IBKR request {} failed with {}: {}", id, code, message);
            return;
        }
        HLOG_WARNING(kLog, "IBKR error {} on {}: {}", code, id, message);
        if (IbEventListener* l = listener.load()) l->on_error(id, code, message);
    }

#if HARVESTER_TWS_PERMID_LONGLONG
    using PermId = long long;
#else
    using PermId = int;
#endif
    void orderStatus(OrderReqId order_id, const std::string& status, Decimal filled, Decimal remaining, double,
                     PermId, int, double, int, const std::string&, double) override {
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            for (IbOpenOrder& row : open_orders_request.results) {
                if (row.order.order_id == order_id) row.status = status;
            }
        }
        if (IbEventListener* l = listener.load()) l->on_order_status(order_id, status, from_decimal(filled), from_decimal(remaining));
    }

    void openOrder(OrderReqId order_id, const Contract& contract, const Order& order, const OrderState& state) override {
        IbOpenOrder row;
        row.contract = from_tws(contract);
        row.order.order_id = order_id;
        row.order.action = order.action;
        row.order.total_quantity = from_decimal(order.totalQuantity);
        row.order.order_type = order.orderType;
        row.order.lmt_price = order.lmtPrice;
        row.order.tif = order.tif;
        row.order.outside_rth = order.outsideRth;
        row.order.account = order.account;
        row.status = state.status;
        std::lock_guard<std::mutex> guard(state_mutex);
        open_orders_request.results.push_back(std::move(row));
    }

    void openOrderEnd() override {
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            open_orders_request.done = true;
        }
        cv.notify_all();
    }

    void execDetails(int, const Contract& contract, const Execution& execution) override {
        IbExecution e;
        e.exec_id = execution.execId;
        e.order_id = execution.orderId;
        e.shares = from_decimal(execution.shares);
        e.price = execution.price;
        e.side = execution.side;
        e.time = 0.0;  // the wall clock at receipt is used
        if (IbEventListener* l = listener.load()) l->on_exec_details(from_tws(contract), e);
    }

#if HARVESTER_TWS_FEES_REPORT
    void commissionAndFeesReport(const CommissionAndFeesReport& report) override {
        if (IbEventListener* l = listener.load()) l->on_commission(report.execId, report.commissionAndFees);
    }
#else
    void commissionReport(const CommissionReport& report) override {
        if (IbEventListener* l = listener.load()) l->on_commission(report.execId, report.commission);
    }
#endif

    void position(const std::string& account, const Contract& contract, Decimal quantity, double avg_cost) override {
        IbPosition row;
        row.account = account;
        row.contract = from_tws(contract);
        row.position = from_decimal(quantity);
        row.avg_cost = avg_cost;
        std::lock_guard<std::mutex> guard(state_mutex);
        positions_request.results.push_back(std::move(row));
    }

    void positionEnd() override {
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            positions_request.done = true;
        }
        cv.notify_all();
    }

    void updateAccountValue(const std::string& key, const std::string& value, const std::string& currency,
                            const std::string& account) override {
        std::lock_guard<std::mutex> guard(state_mutex);
        if (account_request.done) return;  // a later push from the subscription
        account_request.results.push_back(IbAccountValue{key, value, currency, account});
    }

    void accountDownloadEnd(const std::string&) override {
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            account_request.done = true;
        }
        cv.notify_all();
    }

    void contractDetails(int req_id, const ContractDetails& details) override {
        std::lock_guard<std::mutex> guard(state_mutex);
        if (const auto it = contract_requests.find(req_id); it != contract_requests.end()) {
            it->second.results.push_back(from_tws(details.contract));
        }
    }

    void contractDetailsEnd(int req_id) override {
        {
            std::lock_guard<std::mutex> guard(state_mutex);
            if (const auto it = contract_requests.find(req_id); it != contract_requests.end()) it->second.done = true;
        }
        cv.notify_all();
    }

    // -- state ------------------------------------------------------------------

    template <typename Row>
    struct Request {
        std::vector<Row> results;
        bool done = false;
    };
    using ContractRequest = Request<IbContract>;

    EReaderOSSignal signal;
    std::unique_ptr<EClientSocket> client;
    std::unique_ptr<EReader> reader;
    std::thread pump;
    std::atomic<bool> running{false};
    std::mutex send_mutex;
    std::mutex state_mutex;
    std::condition_variable cv;
    std::atomic<IbEventListener*> listener{nullptr};

    bool have_next_id = false;
    bool have_accounts = false;
    long next_order_id_value = 0;
    std::vector<std::string> accounts;
    int next_req_id = 1;
    std::map<int, ContractRequest> contract_requests;
    Request<IbPosition> positions_request;
    Request<IbAccountValue> account_request;
    Request<IbOpenOrder> open_orders_request;
};

TwsGateway::TwsGateway() : impl_(std::make_unique<Impl>()) {}

TwsGateway::~TwsGateway() = default;

void TwsGateway::connect(const std::string& host, int port, int client_id, double timeout_seconds) {
    impl_->connect(host, port, client_id, timeout_seconds);
}

void TwsGateway::disconnect() { impl_->disconnect(); }

bool TwsGateway::is_connected() const { return impl_->client->isConnected(); }

std::vector<std::string> TwsGateway::managed_accounts() {
    std::lock_guard<std::mutex> guard(impl_->state_mutex);
    return impl_->accounts;
}

std::vector<IbContract> TwsGateway::qualify(const IbContract& query) { return impl_->qualify(query); }

std::vector<IbPosition> TwsGateway::positions() { return impl_->positions(); }

std::vector<IbAccountValue> TwsGateway::account_values(const std::string& account) {
    return impl_->account_values(account);
}

long TwsGateway::next_order_id() {
    std::lock_guard<std::mutex> guard(impl_->state_mutex);
    return impl_->next_order_id_value++;
}

void TwsGateway::place_order(const IbContract& contract, const IbOrder& order) {
    Order o;
    o.orderId = order.order_id;
    o.action = order.action;
    o.totalQuantity = DecimalFunctions::doubleToDecimal(order.total_quantity);
    o.orderType = order.order_type;
    o.lmtPrice = order.lmt_price;
    o.tif = order.tif;
    o.outsideRth = order.outside_rth;
    o.account = order.account;
    o.transmit = true;
    std::lock_guard<std::mutex> send(impl_->send_mutex);
    impl_->client->placeOrder(order.order_id, to_tws(contract), o);
}

void TwsGateway::cancel_order(long order_id) {
    std::lock_guard<std::mutex> send(impl_->send_mutex);
    impl_->client->cancelOrder(order_id, OrderCancel{});
}

std::vector<IbOpenOrder> TwsGateway::open_orders() { return impl_->open_orders(); }

void TwsGateway::sleep(double seconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

void TwsGateway::set_listener(IbEventListener* listener) { impl_->listener.store(listener); }

}  // namespace harvester
