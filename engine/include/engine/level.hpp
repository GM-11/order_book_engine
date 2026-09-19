#pragma once

#include "engine/node.hpp"
#include "engine/order.hpp"
namespace engine {
struct Level {
    Price price;
    Node *head;
    Node *tail;
};
} // namespace engine
