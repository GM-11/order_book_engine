#pragma once

#include "engine/order.hpp"
#include <cstdint>
namespace engine {
using TradeId = std::uint64_t;

struct Trade {
    TradeId tradeId;
    Price price;
    Quantity quantity;
    OrderId aggressor_id;
    OrderId passive_id;
};

} // namespace engine
