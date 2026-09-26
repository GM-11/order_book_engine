#pragma once
#include "engine/order.hpp"
#include <cstdint>

namespace engine {
using SequenceNumber = std::uint64_t;

enum class EventKind {
    Accepted,
    Rested,
    Trade,
    Cancelled,
    Modified,
    StopAccepted,
    StopTriggered,
    StopCancelled,
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
    Price price;
    Quantity quantity; // fill qty, rested qty, or cancelled qty
};
} // namespace engine
