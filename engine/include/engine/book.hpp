#pragma once

#include "engine/event.hpp"
#include "engine/level.hpp"
#include "engine/order.hpp"
#include "engine/pool.hpp"
#include "engine/result.hpp"
#include "engine/trade.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
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
    bool stopped_by_collar = false; // market order hit its price collar
};

struct DepthLevel {
    Price price;
    Quantity quantity; // sum of all resting orders at this price
    std::uint32_t order_count;
};

struct DepthSnapshot {
    // Last event reflected in this snapshot. A subscriber applies only
    // events with sequence_number > as_of_sequence. 0 = no events yet.
    SequenceNumber as_of_sequence;
    std::vector<DepthLevel> bids; // best (highest) price first
    std::vector<DepthLevel> asks; // best (lowest) price first
};

class Book {
  public:
    explicit Book(std::size_t pool_capacity = 100000, std::int64_t band_bps = 1000, Timestamp grace_period_ms = 2000,
                  Timestamp halt_duration_ms = 30000,
                  std::int64_t market_collar_bps = 0) // 0 = no collar
        : node_pool_(pool_capacity), next_trade_id_(1), band_bps_(band_bps), grace_period_ms_(grace_period_ms),
          halt_duration_ms_(halt_duration_ms), market_collar_bps_(market_collar_bps) {
        if (band_bps_ <= 0 || band_bps_ >= 10000)
            throw std::invalid_argument("band_bps must be in (0, 10000)");
        if (market_collar_bps_ < 0 || market_collar_bps_ >= 10000)
            throw std::invalid_argument("market_collar_bps must be in [0, 10000)");
    }

    OrderResult add_order(Order order, Timestamp now);
    OrderResult modify_order(OrderId order_id, OwnerId requester, Price new_price, Quantity new_qty, Timestamp now);
    RejectReason cancel_order(OrderId order_id, OwnerId requester, Timestamp now);
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    // Top max_levels price levels per side, aggregated. O(max_levels).
    DepthSnapshot depth(std::size_t max_levels) const;
    // Slow full audit of the book's internal consistency. For tests and
    // debugging only, never on the matching path.
    bool check_invariants() const;

    // Rejects with StopWouldTrigger if the last trade is already at or
    // through the stop price (sell: last <= stop, buy: last >= stop).
    RejectReason place_stop_order(StopOrder stop_order, Timestamp now);
    // None: the dormant stop was cancelled. TooLate: that id was a stop but
    // has already triggered (use cancel_order if it now rests as a limit) or
    // was cancelled. UnknownOrder: no stop was ever placed with that id.
    RejectReason cancel_stop_order(OrderId order_id, OwnerId requester, Timestamp now);
    // Replaces a dormant stop's stop price, limit price (empty = stop-market)
    // and quantity. Same prices and a smaller-or-equal quantity keep its place
    // in the firing order; anything else moves it to the back. Same
    // None/TooLate/UnknownOrder rules as cancel_stop_order; on any reject the
    // stop is left unchanged.
    RejectReason modify_stop_order(OrderId order_id, OwnerId requester, Price new_stop_price,
                                   std::optional<Price> new_limit_price, Quantity new_qty, Timestamp now);
    std::vector<EngineEvent> drain_events() { return std::exchange(events_, {}); }

    bool within_band(Price p) const {
        if (!has_reference_price_)
            return true; // nothing traded yet, nothing to compare against
        return p >= lower_band_price() && p <= upper_band_price();
    }

    std::optional<FinalState> final_state(OrderId id) const {
        const auto it = finished_orders_.find(id);
        if (it == finished_orders_.end())
            return std::nullopt;
        return it->second;
    }

  private:
    // reference_price_ -/+ bps, rounded inward to whole ticks.
    Price lower_limit(std::int64_t bps) const;
    Price upper_limit(std::int64_t bps) const;
    Price lower_band_price() const { return lower_limit(band_bps_); }
    Price upper_band_price() const { return upper_limit(band_bps_); }
    RejectReason validate_order_fields(const Order &order) const;
    RejectReason validate_new_order(const Order &order, Timestamp now);
    RejectReason validate_stop_fields(const StopOrder &stop) const;
    MatchPlan plan_match(const Order &incoming) const;

