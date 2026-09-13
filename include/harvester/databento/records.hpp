// databento-cpp's records, as the builders read them. Field for field; the
// enums become the one-character codes DBN itself uses.
#pragma once

#include <databento/record.hpp>

#include "harvester/book/builder.hpp"

namespace harvester {

inline char action_code(databento::Action action) { return static_cast<char>(action); }
inline char side_code(databento::Side side) { return static_cast<char>(side); }

inline MboRecord to_record(const databento::MboMsg& msg) {
    MboRecord out;
    out.ts_event = static_cast<std::int64_t>(msg.hd.ts_event.time_since_epoch().count());
    out.order_id = msg.order_id;
    out.price = msg.price;
    out.size = msg.size;
    out.sequence = msg.sequence;
    out.flags = static_cast<std::uint8_t>(msg.flags);
    out.action = action_code(msg.action);
    out.side = side_code(msg.side);
    return out;
}

inline Mbp10Record to_record(const databento::Mbp10Msg& msg) {
    Mbp10Record out;
    out.ts_event = static_cast<std::int64_t>(msg.hd.ts_event.time_since_epoch().count());
    out.price = msg.price;
    out.size = msg.size;
    out.sequence = msg.sequence;
    out.flags = static_cast<std::uint8_t>(msg.flags);
    out.action = action_code(msg.action);
    out.side = side_code(msg.side);
    for (std::size_t i = 0; i < out.levels.size(); ++i) {
        const databento::BidAskPair& level = msg.levels[i];
        out.levels[i] = BidAskPair{level.bid_px, level.ask_px, level.bid_sz, level.ask_sz, level.bid_ct, level.ask_ct};
    }
    return out;
}

}  // namespace harvester
