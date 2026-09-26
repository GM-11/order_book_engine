#pragma once

#include "engine/node.hpp"
#include "engine/order.hpp"

#include <cstdint>
namespace engine {
struct Level {
    Price price;
    Node *head;
    Node *tail;
    Quantity total_quantity = 0;
    std::uint32_t order_count = 0;
};

} // namespace engine
