// Command line entry point.
//
//     harvester replay  -c configs/es_replay.yaml                 # generated market
//     harvester replay  -c configs/es_replay.yaml --control       # ...and the gate/skew/both controls
//     harvester replay  -c configs/es_replay.yaml --dbn es.dbn    # a recorded tape
//     harvester fetch   -c configs/es_replay.yaml --start 2026-09-08T13:30 --end 2026-09-08T20:00 -o es.dbn
//     harvester doctor  -c configs/es_paper.yaml                  # preflight, places nothing
//     harvester live    -c configs/es_paper.yaml --dry-run        # real book, simulated fills
//     harvester live    -c configs/es_paper.yaml                  # routes to IBKR
//     harvester report  -c configs/es_paper.yaml
//     harvester config  -o configs/mine.yaml
#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "harvester/config.hpp"
#include "harvester/databento/api.hpp"
#include "harvester/execution/ibkr.hpp"
#include "harvester/live/journal.hpp"
#include "harvester/live/runner.hpp"
#include "harvester/replay/runner.hpp"
#include "harvester/util/format.hpp"
#include "harvester/util/log.hpp"

#if HARVESTER_WITH_IBKR
#include "harvester/execution/tws_gateway.hpp"
#endif

namespace {

using namespace harvester;

// A small argument parser: ``--flag``, ``--key value``, ``-k value``.
class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) tokens_.emplace_back(argv[i]);
    }

    std::string command() const {
        for (std::size_t i = 0; i < tokens_.size(); ++i) {
            if (tokens_[i] == "-v" || tokens_[i] == "--verbose") continue;
            return tokens_[i];
        }
        return "";
    }

    bool flag(std::string_view name) const { return std::find(tokens_.begin(), tokens_.end(), name) != tokens_.end(); }

    std::optional<std::string> value(std::string_view name, std::string_view alias = "") const {
        for (std::size_t i = 0; i + 1 < tokens_.size(); ++i) {
            if (tokens_[i] == name || (!alias.empty() && tokens_[i] == alias)) return tokens_[i + 1];
        }
        for (const auto& token : tokens_) {
            if (token.starts_with(std::string(name) + "=")) return token.substr(name.size() + 1);
        }
        return std::nullopt;
    }

    std::optional<double> number(std::string_view name, std::string_view alias = "") const {
        const auto text = value(name, alias);
        if (!text) return std::nullopt;
        try {
            return std::stod(*text);
        } catch (const std::exception&) {
            throw std::invalid_argument(std::format("{} expects a number, not '{}'", name, *text));
        }
    }

    bool verbose() const { return flag("-v") || flag("--verbose"); }

private:
    std::vector<std::string> tokens_;
};

Config load(const Args& args) {
    const auto path = args.value("--config", "-c");
    Config cfg = path ? Config::from_yaml(*path) : Config{};
    if (const auto dbn = args.value("--dbn")) {
        cfg.replay.source = "dbn";
        cfg.replay.path = *dbn;
    }
    if (const auto seconds = args.number("--seconds")) cfg.replay.synthetic_seconds = *seconds;
    if (const auto seed = args.number("--seed")) cfg.replay.synthetic_seed = static_cast<int>(*seed);
    if (const auto toxic = args.number("--toxic")) cfg.replay.synthetic_toxic_fraction = *toxic;
    cfg.validate();
    return cfg;
}

std::string config_name(const Args& args) { return args.value("--config", "-c").value_or("<config>"); }

// -- replay ------------------------------------------------------------------

// The control arms. ``--control`` used to turn the toxicity gate off and
// the flow skews to zero in one go and print a single difference, which is
// the two changes added together: the skews had never been run as an arm
// of their own. They are not the same kind of thing -- the gate widens the
// spread and cuts size in a busy tape, the skews move the quote sideways
// and are what unsticks it -- so each gets its own run, and ``both`` is
// still there because it is what the old flag measured.
struct ControlArm {
    const char* name;
    const char* label;   // what the difference is against
    const char* lead;    // what the difference is called
    bool gate;           // leave the toxicity gate on
    bool skew;           // leave the flow skews on
};

constexpr ControlArm CONTROL_ARMS[] = {
    {"gate", "gate off, skews intact, same tape", "gate", false, true},
    {"skew", "skews at zero, gate on, same tape", "skews", true, false},
    {"both", "gate off, no skew, same tape", "signals", false, false},
};

