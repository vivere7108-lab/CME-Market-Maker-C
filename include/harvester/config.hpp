// Configuration.
//
// One ``Config`` drives the live runner and the replay alike, so a forward
// walk quotes with the parameters the replay measured.  Load with
// ``Config::from_yaml`` or build in code; every section validates itself
// and an unknown key is an error rather than a silently ignored typo.  The
// schema is the Python system's, key for key, so the same YAML files drive
// both implementations.
//
// The sections follow the pipeline: ``databento`` and ``book`` are where the
// data comes from; ``toxicity`` and ``flow`` are the two speeds of signal;
// ``quoting`` is the reservation price and spread; ``execution`` is how
// those quotes reach IBKR inside its message budget; ``risk`` is what stops
// it.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "harvester/instruments.hpp"

namespace YAML {
class Node;
}

namespace harvester {

struct ConfigError : std::invalid_argument {
    using std::invalid_argument::invalid_argument;
};

// The four toxicity levels, in order.
enum class ToxicityLevel : int { Calm = 0, Elevated = 1, Toxic = 2, Extreme = 3 };
inline constexpr std::array<const char*, 4> TOXICITY_LEVELS{"calm", "elevated", "toxic", "extreme"};
inline const char* to_string(ToxicityLevel level) { return TOXICITY_LEVELS[static_cast<int>(level)]; }
inline int level_index(ToxicityLevel level) { return static_cast<int>(level); }

struct TimeOfDay {
    int hour = 0;
    int minute = 0;
    int second = 0;

    static TimeOfDay parse(std::string_view text);
    // ``HH:MM`` as the Python system wrote it.
    std::string str() const;
    int seconds() const { return hour * 3600 + minute * 60 + second; }
    auto operator<=>(const TimeOfDay& other) const { return seconds() <=> other.seconds(); }
    bool operator==(const TimeOfDay& other) const { return seconds() == other.seconds(); }
};

// The MDP 3.0 feed, via Databento.
struct DatabentoConfig {
    std::string api_key_env = "DATABENTO_API_KEY";
    std::string dataset = "GLBX.MDP3";
    // "mbo" or "mbp-10".
    std::string schema = "mbp-10";
    std::string symbol = "ES.v.0";
    // "continuous" for ``ES.v.0`` style symbols, "raw_symbol" for ``ESZ6``.
    std::string stype_in = "continuous";
    bool reconnect = true;
    // Ask for the book snapshot on subscribe (MBO). The book has to be
    // *complete* before a quote is placed against it.
    bool snapshot = true;
    // A book that has not moved for this long is stale, whatever it says.
    double stale_after_seconds = 3.0;

    void validate() const;
};

// What is read off the reconstructed book.
struct BookConfig {
    // Levels a side kept in each snapshot handed to the signals.
    int depth = 10;
    // "microprice" (size-weighted mid at the touch) or "mid" (the control).
    std::string anchor = "microprice";

    void validate() const;
};

// VPIN, and the regime gate built on it.
struct ToxicityConfig {
    bool enabled = true;
    double bucket_contracts = 500.0;
    int window_buckets = 50;
    int history_buckets = 2000;
    // Trades with no aggressor named: "split" half to each side, or "ignore".
    std::string unknown_side = "split";
    int warmup_buckets = 100;
    double elevated_enter = 0.70;
    double elevated_exit = 0.60;
    double toxic_enter = 0.90;
    double toxic_exit = 0.80;
    double extreme_enter = 0.97;
    double extreme_exit = 0.93;
    int min_dwell_buckets = 3;
    std::map<std::string, double> spread_multiplier{
        {"calm", 1.0}, {"elevated", 1.5}, {"toxic", 2.5}, {"extreme", 4.0}};
    std::map<std::string, double> size_multiplier{
        {"calm", 1.0}, {"elevated", 0.75}, {"toxic", 0.5}, {"extreme", 0.0}};
    // At ``extreme``: "pull" cancels both sides; "reduce_only" keeps the
    // side that flattens inventory.
    std::string extreme_action = "reduce_only";

    void validate() const;
    double spread_multiplier_for(ToxicityLevel level) const;
    double size_multiplier_for(ToxicityLevel level) const;
};

// The fast signals that sit alongside VPIN and act per quote.
struct FlowConfig {
    double ofi_window_ms = 1000.0;
    double depletion_window_ms = 500.0;
    int run_saturation = 8;
    double run_decay_seconds = 2.0;

