#include "engine/book.hpp"
#include "engine/order.hpp"
#include "engine/result.hpp"

#include <algorithm>

namespace engine {

MatchPlan Book::plan_match(const Order &incoming) const {
    MatchPlan plan;
    Quantity remaining = incoming.quantity;

    // Apply the configured collar to market orders when a reference exists.
    const bool collared = incoming.type == OrderType::Market && market_collar_bps_ > 0 && has_reference_price_;
    auto inside_collar = [&](Price price) {
        if (!collared)
            return true;
        return incoming.side == Side::Buy ? price <= upper_limit(market_collar_bps_)
                                          : price >= lower_limit(market_collar_bps_);
    };

    auto walk_side = [&](const auto &side_map, const auto &crosses) {
        for (auto level_it = side_map.begin(); level_it != side_map.end() && remaining > 0; ++level_it) {
            if (!crosses(level_it->first))
                break;
            if (!inside_collar(level_it->first)) {
                plan.stopped_by_collar = true;
                return;
            }

            const Level &level = level_it->second;
            for (Node *node = level.head; node && remaining > 0; node = node->next) {
                if (node->order.owner_id == incoming.owner_id) {
                    plan.halted_by_self_trade = true;
                    return;
                }

                Quantity fill_qty = std::min(remaining, node->order.quantity);
                plan.fills.push_back({node, level_it->first, fill_qty});
                remaining -= fill_qty;
            }
        }
    };

    if (incoming.side == Side::Buy) {
        walk_side(asks_, [&](Price price) { return incoming.type == OrderType::Market || price <= *incoming.price; });
    } else {
        walk_side(bids_, [&](Price price) { return incoming.type == OrderType::Market || price >= *incoming.price; });
    }

    return plan;
}

OrderResult Book::modify_order(OrderId order_id, Price new_price, Quantity new_total_qty, Timestamp now) {
    const auto it = id_index_.find(order_id);
    if (it == id_index_.end()) {
        const RejectReason why =
            finished_orders_.contains(order_id) ? RejectReason::TooLate : RejectReason::UnknownOrder;
        return {{}, new_total_qty, why};
    }

    Node *node = it->second;
    Order replacement = node->order; // carries filled_quantity over
    replacement.price = new_price;
    replacement.quantity = new_total_qty;
    // Validate what the client actually sent (total > 0, price > 0).
    if (const RejectReason bad = validate_order_fields(replacement); bad != RejectReason::None)
        return {{}, new_total_qty, bad};

    const Quantity new_remaining = new_total_qty - node->order.filled;

    // In-flight fill: the client already has as much as (or more than) it
    // now wants. Cancel the rest (CME IFM behaviour). Allowed during a halt,
    // like any cancel: it only reduces risk.
    if (new_remaining <= 0) {
        emit({0, EventKind::Cancelled, now, node->order.id, 0, node->order.owner_id, 0, node->order.side,
              node->order.price, node->order.quantity});
        remove_resting(node); // after emit: node is freed here
        return {{}, 0, RejectReason::None};
    }
    replacement.quantity = new_remaining;

    // Same price and not bigger: edit in place, keep queue priority.
    if (new_price == node->order.price && new_remaining <= node->order.quantity) {
        reduce_resting(node, node->order.quantity - new_remaining);
        emit({0, EventKind::Modified, now, node->order.id, 0, node->order.owner_id, 0, node->order.side,
              node->order.price, node->order.quantity});
        return {{}, 0, RejectReason::None};
    }

    // Bigger or repriced: loses priority (unchanged from before).
    const RejectReason invalid = validate_new_order(replacement, now);
    if (invalid != RejectReason::None)
        return {{}, new_total_qty, invalid};

    emit({0, EventKind::Replaced, now, node->order.id, 0, node->order.owner_id, 0, node->order.side, node->order.price,
          node->order.quantity});
    remove_resting(node);
    return add_order(replacement, now);
}

OrderResult Book::add_order(Order incoming, Timestamp now) {
    const RejectReason invalid = validate_new_order(incoming, now);
    if (invalid != RejectReason::None)
        return {{}, incoming.quantity, invalid};

    if (id_in_use(incoming.id))
        return {{}, incoming.quantity, RejectReason::DuplicateOrderId};

    emit({0, EventKind::Accepted, now, incoming.id, 0, incoming.owner_id, 0, incoming.side, incoming.price,
          incoming.quantity});

    OrderResult result;
    MatchPlan plan = plan_match(incoming);

    bool halted_by_band = false;
    bool band_limited = false;
    // The out-of-band price level this call is allowed to trade at (the
    // grace print). The walk may finish that level but not go past it.
    std::optional<Price> breach_level;

    for (const ProposedFill &fill : plan.fills) {

        if (!within_band(fill.fill_price)) {
            if (!outside_band_since_) {
                outside_band_since_ = now;
                breach_level = fill.fill_price;
            } else if (now - *outside_band_since_ >= grace_period_ms_) {
                halted_by_band = true;
                halted_ = true;
                halt_until_ = now + halt_duration_ms_;
                emit({0, EventKind::Halted, now, incoming.id, 0, incoming.owner_id, 0, incoming.side, fill.fill_price,
                      incoming.quantity});
                break;
            } else if (!breach_level) {
                // This call gets one breach level during the active grace
                // period.
                breach_level = fill.fill_price;
            } else if (fill.fill_price != *breach_level) {
                // Grace permits one breach level, not a wider sweep.
                band_limited = true;
                break;
            }
        } else {
            outside_band_since_.reset();
        }

        const OwnerId passive_owner = fill.passive_node->order.owner_id;
        Trade trade = {next_trade_id_++,
                       fill.fill_price,
                       fill.fill_quantity,
                       incoming.id,
                       fill.passive_node->order.id,
                       incoming.side == Side::Buy ? incoming.owner_id : passive_owner,
                       incoming.side == Side::Sell ? incoming.owner_id : passive_owner,
                       incoming.side,
                       now};
        result.trades.push_back(trade);
        emit({0, EventKind::Trade, now, trade.aggressor_id, trade.passive_id, trade.buy_owner, trade.sell_owner,
              trade.aggressor_side, trade.price, trade.quantity});

        fill.passive_node->order.filled += fill.fill_quantity;
        reduce_resting(fill.passive_node, fill.fill_quantity);
        incoming.quantity -= fill.fill_quantity;
        incoming.filled += fill.fill_quantity;
        if (fill.passive_node->order.is_fully_filled()) {
            finished_orders_[fill.passive_node->order.id] = FinalState::Filled;
            remove_resting(fill.passive_node);
        }
    }
    if (incoming.quantity == 0)
        finished_orders_[incoming.id] = FinalState::Filled;
    if (halted_by_band || band_limited) {
        // Same treatment whether the walk stopped because of a halt or the
        // one-level sweep limit: a limit remainder rests at the band edge
        // (can't cross: everything left on the other side is past the edge), a
        // market remainder is cancelled.
        const RejectReason why = halted_by_band ? RejectReason::SymbolHalted : RejectReason::PriceBand;
        if (incoming.type == OrderType::Limit) {
            const Price band_edge = incoming.side == Side::Buy ? upper_band_price() : lower_band_price();
            incoming.price = incoming.side == Side::Buy ? std::min(*incoming.price, band_edge)
                                                        : std::max(*incoming.price, band_edge);

            if (validate_order_fields(incoming) == RejectReason::None) {
                Node *node = node_pool_.acquire();
                if (node != nullptr) {
                    node->order = incoming;
                    if (incoming.side == Side::Buy)
                        get_or_create_level(bids_, *incoming.price, node);
                    else
                        get_or_create_level(asks_, *incoming.price, node);
                    id_index_[incoming.id] = node;
                    emit({0, EventKind::Rested, now, incoming.id, 0, incoming.owner_id, 0, incoming.side,
                          incoming.price, incoming.quantity});
                    result.unaccepted_quantity = 0;
                    result.reject_reason = RejectReason::None;
                    result.rested_price = incoming.price;
                } else {
                    // A full pool cannot preserve the remainder; report why
                    // the walk stopped rather than dropping silently.
                    result.unaccepted_quantity = incoming.quantity;
                    result.reject_reason = why;
                    emit({0, EventKind::Cancelled, now, incoming.id, 0, incoming.owner_id, 0, incoming.side,
                          incoming.price, incoming.quantity});
                }
            } else {
                // An invalid computed edge falls back to the existing halt
                // rejection path instead of creating an invalid resting order.
                result.unaccepted_quantity = incoming.quantity;
                result.reject_reason = why;
                emit({0, EventKind::Cancelled, now, incoming.id, 0, incoming.owner_id, 0, incoming.side, incoming.price,
                      incoming.quantity});
            }
        } else {
            // A market remainder is never placed on the book; it is cancelled
            // and reports why (SymbolHalted or PriceBand).
            result.unaccepted_quantity = incoming.quantity;
            result.reject_reason = why;
            if (incoming.quantity > 0)
                emit({0, EventKind::Cancelled, now, incoming.id, 0, incoming.owner_id, 0, incoming.side, incoming.price,
                      incoming.quantity});
        }
    } else if (plan.halted_by_self_trade) {
        result.unaccepted_quantity = incoming.quantity;
        result.reject_reason = RejectReason::SelfTrade;
        emit({0, EventKind::Cancelled, now, incoming.id, 0, incoming.owner_id, 0, incoming.side, incoming.price,
              incoming.quantity});
    } else if (incoming.quantity > 0 && incoming.type == OrderType::Limit) {
        Node *node = node_pool_.acquire();
        if (node == nullptr) {
            result.unaccepted_quantity = incoming.quantity;
            result.reject_reason = RejectReason::PoolExhausted;
            // The order was Accepted; it must still reach a terminal event.
            emit({0, EventKind::Cancelled, now, incoming.id, 0, incoming.owner_id, 0, incoming.side, incoming.price,
                  incoming.quantity});
        } else {
            node->order = incoming;

            if (incoming.side == Side::Buy)
                get_or_create_level(bids_, *incoming.price, node);
            else
                get_or_create_level(asks_, *incoming.price, node);

            id_index_[incoming.id] = node;
            emit({0, EventKind::Rested, now, incoming.id, 0, incoming.owner_id, 0, incoming.side, incoming.price,
                  incoming.quantity});
            result.unaccepted_quantity = 0;
            result.reject_reason = RejectReason::None;
        }
    } else {
        result.unaccepted_quantity = incoming.quantity;
        result.reject_reason =
            plan.stopped_by_collar && incoming.quantity > 0 ? RejectReason::PriceCollar : RejectReason::None;
        if (incoming.type == OrderType::Market && incoming.quantity > 0)
            emit({0, EventKind::Cancelled, now, incoming.id, 0, incoming.owner_id, 0, incoming.side, incoming.price,
                  incoming.quantity});
    }

    if (!result.trades.empty()) {
        reference_price_ = result.trades.back().price;
        has_reference_price_ = true;
        last_trade_price_ = result.trades.back().price;

        const auto [lo, hi] = std::minmax_element(result.trades.begin(), result.trades.end(),
                                                  [](const Trade &a, const Trade &b) { return a.price < b.price; });
        check_and_trigger_stops(lo->price, hi->price, now);
    }
    return result;
}

RejectReason Book::cancel_order(OrderId order_id, Timestamp now) {
    auto it = id_index_.find(order_id);
    if (it == id_index_.end())
        return finished_orders_.contains(order_id) ? RejectReason::TooLate : RejectReason::UnknownOrder;

    Node *node = it->second;
    emit({0, EventKind::Cancelled, now, node->order.id, 0, node->order.owner_id, 0, node->order.side, node->order.price,
          node->order.quantity});
    remove_resting(node);
    return RejectReason::None;
}

bool Book::id_in_use(OrderId id) const {
    return id_index_.contains(id) || finished_orders_.contains(id) || stop_index_.contains(id);
}

void Book::remove_resting(Node *node) {
    unlink_and_maybe_erase_level(node);
    id_index_.erase(node->order.id);
    node_pool_.release(node);
}

} // namespace engine
