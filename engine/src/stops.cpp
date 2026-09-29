// Stop and stop-limit orders: entry, cancel, modify, trigger and firing.
#include "engine/book.hpp"

#include <algorithm>
#include <limits>

namespace engine {

RejectReason Book::validate_stop_fields(const StopOrder &stop) const {
    if (stop.quantity <= 0)
        return RejectReason::InvalidQuantity;
    if (stop.stop_price <= 0)
        return RejectReason::InvalidPrice;
    if (stop.limit_price && *stop.limit_price <= 0)
        return RejectReason::InvalidPrice;
    return RejectReason::None;
}

// A stop wakes on a trade at or through its stop price. If the last trade
// already is, the stop would fire the moment it is placed. Rejecting (like
// Binance's "Order would trigger immediately") beats either silently turning
// it into a market order or leaving it dormant until some later trade.
bool Book::would_trigger_now(Side side, Price stop_price) const {
    if (!last_trade_price_)
        return false; // nothing traded yet, nothing to compare against
    return side == Side::Sell ? *last_trade_price_ <= stop_price : *last_trade_price_ >= stop_price;
}

RejectReason Book::stop_gone_reason(OrderId id) const {
    return stop_ids_seen_.contains(id) ? RejectReason::TooLate : RejectReason::UnknownOrder;
}

void Book::insert_dormant_stop(const StopOrder &stop, std::uint64_t priority) {
    const StopKey key{stop.stop_price, priority};
    stops_for(stop.side).emplace(key, stop);
    stop_index_[stop.id] = {stop.side, key};
}

RejectReason Book::place_stop_order(StopOrder stop, Timestamp now) {
    if (const RejectReason bad = validate_stop_fields(stop); bad != RejectReason::None)
        return bad;

    if (id_in_use(stop.id))
        return RejectReason::DuplicateOrderId;

    if (would_trigger_now(stop.side, stop.stop_price))
        return RejectReason::StopWouldTrigger;

    insert_dormant_stop(stop, next_back_priority_++);
    stop_ids_seen_.insert(stop.id);
    emit({0, EventKind::StopAccepted, now, stop.id, 0, stop.owner_id, 0, stop.side, stop.stop_price, stop.quantity,
          stop.limit_price});
    return RejectReason::None;
}

RejectReason Book::cancel_stop_order(OrderId order_id, Timestamp now) {
    const auto it = stop_index_.find(order_id);
    if (it == stop_index_.end())
        return stop_gone_reason(order_id);

    auto &side_map = stops_for(it->second.side);
    const auto node = side_map.extract(it->second.key);
    stop_index_.erase(it);
    const StopOrder &stop = node.mapped();
    // emit() records the id as finished (Cancelled).
    emit({0, EventKind::StopCancelled, now, stop.id, 0, stop.owner_id, 0, stop.side, stop.stop_price, stop.quantity,
          stop.limit_price});
    return RejectReason::None;
}

RejectReason Book::modify_stop_order(OrderId order_id, Price new_stop_price, std::optional<Price> new_limit_price,
                                     Quantity new_qty, Timestamp now) {
    const auto it = stop_index_.find(order_id);
    if (it == stop_index_.end())
        return stop_gone_reason(order_id);

    const StopLocation where = it->second;
    auto &side_map = stops_for(where.side);
    const StopOrder old = side_map.at(where.key);

    StopOrder updated = old;
    updated.stop_price = new_stop_price;
    updated.limit_price = new_limit_price;
    updated.quantity = new_qty;
    // Validate everything before touching the stored stop.
    if (const RejectReason bad = validate_stop_fields(updated); bad != RejectReason::None)
        return bad;
    if (would_trigger_now(updated.side, updated.stop_price))
        return RejectReason::StopWouldTrigger;

    // Same rule as resting orders: only a pure size reduction keeps its place.
    const bool keeps_place =
        new_stop_price == old.stop_price && new_limit_price == old.limit_price && new_qty <= old.quantity;
    side_map.erase(where.key);
    insert_dormant_stop(updated, keeps_place ? where.key.second : next_back_priority_++);
    emit({0, EventKind::StopModified, now, updated.id, 0, updated.owner_id, 0, updated.side, updated.stop_price,
          updated.quantity, updated.limit_price});
    return RejectReason::None;
}

void Book::check_and_trigger_stops(Price low_trade_price, Price high_trade_price, Timestamp now) {
    if (halted_)
        return; // don't fire anything while halted; stops stay dormant

    // Only the stops this sweep wakes are touched: sell stops priced >= the
    // lowest trade, buy stops priced <= the highest trade.
    std::vector<std::pair<std::uint64_t, StopOrder>> woke;
    const auto take = [&](auto &side_map, auto first, auto last) {
        for (auto i = first; i != last; ++i) {
            woke.emplace_back(i->first.second, i->second);
            stop_index_.erase(i->second.id);
        }
        side_map.erase(first, last);
    };
    take(sell_stops_, sell_stops_.lower_bound({low_trade_price, 0}), sell_stops_.end());
    take(buy_stops_, buy_stops_.begin(),
         buy_stops_.upper_bound({high_trade_price, std::numeric_limits<std::uint64_t>::max()}));

    // Stops woken together fire in priority (entry) order, across both sides.
    std::sort(woke.begin(), woke.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (auto &[priority, stop] : woke)
        triggered_stops_.push_back(stop);

    // Nested cascades return; the outer FIFO drain preserves trigger order.
    if (draining_stops_)
        return;

    draining_stops_ = true;
    while (!triggered_stops_.empty()) {
        if (halted_) {
            // Unfired stops go back to dormant, still in trigger order, ahead
            // of stops that never triggered: assign front priorities from the
            // back of the queue forwards so the first gets the smallest.
            for (auto s = triggered_stops_.rbegin(); s != triggered_stops_.rend(); ++s)
                insert_dormant_stop(*s, next_front_priority_--);
            triggered_stops_.clear();
            break;
        }
        const StopOrder s = triggered_stops_.front();
        triggered_stops_.pop_front();
        emit({0, EventKind::StopTriggered, now, s.id, 0, s.owner_id, 0, s.side, s.stop_price, s.quantity,
              s.limit_price});
        // Fires under the stop's own id: a stop-limit becomes a limit order
        // (it may rest), a stop-market becomes a market order.
        if (s.limit_price)
            add_order({s.id, s.owner_id, s.side, OrderType::Limit, s.limit_price, s.quantity}, now);
        else
            add_order({s.id, s.owner_id, s.side, OrderType::Market, std::nullopt, s.quantity}, now);
    }
    draining_stops_ = false;
}

} // namespace engine
