#pragma once
#include "engine/order.hpp"

namespace engine {
struct Node {
    Order order;
    Node *next = nullptr;
    Node *prev = nullptr;
};
} // namespace engine
