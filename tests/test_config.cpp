#include <filesystem>
#include <fstream>

#include "helpers.hpp"

using namespace test;

TEST_SUITE("config") {

TEST_CASE("defaults validate and round trip") {
    const Config cfg;
    const std::string dir = temp_dir("config");
    const std::string path = dir + "/c.yaml";
    cfg.to_yaml(path);
    const Config again = Config::from_yaml(path);
    CHECK(again.to_yaml_string() == cfg.to_yaml_string());
    CHECK(again.live.quote_start.hour == 8);
    CHECK(again.live.quote_start.minute == 45);
    CHECK(again.toxicity.spread_multiplier.at("toxic") == 2.5);
    CHECK_FALSE(again.ibkr.account.has_value());
    CHECK_FALSE(again.live.max_reconnect_attempts.has_value());
}

TEST_CASE("unknown keys are rejected") {
    CHECK_THROWS_WITH_AS(Config::from_yaml_string("quoting:\n  gamm: 1.0\n"), doctest::Contains("unknown"), ConfigError);
    CHECK_THROWS_WITH_AS(Config::from_yaml_string("quotes: {}\n"), doctest::Contains("unknown config keys"), ConfigError);
}

TEST_CASE("validation") {
    const std::vector<std::tuple<std::string, std::string>> cases = {
        {"execution:\n  max_messages_per_second: 60.0\n", "50"},
        {"toxicity:\n  toxic_enter: 0.5\n", "exit <= enter"},
        {"toxicity:\n  extreme_action: panic\n", "extreme_action"},
        {"quoting:\n  max_behind_ticks: 0\n", "max_behind_ticks"},
        {"risk:\n  reduce_only_position: 9\n", "reduce_only_position"},
        {"databento:\n  schema: trades\n", "schema"},
        {"live:\n  quote_end: '08:00'\n", "quote_end"},
        {"replay:\n  source: dbn\n", "replay.path"},
    };
    for (const auto& [text, message] : cases) {
        CAPTURE(text);
        CHECK_THROWS_WITH_AS(Config::from_yaml_string(text), doctest::Contains(message.c_str()), ConfigError);
    }
}

TEST_CASE("quote size cannot exceed the position cap") {
    CHECK_THROWS_WITH_AS(Config::from_yaml_string("quoting:\n  max_size: 5\nrisk:\n  max_position: 2\n"),
                         doctest::Contains("max_position"), ConfigError);
}

TEST_CASE("product lookup by alias") {
    Config cfg;
    cfg.product = "micro-es";
    CHECK(cfg.instrument().name == "MES");
    cfg.product = "ZZ";
    CHECK_THROWS_AS(cfg.validate(), UnknownProduct);
}

TEST_CASE("the shipped configs load") {
    for (const char* name : {"configs/es_paper.yaml", "configs/es_replay.yaml"}) {
        const std::filesystem::path path = std::filesystem::path(HARVESTER_SOURCE_DIR) / name;
        if (!std::filesystem::exists(path)) continue;
        CAPTURE(name);
        const Config cfg = Config::from_yaml(path.string());
        CHECK(cfg.toxicity.bucket_contracts == 200.0);
    }
}

TEST_CASE("time of day parses hours, minutes and seconds") {
    CHECK(TimeOfDay::parse("8:45") == TimeOfDay{8, 45, 0});
    CHECK(TimeOfDay::parse("15:00:30").second == 30);
    CHECK(TimeOfDay::parse("08:45").str() == "08:45");
    CHECK_THROWS_AS(TimeOfDay::parse("noon"), ConfigError);
    CHECK(TimeOfDay{15, 0, 0} > TimeOfDay{8, 45, 0});
}

TEST_CASE("a partial config keeps the other defaults") {
    const Config cfg = Config::from_yaml_string("product: MES\nrisk:\n  max_position: 1\nquoting:\n  max_size: 1\n");
    CHECK(cfg.instrument().name == "MES");
    CHECK(cfg.risk.max_position == 1);
    CHECK(cfg.risk.daily_loss_limit_usd == 500.0);
    CHECK(cfg.databento.schema == "mbp-10");
}

}
