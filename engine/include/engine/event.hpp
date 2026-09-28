#pragma once
#include "engine/order.hpp"
#include <cstdint>
#include <optional>

namespace engine {
using SequenceNumber = std::uint64_t;

enum class EventKind {
    Accepted,
    Rested,
    Trade,
    Cancelled,
    Modified,
    Replaced,
    StopAccepted,
    StopTriggered,
    StopCancelled,
    StopModified,
    Halted,
    Resumed,
};

struct EngineEvent {
    SequenceNumber sequence_number;
    EventKind kind;
    Timestamp ts;
    OrderId order_id;    // for Trade: the aggressor (incoming) order
    OrderId passive_id;  // Trade only
    OwnerId owner_id;    // for Trade: the buyer
    OwnerId other_owner; // for Trade: the seller
    Side side;           // for Trade: the aggressor's side
    // Empty when there is no price: a market order's Accepted/Cancelled,
    // Resumed. Stop events carry the stop (trigger) price here.
    std::optional<Price> price;
    Quantity quantity; // fill qty, rested qty, or cancelled qty
    // Stop events only: the limit price of a stop-limit (empty = stop-market).
    std::optional<Price> limit_price = std::nullopt;
};
} // namespace engine
