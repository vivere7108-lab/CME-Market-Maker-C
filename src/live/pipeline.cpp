#include "harvester/live/pipeline.hpp"

#include <cmath>
#include <format>

#include "harvester/util/format.hpp"
#include "harvester/util/log.hpp"

namespace harvester {

namespace {
constexpr const char* kLog = "harvester.live.pipeline";
}

Pipeline::Pipeline(const Config& cfg_, Broker& broker_, ClockFn clock, SessionJournal* journal_,
                   SimulatedBroker* simulated_)
    : cfg(cfg_),
      product(cfg.instrument()),
      broker(broker_),
      simulated(simulated_),
      journal(journal_),
      gate(cfg.toxicity),
      flow(cfg.flow),
      vol(cfg.quoting.vol_halflife_seconds, cfg.quoting.vol_sample_ms, cfg.quoting.vol_floor, cfg.quoting.vol_ceiling),
      engine(cfg.quoting, cfg.risk, product, cfg.book.anchor, cfg.toxicity.extreme_action),
      budget(cfg.execution.max_messages_per_second, cfg.execution.burst, clock),
      queue(budget),
      quotes(broker, product, cfg.execution, queue, clock),
      inventory(product),
      markouts(product),
      risk(cfg.risk),
      clock_(std::move(clock)) {
    kept_events_.reserve(16);
    flatten_fills_.reserve(4);
}

std::optional<double> Pipeline::anchor_of(const BookSnapshot& snapshot) const {
    return cfg.book.anchor == "microprice" ? snapshot.microprice() : snapshot.mid();
}

StepResult Pipeline::step(double now, const BookSnapshot& snapshot, const std::vector<Trade>& trades, bool in_hours,
                          double feed_age, const AccountValues* account, std::optional<int> broker_position) {
    ++cycles;
    last_snapshot = snapshot;
    const std::optional<double> anchor = anchor_of(snapshot);

    // 1. The tape since the last step.
    for (const Trade& trade : trades) {
        const auto state = gate.update(trade);
        flow.on_trade(trade);
        if (simulated != nullptr) simulated->on_trade(trade);
        if (state) note_transitions(now);
    }

    // 2. The book as it stands.
    if (snapshot.two_sided()) {
        flow.on_book(snapshot);
        vol.update(*anchor, snapshot.ts_event);
    }
    if (simulated != nullptr) simulated->on_book(snapshot);

    // 3. What the broker says happened.
    std::vector<OrderEvent> events = broker.drain_events();
    flatten_fills_.clear();
    split_flatten(events, flatten_fills_);
    std::vector<Fill> fills = quotes.on_events(events);
    fills.insert(fills.end(), flatten_fills_.begin(), flatten_fills_.end());
    const ToxicityLevel level = gate.state().level;
    for (const Fill& fill : fills) {
        const double realised = inventory.apply(fill);
        markouts.record(fill, anchor, level, now);
        HLOG_INFO(kLog, "FILL {} {} @ {:.2f} ({}) -> {}{}", fill.side > 0 ? "bought" : "sold", fill.size, fill.price,
                  to_string(level), inventory.describe(anchor),
                  realised != 0.0 ? std::format(" realised ${}{}", realised > 0 ? "+" : "", fmt::commas(realised)) : "");
        if (journal != nullptr) {
            journal->record("fills", now, [&](json::Writer& w) {
                w.member("side", fill.side).member("price", fill.price).member("size", fill.size).member("fees", fill.fees)
                    .member("order_id", fill.order_id).member("level", to_string(level)).member("anchor", anchor)
                    .member("position", inventory.position).member("realised", inventory.realised);
            });
        }
    }
    for (const MarkoutRow* row : markouts.update(now, anchor)) {
        if (journal == nullptr) continue;
        journal->record("markouts", now, [&](json::Writer& w) {
            w.member("fill_ts", row->ts).member("side", row->side).member("price", row->price).member("size", row->size)
                .member("level", to_string(row->level)).member("edge", row->edge_at_fill);
            for (std::size_t h = 0; h < row->markouts.size(); ++h) {
                if (row->markouts[h]) w.member(markouts.horizon_key(h), *row->markouts[h]);
            }
        });
    }

    // 4. May we quote, and where.
    Verdict verdict = risk.evaluate(inventory, anchor, feed_age, in_hours, account, broker_position,
                                    vol.warmed_up() ? std::optional<double>(vol.sigma()) : std::nullopt);
    last_verdict = verdict;
    std::optional<QuoteDecision> decision;
    if (verdict.pull || !verdict.quote) {
        if (verdict.reason != last_pull_reason_) {
            HLOG_INFO(kLog, "not quoting: {}", verdict.reason);
            if (journal != nullptr) {
                journal->record("events", now, [&](json::Writer& w) { w.member("kind", "pull").member("reason", verdict.reason); });
            }
            last_pull_reason_ = verdict.reason;
        }
        quotes.cancel_all();
        if (verdict.flatten) flatten_position(now, snapshot);
    } else {
        if (!last_pull_reason_.empty()) {
            HLOG_INFO(kLog, "quoting resumed");
            if (journal != nullptr) journal->record("events", now, [&](json::Writer& w) { w.member("kind", "resume"); });
            last_pull_reason_.clear();
        }
        if (!gate.warmed_up() && gate.enabled() && !warned_warmup_) {
            warned_warmup_ = true;
            HLOG_INFO(kLog, "toxicity gate is warming up ({}/{} buckets); quoting at calm multipliers meanwhile",
                      gate.history_size(), cfg.toxicity.warmup_buckets);
        }
        decision = engine.decide(snapshot, vol.sigma(), flow.state(snapshot.ts_event), gate.state(), inventory.position);
        quotes.set_desired(*decision);
    }
    last_decision = decision;

    // 5. Reconcile through the throttle.
    quotes.reconcile(now);
    std::vector<std::string> messages = queue.pump(now);

    if (now - last_journal_ >= cfg.live.snapshot_seconds) {
        last_journal_ = now;
        if (journal != nullptr) journal->record("snapshots", now, [&](json::Writer& w) { snapshot_row(w, now, anchor); });
    }
    return StepResult{std::move(decision), std::move(verdict), static_cast<int>(fills.size()), std::move(messages)};
}

// -- flattening -----------------------------------------------------------

void Pipeline::split_flatten(std::vector<OrderEvent>& events, std::vector<Fill>& flatten_fills) {
    if (!flatten_) return;
    kept_events_.clear();
    for (OrderEvent& event : events) {
        if (event.order_id != flatten_->order_id) {
            kept_events_.push_back(std::move(event));
            continue;
        }
        if (event.kind == OrderEvent::Kind::Fill && event.fill) {
            flatten_fills.push_back(*event.fill);
            flatten_size_ -= event.fill->size;
            if (flatten_size_ <= 0) flatten_.reset();
        } else if (event.kind == OrderEvent::Kind::Cancelled || event.kind == OrderEvent::Kind::Rejected) {
            flatten_.reset();
        }
    }
    events.swap(kept_events_);
}

void Pipeline::flatten_position(double now, const BookSnapshot& snapshot) {
    const int position = inventory.position;
    if (position == 0 || !snapshot.two_sided()) return;
    const double tick = product.tick_size;
    const double cross = cfg.risk.flatten_cross_ticks * tick;
    const int side = position > 0 ? -1 : 1;
    double price = side < 0 ? snapshot.best_bid()->price - cross : snapshot.best_ask()->price + cross;
    price = product.round_to_tick(price);
    if (flatten_) {
        if (now - flatten_sent_at_ < 3.0) return;
        // Still working: chase the touch.
        if (budget.try_acquire(now)) {
            flatten_ = broker.replace(*flatten_, price, flatten_size_);
            flatten_sent_at_ = now;
        }
        return;
    }
    if (!budget.try_acquire(now)) return;
    HLOG_WARNING(kLog, "flattening {:+d} at {:.2f}", position, price);
    flatten_ = broker.place(side, price, std::abs(position));
    flatten_size_ = std::abs(position);
    flatten_sent_at_ = now;
    if (journal != nullptr) {
        journal->record("events", now, [&](json::Writer& w) {
            w.member("kind", "flatten").member("position", position).member("price", price);
        });
    }
}

// -- reporting ------------------------------------------------------------

void Pipeline::note_transitions(double now) {
    while (noted_ < gate.transitions.size()) {
        const ToxicityTransition t = gate.transitions[noted_];
        ++noted_;
        HLOG_INFO(kLog, "toxicity {} -> {} ({})", to_string(t.from), to_string(t.to), gate.describe());
        if (journal != nullptr) {
            journal->record("events", now, [&](json::Writer& w) {
                w.member("kind", "toxicity").member("from", to_string(t.from)).member("to", to_string(t.to))
                    .member("bucket", t.bucket).member("vpin", gate.vpin.value()).member("percentile", gate.state().percentile);
            });
        }
    }
}

void Pipeline::snapshot_row(json::Writer& w, double now, std::optional<double> anchor) {
    const std::optional<BookSnapshot>& s = last_snapshot;
    const std::optional<QuoteDecision>& d = last_decision;
    const bool have_flow = s.has_value();
    const FlowState f = have_flow ? flow.state(s->ts_event) : FlowState{};
    const ToxicityState tox = gate.state();
    const Level* bid = s && s->best_bid() ? s->best_bid() : nullptr;
    const Level* ask = s && s->best_ask() ? s->best_ask() : nullptr;
    w.key("bid");
    bid ? w.value(bid->price) : w.null();
    w.key("bid_size");
    bid ? w.value(bid->size) : w.null();
    w.key("ask");
    ask ? w.value(ask->price) : w.null();
    w.key("ask_size");
    ask ? w.value(ask->size) : w.null();
    w.member("anchor", anchor);
    w.member("sigma", vol.sigma());
    w.key("ofi");
    have_flow ? w.value(f.ofi) : w.null();
    w.key("depletion");
    have_flow ? w.value(f.depletion) : w.null();
    w.key("run");
    have_flow ? w.value(f.run) : w.null();
    w.member("vpin", tox.vpin).member("vpin_pct", tox.percentile).member("level", to_string(tox.level));
    w.key("quote_bid");
    d && d->bid ? w.value(d->bid->price) : w.null();
    w.key("quote_bid_size");
    d && d->bid ? w.value(d->bid->size) : w.null();
    w.key("quote_ask");
    d && d->ask ? w.value(d->ask->price) : w.null();
    w.key("quote_ask_size");
    d && d->ask ? w.value(d->ask->size) : w.null();
    w.key("reservation");
    d ? w.value(d->reservation) : w.null();
    w.key("half_spread");
    d ? w.value(d->half_spread) : w.null();
    w.key("skew_ticks");
    d ? w.value(d->skew_ticks) : w.null();
    w.key("reasons");
    if (d) {
        w.value(d->reasons);
    } else {
        w.begin_array();
        w.value(last_verdict ? last_verdict->reason : std::string{});
        w.end_array();
    }
    w.member("position", inventory.position);
    w.member("pnl", inventory.total_pnl(anchor));
    w.member("messages", budget.spent);
    w.member("tokens", budget.available(now));
}

std::string Pipeline::describe(std::optional<double> anchor) const {
    std::string head = last_decision ? last_decision->describe(product)
                                     : std::format("not quoting ({})", last_verdict ? last_verdict->reason : "-");
    return head + std::format(" | {} | {} | {} | msgs {} (refused {}, coalesced {})", quotes.describe(),
                              inventory.describe(anchor), gate.describe(), budget.spent, budget.refused, queue.coalesced);
}

}  // namespace harvester
