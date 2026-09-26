#pragma once

#include "engine/trade.hpp"
#include <optional>
#include <vector>

namespace engine {

enum class RejectReason {
    None,
    InvalidPrice,
    InvalidQuantity,
    SelfTrade,
    PoolExhausted,
    SymbolHalted,
    UnknownOrder,
};

struct OrderResult {
    std::vector<Trade> trades;
    // Quantity rejected or discarded rather than accepted onto the book.
    Quantity unaccepted_quantity;
    RejectReason reject_reason;
    std::optional<Price> rested_price = std::nullopt;
};

} // namespace engine
