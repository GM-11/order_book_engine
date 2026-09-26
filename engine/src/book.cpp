#include "engine/book.hpp"
#include "engine/order.hpp"

#include <algorithm>

namespace engine {

void Book::unlink_and_maybe_erase_level(Node *node) {
    if (node->order.side == Side::Buy)
        unlink_and_maybe_erase_from_level(bids_, node);
    else
        unlink_and_maybe_erase_from_level(asks_, node);
}

template <typename SideMap>
void Book::unlink_and_maybe_erase_from_level(SideMap &side_map, Node *node) {
    if (node->prev)
        node->prev->next = node->next;
    if (node->next)
        node->next->prev = node->prev;

    auto level_it = side_map.find(node->order.price);
    if (level_it == side_map.end())
        return;

    if (level_it->second.head == node)
        level_it->second.head = node->next;
    if (level_it->second.tail == node)
        level_it->second.tail = node->prev;
    if (level_it->second.head == nullptr)
        side_map.erase(level_it);
}

template <typename SideMap>
void Book::get_or_create_level(SideMap &side_map, Price price, Node *node) {
    auto it = side_map.find(price);
    if (it == side_map.end()) {
        it = side_map.insert({price, Level{price, nullptr, nullptr}}).first;
    }
    Level &level = it->second;
    if (level.head == nullptr) {
        level.head = node;
        level.tail = node;
    } else {
        node->prev = level.tail;
        level.tail->next = node;
        level.tail = node;
    }
}

MatchPlan Book::plan_match(const Order &incoming) const {
    MatchPlan plan;
    Quantity remaining = incoming.quantity;

    auto walk_side = [&](const auto &side_map, const auto &crosses) {
        for (auto level_it = side_map.begin();
             level_it != side_map.end() && remaining > 0; ++level_it) {
            if (!crosses(level_it->first))
                break;

            const Level &level = level_it->second;
            for (Node *node = level.head; node && remaining > 0;
                 node = node->next) {
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
        walk_side(asks_, [&](Price price) {
            return incoming.type == OrderType::Market ||
                   price <= incoming.price;
        });
    } else {
        walk_side(bids_, [&](Price price) {
            return incoming.type == OrderType::Market ||
                   price >= incoming.price;
        });
    }

    return plan;
}

void Book::place_stop_order(StopOrder stop) { pending_stops_.push_back(stop); }

bool Book::cancel_stop_order(OrderId order_id) {
    const auto it = std::find_if(
        pending_stops_.begin(), pending_stops_.end(),
        [order_id](const StopOrder &stop) { return stop.id == order_id; });
    if (it == pending_stops_.end())
        return false;

    pending_stops_.erase(it);
    return true;
}

void Book::check_and_trigger_stops(Price last_trade_price, Timestamp now) {
    if (halted_)
        return; // don't fire anything while halted; stops stay dormant

    std::vector<StopOrder> triggered;

    for (auto it = pending_stops_.begin(); it != pending_stops_.end();) {
        const bool fires =
            (it->side == Side::Sell && last_trade_price <= it->stop_price) ||
            (it->side == Side::Buy && last_trade_price >= it->stop_price);

        if (fires) {
            triggered.push_back(*it);
            it = pending_stops_.erase(it);
        } else {
            ++it;
        }
    }

    for (const StopOrder &stop : triggered) {
        add_order({stop.id, stop.owner_id, stop.side, OrderType::Market, 0,
                   stop.quantity},
                  now);
    }
}

Price Book::lower_band_price() const {
    const std::int64_t product = reference_price_ * (10000 - band_bps_);
    return (product + 9999) / 10000;
}

Price Book::upper_band_price() const {
    const std::int64_t product = reference_price_ * (10000 + band_bps_);
    return product / 10000;
}

RejectReason Book::validate_order_fields(const Order &order) const {
    if (order.type == OrderType::Limit && order.price <= 0)
        return RejectReason::InvalidPrice;
    if (order.quantity <= 0)
        return RejectReason::InvalidQuantity;
    return RejectReason::None;
}

RejectReason Book::validate_new_order(const Order &order, Timestamp now) {
    if (halted_) {
        if (now < halt_until_)
            return RejectReason::SymbolHalted;

        halted_ = false;
        outside_band_since_.reset();
    }
    return validate_order_fields(order);
}

OrderResult Book::modify_order(OrderId order_id, Price new_price,
                               Quantity new_qty, Timestamp now) {
    const auto it = id_index_.find(order_id);
    if (it == id_index_.end())
        return {{}, new_qty, RejectReason::UnknownOrder};

    Node *node = it->second;
    Order replacement = node->order;
    replacement.price = new_price;
    replacement.quantity = new_qty;

    if (new_price == node->order.price && new_qty <= node->order.quantity) {
        const RejectReason invalid = validate_order_fields(replacement);
        if (invalid != RejectReason::None)
            return {{}, new_qty, invalid};
        node->order.quantity = new_qty;
        return {{}, 0, RejectReason::None};
    }

    const RejectReason invalid = validate_new_order(replacement, now);
    if (invalid != RejectReason::None)
        return {{}, new_qty, invalid};

    cancel_order(order_id);
    return add_order(replacement, now);
}

OrderResult Book::add_order(Order incoming, Timestamp now) {
    const RejectReason invalid = validate_new_order(incoming, now);
    if (invalid != RejectReason::None)
        return {{}, incoming.quantity, invalid};

    OrderResult result;
    MatchPlan plan = plan_match(incoming);

    bool halted_by_band = false;

    for (const ProposedFill &fill : plan.fills) {

        if (!within_band(fill.fill_price)) {
            if (!outside_band_since_) {
                outside_band_since_ = now;
            } else if (now - *outside_band_since_ >= grace_period_ms_) {
                halted_by_band = true;
                halted_ = true;
                halt_until_ = now + halt_duration_ms_;
                break;
            }
        } else {
            outside_band_since_.reset();
        }

        Trade trade = {next_trade_id_++, fill.fill_price, fill.fill_quantity,
                       incoming.id, fill.passive_node->order.id};
        result.trades.push_back(trade);

        fill.passive_node->order.quantity -= fill.fill_quantity;
        incoming.quantity -= fill.fill_quantity;

        if (fill.passive_node->order.is_fully_filled()) {
            Node *node = fill.passive_node;
            OrderId passive_id = node->order.id;

            unlink_and_maybe_erase_level(node);
            id_index_.erase(passive_id);
            node_pool_.release(node);
        }
    }
    if (halted_by_band) {
        if (incoming.type == OrderType::Limit) {
            const Price band_edge = incoming.side == Side::Buy
                                        ? upper_band_price()
                                        : lower_band_price();
            incoming.price = incoming.side == Side::Buy
                                 ? std::min(incoming.price, band_edge)
                                 : std::max(incoming.price, band_edge);

            if (validate_order_fields(incoming) == RejectReason::None) {
                Node *node = node_pool_.acquire();
                if (node != nullptr) {
                    node->order = incoming;
                    if (incoming.side == Side::Buy)
                        get_or_create_level(bids_, incoming.price, node);
                    else
                        get_or_create_level(asks_, incoming.price, node);
                    id_index_[incoming.id] = node;
                    result.unaccepted_quantity = 0;
                    result.reject_reason = RejectReason::None;
                    result.rested_price = incoming.price;
                } else {
                    // A full pool cannot preserve the remainder; retain the
                    // existing halt rejection rather than dropping silently.
                    result.unaccepted_quantity = incoming.quantity;
                    result.reject_reason = RejectReason::SymbolHalted;
                }
            } else {
                // An invalid computed edge falls back to the existing halt
                // rejection path instead of creating an invalid resting order.
                result.unaccepted_quantity = incoming.quantity;
                result.reject_reason = RejectReason::SymbolHalted;
            }
        } else {
            // Market orders retain the existing halt behavior: their remainder
            // is not placed on the book and reports SymbolHalted.
            result.unaccepted_quantity = incoming.quantity;
            result.reject_reason = RejectReason::SymbolHalted;
        }
    } else if (plan.halted_by_self_trade) {
        result.unaccepted_quantity = incoming.quantity;
        result.reject_reason = RejectReason::SelfTrade;
    } else if (incoming.quantity > 0 && incoming.type == OrderType::Limit) {
        Node *node = node_pool_.acquire();
        if (node == nullptr) {
            result.unaccepted_quantity = incoming.quantity;
            result.reject_reason = RejectReason::PoolExhausted;
        } else {
            node->order = incoming;

            if (incoming.side == Side::Buy)
                get_or_create_level(bids_, incoming.price, node);
            else
                get_or_create_level(asks_, incoming.price, node);

            id_index_[incoming.id] = node;
            result.unaccepted_quantity = 0;
            result.reject_reason = RejectReason::None;
        }
    } else {
        result.unaccepted_quantity = 0;
        result.reject_reason = RejectReason::None;
    }

    if (!result.trades.empty()) {
        reference_price_ = result.trades.back().price;
        has_reference_price_ = true;
        check_and_trigger_stops(result.trades.back().price, now);
    }
    return result;
}

bool Book::cancel_order(OrderId order_id) {
    auto it = id_index_.find(order_id);
    if (it == id_index_.end())
        return false;

    Node *node = it->second;

    unlink_and_maybe_erase_level(node);

    id_index_.erase(order_id);
    node_pool_.release(node);

    return true;
}

std::optional<Price> Book::best_bid() const {
    if (bids_.empty())
        return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> Book::best_ask() const {
    if (asks_.empty())
        return std::nullopt;
    return asks_.begin()->first;
}

} // namespace engine
