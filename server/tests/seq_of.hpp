#pragma once
#include "server/commands.hpp"
#include <cstdint>
#include <variant>

// The engine takes an order's id from the command's seq. Tests that want a
// specific id for a new order or stop give it as the seq; other commands get 0.
inline server::Seq seq_of(const server::Command &command) {
    if (const auto *n = std::get_if<server::NewOrder>(&command))
        return n->order.id;
    if (const auto *p = std::get_if<server::PlaceStop>(&command))
        return p->stop.id;
    return 0;
}
