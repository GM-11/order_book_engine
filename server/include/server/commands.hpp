#pragma once

#include "engine/event.hpp"
#include "engine/order.hpp"
#include "engine/result.hpp"
#include <cstdint>
#include <optional>
#include <variant>
namespace server {
using SymbolId = std::uint32_t;
using RequestId = std::uint64_t;

struct NewOrder {
    RequestId request_id;
    SymbolId symbol;
    engine::Order order;
};

struct CancelOrder {
    RequestId request_id;
    SymbolId symbol;
    engine::OrderId order_id;
};

struct ModifyOrder {
    RequestId request_id;
    SymbolId symbol;
    engine::OrderId order_id;
    engine::Price new_price;
    engine::Quantity new_quantity;
};

struct PlaceStop {
    RequestId request_id;
    SymbolId symbol;
    engine::StopOrder stop;
};

struct CancelStop {
    RequestId request_id;
    SymbolId symbol;
    engine::OrderId order_id;
};

struct ModifyStop {
    RequestId request_id;
    SymbolId symbol;
    engine::OrderId order_id;
    engine::Price new_stop_price;
    std::optional<engine::Price> new_limit_price;
    engine::Quantity new_quantity;
};

struct Shutdown {};

using Command = std::variant<NewOrder, CancelOrder, ModifyOrder, PlaceStop, CancelStop, ModifyStop, Shutdown>;

struct MarketEvent {
    SymbolId symbol;
    engine::EngineEvent event;
};

struct Reply {
    RequestId request_id;
    SymbolId symbol;
    engine::RejectReason reject_reason;
    engine::Quantity unaccepted_quantity = 0;
    std::optional<engine::Price> rested_price = std::nullopt;
};

using Output = std::variant<MarketEvent, Reply>;

} // namespace server
