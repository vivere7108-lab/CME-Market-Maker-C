// A fake IBKR gateway for the broker and live-runner tests.
//
// Only the gateway seam is faked; everything above it -- the connection's
// account and contract handling, the broker's event mapping, the runner's
// reconciliation -- is the real code, driven through the same interface
// the TWS API adapter implements.
#pragma once

#include <format>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "harvester/execution/ibkr.hpp"
#include "harvester/execution/market_data.hpp"

namespace test {

using namespace harvester;

// An IBKR market-data session that reports nothing on its own: the test
// drives the feed's listener by hand, which is the whole point of the
// seam -- a delete at row 3 does not need a socket.
class FakeMarketData : public IbMarketData {
public:
    struct Subscription {
        IbContract contract;
        int rows = 0;
    };

    std::vector<IbContract> qualified{[] {
        IbContract c;
        c.con_id = 5001;
        c.symbol = "ES";
        c.sec_type = "FUT";
        c.exchange = "CME";
        c.currency = "USD";
        c.local_symbol = "ESZ6";
        return c;
    }()};
    std::vector<IbContract> queries;
    std::optional<Subscription> subscription;
    int connects = 0;
    int disconnects = 0;
    bool connected = false;
    int client_id = 0;

    void connect(const std::string&, int, int id, double) override {
        ++connects;
        client_id = id;
        connected = true;
    }
    void disconnect() override {
        ++disconnects;
        connected = false;
    }
    bool is_connected() const override { return connected; }
    std::vector<IbContract> qualify(const IbContract& query) override {
        queries.push_back(query);
        return qualified;
    }
    void subscribe(const IbContract& contract, int rows) override { subscription = Subscription{contract, rows}; }
    void unsubscribe() override { subscription.reset(); }
    void set_listener(IbMarketDataListener* listener) override { listener_ = listener; }

    IbMarketDataListener* listener_ = nullptr;
};

class FakeGateway : public IbGateway {
public:
    struct Order {
        IbContract contract;
        IbOrder order;
        std::string status = "PendingSubmit";
        double filled = 0.0;
        double remaining = 0.0;
    };

