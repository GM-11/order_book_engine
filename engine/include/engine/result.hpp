#pragma once

#include "engine/trade.hpp"
#include <vector>

namespace engine {

enum class RejectReason {
    None,
    InvalidPrice,
    InvalidQuantity,
    SelfTrade,
    PoolExhausted,
};

struct OrderResult {
    std::vector<Trade> trades;
    Quantity remaining_quantity;
    RejectReason reject_reason;
};

} // namespace engine