void run_control(const Config& cfg, const ReplayResult& result, const ControlArm& arm) {
    Config control = cfg;
    if (!arm.gate) control.toxicity.enabled = false;
    if (!arm.skew) {
        control.quoting.skew_ofi_ticks = control.quoting.skew_depletion_ticks = 0.0;
        control.quoting.skew_run_ticks = 0.0;
    }
    const ReplayResult base = run_replay(control, nullptr);
    std::cout << std::format("\n--- control: {} ---\n", arm.label) << base.summary() << "\n";
    const double delta = result.net() - base.net();
    std::cout << std::format("\n{}: net ${}{} against the control ({} vs {} fills)\n", arm.lead,
                             delta >= 0 ? "+" : "", fmt::commas(delta), result.fills, base.fills);
}

int cmd_replay(const Args& args) {
    const Config cfg = load(args);
    std::unique_ptr<SessionJournal> journal;
    if (!args.flag("--no-journal")) {
        journal = std::make_unique<SessionJournal>(args.value("--out", "-o").value_or(cfg.replay.out_dir));
    }
    const ReplayResult result = run_replay(cfg, journal.get());
    std::cout << "\n" << result.summary() << "\n";
    // ``--control`` runs every arm; ``--control gate|skew|both`` runs one.
    std::optional<std::string> which = args.value("--control");
    if (which && (which->empty() || which->starts_with("-"))) which.reset();
    if (args.flag("--control") || which) {
        const std::string wanted = which.value_or("all");
        bool ran = false;
        for (const ControlArm& arm : CONTROL_ARMS) {
            if (wanted == "all" || wanted == arm.name) {
                run_control(cfg, result, arm);
                ran = true;
            }
        }
        if (!ran) {
            std::cerr << std::format("--control takes gate, skew, both or nothing (all three), not '{}'\n", wanted);
            return 2;
        }
    }
    if (journal) {
        std::cout << "\njournal written to " << std::filesystem::absolute(journal->directory()).string() << "\n";
    }
    return 0;
}

// -- fetch --------------------------------------------------------------------

int cmd_fetch(const Args& args) {
    const Config cfg = load(args);
    const auto start = args.value("--start");
    const auto end = args.value("--end");
    const auto out = args.value("--out", "-o");
    if (!start || !end || !out) {
        std::cerr << "fetch needs --start, --end and -o/--out\n";
        return 2;
    }
    const char* key = std::getenv(cfg.databento.api_key_env.c_str());
    if (key == nullptr || *key == '\0') {
        std::cerr << "set " << cfg.databento.api_key_env << "\n";
        return 2;
    }
    std::cout << std::format("fetching {} {} {} {} -> {} to {}\n", cfg.databento.dataset, cfg.databento.symbol,
                             cfg.databento.schema, *start, *end, *out);
    const std::uint64_t size = fetch_dbn(cfg, key, *start, *end, *out);
    std::cout << std::format("wrote {} ({:.1f} MB)\n", *out, static_cast<double>(size) / 1e6);
    return 0;
}

// -- live ---------------------------------------------------------------------

ConnectionFactory gateway_factory(const Config& cfg, const Product& product) {
#if HARVESTER_WITH_IBKR
    return [&cfg, &product] {
        return std::make_unique<IbkrConnection>(cfg, product, std::make_shared<TwsGateway>());
    };
#else
    (void)cfg;
    (void)product;
    return nullptr;
#endif
}

