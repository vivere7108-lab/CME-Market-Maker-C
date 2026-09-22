// IBKR as a market-data source, behind the same kind of seam the router
// uses.
//
// The strategy was built on Databento's MDP 3.0 feed: every book change as
// the exchange published it, each trade carrying the aggressor's side, and
// an exchange timestamp on both.  IBKR gives none of those three things.
// It gives a *maintained ladder* -- rows numbered from the touch, pushed
// as insert/update/delete -- and a separate tick-by-tick trade stream with
// no side and a one-second timestamp.  This seam is the narrowest
// description of what IBKR actually provides; ``IbkrBookFeed`` turns it
// into the MBP-10 records the rest of the system already reads, and the
// differences that cannot be papered over are written down in
// ``ibkr_feed.hpp``.
//
// The seam exists for the same reason ``IbGateway`` does: the TWS client
// is a socket, two threads and a 200-header SDK, and none of that belongs
// in a test of whether a delete at row 3 shifts rows 4 and 5 up.
#pragma once

#include <string>
#include <vector>

#include "harvester/execution/ibkr.hpp"

namespace harvester {

// What a market-data session reports, on the reader thread. Depth updates
// and trades arrive on that one thread in the order IBKR sent them, so an
// implementation may assume they are serialised with respect to each other.
class IbMarketDataListener {
public:
    virtual ~IbMarketDataListener() = default;

    // One row of the ladder. ``operation`` is 0 insert, 1 update, 2 delete;
    // ``side`` is 0 ask, 1 bid -- IBKR's own encodings, passed through so
    // the translation happens in one place. ``position`` is the row, 0 at
    // the touch. A delete shifts the rows below it up.
    virtual void on_depth(int position, int operation, int side, double price, double size) = 0;
    // One trade off the tick-by-tick stream. ``epoch_seconds`` is IBKR's
    // timestamp, which has one-second resolution; there is no side.
    virtual void on_trade(double epoch_seconds, double price, double size) = 0;
    // The ladder is no longer to be trusted -- a reconnect, a reset, a
    // subscription refused. Everything held is dropped and the book is not
    // quotable until it has been rebuilt from both sides.
    virtual void on_reset(const std::string& reason) = 0;
    virtual void on_error(long id, int code, const std::string& message) = 0;
};

// A market-data session: its own socket, so that a broker reconnect does
// not take the book down with it.
class IbMarketData {
public:
    virtual ~IbMarketData() = default;

    virtual void connect(const std::string& host, int port, int client_id, double timeout_seconds) = 0;
    virtual void disconnect() = 0;
    virtual bool is_connected() const = 0;
    // The contract this session is for, resolved to exactly one match.
    virtual std::vector<IbContract> qualify(const IbContract& query) = 0;
    // Depth and trades for ``contract``; ``rows`` is how deep to ask.
    virtual void subscribe(const IbContract& contract, int rows) = 0;
    virtual void unsubscribe() = 0;
    virtual void set_listener(IbMarketDataListener* listener) = 0;
};

}  // namespace harvester
