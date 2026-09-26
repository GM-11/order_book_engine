#include "engine/book.hpp"
#include "engine/order.hpp"
#include "engine/result.hpp"

#include <algorithm>
#include <cassert>

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

    // Update totals before a possible erase: after erase, `level` is gone.
    Level &level = level_it->second;
    level.total_quantity -= node->order.quantity;
    --level.order_count;

    if (level.head == node)
        level.head = node->next;
    if (level.tail == node)
        level.tail = node->prev;
    if (level.head == nullptr)
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
    level.total_quantity += node->order.quantity;
    ++level.order_count;
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

template <typename SideMap>
void Book::reduce_in_level(SideMap &side_map, Node *node, Quantity by) {
    auto it = side_map.find(node->order.price);
    assert(it != side_map.end()); // a resting order always has a level
    it->second.total_quantity -= by;
    node->order.quantity -= by;
}

void Book::reduce_resting(Node *node, Quantity by) {
    assert(by >= 0 && by <= node->order.quantity);
    if (node->order.side == Side::Buy)
        reduce_in_level(bids_, node, by);
    else
        reduce_in_level(asks_, node, by);
}

DepthSnapshot Book::depth(std::size_t max_levels) const {
    DepthSnapshot snap;
    snap.as_of_sequence = next_seq_ - 1;

    auto collect = [max_levels](const auto &side_map,
                                std::vector<DepthLevel> &out) {
        out.reserve(std::min(max_levels, side_map.size()));
        for (const auto &[price, level] : side_map) {
            if (out.size() == max_levels)
                break;
            out.push_back({price, level.total_quantity, level.order_count});
        }
    };
    collect(bids_, snap.bids);
    collect(asks_, snap.asks);
    return snap;
}

bool Book::check_invariants() const {
    std::size_t resting = 0;

    auto side_ok = [&](const auto &side_map, Side side) {
        for (const auto &[price, level] : side_map) {
            // No empty levels, and the level agrees with its map key.
            if (level.price != price || !level.head || !level.tail)
                return false;
            if (level.head->prev || level.tail->next)
                return false;

            Quantity sum = 0;
            std::uint32_t count = 0;
            const Node *prev = nullptr;
            for (const Node *n = level.head; n; n = n->next) {
                if (n->prev != prev) // links agree in both directions
                    return false;
                if (n->order.price != price || n->order.side != side ||
                    n->order.quantity <= 0)
                    return false;
                const auto it = id_index_.find(n->order.id);
                if (it == id_index_.end() || it->second != n)
                    return false;
                sum += n->order.quantity;
                ++count;
                prev = n;
            }
            if (prev != level.tail)
                return false;
            // The running totals equal the real sum and count.
            if (sum != level.total_quantity || count != level.order_count)
                return false;
            resting += count;
        }
        return true;
    };

    if (!side_ok(bids_, Side::Buy) || !side_ok(asks_, Side::Sell))
        return false;
    // Every indexed order is in some level, and vice versa.
    if (resting != id_index_.size())
        return false;
    // Never crossed.
    if (!bids_.empty() && !asks_.empty() &&
        bids_.begin()->first >= asks_.begin()->first)
        return false;
    return true;
}
} // namespace engine
