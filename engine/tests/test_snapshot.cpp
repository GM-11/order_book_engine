// Book::snapshot(): the full order-by-order copy a subscriber starts from,
// and proof that snapshot + later events always rebuild the live book.
#include <catch2/catch_test_macros.hpp>

#include "book_replica.hpp"
#include "engine/book.hpp"

#include <random>
#include <unordered_map>
#include <vector>

using namespace engine;
using test_support::BookReplica;

namespace {
// Compares two snapshots field by field, stops in a fixed order.
void require_same(BookSnapshot a, BookSnapshot b) {
    BookReplica::sort_stops(a.stops);
    BookReplica::sort_stops(b.stops);
    REQUIRE(a.as_of_sequence == b.as_of_sequence);
    REQUIRE(a.bids == b.bids);
    REQUIRE(a.asks == b.asks);
    REQUIRE(a.stops == b.stops);
    REQUIRE(a.last_trade_price == b.last_trade_price);
    REQUIRE(a.halted == b.halted);
}
} // namespace

TEST_CASE("Snapshot of an empty book") {
    Book book;
    const BookSnapshot snap = book.snapshot();
    CHECK(snap.as_of_sequence == 0);
    CHECK(snap.bids.empty());
    CHECK(snap.asks.empty());
    CHECK(snap.stops.empty());
    CHECK_FALSE(snap.last_trade_price.has_value());
    CHECK_FALSE(snap.halted);
}

TEST_CASE("Snapshot lists every resting order, best price first, queue order within a price") {
    Book book;
    book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 5}, 1);
    book.add_order({2, 12, Side::Buy, OrderType::Limit, 101, 3}, 1);
    book.add_order({3, 13, Side::Buy, OrderType::Limit, 100, 2}, 1);
    book.add_order({4, 14, Side::Sell, OrderType::Limit, 103, 4}, 1);
    book.add_order({5, 15, Side::Sell, OrderType::Limit, 102, 6}, 1);
    book.add_order({6, 16, Side::Sell, OrderType::Limit, 102, 1}, 1);

    const BookSnapshot snap = book.snapshot();
    const std::vector<RestingOrder> bids{
        {2, 12, 101, 3, 0}, // best bid first
        {1, 11, 100, 5, 0}, // then 100: order 1 arrived before order 3
        {3, 13, 100, 2, 0},
    };
    const std::vector<RestingOrder> asks{
        {5, 15, 102, 6, 0},
        {6, 16, 102, 1, 0},
        {4, 14, 103, 4, 0},
    };
    CHECK(snap.bids == bids);
    CHECK(snap.asks == asks);
}

TEST_CASE("Snapshot shows remaining and filled quantity after a partial fill") {
    Book book;
    book.add_order({1, 11, Side::Sell, OrderType::Limit, 100, 10}, 1);
    book.add_order({2, 12, Side::Buy, OrderType::Market, std::nullopt, 4}, 2);

    const BookSnapshot snap = book.snapshot();
    REQUIRE(snap.asks.size() == 1);
    CHECK(snap.asks[0] == RestingOrder{1, 11, 100, 6, 4});
    CHECK(snap.last_trade_price == 100);
}

TEST_CASE("Snapshot lists dormant stops, and drops them once they trigger or are cancelled") {
    Book book;
    book.place_stop_order({7, 17, Side::Sell, 95, 2}, 1);
    book.place_stop_order({8, 18, Side::Buy, 110, 3, Price{112}}, 1);
    book.place_stop_order({9, 19, Side::Sell, 90, 1}, 1);

    BookSnapshot snap = book.snapshot();
    REQUIRE(snap.stops.size() == 3);
    // Sell stops first, by stop price; then buy stops.
    CHECK(snap.stops[0] == DormantStop{9, 19, Side::Sell, 90, std::nullopt, 1});
    CHECK(snap.stops[1] == DormantStop{7, 17, Side::Sell, 95, std::nullopt, 2});
    CHECK(snap.stops[2] == DormantStop{8, 18, Side::Buy, 110, Price{112}, 3});

    REQUIRE(book.cancel_stop_order(9, 19, 2) == RejectReason::None);
    // A trade at 95 wakes the sell stop at 95.
    book.add_order({20, 30, Side::Buy, OrderType::Limit, 95, 1}, 3);
    book.add_order({21, 31, Side::Sell, OrderType::Limit, 95, 1}, 3);

    snap = book.snapshot();
    REQUIRE(snap.stops.size() == 1);
    CHECK(snap.stops[0].order_id == 8);
}

TEST_CASE("Snapshot reports the halt and the last trade price") {
    Book book(100, 1000, 10, 50); // band 10%, grace 10ms, halt 50ms
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, std::nullopt, 1}, 0);
    book.add_order({3, 3, Side::Sell, OrderType::Limit, 120, 1}, 1);
    book.add_order({4, 4, Side::Buy, OrderType::Market, std::nullopt, 1}, 1);
    book.add_order({5, 5, Side::Sell, OrderType::Limit, 150, 1}, 20);
    REQUIRE(book.add_order({6, 6, Side::Buy, OrderType::Market, std::nullopt, 1}, 20).reject_reason ==
            RejectReason::SymbolHalted);

    BookSnapshot snap = book.snapshot();
    CHECK(snap.halted);
    CHECK(snap.last_trade_price == 120);

    // The first order after the halt time is over resumes the symbol.
    book.add_order({7, 7, Side::Buy, OrderType::Limit, 140, 1}, 100);
    snap = book.snapshot();
    CHECK_FALSE(snap.halted);
}

