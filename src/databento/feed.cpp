// The live Databento session, feeding the book on its own thread.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <databento/live.hpp>
#include <databento/log.hpp>
#include <databento/record.hpp>
#include <databento/symbol_map.hpp>
#include <format>
#include <memory>
#include <mutex>
#include <optional>

#include "harvester/databento/api.hpp"
#include "harvester/databento/records.hpp"
#include "harvester/util/log.hpp"

namespace harvester {

namespace {
constexpr const char* kLog = "harvester.book.feed";

// Routes databento-cpp's own log lines into ours.
class LogBridge : public databento::ILogReceiver {
public:
    void Receive(databento::LogLevel level, const std::string& msg) override {
        switch (level) {
            case databento::LogLevel::Debug: HLOG_DEBUG("databento", "{}", msg); break;
            case databento::LogLevel::Info: HLOG_INFO("databento", "{}", msg); break;
            case databento::LogLevel::Warning: HLOG_WARNING("databento", "{}", msg); break;
            case databento::LogLevel::Error: HLOG_ERROR("databento", "{}", msg); break;
        }
    }
};

class DatabentoBookFeed : public RecordFeed {
public:
    DatabentoBookFeed(const DatabentoConfig& cfg, const Product& product, int depth)
        : RecordFeed(cfg.schema, depth), cfg_(cfg), product_(product) {}

    ~DatabentoBookFeed() override { close(); }

    bool is_live() const override { return true; }
    std::int64_t errors() const override { return errors_.load(); }

    void start() override {
        const char* key = std::getenv(cfg_.api_key_env.c_str());
        if (key == nullptr || *key == '\0') {
            throw std::runtime_error(std::format("set {} to a Databento API key with {} entitlement", cfg_.api_key_env,
                                                 cfg_.dataset));
        }
        const bool mbo = schema() == Schema::Mbo;
        auto builder = databento::LiveBuilder{}
                           .SetKey(key)
                           .SetDataset(cfg_.dataset)
                           .SetLogReceiver(&log_bridge_)
                           .SetUpgradePolicy(databento::VersionUpgradePolicy::UpgradeToV3);
        client_ = std::make_unique<databento::LiveThreaded>(builder.BuildThreaded());
        const databento::Schema schema = mbo ? databento::Schema::Mbo : databento::Schema::Mbp10;
        const databento::SType stype = stype_in();
        // MBO: the standing book, delivered as flagged adds before the live
        // stream. Without it the book is partial until every resting order
        // has been touched once, which is never.
        if (cfg_.snapshot && mbo) {
            client_->SubscribeWithSnapshot({cfg_.symbol}, schema, stype);
        } else {
            client_->Subscribe({cfg_.symbol}, schema, stype);
        }
        client_->Start(
            [this](databento::Metadata&&) {},
            [this](const databento::Record& record) { return on_record(record); },
            [this](const std::exception& exc) {
                ++errors_;
                HLOG_WARNING(kLog, "databento live session error: {}", exc.what());
                return cfg_.reconnect ? databento::LiveThreaded::ExceptionAction::Restart
                                      : databento::LiveThreaded::ExceptionAction::Stop;
            });
        started_ = true;
        HLOG_INFO(kLog, "databento subscribed to {} {} ({}) with{} snapshot", cfg_.dataset, cfg_.symbol, cfg_.schema,
                  cfg_.snapshot ? "" : "out");
    }

    void close() override {
        if (client_ && started_) {
            try {
                client_.reset();
            } catch (const std::exception& exc) {  // shutting down
                HLOG_WARNING(kLog, "databento: session did not stop cleanly ({})", exc.what());
            }
        }
        client_.reset();
        started_ = false;
    }

    // Block until the raw contract is known, or ``timeout`` passes.
    std::optional<std::string> wait_for_symbol(double timeout_seconds) override {
        std::unique_lock<std::mutex> lock(symbol_mutex_);
        symbol_seen_.wait_for(lock, std::chrono::duration<double>(timeout_seconds), [this] { return symbol_known_; });
        lock.unlock();
        return symbol_known_ ? raw_symbol() : std::nullopt;
    }

private:
    databento::SType stype_in() const {
        if (cfg_.stype_in == "raw_symbol") return databento::SType::RawSymbol;
        if (cfg_.stype_in == "parent") return databento::SType::Parent;
        return databento::SType::Continuous;
    }

    databento::KeepGoing on_record(const databento::Record& record) {
        if (const auto* mbo = record.GetIf<databento::MboMsg>()) {
            push(to_record(*mbo));
        } else if (const auto* mbp = record.GetIf<databento::Mbp10Msg>()) {
            push(to_record(*mbp));
        } else if (const auto* mapping = record.GetIf<databento::SymbolMappingMsg>()) {
            const std::string raw = mapping->STypeOutSymbol();
            const std::optional<std::string> current = raw_symbol();
            if (!raw.empty() && raw != current) {
                if (current) {
                    HLOG_WARNING(kLog,
                                 "databento: {} now maps to {} (was {}) -- the front contract rolled; the runner "
                                 "re-qualifies on its next cycle",
                                 cfg_.symbol, raw, *current);
                } else {
                    HLOG_INFO(kLog, "databento: {} is {}", cfg_.symbol, raw);
                }
                set_symbol(raw);
                {
                    const std::lock_guard<std::mutex> guard(symbol_mutex_);
                    symbol_known_ = true;
                }
                symbol_seen_.notify_all();
            }
        } else if (const auto* def = record.GetIf<databento::InstrumentDefMsg>()) {
            check_definition(*def);
        } else if (const auto* err = record.GetIf<databento::ErrorMsg>()) {
            ++errors_;
            HLOG_ERROR(kLog, "databento: {}", err->Err());
        } else if (const auto* sys = record.GetIf<databento::SystemMsg>()) {
            if (!sys->IsHeartbeat()) HLOG_INFO(kLog, "databento: {}", sys->Msg());
        }
        return databento::KeepGoing::Continue;
    }

    void check_definition(const databento::InstrumentDefMsg& def) {
        const std::int64_t increment = def.min_price_increment;
        if (increment != 0 && increment != databento::kUndefPrice && increment != product_.tick_int()) {
            // The wrong product or a changed contract spec: every price in
            // the system would be wrong, so it is fatal rather than logged.
            HLOG_ERROR(kLog,
                       "the feed's instrument definition says a tick of {:g} but product {} is configured with {:g}; "
                       "every price in the system would be wrong",
                       static_cast<double>(increment) / static_cast<double>(PRICE_SCALE), product_.name,
                       product_.tick_size);
            throw std::runtime_error(std::format(
                "the feed's instrument definition says a tick of {:g} but product {} is configured with {:g}",
                static_cast<double>(increment) / static_cast<double>(PRICE_SCALE), product_.name, product_.tick_size));
        }
    }

    DatabentoConfig cfg_;
    const Product& product_;
    LogBridge log_bridge_;
    std::unique_ptr<databento::LiveThreaded> client_;
    bool started_ = false;
    std::atomic<std::int64_t> errors_{0};
    std::mutex symbol_mutex_;
    std::condition_variable symbol_seen_;
    bool symbol_known_ = false;
};

}  // namespace

bool databento_supported() { return true; }

std::shared_ptr<RecordFeed> make_databento_feed(const DatabentoConfig& cfg, const Product& product, int depth) {
    return std::make_shared<DatabentoBookFeed>(cfg, product, depth);
}

}  // namespace harvester
