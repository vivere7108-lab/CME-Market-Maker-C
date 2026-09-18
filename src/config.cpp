#include "harvester/config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include "harvester/util/format.hpp"

namespace harvester {

// -- TimeOfDay ---------------------------------------------------------------

TimeOfDay TimeOfDay::parse(std::string_view text) {
    std::array<int, 3> parts{0, 0, 0};
    std::size_t count = 0;
    std::string token;
    std::stringstream stream{std::string(text)};
    while (std::getline(stream, token, ':')) {
        if (count >= 3) break;
        if (token.empty() || !std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c); })) {
            throw ConfigError("cannot read a time-of-day from '" + std::string(text) + "'");
        }
        parts[count++] = std::stoi(token);
    }
    if (count == 0) throw ConfigError("cannot read a time-of-day from '" + std::string(text) + "'");
    const TimeOfDay t{parts[0], parts[1], parts[2]};
    if (t.hour > 23 || t.minute > 59 || t.second > 59) {
        throw ConfigError("time-of-day out of range: '" + std::string(text) + "'");
    }
    return t;
}

std::string TimeOfDay::str() const { return std::format("{:02d}:{:02d}", hour, minute); }

// -- validation ----------------------------------------------------------------

namespace {

[[noreturn]] void fail(const std::string& message) { throw ConfigError(message); }

}  // namespace

void DatabentoConfig::validate() const {
    std::string lower = schema;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (lower != "mbo" && lower != "mbp-10") fail("databento.schema must be 'mbo' or 'mbp-10'");
    if (stype_in != "continuous" && stype_in != "raw_symbol" && stype_in != "parent") {
        fail("databento.stype_in must be continuous, raw_symbol or parent");
    }
    if (stale_after_seconds <= 0) fail("databento.stale_after_seconds must be > 0");
}

void BookConfig::validate() const {
    if (depth < 1) fail("book.depth must be >= 1");
    if (depth > 32) fail("book.depth must be <= 32 (the snapshot's fixed capacity)");
    if (anchor != "microprice" && anchor != "mid") fail("book.anchor must be 'microprice' or 'mid'");
}

void ToxicityConfig::validate() const {
    if (bucket_contracts <= 0) fail("toxicity.bucket_contracts must be > 0");
    if (window_buckets < 1 || history_buckets < window_buckets) {
        fail("toxicity.history_buckets must be >= window_buckets >= 1");
    }
    if (unknown_side != "split" && unknown_side != "ignore") fail("toxicity.unknown_side must be 'split' or 'ignore'");
    const std::array<std::pair<double, double>, 3> pairs{{
        {elevated_enter, elevated_exit}, {toxic_enter, toxic_exit}, {extreme_enter, extreme_exit}}};
    double last_enter = 0.0;
    for (const auto& [enter, exit] : pairs) {
        if (!(0.0 < exit && exit <= enter && enter <= 1.0)) {
            fail(std::format("each toxicity threshold needs 0 < exit <= enter <= 1; got enter={}, exit={}",
                             fmt::repr(enter), fmt::repr(exit)));
        }
        if (enter < last_enter) fail("toxicity enter thresholds must be ascending");
        last_enter = enter;
    }
    for (const auto& [table, name] : {std::pair{&spread_multiplier, "spread"}, std::pair{&size_multiplier, "size"}}) {
        std::vector<std::string> missing;
        for (const char* level : TOXICITY_LEVELS) {
            if (!table->contains(level)) missing.emplace_back(level);
        }
        if (!missing.empty()) {
            std::sort(missing.begin(), missing.end());
            std::string list;
            for (const auto& m : missing) list += (list.empty() ? "'" : ", '") + m + "'";
            fail(std::format("toxicity.{}_multiplier is missing [{}]", name, list));
        }
        for (const auto& [_, v] : *table) {
            if (v < 0) fail(std::format("toxicity.{}_multiplier values must be >= 0", name));
        }
    }
    if (extreme_action != "pull" && extreme_action != "reduce_only") {
        fail("toxicity.extreme_action must be 'pull' or 'reduce_only'");
    }
    if (min_dwell_buckets < 0) fail("toxicity.min_dwell_buckets must be >= 0");
}

