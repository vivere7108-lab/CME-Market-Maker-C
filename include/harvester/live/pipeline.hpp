// One decision cycle, shared by the live runner and the replay.
//
// ``Pipeline::step`` is the whole strategy: fold the trades since the last
// step into the toxicity gate and the fast signals, fold the book into the
// signals and the vol estimate, apply the broker's events to the working
// quotes and the inventory, ask the risk layer whether to quote at all,
// ask the engine where, and reconcile the working quotes to that through
// the throttle.  The live runner calls it on a timer with the real feed
// and IBKR; the replay calls it on the recorded clock with a simulated
// exchange.  Neither adds logic of its own, which is what makes a replay
// result a statement about the live system.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "harvester/book/book.hpp"
#include "harvester/config.hpp"
#include "harvester/execution/base.hpp"
#include "harvester/execution/quotes.hpp"
#include "harvester/execution/simulated.hpp"
#include "harvester/execution/throttle.hpp"
#include "harvester/inventory.hpp"
#include "harvester/live/journal.hpp"
#include "harvester/quoting/engine.hpp"
#include "harvester/risk.hpp"
#include "harvester/signals/flow.hpp"
#include "harvester/signals/vol.hpp"
#include "harvester/signals/vpin.hpp"
#include "harvester/util/clock.hpp"

namespace harvester {

struct StepResult {
    std::optional<QuoteDecision> decision;
    Verdict verdict;
    int fills = 0;
    std::vector<std::string> messages;
};

class Pipeline {
public:
    Pipeline(const Config& cfg, Broker& broker, ClockFn clock = wall_now, SessionJournal* journal = nullptr,
             SimulatedBroker* simulated = nullptr);

    StepResult step(double now, const BookSnapshot& snapshot, const std::vector<Trade>& trades, bool in_hours,
                    double feed_age, const AccountValues* account = nullptr,
                    std::optional<int> broker_position = std::nullopt);

    // The decision snapshot the journal writes, as JSON members.
    void snapshot_row(json::Writer& w, double now, std::optional<double> anchor);
    std::string describe(std::optional<double> anchor) const;
    std::optional<double> anchor_of(const BookSnapshot& snapshot) const;

    Config cfg;
    const Product& product;
    Broker& broker;
    SimulatedBroker* simulated;
    SessionJournal* journal;

    ToxicityGate gate;
    FlowSignals flow;
    RealisedVol vol;
    QuoteEngine engine;
    MessageBudget budget;
    ActionQueue queue;
    QuoteManager quotes;
    Inventory inventory;
    MarkoutTracker markouts;
    RiskMonitor risk;

    std::optional<BookSnapshot> last_snapshot;
    std::optional<QuoteDecision> last_decision;
    std::optional<Verdict> last_verdict;
    std::int64_t cycles = 0;

private:
    void split_flatten(std::vector<OrderEvent>& events, std::vector<Fill>& flatten_fills);
    void flatten_position(double now, const BookSnapshot& snapshot);
    void note_transitions(double now);

    ClockFn clock_;
    double last_journal_ = 0.0;
    std::string last_pull_reason_;
    std::optional<OrderHandle> flatten_;
    double flatten_sent_at_ = 0.0;
    int flatten_size_ = 0;
    bool warned_warmup_ = false;
    std::size_t noted_ = 0;
    std::vector<OrderEvent> kept_events_;
    std::vector<Fill> flatten_fills_;
};

}  // namespace harvester
