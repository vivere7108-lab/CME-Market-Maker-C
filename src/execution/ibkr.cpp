#include "harvester/execution/ibkr.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

#include "harvester/util/clock.hpp"
#include "harvester/util/log.hpp"

namespace harvester {

namespace {
constexpr const char* kLog = "harvester.execution.ibkr";

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) out += ", ";
        out += p;
    }
    return out;
}

std::optional<double> to_double(const std::string& text) {
    if (text.empty()) return std::nullopt;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') return std::nullopt;
    return value;
}
}  // namespace

bool is_paper_account(const std::string& account) {
    return !account.empty() && std::toupper(static_cast<unsigned char>(account[0])) == 'D';
}

// -- IbkrConnection ------------------------------------------------------

IbkrConnection::IbkrConnection(const Config& cfg, const Product& product, std::shared_ptr<IbGateway> gateway)
    : cfg_(cfg), product_(product), gateway_(std::move(gateway)) {}

void IbkrConnection::connect() {
    const IBKRConfig& ib_cfg = cfg_.ibkr;
    HLOG_INFO(kLog, "connecting to IBKR {}:{} clientId={}", ib_cfg.host, ib_cfg.port, ib_cfg.client_id);
    gateway_->connect(ib_cfg.host, ib_cfg.port, ib_cfg.client_id, ib_cfg.connect_timeout);

    const std::vector<std::string> accounts = gateway_->managed_accounts();
    if (accounts.empty()) throw ExecutionError("IBKR returned no managed accounts");
    account_ = ib_cfg.account.value_or(accounts[0]);
    if (std::find(accounts.begin(), accounts.end(), account_) == accounts.end()) {
        throw ExecutionError(std::format("account {} is not in this session's managed accounts: {}", account_,
                                         join(accounts)));
    }
    if (!is_paper_account(account_) && !ib_cfg.allow_live_trading) {
        gateway_->disconnect();
        throw ExecutionError(std::format("account {} does not look like an IBKR paper account and "
                                         "ibkr.allow_live_trading is False. Set it only when you intend to route "
                                         "real orders.",
                                         account_));
    }
    HLOG_INFO(kLog, "connected to account {} ({})", account_, is_paper_account(account_) ? "paper" : "LIVE");
}

void IbkrConnection::disconnect() {
    if (gateway_->is_connected()) gateway_->disconnect();
}

bool IbkrConnection::is_connected() const { return gateway_->is_connected(); }

const IbContract& IbkrConnection::qualify(const std::optional<std::string>& local_symbol) {
    const Product& p = product_;
    const std::optional<std::string> symbol = cfg_.ibkr.local_symbol ? cfg_.ibkr.local_symbol : local_symbol;
    if (symbol && !symbol->empty()) {
        IbContract query;
        query.symbol = p.ibkr_symbol;
        query.sec_type = "FUT";
        query.local_symbol = *symbol;
        query.exchange = p.exchange;
        query.currency = p.currency;
        const std::vector<IbContract> qualified = gateway_->qualify(query);
        if (qualified.empty()) {
            throw ExecutionError(std::format(
                "IBKR could not qualify {} {} on {}. The feed's raw symbol and IBKR's local symbol may differ for "
                "this product; set ibkr.local_symbol explicitly.",
                p.ibkr_symbol, *symbol, p.exchange));
        }
        contract_ = qualified[0];
    } else {
        IbContract cont;
        cont.symbol = p.ibkr_symbol;
        cont.sec_type = "CONTFUT";
        cont.exchange = p.exchange;
        cont.currency = p.currency;
        const std::vector<IbContract> resolved = gateway_->qualify(cont);
        if (resolved.empty()) {
            throw ExecutionError(std::format("IBKR could not resolve the continuous {} contract on {}", p.ibkr_symbol,
                                             p.exchange));
        }
        IbContract front;
        front.symbol = p.ibkr_symbol;
        front.sec_type = "FUT";
        front.last_trade_date = resolved[0].last_trade_date;
        front.exchange = p.exchange;
        front.currency = p.currency;
        const std::vector<IbContract> qualified = gateway_->qualify(front);
        if (qualified.empty()) {
            throw ExecutionError(std::format("IBKR could not qualify the front {} contract ({})", p.ibkr_symbol,
                                             resolved[0].last_trade_date));
        }
        contract_ = qualified[0];
    }
    HLOG_INFO(kLog, "quoting {} (conId {})", contract_->local_symbol.empty() ? "?" : contract_->local_symbol,
              contract_->con_id);
    return *contract_;
}

