#pragma once

#include <cstdint>
#include <string_view>

namespace engine {

using OrderId = std::uint64_t;
using OwnerId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::int64_t;
enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };

struct Order {
    OrderId id;
    OwnerId owner_id;
    Side side;
    OrderType type;
    Price price;
    Quantity quantity;

    bool is_fully_filled() const { return quantity == 0; }
};

struct StopOrder {
    OrderId id;
    OwnerId owner_id;
    Side side;
    Price stop_price;
    Quantity quantity;
};

std::string_view to_string(Side side);
std::string_view to_string(OrderType type);

} // namespace engine