    std::map<std::string, double> account_values_map{{"NetLiquidation", 250'000.0}, {"FullInitMarginReq", 0.0}};
    std::vector<std::string> accounts{"DU1234567"};
    std::map<long, Order> trades;
    std::vector<std::pair<IbContract, IbOrder>> placed;
    std::vector<long> cancelled;
    std::vector<IbOpenOrder> foreign;
    std::vector<IbPosition> positions_rows;
    std::optional<int> drop_after;
    int sleeps = 0;
    int connects = 0;
    long next_id = 1;
    int execs = 0;
    int qualified = 0;
    // The one contract every qualification resolves to, like the fixed
    // contract the Python fake connection held.
    long contract_con_id = 5001;
    bool connected = false;
    IbEventListener* listener = nullptr;

    // -- IbGateway --
    void connect(const std::string&, int, int, double) override {
        connected = true;
        ++connects;
    }
    void disconnect() override { connected = false; }
    bool is_connected() const override { return connected; }
    std::vector<std::string> managed_accounts() override { return accounts; }

    std::vector<IbContract> qualify(const IbContract& query) override {
        IbContract c = query;
        ++qualified;
        if (c.con_id == 0) c.con_id = contract_con_id;
        if (c.last_trade_date.empty()) c.last_trade_date = "20261218";
        if (c.local_symbol.empty()) c.local_symbol = c.symbol + "Z6";
        if (c.sec_type == "CONTFUT") c.sec_type = "FUT";
        return {c};
    }

    std::vector<IbPosition> positions() override { return positions_rows; }

    std::vector<IbAccountValue> account_values(const std::string&) override {
        std::vector<IbAccountValue> rows;
        for (const auto& [tag, value] : account_values_map) {
            rows.push_back({tag, std::format("{}", value), "USD", "DU1234567"});
        }
        rows.push_back({"AccountType", "INDIVIDUAL", "", "DU1234567"});
        return rows;
    }

    long next_order_id() override { return next_id++; }

    void place_order(const IbContract& contract, const IbOrder& order) override {
        placed.emplace_back(contract, order);
        if (auto it = trades.find(order.order_id); it != trades.end()) {
            it->second.order = order;
            status(order.order_id, "Submitted");
            return;
        }
        Order o;
        o.contract = contract;
        o.order = order;
        o.remaining = order.total_quantity;
        trades[order.order_id] = o;
        status(order.order_id, "Submitted");
    }

    void cancel_order(long order_id) override {
        cancelled.push_back(order_id);
        if (auto it = trades.find(order_id); it != trades.end() && it->second.status != "Filled") {
            status(order_id, "Cancelled");
        }
    }

    std::vector<IbOpenOrder> open_orders() override {
        std::vector<IbOpenOrder> out;
        for (const auto& [id, t] : trades) {
            if (t.status == "Filled" || t.status == "Cancelled") continue;
            out.push_back({t.contract, t.order, t.status});
        }
        for (const auto& f : foreign) {
            if (f.status == "Filled" || f.status == "Cancelled") continue;
            out.push_back(f);
        }
        return out;
    }

    void sleep(double) override {
        ++sleeps;
        if (drop_after && sleeps >= *drop_after) connected = false;
    }

    void set_listener(IbEventListener* l) override { listener = l; }

    // -- scripting --
    void status(long order_id, const std::string& s) {
        Order& t = trades[order_id];
        t.status = s;
        if (listener != nullptr) listener->on_order_status(order_id, s, t.filled, t.remaining);
    }

    IbExecution fill(long order_id, double size, double price, double commission = 0.0) {
        Order& t = trades.at(order_id);
        ++execs;
        t.filled += size;
        t.remaining = std::max(t.order.total_quantity - t.filled, 0.0);
        IbExecution e;
        e.exec_id = std::format("exec-{}", execs);
        e.order_id = order_id;
        e.shares = size;
        e.price = price;
        e.side = t.order.action == "BUY" ? "BOT" : "SLD";
        e.time = 1'789'000'000.0;
        if (t.remaining <= 0) t.status = "Filled";
        if (listener != nullptr) {
            if (commission != 0.0) listener->on_commission(e.exec_id, commission);
            listener->on_exec_details(t.contract, e);
        }
        return e;
    }

    void emit_exec(long order_id, const IbExecution& e) {
        if (listener != nullptr) listener->on_exec_details(trades.at(order_id).contract, e);
    }

    void emit_status(const IbOpenOrder& row) {
        if (listener != nullptr) listener->on_order_status(row.order.order_id, row.status, 0.0, row.order.total_quantity);
    }

    void reject(long order_id, int code = 201) {
        if (listener != nullptr) listener->on_error(order_id, code, "Order rejected - reason:");
    }

    IbOpenOrder add_foreign_order(const IbContract& contract, const std::string& side = "BUY") {
        IbOpenOrder row;
        row.contract = contract;
        row.order.order_id = 900 + static_cast<long>(foreign.size());
        row.order.action = side;
        row.order.total_quantity = 1;
        row.order.order_type = "LMT";
        row.order.lmt_price = 1.0;
        row.status = "Submitted";
        foreign.push_back(row);
        return row;
    }
};

inline IbContract contract(long con_id, const std::string& symbol, const std::string& local_symbol = "") {
    IbContract c;
    c.con_id = con_id;
    c.symbol = symbol;
    c.local_symbol = local_symbol;
    c.sec_type = "FUT";
    c.exchange = "CME";
    c.currency = "USD";
    return c;
}

inline IbPosition fake_position(const IbContract& c, double quantity, double avg_cost) {
    return IbPosition{"DU1234567", c, quantity, avg_cost};
}

}  // namespace test