IbkrConnection::PositionReport IbkrConnection::position() {
    PositionReport report;
    double cost = 0.0;
    for (const IbPosition& row : gateway_->positions()) {
        if (!row.account.empty() && !account_.empty() && row.account != account_) continue;
        const int n = static_cast<int>(row.position);
        if (n == 0) continue;
        if (contract_ && row.contract.con_id == contract_->con_id) {
            report.quantity += n;
            cost += n * row.avg_cost / (product_.multiplier != 0.0 ? product_.multiplier : 1.0);
        } else {
            report.foreign.push_back(!row.contract.local_symbol.empty()
                                         ? row.contract.local_symbol
                                         : row.contract.symbol + " " + row.contract.sec_type);
        }
    }
    report.avg_price = report.quantity != 0 ? cost / report.quantity : 0.0;
    return report;
}

AccountValues IbkrConnection::account_values() {
    const std::vector<IbAccountValue> rows = gateway_->account_values(account_);
    std::unordered_map<std::string, double> rates;
    for (const IbAccountValue& row : rows) {
        if (row.tag != "ExchangeRate" || row.currency.empty() || row.currency == "BASE") continue;
        if (const auto value = to_double(row.value)) rates[row.currency] = *value;
    }
    std::string base = "USD";
    const auto usd = rates.find("USD");
    if (!(usd != rates.end() && usd->second == 1.0) && !rates.empty()) {
        base = "USD";
        for (const auto& [currency, rate] : rates) {
            if (rate == 1.0) {
                base = currency;
                break;
            }
        }
    }
    std::optional<double> to_usd = base == "USD" ? std::optional<double>{1.0}
                                                 : (usd != rates.end() ? std::optional<double>{usd->second} : std::nullopt);
    if (!to_usd || *to_usd <= 0.0) {
        if (!warned_base_) {
            warned_base_ = true;
            HLOG_WARNING(kLog, "the account's base currency is {} and IBKR reports no USD rate; equity and margin "
                               "cannot be read", base);
        }
        return {};
    }
    AccountValues values;
    for (const IbAccountValue& row : rows) {
        if (!(row.currency.empty() || row.currency == base || row.currency == "BASE")) continue;
        const auto value = to_double(row.value);
        if (!value) continue;
        values[row.tag] = row.currency.empty() ? *value : *value / *to_usd;
    }
    return values;
}

// -- IbkrBroker --------------------------------------------------------------

IbkrBroker::IbkrBroker(IbkrConnection& connection, const Product& product, bool outside_rth)
    : conn_(connection), product_(product), outside_rth_(outside_rth) {
    conn_.gateway().set_listener(this);
}

IbkrBroker::~IbkrBroker() { close(); }

void IbkrBroker::close() {
    if (closed_) return;
    closed_ = true;
    try {
        conn_.gateway().set_listener(nullptr);
    } catch (...) {  // detaching from a dead handle
    }
}

OrderHandle IbkrBroker::place(int side, double price, int size) {
    if (!conn_.contract()) throw ExecutionError("no contract has been qualified");
    IbOrder order;
    order.action = side > 0 ? "BUY" : "SELL";
    order.total_quantity = size;
    order.order_type = "LMT";
    order.lmt_price = product_.round_to_tick(price);
    order.tif = "DAY";
    order.outside_rth = outside_rth_;
    order.account = conn_.account();
    // Take the order id first so a status event that lands before the
    // place returns is recognised as ours rather than dropped.
    order.order_id = conn_.gateway().next_order_id();
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        orders_[order.order_id] = Tracked{order, 0.0};
    }
    conn_.gateway().place_order(*conn_.contract(), order);
    ++sent_;
    return OrderHandle{order.order_id};
}

OrderHandle IbkrBroker::replace(OrderHandle handle, double price, int size) {
    IbOrder order;
    {
        const std::lock_guard<std::mutex> guard(mutex_);
        const auto it = orders_.find(static_cast<long>(handle.order_id));
        if (it == orders_.end()) {
            throw ExecutionError(std::format("order #{} is not held by this broker; it cannot be modified", handle.order_id));
        }
        // A modify in place: same orderId, one message.
        it->second.order.lmt_price = product_.round_to_tick(price);
        it->second.order.total_quantity = size + static_cast<int>(it->second.filled);
        order = it->second.order;
    }
    conn_.gateway().place_order(*conn_.contract(), order);
    ++sent_;
    return handle;
}

void IbkrBroker::cancel(OrderHandle handle) {
    conn_.gateway().cancel_order(static_cast<long>(handle.order_id));
    ++sent_;
}

