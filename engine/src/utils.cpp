#include "engine/book.hpp"
#include "engine/order.hpp"
#include "engine/result.hpp"

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

template void
Book::get_or_create_level(std::map<Price, Level, std::greater<Price>> &, Price,
                          Node *);
template void Book::get_or_create_level(std::map<Price, Level> &, Price,
                                        Node *);

Price Book::lower_band_price() const {
    const std::int64_t product = reference_price_ * (10000 - band_bps_);
    return (product + 9999) / 10000;
}

Price Book::upper_band_price() const {
    const std::int64_t product = reference_price_ * (10000 + band_bps_);
    return product / 10000;
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
        emit({0, EventKind::Resumed, now, 0, 0, 0, 0, Side::Buy, 0, 0});
    }
    return validate_order_fields(order);
}

} // namespace engine
