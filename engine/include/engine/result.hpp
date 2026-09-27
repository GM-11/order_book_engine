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
    DuplicateOrderId,
    TooLate,
    // Walk stopped: the next fill would go past this call's first
    // out-of-band price level (one breach print per sweep, not a whole sweep).
    PriceBand,
    // Market order stopped by its collar: no fills beyond
    // reference price +/- collar_bps.
    PriceCollar,
};

struct OrderResult {
    std::vector<Trade> trades;
    // Quantity rejected or discarded rather than accepted onto the book.
    Quantity unaccepted_quantity;
    RejectReason reject_reason;
    std::optional<Price> rested_price = std::nullopt;
};

} // namespace engine
