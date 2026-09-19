#pragma once

#include "engine/level.hpp"
#include "engine/order.hpp"
#include "engine/pool.hpp"
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

class Book {
  public:
    explicit Book(std::size_t pool_capacity = 100000)
        : node_pool_(pool_capacity), next_trade_id_(1) {}

    std::vector<Trade> add_order(Order order);
    bool cancel_order(OrderId order_id);
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

  private:
    std::vector<ProposedFill> plan_match(const Order &incoming) const;
    void unlink_and_maybe_erase_level(Node *node);

    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level> asks_;
    std::unordered_map<OrderId, Node *> id_index_;
    NodePool node_pool_;
    TradeId next_trade_id_;
};
} // namespace engine