std::vector<OrderEvent> IbkrBroker::drain_events() {
    const std::lock_guard<std::mutex> guard(mutex_);
    std::vector<OrderEvent> out;
    out.swap(events_);
    return out;
}

bool IbkrBroker::holds(long order_id) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return orders_.contains(order_id);
}

void IbkrBroker::on_order_status(long order_id, const std::string& status, double filled, double /*remaining*/) {
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto it = orders_.find(order_id);
    if (it == orders_.end()) return;
    if (filled > it->second.filled) it->second.filled = filled;
    OrderEvent::Kind kind;
    if (status == "Submitted" || status == "PreSubmitted") {
        kind = OrderEvent::Kind::Ack;
    } else if (status == "Cancelled" || status == "ApiCancelled") {
        kind = OrderEvent::Kind::Cancelled;
    } else if (status == "Inactive") {
        kind = OrderEvent::Kind::Rejected;
    } else {
        return;  // Filled arrives through the executions; PendingSubmit is not an ack
    }
    OrderEvent event;
    event.kind = kind;
    event.order_id = order_id;
    event.detail = status;
    events_.push_back(std::move(event));
    if (kind == OrderEvent::Kind::Cancelled || kind == OrderEvent::Kind::Rejected) orders_.erase(it);
}

void IbkrBroker::on_exec_details(const IbContract& /*contract*/, const IbExecution& execution) {
    const std::lock_guard<std::mutex> guard(mutex_);
    if (!seen_execs_.insert(execution.exec_id).second) return;
    int side = execution.side == "BOT" ? 1 : (execution.side == "SLD" ? -1 : 0);
    const auto it = orders_.find(execution.order_id);
    if (it != orders_.end()) {
        if (side == 0) side = it->second.order.action == "BUY" ? 1 : -1;
        it->second.filled += execution.shares;
    }
    if (side == 0) return;
    const int size = static_cast<int>(execution.shares);
    double fees = product_.fee_per_contract * size;
    if (const auto known = commissions_.find(execution.exec_id); known != commissions_.end() && known->second != 0.0) {
        fees = known->second;
    }
    Fill fill;
    fill.ts = execution.time != 0.0 ? execution.time : wall_now();
    fill.side = side;
    fill.price = execution.price;
    fill.size = size;
    fill.fees = fees;
    fill.order_id = execution.order_id;
    events_.push_back(OrderEvent::filled(execution.order_id, fill));
}

void IbkrBroker::on_commission(const std::string& exec_id, double commission) {
    const std::lock_guard<std::mutex> guard(mutex_);
    commissions_[exec_id] = commission;
}

void IbkrBroker::on_error(long id, int code, const std::string& message) {
    static constexpr int kOrderCodes[] = {201, 202, 10147, 10148, 110, 104, 105};
    if (std::find(std::begin(kOrderCodes), std::end(kOrderCodes), code) == std::end(kOrderCodes)) return;
    const std::lock_guard<std::mutex> guard(mutex_);
    const auto it = orders_.find(id);
    if (it == orders_.end()) return;
    const bool cancelled = code == 202 || code == 10147 || code == 10148;
    OrderEvent event;
    event.kind = cancelled ? OrderEvent::Kind::Cancelled : OrderEvent::Kind::Rejected;
    event.order_id = id;
    event.detail = std::format("{}: {}", code, message);
    events_.push_back(std::move(event));
    orders_.erase(it);
}

std::vector<OrderHandle> IbkrBroker::open_orders() {
    std::vector<OrderHandle> found;
    if (!conn_.contract()) return found;
    for (const IbOpenOrder& row : conn_.gateway().open_orders()) {
        if (row.contract.con_id != conn_.contract()->con_id) continue;
        if (row.status == "Filled" || row.status == "Cancelled" || row.status == "ApiCancelled") continue;
        found.push_back(OrderHandle{row.order.order_id});
    }
    return found;
}

int IbkrBroker::cancel_foreign() {
    int count = 0;
    for (const OrderHandle& handle : open_orders()) {
        if (holds(static_cast<long>(handle.order_id))) continue;
        HLOG_WARNING(kLog, "cancelling order #{} on {} that this process did not place", handle.order_id,
                     conn_.contract() && !conn_.contract()->local_symbol.empty() ? conn_.contract()->local_symbol : "?");
        conn_.gateway().cancel_order(static_cast<long>(handle.order_id));
        ++sent_;
        ++count;
    }
    return count;
}

}  // namespace harvester