int cmd_live(const Args& args) {
    const Config cfg = load(args);
    const bool dry_run = args.flag("--dry-run");
    if (!dry_run && !cfg.ibkr.allow_live_trading) {
        std::cerr << "Refusing to route orders: ibkr.allow_live_trading is False.\n"
                     "Run with --dry-run to quote against the real book with simulated fills, or set\n"
                     "ibkr.allow_live_trading: true in the config once you are connected to the\n"
                     "paper account you intend to trade.\n";
        return 2;
    }
#if !HARVESTER_WITH_IBKR
    if (!dry_run) {
        std::cerr << "This build has no IBKR router (HARVESTER_WITH_IBKR is off); only --dry-run can run.\n";
        return 2;
    }
#endif
    std::optional<std::int64_t> max_cycles;
    if (const auto n = args.number("--max-cycles")) max_cycles = static_cast<std::int64_t>(*n);
    const Product& product = cfg.instrument();
    LiveRunner runner(cfg, dry_run, nullptr, gateway_factory(cfg, product));
    Pipeline& pipeline = runner.run(max_cycles);
    const std::optional<double> anchor = pipeline.last_snapshot ? pipeline.last_snapshot->microprice() : std::nullopt;
    std::cout << "\n" << pipeline.inventory.describe(anchor) << "\n" << pipeline.markouts.describe() << "\n";
    if (cfg.live.journal) {
        std::cout << std::format("\nThe full record is under {}.\n  harvester report -c {}\n",
                                 std::filesystem::absolute(cfg.live.journal_dir).string(), config_name(args));
    }
    return 0;
}

// -- doctor -------------------------------------------------------------------

struct Check {
    std::string name;
    bool ok;
    std::string detail;
};

int print_checks(const std::vector<Check>& checks) {
    int failed = 0;
    for (const auto& c : checks) {
        std::cout << std::format("  [{}] {}{}\n", c.ok ? "ok  " : "FAIL", c.name, c.detail.empty() ? "" : ": " + c.detail);
        failed += c.ok ? 0 : 1;
    }
    return failed;
}

