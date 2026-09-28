#pragma once
#include "engine/order.hpp"

namespace engine {
struct Level;

struct Node {
    Order order;
    Node *next = nullptr;
    Node *prev = nullptr;
    // The price level this node rests in. std::map never moves its elements,
    // and a level is erased only once it is empty, so this stays valid for as
    // long as the node rests. Saves a map lookup on every fill and cancel.
    Level *level = nullptr;
};
} // namespace engine
