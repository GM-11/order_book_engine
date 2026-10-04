#pragma once
#include "engine/book.hpp"
#include "engine/event.hpp"

#include <algorithm>
#include <functional>
#include <list>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace test_support {

class BookReplica {
  public:
    BookReplica() = default;
    explicit BookReplica(const engine::BookSnapshot &snap) { load(snap); }

    void load(const engine::BookSnapshot &snap) {
        *this = BookReplica{};
        last_seq_ = snap.as_of_sequence;
        for (const auto &o : snap.bids)
            add_resting(o.order_id, o.owner_id, engine::Side::Buy, o.price, o.quantity, o.filled);
        for (const auto &o : snap.asks)
            add_resting(o.order_id, o.owner_id, engine::Side::Sell, o.price, o.quantity, o.filled);
        for (const auto &s : snap.stops)
            stops_[s.order_id] = s;
        last_trade_ = snap.last_trade_price;
        halted_ = snap.halted;
    }

    void apply(const engine::EngineEvent &e) {
        using engine::EventKind;
        if (e.sequence_number != last_seq_ + 1)
            fail("gap: expected seq " + std::to_string(last_seq_ + 1) + ", got " + std::to_string(e.sequence_number));
        last_seq_ = e.sequence_number;

        switch (e.kind) {
        case EventKind::Accepted:
            break; // not on the book yet
        case EventKind::Rested:
            if (!e.price)
                fail("Rested without a price");
            add_resting(e.order_id, e.owner_id, e.side, *e.price, e.quantity, filled_[e.order_id]);
            break;
        case EventKind::Trade: {
            last_trade_ = e.price;
            filled_[e.order_id] += e.quantity;
            filled_[e.passive_id] += e.quantity;
            auto it = resting_.find(e.passive_id);
            if (it == resting_.end())
                fail("trade against unknown passive order " + std::to_string(e.passive_id));
            if (it->second.price != e.price)
                fail("trade price differs from the passive order's price");
            it->second.quantity -= e.quantity;
            if (it->second.quantity < 0)
                fail("passive order overfilled");
            if (it->second.quantity == 0)
                remove_resting(e.passive_id);
            break;
        }
        case EventKind::Cancelled:
            if (auto it = resting_.find(e.order_id); it != resting_.end()) {
                if (it->second.quantity != e.quantity)
                    fail("cancelled quantity differs from the resting quantity");
                remove_resting(e.order_id);
            } // else: never rested (market remainder, self-trade, ...)
            break;
        case EventKind::Modified: {
            auto it = resting_.find(e.order_id);
            if (it == resting_.end() || it->second.price != e.price)
                fail("modify of an order the replica doesn't have at that price");
            it->second.quantity = e.quantity; // keeps its place in the queue
            break;
        }
        case EventKind::Replaced:
            if (!resting_.contains(e.order_id))
                fail("replace of an order the replica doesn't have");
            remove_resting(e.order_id); // re-enters via Accepted/Trade/Rested
            break;
        case EventKind::StopAccepted:
        case EventKind::StopModified:
            stops_[e.order_id] = {e.order_id, e.owner_id, e.side, e.price.value(), e.limit_price, e.quantity};
            break;
        case EventKind::StopCancelled:
        case EventKind::StopTriggered:
            if (stops_.erase(e.order_id) == 0)
                fail("stop event for an unknown stop");
            break;
        case EventKind::Halted:
            halted_ = true;
            break;
        case EventKind::Resumed:
            halted_ = false;
            break;
        }
    }

    // Same shape as Book::snapshot(); stops sorted the same way.
    engine::BookSnapshot to_snapshot() const {
        engine::BookSnapshot snap{};
        snap.as_of_sequence = last_seq_;
        auto collect = [this](const auto &side_map, std::vector<engine::RestingOrder> &out) {
            for (const auto &[price, queue] : side_map)
                for (engine::OrderId id : queue) {
                    const Entry &o = resting_.at(id);
                    out.push_back({id, o.owner, price, o.quantity, filled_.contains(id) ? filled_.at(id) : 0});
                }
        };
        collect(bids_, snap.bids);
        collect(asks_, snap.asks);
        for (const auto &[id, s] : stops_)
            snap.stops.push_back(s);
        sort_stops(snap.stops);
        snap.last_trade_price = last_trade_;
        snap.halted = halted_;
        return snap;
    }

    engine::SequenceNumber last_seq() const { return last_seq_; }

    // The engine lists stops by (side, stop price, firing priority); the
    // replica can't know the priority, so compare stops in a fixed order.
    static void sort_stops(std::vector<engine::DormantStop> &stops) {
        std::sort(stops.begin(), stops.end(), [](const auto &a, const auto &b) { return a.order_id < b.order_id; });
    }

  private:
    struct Entry {
        engine::OwnerId owner;
        engine::Side side;
        engine::Price price;
        engine::Quantity quantity;
    };

    [[noreturn]] static void fail(const std::string &why) { throw std::runtime_error("replica: " + why); }

    void add_resting(engine::OrderId id, engine::OwnerId owner, engine::Side side, engine::Price price,
                     engine::Quantity qty, engine::Quantity filled) {
        if (resting_.contains(id))
            fail("order " + std::to_string(id) + " rested twice");
        resting_[id] = {owner, side, price, qty};
        if (filled != 0)
            filled_[id] = filled;
        if (side == engine::Side::Buy)
            bids_[price].push_back(id);
        else
            asks_[price].push_back(id);
    }

    void remove_resting(engine::OrderId id) {
        const Entry o = resting_.at(id);
        resting_.erase(id);
        auto erase_from = [&](auto &side_map) {
            auto level = side_map.find(o.price);
            level->second.remove(id);
            if (level->second.empty())
                side_map.erase(level);
        };
        if (o.side == engine::Side::Buy)
            erase_from(bids_);
        else
            erase_from(asks_);
    }

    std::map<engine::Price, std::list<engine::OrderId>, std::greater<engine::Price>> bids_;
    std::map<engine::Price, std::list<engine::OrderId>> asks_;
    std::unordered_map<engine::OrderId, Entry> resting_;
    std::unordered_map<engine::OrderId, engine::Quantity> filled_;
    std::map<engine::OrderId, engine::DormantStop> stops_;
    std::optional<engine::Price> last_trade_;
    bool halted_ = false;
    engine::SequenceNumber last_seq_ = 0;
};

} // namespace test_support