int cmd_doctor(const Args& args) {
    const Config cfg = load(args);
    const Product& product = cfg.instrument();
    const double feed_seconds = args.number("--feed-seconds").value_or(15.0);
    std::vector<Check> checks;
    auto check = [&](std::string name, bool ok, std::string detail = "") {
        checks.push_back({std::move(name), ok, std::move(detail)});
        return ok;
    };

    // -- the feed --
    const char* key = std::getenv(cfg.databento.api_key_env.c_str());
    check(std::format("{} is set", cfg.databento.api_key_env), key != nullptr && *key != '\0');
    std::optional<std::string> raw_symbol;
    if (key != nullptr && *key != '\0') {
        if (!databento_supported()) {
            check("databento feed", false, "this build has no Databento client (HARVESTER_WITH_DATABENTO is off)");
        } else {
            std::shared_ptr<RecordFeed> feed;
            try {
                feed = make_databento_feed(cfg.databento, cfg.live, product, cfg.book.depth);
                feed->start();
                raw_symbol = feed->wait_for_symbol(20.0);
                check("feed named the contract", raw_symbol.has_value(),
                      raw_symbol.value_or(std::format("no symbol mapping for {} in 20s", cfg.databento.symbol)));
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(feed_seconds);
                BookSnapshot snapshot = feed->snapshot();
                while (std::chrono::steady_clock::now() < deadline && !snapshot.two_sided()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    snapshot = feed->snapshot();
                }
                check("book is two-sided and complete", snapshot.two_sided(),
                      snapshot.two_sided()
                          ? std::format("{}@{:.2f} / {}@{:.2f} microprice {:.3f}", snapshot.best_bid()->size,
                                        snapshot.best_bid()->price, snapshot.best_ask()->size,
                                        snapshot.best_ask()->price, *snapshot.microprice())
                          : std::format("{} bid / {} ask levels, complete={} after {:.0f}s (outside the session this "
                                        "is normal; the MBO snapshot may also take a moment)",
                                        snapshot.bids().size(), snapshot.asks().size(), snapshot.complete,
                                        feed_seconds));
                const std::vector<Trade> trades = feed->take_trades();
                const auto flagged = std::count_if(trades.begin(), trades.end(), [](const Trade& t) { return t.aggressor != 0; });
                check("tape carries the aggressor flag", trades.empty() || flagged > 0,
                      trades.empty() ? "no trades yet -- re-run during the session"
                                     : std::format("{} trades in {:.0f}s, {} with an aggressor", trades.size(),
                                                   feed_seconds, flagged));
                check("feed errors", feed->errors() == 0, std::format("{} errors", feed->errors()));
            } catch (const std::exception& exc) {  // this is the report
                check("databento feed", false, exc.what());
            }
            if (feed) feed->close();
        }
    }

    // -- the router --
#if HARVESTER_WITH_IBKR
    {
        auto conn = std::make_unique<IbkrConnection>(cfg, product, std::make_shared<TwsGateway>());
        try {
            conn->connect();
        } catch (const std::exception& exc) {
            check("connect to IBKR", false, exc.what());
            print_checks(checks);
            return 1;
        }
        check("connect to IBKR", true, std::format("{}:{}", cfg.ibkr.host, cfg.ibkr.port));
        const bool paper = is_paper_account(conn->account());
        check("account is paper", paper || cfg.ibkr.allow_live_trading,
              std::format("{} ({})", conn->account(), paper ? "paper" : "LIVE"));
        try {
            const IbContract& contract = conn->qualify(raw_symbol);
            check("qualified the contract", true,
                  std::format("{} (feed says {})", contract.local_symbol, raw_symbol.value_or("n/a")));
            if (raw_symbol && contract.local_symbol != *raw_symbol) {
                check("feed and router agree on the month", false,
                      std::format("feed {} vs IBKR {}; set ibkr.local_symbol", *raw_symbol, contract.local_symbol));
            }
        } catch (const std::exception& exc) {
            check("qualified the contract", false, exc.what());
        }
        try {
            const auto report = conn->position();
            std::string foreign;
            for (const auto& f : report.foreign) foreign += (foreign.empty() ? "" : ", ") + f;
            check("no foreign positions", report.foreign.empty(), foreign.empty() ? "none" : foreign);
            check("position on the contract", true,
                  report.quantity != 0 ? std::format("{:+d} @ {:.2f}", report.quantity, report.avg_price) : "flat");
        } catch (const std::exception& exc) {
            check("positions", false, exc.what());
        }
        try {
            const AccountValues values = conn->account_values();
            const auto nav_it = values.find("NetLiquidation");
            const double held = values.contains("FullInitMarginReq") ? values.at("FullInitMarginReq") : 0.0;
            const bool have_nav = nav_it != values.end() && nav_it->second != 0.0;
            check("account values", nav_it != values.end(),
                  have_nav ? std::format("NetLiquidation ${}, initial margin ${} ({} of the {} cap)",
                                         fmt::commas(nav_it->second), fmt::commas(held),
                                         fmt::pct0(held / nav_it->second), fmt::pct0(cfg.risk.max_margin_utilisation))
                           : "no NetLiquidation");
            if (have_nav) {
                const double nav = nav_it->second;
                const double cap_margin = cfg.risk.max_position * product.initial_margin;
                check("position cap sits well inside margin", cap_margin < nav * cfg.risk.max_margin_utilisation,
                      std::format("{} x ${} = ${} vs ${} allowed", cfg.risk.max_position,
                                  fmt::commas(product.initial_margin), fmt::commas(cap_margin),
                                  fmt::commas(nav * cfg.risk.max_margin_utilisation)));
            }
        } catch (const std::exception& exc) {
            check("account values", false, exc.what());
        }
        conn->disconnect();
    }
#else
    check("IBKR router", false, "this build has no IBKR client (build with -DHARVESTER_WITH_IBKR=ON and TWS_API_DIR)");
#endif

    // -- the budget --
    const ExecutionConfig& e = cfg.execution;
    const double per_cycle = 1000.0 / cfg.live.decision_interval_ms;
    check("message budget", e.max_messages_per_second <= 50,
          std::format("{:g}/s of IBKR's 50, burst {}; the loop can decide {:.0f}x/s and re-quote a side at most every "
                      "{:.0f}ms (worst case {:.0f} msg/s before the throttle)",
                      e.max_messages_per_second, e.burst, per_cycle, e.requote_min_interval_ms,
                      2000.0 / e.requote_min_interval_ms));
    const std::filesystem::path journal_dir(cfg.live.journal_dir);
    try {
        std::filesystem::create_directories(journal_dir);
        const std::filesystem::path probe = journal_dir / ".probe";
        {
            std::ofstream out(probe);
            if (!out) throw std::runtime_error("cannot write " + probe.string());
            out << "ok";
        }
        std::filesystem::remove(probe);
        check("journal directory is writable", true, std::filesystem::absolute(journal_dir).string());
    } catch (const std::exception& exc) {
        check("journal directory is writable", false, exc.what());
    }
    check("kill file absent", !std::filesystem::exists(cfg.risk.kill_file), cfg.risk.kill_file);

    if (print_checks(checks) > 0) return 1;
    std::cout << std::format("\nReady.\n  harvester live -c {} --dry-run   # real book, simulated fills\n"
                             "  harvester live -c {}             # routes to the paper account\n",
                             config_name(args), config_name(args));
    return 0;
}