    void validate() const;
};

// The reservation price and the spread, Avellaneda-Stoikov style.
struct QuotingConfig {
    double gamma = 0.05;
    double kappa = 1.5;
    double horizon_seconds = 60.0;
    double vol_halflife_seconds = 30.0;
    double vol_sample_ms = 250.0;
    double vol_floor = 0.02;
    double vol_ceiling = 2.0;
    double min_half_spread_ticks = 1.0;
    int behind_best_ticks = 1;
    int max_behind_ticks = 8;
    int base_size = 1;
    int max_size = 2;
    double skew_ofi_ticks = 1.0;
    double skew_depletion_ticks = 1.0;
    double skew_run_ticks = 0.5;
    // The same measurement hook as ``risk.external_file``, for the other
    // half of the decision: a CSV of ``ts_ns,half_spread_ticks`` that
    // stands in for the Avellaneda-Stoikov spread, so a depth policy
    // fitted offline can be replayed before it is implemented here. The
    // toxicity multiplier and ``min_half_spread_ticks`` still apply on
    // top. Empty is off, which is every configuration that ships.
    std::string external_half_file;

    void validate() const;
};

// IBKR's documented ceiling on API messages per second, per client.
inline constexpr int IBKR_MESSAGE_CEILING = 50;

// How quotes reach the exchange inside IBKR's message budget.
struct ExecutionConfig {
    double max_messages_per_second = 30.0;
    int burst = 10;
    double requote_min_interval_ms = 250.0;
    double requote_price_ticks = 0.0;
    double requote_size_fraction = 0.34;
    double poll_positions_seconds = 30.0;
    double poll_account_seconds = 120.0;
    double ack_timeout_seconds = 5.0;

    void validate() const;
};

// What stops the quoter.
struct RiskConfig {
    int max_position = 2;
    int reduce_only_position = 1;
    double daily_loss_limit_usd = 500.0;
    bool flatten_on_halt = true;
    int flatten_cross_ticks = 2;
    bool flatten_outside_hours = true;
    double max_margin_utilisation = 0.25;
    double stale_book_cancel_seconds = 2.0;
    // Realised vol, in points per root-second, above which no quote is
    // placed: the fills taken while the anchor is moving this fast mark
    // out worst, and the spread the engine widens to does not cover it.
    // A pull, not a halt -- quoting resumes when vol falls back. Zero is
    // off, which is the shipped behaviour.
    double max_sigma = 0.0;
    // A measurement hook, not a shipped gate: a CSV of ``ts_ns,value``
    // beside the tape, and the ceiling on that value above which no quote
    // is placed. It lets a rule fitted offline be scored on the real
    // engine before it is implemented in it. Empty path, or a zero
    // ceiling, is off -- which is every configuration that ships.
    std::string external_file;
    double max_external = 0.0;
    std::string kill_file = "runs/HALT";

    void validate() const;
};

// Connection settings for TWS / IB Gateway.
struct IBKRConfig {
    std::string host = "127.0.0.1";
    int port = 4002;
    int client_id = 23;
    std::optional<std::string> account;
    bool allow_live_trading = false;
    double connect_timeout = 15.0;
    std::optional<std::string> local_symbol;
    bool outside_rth = true;

    void validate() const;
};

// Running unattended.
struct LiveConfig {
    bool journal = true;
    std::string journal_dir = "runs/live";
    double decision_interval_ms = 100.0;
    TimeOfDay quote_start{8, 45, 0};
    TimeOfDay quote_end{15, 0, 0};
    bool reconnect = true;
    double reconnect_backoff_seconds = 15.0;
    double max_reconnect_backoff_seconds = 300.0;
    std::optional<int> max_reconnect_attempts;
    double heartbeat_seconds = 300.0;
    double snapshot_seconds = 1.0;

    void validate() const;
};

// Running the pipeline over recorded or generated data.
struct ReplayConfig {
    std::string source = "synthetic";
    std::optional<std::string> path;
    double synthetic_seconds = 1800.0;
    double synthetic_start_price = 5000.0;
    int synthetic_seed = 7;
    double synthetic_toxic_fraction = 0.2;
    std::string queue_position = "back";
    std::string out_dir = "runs/replay";

    void validate() const;
};

struct Config {
    std::string product = "ES";
    DatabentoConfig databento;
    BookConfig book;
    ToxicityConfig toxicity;
    FlowConfig flow;
    QuotingConfig quoting;
    ExecutionConfig execution;
    RiskConfig risk;
    IBKRConfig ibkr;
    LiveConfig live;
    ReplayConfig replay;

    const Product& instrument() const { return get_product(product); }
    void validate() const;

    static Config from_yaml(const std::string& path);
    static Config from_yaml_string(const std::string& text);
    static Config from_node(const YAML::Node& node);
    std::string to_yaml_string() const;
    void to_yaml(const std::string& path) const;
};

}  // namespace harvester
