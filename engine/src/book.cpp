#include "engine/book.hpp"

namespace engine {

void Book::unlink_and_maybe_erase_level(Node *node) {
    if (node->prev)
        node->prev->next = node->next;
    if (node->next)
        node->next->prev = node->prev;

    if (node->order.side == Side::Buy) {
        auto level_it = bids_.find(node->order.price);
        if (level_it == bids_.end())
            return;

        if (level_it->second.head == node)
            level_it->second.head = node->next;
        if (level_it->second.tail == node)
            level_it->second.tail = node->prev;
        if (level_it->second.head == nullptr)
            bids_.erase(level_it);
    } else {
        auto level_it = asks_.find(node->order.price);
        if (level_it == asks_.end())
            return;

        if (level_it->second.head == node)
            level_it->second.head = node->next;
        if (level_it->second.tail == node)
            level_it->second.tail = node->prev;
        if (level_it->second.head == nullptr)
            asks_.erase(level_it);
    }
}

std::vector<ProposedFill> Book::plan_match(const Order &incoming) const {
    std::vector<ProposedFill> fills;
    Quantity remaining = incoming.quantity;

    if (incoming.side == Side::Buy) {
        for (auto level_it = asks_.begin(); level_it != asks_.end();
             ++level_it) {

            if (remaining < 0)
                break;
            if (level_it->first > incoming.price)
                break;

            const Level &level = level_it->second;
            for (Node *node = level.head; node; node = node->next) {
                if (remaining < 0)
                    break;

                Quantity fill_qty = std::min(remaining, node->order.quantity);
                ProposedFill fill = {node, level_it->first, fill_qty};
                fills.push_back(fill);
                remaining -= fill_qty;
            }
        }
    } else {
        for (auto level_it = bids_.begin(); level_it != bids_.end();
             ++level_it) {

            if (remaining < 0)
                break;
            if (level_it->first < incoming.price)
                break;

            const Level &level = level_it->second;
            for (Node *node = level.head; node; node = node->next) {
                if (remaining < 0)
                    break;

                Quantity fill_qty = std::min(remaining, node->order.quantity);
                ProposedFill fill = {node, level_it->first, fill_qty};
                fills.push_back(fill);
                remaining -= fill_qty;
            }
        }
    }

    return fills;
}

std::vector<Trade> Book::add_order(Order incoming) {
    std::vector<Trade> trades;
    std::vector<ProposedFill> proposed_fills = plan_match(incoming);

    for (const ProposedFill &fill : proposed_fills) {
        Trade trade = {next_trade_id_++, fill.fill_price, fill.fill_quantity,
                       incoming.id, fill.passive_node->order.id};
        trades.push_back(trade);

        fill.passive_node->order.quantity -= fill.fill_quantity;
        incoming.quantity -= fill.fill_quantity;

        if (fill.passive_node->order.quantity == 0) {
            Node *node = fill.passive_node;
            OrderId passive_id = node->order.id;

            if (node->prev)
                node->prev->next = node->next;
            if (node->next)
                node->next->prev = node->prev;

            unlink_and_maybe_erase_level(node);

            id_index_.erase(passive_id);
            node_pool_.release(node);
        }
    }

    if (incoming.quantity > 0) {
        Node *node = node_pool_.acquire();
        if (node == nullptr)
            return trades;

        node->order.side = incoming.side;
        node->order.price = incoming.price;
        node->order.quantity = incoming.quantity;

        if (incoming.side == Side::Buy) {
            auto level_it = bids_.find(incoming.price);
            if (level_it == bids_.end()) {
                level_it =
                    bids_
                        .insert({incoming.price,
                                 Level{incoming.price, nullptr, nullptr}})
                        .first;
            }

            Level &level = level_it->second;
            if (level.head == nullptr) {
                level.head = node;
                level.tail = node;
            } else {
                node->prev = level.tail;
                level.tail->next = node;
                level.tail = node;
            }
        } else {
            auto level_it = asks_.find(incoming.price);
            if (level_it == asks_.end()) {
                level_it =
                    asks_
                        .insert({incoming.price,
                                 Level{incoming.price, nullptr, nullptr}})
                        .first;
            }

            Level &level = level_it->second;
            if (level.head == nullptr) {
                level.head = node;
                level.tail = node;
            } else {
                node->prev = level.tail;
                level.tail->next = node;
                level.tail = node;
            }
        }

        id_index_[incoming.id] = node;
    }

    return trades;
}

bool Book::cancel_order(OrderId order_id) {
    auto it = id_index_.find(order_id);
    if (it == id_index_.end())
        return false;

    Node *node = it->second;

    if (node->prev)
        node->prev->next = node->next;
    if (node->next)
        node->next->prev = node->prev;

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
