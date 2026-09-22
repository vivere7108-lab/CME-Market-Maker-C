// IBKR: the connection, and the broker that routes quotes through it.
//
// What IBKR is, on this path
// --------------------------
// It is the order router and nothing else.  The book comes from Databento;
// no market-data line is opened here, and the only requests that go out
// besides orders are the position and account polls the risk layer wants,
// at the lowest priority the throttle has.
//
// Every call that reaches the gateway counts against IBKR's 50-messages-a-
// second ceiling, so every one of them goes through the ``MessageBudget``
// the quote manager shares.  The broker itself does not enforce that -- it
// would have to refuse a cancel, which is the one message that must never
// be refused -- but it counts, and ``messages_sent`` is what the heartbeat
// reports against the budget.
//
// The gateway seam
// ----------------
// Everything that talks to TWS goes through ``IbGateway``: a dozen calls
// and four callbacks, which is the whole surface this system uses of the
// API.  ``TwsGateway`` (built with ``HARVESTER_WITH_IBKR``) implements it
// over IBKR's C++ client; the tests implement it with a fake.  The logic
// above the seam -- the paper-account gate, contract qualification,
// position adoption, base-currency handling, the mapping of statuses,
// executions and error codes onto order events, the cancel of foreign
// orders -- is therefore exercised by the tests exactly as it runs live.
//
// Safety
// ------
// ``allow_live_trading`` must be set to connect to anything that is not a
// paper account (ids beginning with ``D``); every order is a limit order
// with an explicit TIF; a modify is sent only against an order this
// process placed.  Orders on the quoted contract that this client did not
// place are cancelled at connect.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "harvester/config.hpp"
#include "harvester/execution/base.hpp"
#include "harvester/instruments.hpp"
#include "harvester/risk.hpp"

namespace harvester {

struct IbContract {
    long con_id = 0;
    std::string symbol;
    std::string sec_type = "FUT";
    std::string last_trade_date;  // lastTradeDateOrContractMonth
    std::string exchange;
    std::string currency;
    std::string local_symbol;
    std::string multiplier;
};

struct IbOrder {
    long order_id = 0;
    std::string action;  // BUY / SELL
    double total_quantity = 0.0;
    std::string order_type = "LMT";
    double lmt_price = 0.0;
    std::string tif = "DAY";
    bool outside_rth = false;
    std::string account;
};

struct IbPosition {
    std::string account;
    IbContract contract;
    double position = 0.0;
    double avg_cost = 0.0;
};

struct IbAccountValue {
    std::string tag;
    std::string value;
    std::string currency;
    std::string account;
};

struct IbOpenOrder {
    IbContract contract;
    IbOrder order;
    std::string status;
};

struct IbExecution {
    std::string exec_id;
    long order_id = 0;
    double shares = 0.0;
    double price = 0.0;
    std::string side;  // BOT / SLD
    double time = 0.0;  // seconds since the epoch, 0 when unknown
};

// Resolve the contract to quote: the one ``local_symbol`` names, or the
// front month by volume when it is empty. ``qualify`` is the session's own
// contract lookup -- the router and the market-data feed each pass their
// own, so both land on the same contract by the same route, which is what
// lets the runner compare the feed's symbol against the one it routes to.
// Throws ``ExecutionError`` when nothing matches.
using QualifyFn = std::function<std::vector<IbContract>(const IbContract&)>;
IbContract qualify_front_contract(const Product& product, const std::optional<std::string>& local_symbol,
                                  const QualifyFn& qualify);

// What the gateway reports back, on its own thread.
class IbEventListener {
public:
    virtual ~IbEventListener() = default;
    virtual void on_order_status(long order_id, const std::string& status, double filled, double remaining) = 0;
    virtual void on_exec_details(const IbContract& contract, const IbExecution& execution) = 0;
    virtual void on_commission(const std::string& exec_id, double commission) = 0;
    virtual void on_error(long id, int code, const std::string& message) = 0;
};

// The surface of the TWS API this system uses.
class IbGateway {
public:
    virtual ~IbGateway() = default;
    // Throws ``ExecutionError`` when the gateway cannot be reached.
    virtual void connect(const std::string& host, int port, int client_id, double timeout_seconds) = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    virtual std::vector<std::string> managed_accounts() = 0;
    // Contract details for ``query`` (``sec_type`` FUT or CONTFUT).
    virtual std::vector<IbContract> qualify(const IbContract& query) = 0;
    virtual std::vector<IbPosition> positions() = 0;
    virtual std::vector<IbAccountValue> account_values(const std::string& account) = 0;
    virtual long next_order_id() = 0;
    // A ``place_order`` with an id already used is a modify, as TWS defines it.
    virtual void place_order(const IbContract& contract, const IbOrder& order) = 0;
    virtual void cancel_order(long order_id) = 0;
    virtual std::vector<IbOpenOrder> open_orders() = 0;
    // Wait while the gateway's events keep flowing.
    virtual void sleep(double seconds) = 0;
    virtual void set_listener(IbEventListener* listener) = 0;
};

bool is_paper_account(const std::string& account);

// Owns the gateway handle and the quoted contract.
class IbkrConnection {
public:
    struct PositionReport {
        int quantity = 0;
        double avg_price = 0.0;
        // Positions in anything else, by label -- what the book cannot see.
        std::vector<std::string> foreign;
    };

