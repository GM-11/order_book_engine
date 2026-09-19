#include "engine/book.hpp"
#include "engine/order.hpp"

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

std::vector<ProposedFill> Book::plan_match(const Order &incoming) const {
    std::vector<ProposedFill> fills;
    Quantity remaining = incoming.quantity;

    if (incoming.side == Side::Buy) {
        for (auto level_it = asks_.begin();
             level_it != asks_.end() && remaining > 0; ++level_it) {
            if (incoming.type == OrderType::Limit &&
                level_it->first > incoming.price)
                break;

            const Level &level = level_it->second;
            for (Node *node = level.head; node && remaining > 0;
                 node = node->next) {
                Quantity fill_qty = std::min(remaining, node->order.quantity);
                fills.push_back({node, level_it->first, fill_qty});
                remaining -= fill_qty;
            }
        }
    } else {
        for (auto level_it = bids_.begin();
             level_it != bids_.end() && remaining > 0; ++level_it) {
            if (incoming.type == OrderType::Limit &&
                level_it->first < incoming.price)
                break;

            const Level &level = level_it->second;
            for (Node *node = level.head; node && remaining > 0;
                 node = node->next) {
                Quantity fill_qty = std::min(remaining, node->order.quantity);
                fills.push_back({node, level_it->first, fill_qty});
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

        if (fill.passive_node->order.is_fully_filled()) {
            Node *node = fill.passive_node;
            OrderId passive_id = node->order.id;

            unlink_and_maybe_erase_level(node);

            id_index_.erase(passive_id);
            node_pool_.release(node);
        }
    }

    if (incoming.quantity > 0 && incoming.type == OrderType::Limit) {
        Node *node = node_pool_.acquire();
        if (node == nullptr)
            return trades;

        node->order.side = incoming.side;
        node->order.price = incoming.price;
        node->order.quantity = incoming.quantity;

        if (incoming.side == Side::Buy)
            get_or_create_level(bids_, incoming.price, node);
        else
            get_or_create_level(asks_, incoming.price, node);

        id_index_[incoming.id] = node;
    }

    return trades;
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