double ToxicityConfig::spread_multiplier_for(ToxicityLevel level) const {
    const auto it = spread_multiplier.find(to_string(level));
    return it == spread_multiplier.end() ? 1.0 : it->second;
}

double ToxicityConfig::size_multiplier_for(ToxicityLevel level) const {
    const auto it = size_multiplier.find(to_string(level));
    return it == size_multiplier.end() ? 1.0 : it->second;
}

void FlowConfig::validate() const {
    if (ofi_window_ms <= 0) fail("flow.ofi_window_ms must be > 0");
    if (depletion_window_ms <= 0) fail("flow.depletion_window_ms must be > 0");
    if (run_decay_seconds <= 0) fail("flow.run_decay_seconds must be > 0");
    if (run_saturation < 1) fail("flow.run_saturation must be >= 1");
}

void QuotingConfig::validate() const {
    if (gamma <= 0) fail("quoting.gamma must be > 0");
    if (kappa <= 0) fail("quoting.kappa must be > 0");
    if (horizon_seconds <= 0) fail("quoting.horizon_seconds must be > 0");
    if (vol_halflife_seconds <= 0) fail("quoting.vol_halflife_seconds must be > 0");
    if (vol_sample_ms <= 0) fail("quoting.vol_sample_ms must be > 0");
    if (vol_floor <= 0) fail("quoting.vol_floor must be > 0");
    if (vol_ceiling < vol_floor) fail("quoting.vol_ceiling < vol_floor");
    if (min_half_spread_ticks < 0.5) fail("quoting.min_half_spread_ticks must be >= 0.5");
    if (behind_best_ticks < 0) fail("quoting.behind_best_ticks must be >= 0");
    if (max_behind_ticks < behind_best_ticks) fail("quoting.max_behind_ticks < behind_best_ticks");
    if (base_size < 1 || max_size < base_size) fail("quoting.max_size >= base_size >= 1 required");
    if (skew_ofi_ticks < 0) fail("quoting.skew_ofi_ticks must be >= 0");
    if (skew_depletion_ticks < 0) fail("quoting.skew_depletion_ticks must be >= 0");
    if (skew_run_ticks < 0) fail("quoting.skew_run_ticks must be >= 0");
}

void ExecutionConfig::validate() const {
    if (!(0 < max_messages_per_second && max_messages_per_second <= IBKR_MESSAGE_CEILING)) {
        fail(std::format("execution.max_messages_per_second must be in (0, {}]", IBKR_MESSAGE_CEILING));
    }
    if (burst < 1) fail("execution.burst must be >= 1");
    if (requote_min_interval_ms < 0 || requote_price_ticks < 0) fail("execution.requote_* must be >= 0");
    if (!(0 <= requote_size_fraction && requote_size_fraction <= 1)) {
        fail("execution.requote_size_fraction must be in [0, 1]");
    }
    if (poll_positions_seconds <= 0 || poll_account_seconds <= 0) fail("execution.poll_*_seconds must be > 0");
    if (ack_timeout_seconds <= 0) fail("execution.ack_timeout_seconds must be > 0");
}

void RiskConfig::validate() const {
    if (max_position < 1) fail("risk.max_position must be >= 1");
    if (!(0 <= reduce_only_position && reduce_only_position <= max_position)) {
        fail("risk.reduce_only_position must be in [0, max_position]");
    }
    if (daily_loss_limit_usd <= 0) fail("risk.daily_loss_limit_usd must be > 0");
    if (flatten_cross_ticks < 0) fail("risk.flatten_cross_ticks must be >= 0");
    if (!(0 < max_margin_utilisation && max_margin_utilisation <= 1)) {
        fail("risk.max_margin_utilisation must be in (0, 1]");
    }
    if (stale_book_cancel_seconds <= 0) fail("risk.stale_book_cancel_seconds must be > 0");
    if (max_sigma < 0) fail("risk.max_sigma must be >= 0 (0 turns the ceiling off)");
}

