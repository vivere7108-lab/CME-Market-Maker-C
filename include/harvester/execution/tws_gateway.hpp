// IBKR's TWS API C++ client behind the ``IbGateway`` seam.
//
// Built only with ``HARVESTER_WITH_IBKR`` and a ``TWS_API_DIR``.  The TWS
// API is asynchronous -- every request is answered by callbacks on the
// reader thread -- and the runner wants synchronous answers, so each
// request here blocks on a condition variable until its ``...End``
// callback lands, with a timeout.  Order events are forwarded to the
// broker as they arrive; nothing blocks on those.
//
// Threads: the API's ``EReader`` reads the socket on its own thread and
// queues messages; this class runs a second thread that dispatches them
// (``processMsgs``) and so calls every ``EWrapper`` callback; the runner's
// thread sends.  Sends are serialised with a mutex, callbacks never call
// back into the listener while holding a lock.
#pragma once

#include <memory>

#include "harvester/execution/ibkr.hpp"

namespace harvester {

class TwsGateway : public IbGateway {
public:
    // Seconds to wait for the answer to a synchronous request.
    static constexpr double kRequestTimeout = 15.0;

    TwsGateway();
    ~TwsGateway() override;

    void connect(const std::string& host, int port, int client_id, double timeout_seconds) override;
    void disconnect() override;
    bool is_connected() const override;
    std::vector<std::string> managed_accounts() override;
    std::vector<IbContract> qualify(const IbContract& query) override;
    std::vector<IbPosition> positions() override;
    std::vector<IbAccountValue> account_values(const std::string& account) override;
    long next_order_id() override;
    void place_order(const IbContract& contract, const IbOrder& order) override;
    void cancel_order(long order_id) override;
    std::vector<IbOpenOrder> open_orders() override;
    void sleep(double seconds) override;
    void set_listener(IbEventListener* listener) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace harvester