// -- report -------------------------------------------------------------------

std::string plain(const json::Value& v) {
    if (v.is_string()) return v.string();
    if (v.is_number()) {
        const double x = v.number();
        return std::floor(x) == x && std::fabs(x) < 1e15 ? std::format("{}", static_cast<long long>(x)) : fmt::repr(x);
    }
    if (std::holds_alternative<bool>(v.data)) return std::get<bool>(v.data) ? "True" : "False";
    if (v.is_array()) {
        std::string out = "[";
        for (const auto& item : v.array()) out += (out.size() > 1 ? ", " : "") + (item.is_string() ? "'" + item.string() + "'" : plain(item));
        return out + "]";
    }
    return "None";
}

int cmd_report(const Args& args) {
    const Config cfg = load(args);
    const std::string directory = args.value("--dir").value_or(cfg.live.journal_dir);
    const auto fills = read_journal(directory, "fills");
    const auto markouts = read_journal(directory, "markouts");
    const auto snapshots = read_journal(directory, "snapshots");
    const auto events = read_journal(directory, "events");
    std::cout << "journal: " << directory << "\n";
    if (fills.empty()) {
        std::cout << "no fills recorded\n";
    } else {
        double contracts = 0, bought = 0, sold = 0, bought_px = 0, sold_px = 0, fees = 0;
        int n_bought = 0, n_sold = 0;
        std::map<std::string, double> by_level;
        for (const auto& row : fills) {
            const double size = row.number("size").value_or(0);
            const double side = row.number("side").value_or(0);
            const double price = row.number("price").value_or(0);
            contracts += size;
            fees += row.number("fees").value_or(0);
            if (side > 0) {
                bought += size;
                bought_px += price;
                ++n_bought;
            } else if (side < 0) {
                sold += size;
                sold_px += price;
                ++n_sold;
            }
            if (const auto* level = row.get("level"); level && level->is_string()) by_level[level->string()] += size;
        }
        std::cout << std::format("fills: {} ({} contracts): {} bought @ {:.2f} avg, {} sold @ {:.2f} avg; fees ${}\n",
                                 fills.size(), static_cast<long long>(contracts), static_cast<long long>(bought),
                                 bought_px / n_bought, static_cast<long long>(sold), sold_px / n_sold, fmt::commas(fees));
        const auto& last = fills.back();
        std::cout << std::format("final position {:+d}, realised ${}\n",
                                 static_cast<int>(last.number("position").value_or(0)),
                                 fmt::commas(last.number("realised").value_or(0)));
        std::string levels;
        for (const auto& [level, size] : by_level) levels += std::format("{}{} {}", levels.empty() ? "" : ", ", level, static_cast<long long>(size));
        std::cout << "fills by toxicity level: " << levels << "\n";
    }
    if (!markouts.empty()) {
        // The horizon columns, in order of first appearance.
        std::vector<std::string> horizons;
        for (const auto& row : markouts) {
            for (const auto& [key, _] : row.object()) {
                if (key.size() > 1 && key.back() == 's' &&
                    std::all_of(key.begin(), key.end() - 1, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '.'; }) &&
                    std::find(horizons.begin(), horizons.end(), key) == horizons.end()) {
                    horizons.push_back(key);
                }
            }
        }
        std::cout << "markouts, $ per contract, by level at the fill:\n";
        std::map<std::string, std::vector<const json::Value*>> groups;
        for (const auto& row : markouts) {
            if (const auto* level = row.get("level"); level && level->is_string()) groups[level->string()].push_back(&row);
        }
        for (const auto& [level, rows] : groups) {
            double weight = 0, edge = 0;
            std::map<std::string, double> sums;
            for (const auto* row : rows) {
                const double w = row->number("size").value_or(0);
                weight += w;
                edge += row->number("edge").value_or(0) * w;
                for (const auto& h : horizons) {
                    if (const auto v = row->number(h)) sums[h] += *v * w;
                }
            }
            std::string marks;
            for (const auto& h : horizons) marks += std::format("{}{} {:+.2f}", marks.empty() ? "" : "  ", h, sums[h] / weight);
            std::cout << std::format("  {:<9} {:5d} ct  edge {:+.2f}  {}\n", level, static_cast<long long>(weight), edge / weight, marks);
        }
    }
    if (!snapshots.empty()) {
        std::map<std::string, double> counts;
        double quoting = 0, messages = 0;
        for (const auto& row : snapshots) {
            if (const auto* level = row.get("level"); level && level->is_string()) counts[level->string()] += 1;
            const auto* qb = row.get("quote_bid");
            const auto* qa = row.get("quote_ask");
            if ((qb && !qb->is_null()) || (qa && !qa->is_null())) quoting += 1;
            messages = std::max(messages, row.number("messages").value_or(0));
        }
        std::vector<std::pair<std::string, double>> share(counts.begin(), counts.end());
        std::stable_sort(share.begin(), share.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string text;
        for (const auto& [level, n] : share) text += std::format("{}{} {}", text.empty() ? "" : ", ", level, fmt::pct0(n / snapshots.size()));
        std::cout << "time in level: " << text << "\n";
        std::cout << std::format("quoting {} of snapshots; messages sent {}\n", fmt::pct0(quoting / snapshots.size()),
                                 static_cast<long long>(messages));
    }
    if (!events.empty() && args.flag("--show-events")) {
        std::cout << "\nevents\n";
        for (const auto& row : events) {
            std::string rest;
            for (const auto& [key, value] : row.object()) {
                if (key == "ts" || key == "kind" || value.is_null()) continue;
                if (value.is_number() && std::isnan(value.number())) continue;
                rest += std::format("{}{}={}", rest.empty() ? "" : " ", key, plain(value));
            }
            const auto* kind = row.get("kind");
            std::cout << std::format("  {:.3f}  {:<10} {}\n", row.number("ts").value_or(0), kind && kind->is_string() ? kind->string() : "", rest);
        }
    }
    return 0;
}