void IBKRConfig::validate() const {
    if (connect_timeout <= 0) fail("ibkr.connect_timeout must be > 0");
}

void LiveConfig::validate() const {
    if (decision_interval_ms <= 0) fail("live.decision_interval_ms must be > 0");
    if (quote_end <= quote_start) fail("live.quote_end must be after quote_start");
    if (reconnect_backoff_seconds <= 0) fail("live.reconnect_backoff_seconds must be > 0");
    if (max_reconnect_backoff_seconds < reconnect_backoff_seconds) {
        fail("live.max_reconnect_backoff_seconds < reconnect_backoff_seconds");
    }
    if (max_reconnect_attempts && *max_reconnect_attempts < 1) {
        fail("live.max_reconnect_attempts must be >= 1 or null");
    }
    if (snapshot_seconds <= 0) fail("live.snapshot_seconds must be > 0");
}

void ReplayConfig::validate() const {
    if (source != "dbn" && source != "synthetic") fail("replay.source must be 'dbn' or 'synthetic'");
    if (source == "dbn" && (!path || path->empty())) fail("replay.path must be set when replay.source == 'dbn'");
    if (synthetic_seconds <= 0) fail("replay.synthetic_seconds must be > 0");
    if (!(0 <= synthetic_toxic_fraction && synthetic_toxic_fraction <= 1)) {
        fail("replay.synthetic_toxic_fraction must be in [0, 1]");
    }
    if (queue_position != "back" && queue_position != "front") fail("replay.queue_position must be 'back' or 'front'");
}

void Config::validate() const {
    get_product(product);
    databento.validate();
    book.validate();
    toxicity.validate();
    flow.validate();
    quoting.validate();
    execution.validate();
    risk.validate();
    ibkr.validate();
    live.validate();
    replay.validate();
    if (quoting.max_size > risk.max_position) {
        fail("quoting.max_size exceeds risk.max_position: a single fill would breach the inventory cap");
    }
}

// -- loading -------------------------------------------------------------------

namespace {

// Reads one section's keys, refusing any it does not know -- a typo in a
// config is an error, never a silently ignored setting.
class Section {
public:
    Section(const YAML::Node& node, const char* name) : node_(node), name_(name) {}

    template <typename T>
    void read(const char* key, T& field) {
        known_.insert(key);
        if (!node_ || !node_.IsMap()) return;
        const YAML::Node child = node_[key];
        if (!child) return;
        try {
            convert(child, field);
        } catch (const ConfigError&) {
            throw;
        } catch (const std::exception& exc) {
            fail(std::format("{}.{}: {}", name_, key, exc.what()));
        }
    }

    void finish() const {
        if (!node_) return;
        if (!node_.IsMap()) fail(std::format("{} must be a mapping", name_));
        std::set<std::string> unknown;
        for (const auto& item : node_) {
            const std::string key = item.first.as<std::string>();
            if (!known_.contains(key)) unknown.insert(key);
        }
        if (!unknown.empty()) {
            std::string list;
            for (const auto& key : unknown) list += (list.empty() ? "" : ", ") + key;
            fail(std::format("unknown {} keys: {}", name_, list));
        }
    }

private:
    static void convert(const YAML::Node& node, std::string& out) { out = node.as<std::string>(); }
    static void convert(const YAML::Node& node, bool& out) { out = node.as<bool>(); }
    static void convert(const YAML::Node& node, double& out) { out = node.as<double>(); }
    static void convert(const YAML::Node& node, int& out) {
        try {
            out = node.as<int>();
        } catch (const YAML::Exception&) {
            const double value = node.as<double>();
            if (std::floor(value) != value) throw ConfigError("expected an integer");
            out = static_cast<int>(value);
        }
    }
    static void convert(const YAML::Node& node, std::optional<std::string>& out) {
        if (node.IsNull()) {
            out.reset();
        } else {
            out = node.as<std::string>();
        }
    }
    static void convert(const YAML::Node& node, std::optional<int>& out) {
        if (node.IsNull()) {
            out.reset();
        } else {
            int value = 0;
            convert(node, value);
            out = value;
        }
    }
    static void convert(const YAML::Node& node, TimeOfDay& out) {
        if (!node.IsScalar()) throw ConfigError("expected a time of day as 'HH:MM'");
        out = TimeOfDay::parse(node.as<std::string>());
    }
    static void convert(const YAML::Node& node, std::map<std::string, double>& out) {
        if (!node.IsMap()) throw ConfigError("expected a mapping of level -> multiplier");
        std::map<std::string, double> table;
        for (const auto& item : node) table[item.first.as<std::string>()] = item.second.as<double>();
        out = std::move(table);
    }