    bool id_in_use(OrderId id) const;
    void remove_resting(Node *node);
    // A resting order got smaller but stays in the book (partial fill,
    // in-place modify). Keeps Level::total_quantity equal to the real sum.
    void reduce_resting(Node *node, Quantity by);
    void check_and_trigger_stops(Price low_trade_price, Price high_trade_price, Timestamp now);
    void unlink_and_maybe_erase_level(Node *node);
    template <typename SideMap> void unlink_and_maybe_erase_from_level(SideMap &side_map, Node *node);
    template <typename SideMap> void get_or_create_level(SideMap &side_map, Price price, Node *node);

    std::map<Price, Level, std::greater<Price>> bids_;
    std::map<Price, Level> asks_;
    std::unordered_map<OrderId, Node *> id_index_;
    std::unordered_map<OrderId, FinalState> finished_orders_;
    // Owner of every id ever accepted (orders and stops), recorded by emit()
    // on Accepted/StopAccepted. Kept after the order finishes so a late
    // cancel from the owner still gets TooLate. Grows like finished_orders_
    // (session reset).
    std::unordered_map<OrderId, OwnerId> owner_of_;
    bool owned_by(OrderId id, OwnerId requester) const {
        const auto it = owner_of_.find(id);
        return it != owner_of_.end() && it->second == requester;
    }

    // Dormant stops, sorted by stop price so a trade touches only the stops
    // it wakes: sells wake on a trade <= stop (the highest stops first),
    // buys on a trade >= stop (the lowest first). Key = (stop price,
    // priority). Priority records firing order when several stops trigger
    // together: new stops take increasing values from next_back_priority_;
    // stops sent back by a halt take decreasing values from
    // next_front_priority_, so they go ahead of every other dormant stop.
    using StopKey = std::pair<Price, std::uint64_t>;
    struct StopLocation {
        Side side;
        StopKey key;
    };
    std::map<StopKey, StopOrder> sell_stops_;
    std::map<StopKey, StopOrder> buy_stops_;
    std::unordered_map<OrderId, StopLocation> stop_index_; // dormant only
    // Every id ever placed as a stop, so a late cancel/modify can say TooLate
    // instead of UnknownOrder. Grows like finished_orders_ (session reset).
    std::unordered_set<OrderId> stop_ids_seen_;
    static constexpr std::uint64_t kPriorityMid = std::uint64_t{1} << 62;
    std::uint64_t next_back_priority_ = kPriorityMid;
    std::uint64_t next_front_priority_ = kPriorityMid - 1;
    void insert_dormant_stop(const StopOrder &stop, std::uint64_t priority);
    std::map<StopKey, StopOrder> &stops_for(Side side) { return side == Side::Sell ? sell_stops_ : buy_stops_; }
    bool would_trigger_now(Side side, Price stop_price) const;
    // Why a cancel/modify found no dormant stop: TooLate or UnknownOrder.
    RejectReason stop_gone_reason(OrderId id) const;

    // Stops that have triggered but not fired yet, in trigger order (FIFO).
    // Only the outermost check_and_trigger_stops call drains it; nested
    // calls (from a triggered stop's own fills) only append to the back.
    std::deque<StopOrder> triggered_stops_;
    bool draining_stops_ = false;
    NodePool node_pool_;
    TradeId next_trade_id_;
    std::vector<EngineEvent> events_;
    SequenceNumber next_seq_ = 1;
    void emit(EngineEvent e) {
        e.sequence_number = next_seq_++;
        if (e.kind == EventKind::Cancelled || e.kind == EventKind::StopCancelled)
            finished_orders_[e.order_id] = FinalState::Cancelled;
        if (e.kind == EventKind::Accepted || e.kind == EventKind::StopAccepted)
            owner_of_.emplace(e.order_id, e.owner_id); // first accept wins
        events_.push_back(e);
    }

    // reference_price_ * (10000 +/- band_bps_) must fit in int64_t.
    static_assert(sizeof(Price) >= sizeof(std::int64_t));
    std::int64_t band_bps_;
    Timestamp grace_period_ms_;
    Timestamp halt_duration_ms_;
    std::int64_t market_collar_bps_;
    // Band reference. Today it equals the last trade; it is kept separate
    // from last_trade_price_ because real LULD uses a moving average.
    Price reference_price_ = 0;
    std::optional<Price> last_trade_price_; // for the stop entry check
    bool has_reference_price_ = false;
    std::optional<Timestamp> outside_band_since_;
    bool halted_ = false;
    Timestamp halt_until_ = 0;
};
} // namespace engine
