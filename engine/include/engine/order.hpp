#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace engine {

using OrderId = std::uint64_t;
using OwnerId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::int64_t;
enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };
using Timestamp = std::int64_t;

struct Order {
    OrderId id;
    OwnerId owner_id;
    Side side;
    OrderType type;
    // Limit: required, > 0. Market: must be empty (a market order has no
    // price; a value here is rejected rather than silently ignored).
    std::optional<Price> price;
    Quantity quantity;
    Quantity filled = 0;

    bool is_fully_filled() const { return quantity == 0; }
};

enum class FinalState { Filled, Cancelled };

struct StopOrder {
    OrderId id;
    OwnerId owner_id;
    Side side;
    Price stop_price;
    Quantity quantity;
    // Empty = stop-market (fires a market order). Set = stop-limit (fires a
    // limit order at this price, which may rest if it cannot fill).
    std::optional<Price> limit_price = std::nullopt;
};

std::string_view to_string(Side side);
std::string_view to_string(OrderType type);

} // namespace engine