    const YAML::Node node_;
    const char* name_;
    std::set<std::string> known_;
};

}  // namespace

Config Config::from_node(const YAML::Node& root) {
    Config cfg;
    if (!root || root.IsNull()) return cfg;  // an empty file is the defaults
    if (!root.IsMap()) fail("the config must be a mapping");
    static const std::set<std::string> top_level = {
        "product", "databento", "book", "toxicity", "flow", "quoting",
        "execution", "risk", "ibkr", "live", "replay",
    };
    std::set<std::string> unknown;
    for (const auto& item : root) {
        const std::string key = item.first.as<std::string>();
        if (!top_level.contains(key)) unknown.insert(key);
    }
    if (!unknown.empty()) {
        std::string list;
        for (const auto& key : unknown) list += (list.empty() ? "" : ", ") + key;
        fail("unknown config keys: " + list);
    }
    if (const YAML::Node p = root["product"]) cfg.product = p.as<std::string>();

    {
        Section s(root["databento"], "DatabentoConfig");
        s.read("api_key_env", cfg.databento.api_key_env);
        s.read("dataset", cfg.databento.dataset);
        s.read("schema", cfg.databento.schema);
        s.read("symbol", cfg.databento.symbol);
        s.read("stype_in", cfg.databento.stype_in);
        s.read("reconnect", cfg.databento.reconnect);
        s.read("snapshot", cfg.databento.snapshot);
        s.read("stale_after_seconds", cfg.databento.stale_after_seconds);
        s.finish();
    }
    {
        Section s(root["book"], "BookConfig");
        s.read("depth", cfg.book.depth);
        s.read("anchor", cfg.book.anchor);
        s.finish();
    }
    {
        Section s(root["toxicity"], "ToxicityConfig");
        s.read("enabled", cfg.toxicity.enabled);
        s.read("bucket_contracts", cfg.toxicity.bucket_contracts);
        s.read("window_buckets", cfg.toxicity.window_buckets);
        s.read("history_buckets", cfg.toxicity.history_buckets);
        s.read("unknown_side", cfg.toxicity.unknown_side);
        s.read("warmup_buckets", cfg.toxicity.warmup_buckets);
        s.read("elevated_enter", cfg.toxicity.elevated_enter);
        s.read("elevated_exit", cfg.toxicity.elevated_exit);
        s.read("toxic_enter", cfg.toxicity.toxic_enter);
        s.read("toxic_exit", cfg.toxicity.toxic_exit);
        s.read("extreme_enter", cfg.toxicity.extreme_enter);
        s.read("extreme_exit", cfg.toxicity.extreme_exit);
        s.read("min_dwell_buckets", cfg.toxicity.min_dwell_buckets);
        s.read("spread_multiplier", cfg.toxicity.spread_multiplier);
        s.read("size_multiplier", cfg.toxicity.size_multiplier);
        s.read("extreme_action", cfg.toxicity.extreme_action);
        s.finish();
    }
    {
        Section s(root["flow"], "FlowConfig");
        s.read("ofi_window_ms", cfg.flow.ofi_window_ms);
        s.read("depletion_window_ms", cfg.flow.depletion_window_ms);
        s.read("run_saturation", cfg.flow.run_saturation);
        s.read("run_decay_seconds", cfg.flow.run_decay_seconds);
        s.finish();
    }
    {
        Section s(root["quoting"], "QuotingConfig");
        s.read("gamma", cfg.quoting.gamma);
        s.read("kappa", cfg.quoting.kappa);
        s.read("horizon_seconds", cfg.quoting.horizon_seconds);
        s.read("vol_halflife_seconds", cfg.quoting.vol_halflife_seconds);
        s.read("vol_sample_ms", cfg.quoting.vol_sample_ms);
        s.read("vol_floor", cfg.quoting.vol_floor);
        s.read("vol_ceiling", cfg.quoting.vol_ceiling);
        s.read("min_half_spread_ticks", cfg.quoting.min_half_spread_ticks);
        s.read("behind_best_ticks", cfg.quoting.behind_best_ticks);
        s.read("max_behind_ticks", cfg.quoting.max_behind_ticks);
        s.read("base_size", cfg.quoting.base_size);
        s.read("max_size", cfg.quoting.max_size);
        s.read("skew_ofi_ticks", cfg.quoting.skew_ofi_ticks);
        s.read("skew_depletion_ticks", cfg.quoting.skew_depletion_ticks);
        s.read("skew_run_ticks", cfg.quoting.skew_run_ticks);
        s.finish();
    }
    {
        Section s(root["execution"], "ExecutionConfig");
        s.read("max_messages_per_second", cfg.execution.max_messages_per_second);
        s.read("burst", cfg.execution.burst);
        s.read("requote_min_interval_ms", cfg.execution.requote_min_interval_ms);
        s.read("requote_price_ticks", cfg.execution.requote_price_ticks);
        s.read("requote_size_fraction", cfg.execution.requote_size_fraction);
        s.read("poll_positions_seconds", cfg.execution.poll_positions_seconds);
        s.read("poll_account_seconds", cfg.execution.poll_account_seconds);
        s.read("ack_timeout_seconds", cfg.execution.ack_timeout_seconds);
        s.finish();
    }
    {
        Section s(root["risk"], "RiskConfig");
        s.read("max_position", cfg.risk.max_position);
        s.read("reduce_only_position", cfg.risk.reduce_only_position);
        s.read("daily_loss_limit_usd", cfg.risk.daily_loss_limit_usd);
        s.read("flatten_on_halt", cfg.risk.flatten_on_halt);
        s.read("flatten_cross_ticks", cfg.risk.flatten_cross_ticks);
        s.read("flatten_outside_hours", cfg.risk.flatten_outside_hours);
        s.read("max_margin_utilisation", cfg.risk.max_margin_utilisation);
        s.read("stale_book_cancel_seconds", cfg.risk.stale_book_cancel_seconds);
        s.read("max_sigma", cfg.risk.max_sigma);
        s.read("kill_file", cfg.risk.kill_file);
        s.finish();
    }
    {
        Section s(root["ibkr"], "IBKRConfig");
        s.read("host", cfg.ibkr.host);
        s.read("port", cfg.ibkr.port);
        s.read("client_id", cfg.ibkr.client_id);
        s.read("account", cfg.ibkr.account);
        s.read("allow_live_trading", cfg.ibkr.allow_live_trading);
        s.read("connect_timeout", cfg.ibkr.connect_timeout);
        s.read("local_symbol", cfg.ibkr.local_symbol);
        s.read("outside_rth", cfg.ibkr.outside_rth);
        s.finish();
    }
    {
        Section s(root["live"], "LiveConfig");
        s.read("journal", cfg.live.journal);
        s.read("journal_dir", cfg.live.journal_dir);
        s.read("decision_interval_ms", cfg.live.decision_interval_ms);
        s.read("quote_start", cfg.live.quote_start);
        s.read("quote_end", cfg.live.quote_end);
        s.read("reconnect", cfg.live.reconnect);
        s.read("reconnect_backoff_seconds", cfg.live.reconnect_backoff_seconds);
        s.read("max_reconnect_backoff_seconds", cfg.live.max_reconnect_backoff_seconds);
        s.read("max_reconnect_attempts", cfg.live.max_reconnect_attempts);
        s.read("heartbeat_seconds", cfg.live.heartbeat_seconds);
        s.read("snapshot_seconds", cfg.live.snapshot_seconds);
        s.finish();
    }
    {
        Section s(root["replay"], "ReplayConfig");
        s.read("source", cfg.replay.source);
        s.read("path", cfg.replay.path);
        s.read("synthetic_seconds", cfg.replay.synthetic_seconds);
        s.read("synthetic_start_price", cfg.replay.synthetic_start_price);
        s.read("synthetic_seed", cfg.replay.synthetic_seed);
        s.read("synthetic_toxic_fraction", cfg.replay.synthetic_toxic_fraction);
        s.read("queue_position", cfg.replay.queue_position);
        s.read("out_dir", cfg.replay.out_dir);
        s.finish();
    }
    cfg.validate();
    return cfg;
}

