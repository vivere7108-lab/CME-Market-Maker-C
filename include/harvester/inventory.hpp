// The position, its P&L, and the markout on every fill.
//
// Markouts are the number this whole system is judged on.  A market
// maker's edge is the spread captured *less* what the price does after the
// fill, and a quote that sits behind the touch fills precisely when the
// touch is swept -- so the post-fill move is the cost of the strategy, and
// the toxicity gate exists to reduce it.  For each fill the anchor is
// recorded at the fill and again at each horizon after it::
//
//     markout_h = side * (anchor(t + h) - fill_price) * multiplier
//
// in dollars per contract.  Positive means the price went the quote's way;
// negative is adverse selection.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "harvester/config.hpp"
#include "harvester/execution/base.hpp"
#include "harvester/instruments.hpp"

namespace harvester {

class Inventory {
public:
    explicit Inventory(const Product& product) : product_(product) {}

    // Book a fill. Returns the realised P&L it closed out, gross of fees.
    double apply(const Fill& fill);
    void adopt(int position, double avg_price);
    double unrealised(std::optional<double> mark) const;
    double total_pnl(std::optional<double> mark) const;
    std::string describe(std::optional<double> mark) const;

    const Product& product() const { return product_; }

    int position = 0;
    double avg_price = 0.0;
    double realised = 0.0;
    double fees = 0.0;
    std::vector<Fill> fills;
    std::int64_t contracts_traded = 0;

private:
    const Product& product_;
};

struct MarkoutRow {
    double ts = 0.0;
    int side = 0;
    double price = 0.0;
    int size = 0;
    ToxicityLevel level = ToxicityLevel::Calm;
    double anchor_at_fill = 0.0;
    // One slot per horizon, in horizon order; filled as each elapses.
    std::vector<std::optional<double>> markouts;
    // The spread captured against the anchor at the fill, $ per contract.
    double edge_at_fill = 0.0;

    bool complete() const;
};

struct MarkoutSummary {
    double fills = 0;
    double contracts = 0;
    double edge = 0.0;
    // Mean markout per horizon key ("1s", "5s", ...), where any row had it.
    std::map<std::string, double> markouts;
};

class MarkoutTracker {
public:
    explicit MarkoutTracker(const Product& product, std::vector<double> horizons = {1.0, 5.0, 30.0});

    MarkoutRow& record(const Fill& fill, std::optional<double> anchor, ToxicityLevel level, double now);
    // Fill in every horizon that has elapsed. Returns the rows just completed.
    std::vector<const MarkoutRow*> update(double now, std::optional<double> anchor);
    // Per toxicity level: fills, contracts, mean edge and mean markouts.
    std::map<std::string, MarkoutSummary> summary() const;
    std::string describe() const;
    // The key of one horizon as the journal writes it: ``1s``, ``30s``.
    std::string horizon_key(std::size_t index) const;

    const std::vector<double>& horizons() const { return horizons_; }
    const Product& product() const { return product_; }
    // Rows live in a deque-like vector that is never reordered, so a
    // pointer into it stays valid; the count is the number of fills.
    std::vector<MarkoutRow> rows;

private:
    const Product& product_;
    std::vector<double> horizons_;
    std::vector<std::size_t> pending_;
};

}  // namespace harvester