TEST_CASE("Snapshot as_of_sequence is the last emitted event; the next event follows it") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 5}, 1);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 2}, 2);
    const auto events = book.drain_events();
    REQUIRE_FALSE(events.empty());

    const BookSnapshot snap = book.snapshot();
    CHECK(snap.as_of_sequence == events.back().sequence_number);

    book.cancel_order(1, 1, 3);
    const auto later = book.drain_events();
    REQUIRE(later.size() == 1);
    CHECK(later[0].sequence_number == snap.as_of_sequence + 1);
}

TEST_CASE("Taking a snapshot changes nothing") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 5}, 1);
    book.place_stop_order({2, 2, Side::Sell, 90, 1}, 1);
    book.drain_events();

    const BookSnapshot first = book.snapshot();
    const BookSnapshot second = book.snapshot();
    require_same(first, second);
    CHECK(book.drain_events().empty()); // no events emitted
    CHECK(book.check_invariants());
}

TEST_CASE("A replica fed only events rebuilds a hand-made book exactly") {
    Book book;
    BookReplica replica;
    auto sync = [&] {
        for (const EngineEvent &e : book.drain_events())
            replica.apply(e);
        require_same(replica.to_snapshot(), book.snapshot());
    };
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 10}, 1);
    sync();
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 5}, 1);
    sync();
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 101, 12}, 2); // sweeps 1, partially fills 2
    sync();
    book.modify_order(2, 2, 100, 4, 3); // total 4, 2 filled: in-place reduce to 2 left
    sync();
    book.modify_order(2, 2, 102, 9, 4); // reprice: Replaced, then rests at 102
    sync();
    book.cancel_order(2, 2, 5);
    sync();
}

// The main property: at every step of a long random run, a replica built
// from events alone equals the engine's own snapshot. A second replica joins
// halfway from a snapshot (a late joiner) and must match too.
TEST_CASE("Snapshot + later events always rebuild the live book (random run)") {
    Book book(500, 300, 5, 20); // 3% band on prices 95..105: breaches and halts happen
    std::mt19937_64 rng(777);
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };

    BookReplica from_start;
    std::optional<BookReplica> late_joiner;
    OrderId next_id = 1;
    std::vector<OrderId> ids;
    std::vector<OrderId> stop_ids;
    std::unordered_map<OrderId, OwnerId> owner_of;
    Timestamp now = 0;
    bool saw_halt = false;
    bool saw_trigger = false;

    for (int step = 0; step < 6000; ++step) {
        now += pick(0, 3);
        const int op = pick(0, 9);
        const Side side = pick(0, 1) ? Side::Buy : Side::Sell;
        const OwnerId owner = static_cast<OwnerId>(pick(1, 6));

        if (op <= 4) {
            const OrderId id = next_id++;
            book.add_order({id, owner, side, OrderType::Limit, static_cast<Price>(pick(95, 105)),
                            static_cast<Quantity>(pick(1, 10))},
                           now);
            ids.push_back(id);
            owner_of[id] = owner;
        } else if (op == 5) {
            book.add_order({next_id++, owner, side, OrderType::Market, std::nullopt,
                            static_cast<Quantity>(pick(1, 15))},
                           now);
        } else if (op == 6 && !ids.empty()) {
            const OrderId id = ids[pick(0, static_cast<int>(ids.size()) - 1)];
            book.cancel_order(id, owner_of.at(id), now);
        } else if (op == 7 && !ids.empty()) {
            const OrderId id = ids[pick(0, static_cast<int>(ids.size()) - 1)];
            book.modify_order(id, owner_of.at(id), static_cast<Price>(pick(95, 105)),
                              static_cast<Quantity>(pick(1, 10)), now);
        } else if (op == 8) {
            const OrderId id = next_id++;
            const std::optional<Price> limit = pick(0, 1) ? std::optional<Price>(pick(93, 107)) : std::nullopt;
            book.place_stop_order(
                {id, owner, side, static_cast<Price>(pick(88, 112)), static_cast<Quantity>(pick(1, 5)), limit}, now);
            stop_ids.push_back(id);
            owner_of[id] = owner;
        } else if (op == 9 && !stop_ids.empty()) {
            const OrderId id = stop_ids[pick(0, static_cast<int>(stop_ids.size()) - 1)];
            if (pick(0, 1))
                book.cancel_stop_order(id, owner_of.at(id), now);
            else
                book.modify_stop_order(id, owner_of.at(id), static_cast<Price>(pick(88, 112)),
                                       pick(0, 1) ? std::optional<Price>(pick(93, 107)) : std::nullopt,
                                       static_cast<Quantity>(pick(1, 5)), now);
        }

        INFO("step " << step);
        for (const EngineEvent &e : book.drain_events()) {
            saw_halt |= e.kind == EventKind::Halted;
            saw_trigger |= e.kind == EventKind::StopTriggered;
            REQUIRE_NOTHROW(from_start.apply(e));
            if (late_joiner)
                REQUIRE_NOTHROW(late_joiner->apply(e));
        }
        const BookSnapshot live = book.snapshot();
        require_same(from_start.to_snapshot(), live);
        if (step == 3000)
            late_joiner.emplace(live); // joins from a snapshot, mid-run
        if (late_joiner)
            require_same(late_joiner->to_snapshot(), live);
    }
    // The run must actually exercise the hard cases.
    CHECK(saw_halt);
    CHECK(saw_trigger);
}
