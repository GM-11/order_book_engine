#pragma once

#include "engine/event.hpp"
#include "engine/level.hpp"
#include "engine/order.hpp"
#include "engine/pool.hpp"
#include "engine/result.hpp"
#include "engine/trade.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
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

struct DepthLevel {
    Price price;
    Quantity quantity; // sum of all resting orders at this price
    std::uint32_t order_count;
};

struct DepthSnapshot {
    // Last event reflected in this snapshot. A subscriber applies only
    // events with sequence_number > as_of_seq. 0 = no events yet.
    SequenceNumber as_of_seq;
    std::vector<DepthLevel> bids; // best (highest) price first
    std::vector<DepthLevel> asks; // best (lowest) price first
};

class Book {
  public:
    explicit Book(std::size_t pool_capacity = 100000,
                  std::int64_t band_bps = 1000,
                  Timestamp grace_period_ms = 2000,
                  Timestamp halt_duration_ms = 30000)
        : node_pool_(pool_capacity), next_trade_id_(1), band_bps_(band_bps),
          grace_period_ms_(grace_period_ms),
          halt_duration_ms_(halt_duration_ms) {
        if (band_bps_ <= 0 || band_bps_ >= 10000)
            throw std::invalid_argument("band_bps must be in (0, 10000)");
    }

    OrderResult add_order(Order order, Timestamp now);
    OrderResult modify_order(OrderId order_id, Price new_price,
                             Quantity new_qty, Timestamp now);
    bool cancel_order(OrderId order_id, Timestamp now);
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    // Top max_levels price levels per side, aggregated. O(max_levels).
    DepthSnapshot depth(std::size_t max_levels) const;
    // Slow full audit of the book's internal consistency. For tests and
    // debugging only, never on the matching path.
    bool check_invariants() const;

    RejectReason place_stop_order(StopOrder stop_order, Timestamp now);
    bool cancel_stop_order(OrderId order_id, Timestamp now);
    std::vector<EngineEvent> drain_events() {
        return std::exchange(events_, {});
    }

    bool within_band(Price p) const {
        if (!has_reference_price_)
            return true; // nothing traded yet, nothing to compare against
        return p >= lower_band_price() && p <= upper_band_price();
    }

  private:
    Price lower_band_price() const;
    Price upper_band_price() const;
    RejectReason validate_order_fields(const Order &order) const;
    RejectReason validate_new_order(const Order &order, Timestamp now);
    MatchPlan plan_match(const Order &incoming) const;

    bool id_in_use(OrderId id) const;
    void remove_resting(Node *node);
    // A resting order got smaller but stays in the book (partial fill,
    // in-place modify). Keeps Level::total_quantity equal to the real sum.
    void reduce_resting(Node *node, Quantity by);
    template <typename SideMap>
    void reduce_in_level(SideMap &side_map, Node *node, Quantity by);
    void check_and_trigger_stops(Price low_trade_price, Price high_trade_price,
                                 Timestamp now);
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
    std::vector<EngineEvent> events_;
    SequenceNumber next_seq_ = 1;
    void emit(EngineEvent e) {
        e.sequence_number = next_seq_++;
        events_.push_back(e);
    }

    // reference_price_ * (10000 +/- band_bps_) must fit in int64_t.
    static_assert(sizeof(Price) >= sizeof(std::int64_t));
    std::int64_t band_bps_;
    Timestamp grace_period_ms_;
    Timestamp halt_duration_ms_;
    Price reference_price_ = 0;
    bool has_reference_price_ = false;
    std::optional<Timestamp> outside_band_since_;
    bool halted_ = false;
    Timestamp halt_until_ = 0;
};
} // namespace engine