    IbkrConnection(const Config& cfg, const Product& product, std::shared_ptr<IbGateway> gateway);

    void connect();
    void disconnect();
    bool is_connected() const;

    // Resolve the contract to quote. ``local_symbol`` is the raw contract
    // the feed is building the book from (``ESZ6``); the order is routed
    // to exactly that. With none, the front month by IBKR's continuous
    // contract is used.
    const IbContract& qualify(const std::optional<std::string>& local_symbol);
    PositionReport position();
    // Numeric account tags in USD, with the base-currency handling.
    AccountValues account_values();

    IbGateway& gateway() { return *gateway_; }
    const std::string& account() const { return account_; }
    const std::optional<IbContract>& contract() const { return contract_; }
    const Product& product() const { return product_; }

private:
    const Config& cfg_;
    const Product& product_;
    std::shared_ptr<IbGateway> gateway_;
    std::string account_;
    std::optional<IbContract> contract_;
    bool warned_base_ = false;
};

// Quotes as IBKR limit orders, with events read off the gateway's callbacks.
class IbkrBroker : public Broker, public IbEventListener {
public:
    IbkrBroker(IbkrConnection& connection, const Product& product, bool outside_rth = true);
    ~IbkrBroker() override;

    void close();

    // -- the three verbs --
    OrderHandle place(int side, double price, int size) override;
    OrderHandle replace(OrderHandle handle, double price, int size) override;
    void cancel(OrderHandle handle) override;
    std::vector<OrderEvent> drain_events() override;
    std::int64_t messages_sent() const override { return sent_; }

    // -- reconciliation --
    // Working orders on the quoted contract, whoever placed them.
    std::vector<OrderHandle> open_orders();
    // Cancel working orders on the contract this broker did not place.
    int cancel_foreign();
    bool holds(long order_id) const;

    // -- IbEventListener --
    void on_order_status(long order_id, const std::string& status, double filled, double remaining) override;
    void on_exec_details(const IbContract& contract, const IbExecution& execution) override;
    void on_commission(const std::string& exec_id, double commission) override;
    void on_error(long id, int code, const std::string& message) override;

private:
    struct Tracked {
        IbOrder order;
        double filled = 0.0;
    };

    IbkrConnection& conn_;
    const Product& product_;
    bool outside_rth_;
    mutable std::mutex mutex_;
    std::unordered_map<long, Tracked> orders_;
    std::vector<OrderEvent> events_;
    std::unordered_set<std::string> seen_execs_;
    std::unordered_map<std::string, double> commissions_;
    std::int64_t sent_ = 0;
    bool closed_ = false;
};

}  // namespace harvester
