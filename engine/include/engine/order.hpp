#pragma once

#include <cstdint>
#include <string_view>

namespace engine {

using OrderId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::int64_t;
enum class Side { Buy, Sell };

struct Order {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;

    bool is_fully_filled() const { return quantity == 0; }
};

std::string_view to_string(Side side);

} // namespace engine
