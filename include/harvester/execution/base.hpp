// The seam between the quote manager and whatever fills its orders.
//
// The manager speaks in three verbs -- place, replace, cancel -- and reads
// back a stream of ``OrderEvent``: acknowledged, filled (partially or
// fully), cancelled, rejected.  ``IbkrBroker`` maps those onto the TWS
// API; ``SimulatedBroker`` fills them off the book for a replay or a dry
// run.  The manager is written once against this and does not know which
// it has.
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace harvester {

// How the broker names one order.
struct OrderHandle {
    std::int64_t order_id = 0;
    bool operator==(const OrderHandle&) const = default;
};

struct Fill {
    double ts = 0.0;  // wall-clock seconds since the epoch (replay: simulated)
    int side = 0;
    double price = 0.0;
    int size = 0;
    double fees = 0.0;
    std::int64_t order_id = 0;
    // True when the fill was the resting side -- always, for a quote.
    bool passive = true;

    std::int64_t signed_size() const { return static_cast<std::int64_t>(size) * side; }
};

// One thing the broker reports about one order.
struct OrderEvent {
    enum class Kind { Ack, Fill, Cancelled, Rejected };

    Kind kind = Kind::Ack;
    std::int64_t order_id = 0;
    std::optional<harvester::Fill> fill;
    std::string detail;

    static OrderEvent ack(std::int64_t order_id, std::string detail = "") { return {Kind::Ack, order_id, std::nullopt, std::move(detail)}; }
    static OrderEvent filled(std::int64_t order_id, harvester::Fill fill) { return {Kind::Fill, order_id, fill, ""}; }
    static OrderEvent cancelled(std::int64_t order_id, std::string detail = "") { return {Kind::Cancelled, order_id, std::nullopt, std::move(detail)}; }
    static OrderEvent rejected(std::int64_t order_id, std::string detail = "") { return {Kind::Rejected, order_id, std::nullopt, std::move(detail)}; }
};

const char* to_string(OrderEvent::Kind kind);

class Broker {
public:
    virtual ~Broker() = default;
    virtual OrderHandle place(int side, double price, int size) = 0;
    virtual OrderHandle replace(OrderHandle handle, double price, int size) = 0;
    virtual void cancel(OrderHandle handle) = 0;
    virtual std::vector<OrderEvent> drain_events() = 0;
    virtual std::int64_t messages_sent() const = 0;
};

// An order could not be placed. Nothing reached the exchange.
struct ExecutionError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// An order was sent and its outcome could not be established.
struct OrderStateUnknown : ExecutionError {
    using ExecutionError::ExecutionError;
};

}  // namespace harvester
