#pragma once

#include "engine/level.hpp"
#include "engine/order.hpp"
#include "engine/pool.hpp"
#include "engine/result.hpp"
#include "engine/trade.hpp"
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>
namespace engine {

struct ProposedFill {
    Node *passive_node;
    Price fill_price;
    Quantity fill_quantity;
};

struct MatchPlan {
    std::vector<ProposedFill> fills;
    bool halted_by_self_trade = false;
};

class Book {
  public:
    explicit Book(std::size_t pool_capacity = 100000,
                  double band_percentage = 0.10,
                  Timestamp grace_period_ms = 2000,
                  Timestamp halt_duratio_ms = 30000)
        : node_pool_(pool_capacity), next_trade_id_(1),
          band_percentage_(band_percentage), grace_period_ms_(grace_period_ms),
          halt_duration_ms_(halt_duratio_ms) {}

    OrderResult add_order(Order order, Timestamp now);
    bool cancel_order(OrderId order_id);
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    void place_stop_order(StopOrder stop_order);
    bool cancel_stop_order(OrderId order_id);

    bool within_band(Price p) const {
        if (!has_reference_price_)
            return true; // nothing traded yet, nothing to compare against
        Price band_low =
            static_cast<Price>(reference_price_ * (1.0 - band_percentage_));
        Price band_high =
            static_cast<Price>(reference_price_ * (1.0 + band_percentage_));
        return p >= band_low && p <= band_high;
    }

  private:
    MatchPlan plan_match(const Order &incoming) const;
    void check_and_trigger_stops(Price last_trade_price, Timestamp now);
    void unlink_and_maybe_erase_level(Node *node);
    template <typename SideMap>
    void unlink_and_maybe_erase_from_level(SideMap &side_map, Node *node);
    template <typename SideMap>
    void get_or_create_level(SideMap &side_map, Price price, Node *node);

    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level> asks_;
    std::unordered_map<OrderId, Node *> id_index_;
    std::vector<StopOrder> pending_stops_;
    NodePool node_pool_;
    TradeId next_trade_id_;

    double band_percentage_;
    Timestamp grace_period_ms_;
    Timestamp halt_duration_ms_;
    Price reference_price_ = 0;
    bool has_reference_price_ = false;
    std::optional<Timestamp> outside_band_since_;
    bool halted_ = false;
    Timestamp halt_until_ = 0;
};
} // namespace engine