Config Config::from_yaml_string(const std::string& text) {
    try {
        return from_node(YAML::Load(text));
    } catch (const YAML::Exception& exc) {
        fail(std::string("could not parse the config: ") + exc.what());
    }
}

Config Config::from_yaml(const std::string& path) {
    std::ifstream in(path);
    if (!in) fail("cannot read config file " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return from_yaml_string(buffer.str());
}

// -- emitting ------------------------------------------------------------------

namespace {

// Quotes a string exactly when PyYAML would: when the plain scalar would
// resolve to something other than a string under YAML 1.1's implicit
// resolvers (int, float, bool, null, timestamp, incl. sexagesimal ``15:00``
// but not ``08:45``), or when it carries characters a plain scalar may
// not. So ``harvester config`` here writes the file Python's writes.
std::string yaml_string(const std::string& value) {
    static const std::regex yaml_int(
        R"(^[-+]?(0b[0-1_]+|0[0-7_]+|(0|[1-9][0-9_]*)|0x[0-9a-fA-F_]+|[1-9][0-9_]*(:[0-5]?[0-9])+)$)");
    static const std::regex yaml_float(
        R"(^([-+]?([0-9][0-9_]*)?\.[0-9_]*([eE][-+][0-9]+)?|[-+]?[0-9][0-9_]*(:[0-5]?[0-9])+\.[0-9_]*|[-+]?\.(inf|Inf|INF)|\.(nan|NaN|NAN))$)");
    static const std::regex yaml_timestamp(R"(^[0-9]{4}-[0-9]{1,2}-[0-9]{1,2}([Tt ].*)?$)");
    static const std::regex yaml_bool(R"(^(yes|Yes|YES|no|No|NO|true|True|TRUE|false|False|FALSE|on|On|ON|off|Off|OFF)$)");
    static const std::regex yaml_null(R"(^(~|null|Null|NULL|)$)");
    const bool resolves = std::regex_match(value, yaml_int) || std::regex_match(value, yaml_float) ||
                          std::regex_match(value, yaml_timestamp) || std::regex_match(value, yaml_bool) ||
                          std::regex_match(value, yaml_null);
    const bool special = value.empty() || std::strchr("-?:,[]{}#&*!|>'\"%@`", value[0]) != nullptr ||
                         value.find(": ") != std::string::npos || value.find(" #") != std::string::npos ||
                         value.back() == ':' || value.back() == ' ' || value.find_first_of("\n\t") != std::string::npos;
    if (!resolves && !special) return value;
    std::string quoted = "'";
    for (const char c : value) {
        if (c == '\'') quoted += '\'';
        quoted += c;
    }
    return quoted + "'";
}

class Emitter {
public:
    void section(const char* name) { out_ += std::string(name) + ":\n"; }
    void kv(const char* key, const std::string& value) { line(key, yaml_string(value)); }
    void kv(const char* key, const char* value) { kv(key, std::string(value)); }
    void kv(const char* key, bool value) { line(key, value ? "true" : "false"); }
    void kv(const char* key, int value) { line(key, std::to_string(value)); }
    void kv(const char* key, double value) { line(key, fmt::repr(value)); }
    void kv(const char* key, const TimeOfDay& value) { line(key, yaml_string(value.str())); }
    void kv(const char* key, const std::optional<std::string>& value) { line(key, value ? yaml_string(*value) : "null"); }
    void kv(const char* key, const std::optional<int>& value) { line(key, value ? std::to_string(*value) : "null"); }
    void table(const char* key, const std::map<std::string, double>& value) {
        out_ += "  " + std::string(key) + ":\n";
        std::set<std::string> written;
        for (const char* level : TOXICITY_LEVELS) {
            if (const auto it = value.find(level); it != value.end()) {
                out_ += "    " + std::string(level) + ": " + fmt::repr(it->second) + "\n";
                written.insert(level);
            }
        }
        for (const auto& [k, v] : value) {
            if (!written.contains(k)) out_ += "    " + yaml_string(k) + ": " + fmt::repr(v) + "\n";
        }
    }
    std::string take() { return std::move(out_); }

private:
    void line(const char* key, const std::string& text) { out_ += "  " + std::string(key) + ": " + text + "\n"; }
    std::string out_;
};

}  // namespace

std::string Config::to_yaml_string() const {
    Emitter e;
    std::string out = "product: " + yaml_string(product) + "\n";
    e.section("databento");
    e.kv("api_key_env", databento.api_key_env);
    e.kv("dataset", databento.dataset);
    e.kv("schema", databento.schema);
    e.kv("symbol", databento.symbol);
    e.kv("stype_in", databento.stype_in);
    e.kv("reconnect", databento.reconnect);
    e.kv("snapshot", databento.snapshot);
    e.kv("stale_after_seconds", databento.stale_after_seconds);
    e.section("book");
    e.kv("depth", book.depth);
    e.kv("anchor", book.anchor);
    e.section("toxicity");
    e.kv("enabled", toxicity.enabled);
    e.kv("bucket_contracts", toxicity.bucket_contracts);
    e.kv("window_buckets", toxicity.window_buckets);
    e.kv("history_buckets", toxicity.history_buckets);
    e.kv("unknown_side", toxicity.unknown_side);
    e.kv("warmup_buckets", toxicity.warmup_buckets);
    e.kv("elevated_enter", toxicity.elevated_enter);
    e.kv("elevated_exit", toxicity.elevated_exit);
    e.kv("toxic_enter", toxicity.toxic_enter);
    e.kv("toxic_exit", toxicity.toxic_exit);
    e.kv("extreme_enter", toxicity.extreme_enter);
    e.kv("extreme_exit", toxicity.extreme_exit);
    e.kv("min_dwell_buckets", toxicity.min_dwell_buckets);
    e.table("spread_multiplier", toxicity.spread_multiplier);
    e.table("size_multiplier", toxicity.size_multiplier);
    e.kv("extreme_action", toxicity.extreme_action);
    e.section("flow");
    e.kv("ofi_window_ms", flow.ofi_window_ms);
    e.kv("depletion_window_ms", flow.depletion_window_ms);
    e.kv("run_saturation", flow.run_saturation);
    e.kv("run_decay_seconds", flow.run_decay_seconds);
    e.section("quoting");
    e.kv("gamma", quoting.gamma);
    e.kv("kappa", quoting.kappa);
    e.kv("horizon_seconds", quoting.horizon_seconds);
    e.kv("vol_halflife_seconds", quoting.vol_halflife_seconds);
    e.kv("vol_sample_ms", quoting.vol_sample_ms);
    e.kv("vol_floor", quoting.vol_floor);
    e.kv("vol_ceiling", quoting.vol_ceiling);
    e.kv("min_half_spread_ticks", quoting.min_half_spread_ticks);
    e.kv("behind_best_ticks", quoting.behind_best_ticks);
    e.kv("max_behind_ticks", quoting.max_behind_ticks);
    e.kv("base_size", quoting.base_size);
    e.kv("max_size", quoting.max_size);
    e.kv("skew_ofi_ticks", quoting.skew_ofi_ticks);
    e.kv("skew_depletion_ticks", quoting.skew_depletion_ticks);
    e.kv("skew_run_ticks", quoting.skew_run_ticks);
    e.section("execution");
    e.kv("max_messages_per_second", execution.max_messages_per_second);
    e.kv("burst", execution.burst);
    e.kv("requote_min_interval_ms", execution.requote_min_interval_ms);
    e.kv("requote_price_ticks", execution.requote_price_ticks);
    e.kv("requote_size_fraction", execution.requote_size_fraction);
    e.kv("poll_positions_seconds", execution.poll_positions_seconds);
    e.kv("poll_account_seconds", execution.poll_account_seconds);
    e.kv("ack_timeout_seconds", execution.ack_timeout_seconds);
    e.section("risk");
    e.kv("max_position", risk.max_position);
    e.kv("reduce_only_position", risk.reduce_only_position);
    e.kv("daily_loss_limit_usd", risk.daily_loss_limit_usd);
    e.kv("flatten_on_halt", risk.flatten_on_halt);
    e.kv("flatten_cross_ticks", risk.flatten_cross_ticks);
    e.kv("flatten_outside_hours", risk.flatten_outside_hours);
    e.kv("max_margin_utilisation", risk.max_margin_utilisation);
    e.kv("stale_book_cancel_seconds", risk.stale_book_cancel_seconds);
    e.kv("max_sigma", risk.max_sigma);
    e.kv("kill_file", risk.kill_file);
    e.section("ibkr");
    e.kv("host", ibkr.host);
    e.kv("port", ibkr.port);
    e.kv("client_id", ibkr.client_id);
    e.kv("account", ibkr.account);
    e.kv("allow_live_trading", ibkr.allow_live_trading);
    e.kv("connect_timeout", ibkr.connect_timeout);
    e.kv("local_symbol", ibkr.local_symbol);
    e.kv("outside_rth", ibkr.outside_rth);
    e.section("live");
    e.kv("journal", live.journal);
    e.kv("journal_dir", live.journal_dir);
    e.kv("decision_interval_ms", live.decision_interval_ms);
    e.kv("quote_start", live.quote_start);
    e.kv("quote_end", live.quote_end);
    e.kv("reconnect", live.reconnect);
    e.kv("reconnect_backoff_seconds", live.reconnect_backoff_seconds);
    e.kv("max_reconnect_backoff_seconds", live.max_reconnect_backoff_seconds);
    e.kv("max_reconnect_attempts", live.max_reconnect_attempts);
    e.kv("heartbeat_seconds", live.heartbeat_seconds);
    e.kv("snapshot_seconds", live.snapshot_seconds);
    e.section("replay");
    e.kv("source", replay.source);
    e.kv("path", replay.path);
    e.kv("synthetic_seconds", replay.synthetic_seconds);
    e.kv("synthetic_start_price", replay.synthetic_start_price);
    e.kv("synthetic_seed", replay.synthetic_seed);
    e.kv("synthetic_toxic_fraction", replay.synthetic_toxic_fraction);
    e.kv("queue_position", replay.queue_position);
    e.kv("out_dir", replay.out_dir);
    return out + e.take();
}

void Config::to_yaml(const std::string& path) const {
    std::ofstream out(path);
    if (!out) fail("cannot write config file " + path);
    out << to_yaml_string();
}

}  // namespace harvester
