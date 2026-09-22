// The futures this system can quote.
//
// One ``Product`` is one outright future: the Databento root it is listed
// under, the IBKR symbol it is routed as, and the contract arithmetic --
// tick size, multiplier, fees -- that every price and P&L figure in the
// system is expressed in.  Nothing here is hedged; inventory is managed by
// skewing the quotes, so a product is one contract rather than a pair.
//
// Prices
// ------
// Every price the book holds is an **integer** in Databento's fixed-point
// convention (1e-9 of a point), converted to a double only at the edges.
// A tick of 0.25 has an exact binary representation and ES would survive
// doubles; the 0.0001 ticks of a currency future would not, and a book
// whose levels drift by a rounding error is a book that reports a crossed
// market that never happened.  ``Product::ticks`` and ``Product::price``
// convert.
//
// Fees
// ----
// ``fee_per_contract`` is the all-in cost of one contract traded one way --
// exchange, clearing, NFA and the broker's commission -- and it is what a
// captured spread has to clear before it is a profit.  Verify against the
// account's own statement before believing a P&L figure.
#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace harvester {

// Databento's fixed-price scale: prices are integers in units of 1e-9.
inline constexpr std::int64_t PRICE_SCALE = 1'000'000'000;

struct Product {
    std::string name;
    // The CME product root as Databento lists it (raw symbol prefix).
    std::string databento_root;
    // IBKR's symbol for the same product; the exchange it routes to.
    std::string ibkr_symbol;
    std::string exchange = "CME";
    std::string currency = "USD";
    // Dollars of P&L per 1.00 move in the price, per contract.
    double multiplier = 50.0;
    // Minimum price increment, in points.
    double tick_size = 0.25;
    // All-in cost of one contract traded one way, USD.
    double fee_per_contract = 2.30;
    // What the broker holds against one contract, USD. IBKR's house
    // requirement rather than CME's bond; re-check against the account.
    double initial_margin = 34'500.0;
    // Exchange-local session hints. CME equity index products trade
    // nearly around the clock; these are the hours worth *quoting*.
    std::string timezone = "America/Chicago";
    std::vector<std::string> aliases;

    // One tick in the book's fixed-point integer units.
    std::int64_t tick_int() const { return static_cast<std::int64_t>(std::llround(tick_size * PRICE_SCALE)); }
    // Dollars one tick is worth on one contract.
    double tick_value() const { return tick_size * multiplier; }
    // A fixed-point book price as a double in points.
    double price(std::int64_t fixed) const { return static_cast<double>(fixed) / static_cast<double>(PRICE_SCALE); }
    // A double price in points, snapped to the tick grid, as fixed-point.
    // (Python ``round`` is half-to-even; so is ``std::nearbyint``.)
    std::int64_t fixed(double price) const {
        return static_cast<std::int64_t>(std::nearbyint(price / tick_size)) * tick_int();
    }
    // A difference in points expressed in ticks.
    double ticks(double price_difference) const { return price_difference / tick_size; }
    double round_to_tick(double price) const {
        const double snapped = std::nearbyint(price / tick_size) * tick_size;
        return std::nearbyint(snapped * 1e10) / 1e10;
    }
};

struct UnknownProduct : std::runtime_error {
    using std::runtime_error::runtime_error;
};

extern const Product ES;
extern const Product MES;
extern const Product NQ;
extern const Product MNQ;
extern const Product ZN;
extern const Product ZF;
extern const Product ZT;
extern const Product GC;
extern const Product CL;

void register_product(const Product& product);
// Case-insensitive lookup by name or alias. Throws ``UnknownProduct``.
const Product& get_product(std::string_view name);
std::vector<std::string> registered_products();

}  // namespace harvester