// -- config -------------------------------------------------------------------

int cmd_config(const Args& args) {
    const std::string out = args.value("--out", "-o").value_or("harvester.yaml");
    Config{}.to_yaml(out);
    std::cout << "wrote defaults to " << out << "\n";
    return 0;
}

void usage() {
    std::cerr << "usage: harvester [-v] {replay,fetch,live,doctor,report,config} [options]\n\n"
                 "Order-flow / toxicity aware market making on CME futures: Databento MDP 3.0 book,\n"
                 "Avellaneda-Stoikov quotes, IBKR execution.\n\n"
                 "  replay  -c CONFIG [--dbn FILE] [--seconds N] [--seed N] [--toxic F] [-o DIR] [--no-journal]\n"
                 "          [--control [gate|skew|both]]   control arms; bare runs all three\n"
                 "  fetch   -c CONFIG --start ISO --end ISO -o FILE\n"
                 "  live    -c CONFIG [--dry-run] [--max-cycles N]\n"
                 "  doctor  -c CONFIG [--feed-seconds S]\n"
                 "  report  -c CONFIG [--dir DIR] [--show-events]\n"
                 "  config  [-o FILE]\n";
}

}  // namespace

int main(int argc, char** argv) {
    // A peer that closes the socket mid-write -- a Databento gateway refusing
    // the session, IBKR dropping the API connection -- otherwise raises
    // SIGPIPE, whose default disposition kills the process outright: no
    // journal flush, no error, just exit 141. Every socket write here already
    // reports failure through its return value, so the signal is noise.
    std::signal(SIGPIPE, SIG_IGN);
    const Args args(argc, argv);
    log::set_level(args.verbose() ? log::Level::Debug : log::Level::Info);
    const std::string command = args.command();
    try {
        if (command == "replay") return cmd_replay(args);
        if (command == "fetch") return cmd_fetch(args);
        if (command == "live") return cmd_live(args);
        if (command == "doctor") return cmd_doctor(args);
        if (command == "report") return cmd_report(args);
        if (command == "config") return cmd_config(args);
        usage();
        return 2;
    } catch (const std::exception& exc) {
        std::cerr << "error: " << exc.what() << "\n";
        return 1;
    }
}
