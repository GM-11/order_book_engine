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
}; // namespace engine
