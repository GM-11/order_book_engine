#include "engine/order.hpp"

namespace engine {
std::string_view to_string(Side side) {
    switch (side) {
    case Side::Buy:
        return "Buy";
    case Side::Sell:
        return "Sell";
    }
    return "unknown";
}

std::string_view to_string(OrderType type) {
    switch (type) {
    case OrderType::Limit:
        return "Limit";
    case OrderType::Market:
        return "Market";
    }
    return "unknown";
}

}; // namespace engine
