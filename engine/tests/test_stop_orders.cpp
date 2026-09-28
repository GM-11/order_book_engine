// Stop-limit orders, stop modify, the "would trigger immediately" entry rule,
// stop cancel reasons, and market orders having no price.
#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

#include <algorithm>
#include <vector>

using namespace engine;

namespace {
// One trade at `price` between two throwaway owners (ids 1000+).
void print_trade(Book &book, Price price, OrderId &next_id, Timestamp now) {
    book.add_order({next_id++, 1000, Side::Sell, OrderType::Limit, price, 1},
                   now);
    book.add_order({next_id++, 1001, Side::Buy, OrderType::Limit, price, 1},
                   now);
}

std::vector<OrderId> triggered_ids(const std::vector<EngineEvent> &events) {
    std::vector<OrderId> ids;
    for (const auto &e : events)
        if (e.kind == EventKind::StopTriggered)
            ids.push_back(e.order_id);
    return ids;
}
} // namespace

TEST_CASE("Market orders carry no price; limit orders must have one") {
    Book book;
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 5}, 0);

    SECTION("a market order with a price is rejected, not silently ignored") {
        const auto r =
            book.add_order({2, 2, Side::Buy, OrderType::Market, 100, 1}, 1);
        CHECK(r.reject_reason == RejectReason::InvalidPrice);
        CHECK(r.trades.empty());
        CHECK(book.best_ask() == 100);
    }

    SECTION("a limit order without a price is rejected") {
        const auto r = book.add_order(
            {2, 2, Side::Buy, OrderType::Limit, std::nullopt, 1}, 1);
        CHECK(r.reject_reason == RejectReason::InvalidPrice);
        CHECK(r.trades.empty());
    }

    SECTION("a market order's events have no price, its trades do") {
        book.drain_events();
        book.add_order({2, 2, Side::Buy, OrderType::Market, std::nullopt, 7},
                       1);
        const auto events = book.drain_events();
        REQUIRE(events.size() == 3); // Accepted, Trade, Cancelled (2 left)
        CHECK(events[0].kind == EventKind::Accepted);
        CHECK_FALSE(events[0].price.has_value());
        CHECK(events[1].kind == EventKind::Trade);
        CHECK(events[1].price == 100);
        CHECK(events[2].kind == EventKind::Cancelled);
        CHECK_FALSE(events[2].price.has_value());
        CHECK(events[2].quantity == 2);
    }
}

TEST_CASE("cancel_stop_order says why it could not cancel") {
    Book book;
    OrderId next = 500;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 2}, 0);
    REQUIRE(book.place_stop_order({10, 10, Side::Sell, 100, 1}, 0) ==
            RejectReason::None);
    REQUIRE(book.place_stop_order({11, 11, Side::Sell, 90, 1}, 0) ==
            RejectReason::None);

    SECTION("dormant: cancelled; a second cancel is too late") {
        CHECK(book.cancel_stop_order(11, 1) == RejectReason::None);
        CHECK(book.cancel_stop_order(11, 2) == RejectReason::TooLate);
        CHECK(book.final_state(11) == FinalState::Cancelled);
    }

    SECTION("already triggered: too late") {
        print_trade(book, 100, next, 1); // wakes stop 10 (sell at 100)
        CHECK(book.cancel_stop_order(10, 2) == RejectReason::TooLate);
        CHECK(book.cancel_stop_order(11, 2) == RejectReason::None);
    }

    SECTION("never a stop: unknown, even if the id is a live limit order") {
        CHECK(book.cancel_stop_order(1, 1) == RejectReason::UnknownOrder);
        CHECK(book.cancel_stop_order(999, 1) == RejectReason::UnknownOrder);
    }
}

TEST_CASE("A stop-limit fires as a limit order and can rest") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 97, 1}, 0);
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 94, 5}, 0);
    book.drain_events();
    // Sell stop-limit: wake at <= 99, then sell 4 no lower than 96.
    REQUIRE(book.place_stop_order({10, 10, Side::Sell, 99, 4, 96}, 0) ==
            RejectReason::None);
    const auto accepted = book.drain_events();
    REQUIRE(accepted.size() == 1);
    CHECK(accepted[0].kind == EventKind::StopAccepted);
    CHECK(accepted[0].price == 99);
    CHECK(accepted[0].limit_price == 96);

    // A trade at 100 does not wake it; the next sell prints 97 and does.
    book.add_order({20, 20, Side::Sell, OrderType::Limit, 100, 1}, 1);
    REQUIRE(triggered_ids(book.drain_events()).empty());
    book.add_order({21, 21, Side::Sell, OrderType::Limit, 97, 1}, 2);
    REQUIRE(triggered_ids(book.drain_events()) == std::vector<OrderId>{10});

    // The 97 bid is gone (taken by order 21), so the stop-limit finds nothing
    // at >= 96: the 94 bid is below its limit. All 4 rest as an ask at 96.
    CHECK(book.best_ask() == 96);
    CHECK(book.best_bid() == 94);
    const auto d = book.depth(1);
    REQUIRE(d.asks.size() == 1);
    CHECK(d.asks[0].quantity == 4);

    // It is now a live limit order: cancel_stop_order is too late,
    // cancel_order works.
    CHECK(book.cancel_stop_order(10, 3) == RejectReason::TooLate);
    CHECK(book.cancel_order(10, 3) == RejectReason::None);
    CHECK_FALSE(book.best_ask().has_value());
    CHECK(book.check_invariants());
}

TEST_CASE("A stop-limit fills up to its limit and rests the rest") {
    Book book;
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 101, 2}, 0);
    book.add_order({3, 3, Side::Sell, OrderType::Limit, 105, 5}, 0);
    // Buy stop-limit: wake at >= 100, buy 5 paying at most 102.
    REQUIRE(book.place_stop_order({10, 10, Side::Buy, 100, 5, 102}, 0) ==
            RejectReason::None);

    book.add_order({20, 20, Side::Buy, OrderType::Limit, 100, 1}, 1);
    const auto events = book.drain_events();
    Quantity stop_filled = 0;
    for (const auto &e : events)
        if (e.kind == EventKind::Trade && e.order_id == 10) {
            CHECK(e.price == 101);
            stop_filled += e.quantity;
        }
    CHECK(stop_filled == 2);
    CHECK(book.best_bid() == 102); // 3 left, resting at its limit
    CHECK(book.best_ask() == 105);
    CHECK(book.depth(1).bids[0].quantity == 3);
    CHECK(book.check_invariants());
}

TEST_CASE("Stop-limit rejects a non-positive limit price") {
    Book book;
    CHECK(book.place_stop_order({1, 1, Side::Sell, 90, 1, 0}, 0) ==
          RejectReason::InvalidPrice);
    CHECK(book.place_stop_order({1, 1, Side::Sell, 90, 1, -5}, 0) ==
          RejectReason::InvalidPrice);
    CHECK(book.cancel_stop_order(1, 0) == RejectReason::UnknownOrder);
}

TEST_CASE("A stop the last trade has already reached is rejected on entry") {
    Book book;
    OrderId next = 500;

    SECTION("no trade yet: nothing to compare against, accepted") {
        CHECK(book.place_stop_order({1, 1, Side::Sell, 100, 1}, 0) ==
              RejectReason::None);
        CHECK(book.place_stop_order({2, 2, Side::Buy, 100, 1}, 0) ==
              RejectReason::None);
    }

    SECTION("sell stop: at or above the last trade is rejected") {
        print_trade(book, 100, next, 0);
        CHECK(book.place_stop_order({1, 1, Side::Sell, 100, 1}, 1) ==
              RejectReason::StopWouldTrigger); // equal: a trade at 100 fires it
        CHECK(book.place_stop_order({2, 2, Side::Sell, 101, 1}, 1) ==
              RejectReason::StopWouldTrigger);
        CHECK(book.place_stop_order({3, 3, Side::Sell, 99, 1}, 1) ==
              RejectReason::None);
        // A rejected stop leaves no trace: its id is free.
        CHECK(book.cancel_stop_order(1, 1) == RejectReason::UnknownOrder);
        CHECK(book.place_stop_order({1, 1, Side::Sell, 98, 1}, 1) ==
              RejectReason::None);
    }

    SECTION("buy stop: at or below the last trade is rejected") {
        print_trade(book, 100, next, 0);
        CHECK(book.place_stop_order({1, 1, Side::Buy, 100, 1}, 1) ==
              RejectReason::StopWouldTrigger);
        CHECK(book.place_stop_order({2, 2, Side::Buy, 99, 1}, 1) ==
              RejectReason::StopWouldTrigger);
        CHECK(book.place_stop_order({3, 3, Side::Buy, 101, 1}, 1) ==
              RejectReason::None);
    }

    SECTION("nothing was emitted for a rejected stop") {
        print_trade(book, 100, next, 0);
        book.drain_events();
        book.place_stop_order({1, 1, Side::Sell, 100, 1}, 1);
        CHECK(book.drain_events().empty());
    }
}

TEST_CASE("Modifying a stop moves its trigger") {
    Book book;
    OrderId next = 500;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 95, 10}, 0);
    print_trade(book, 100, next, 0); // last trade 100
    REQUIRE(book.place_stop_order({10, 10, Side::Sell, 98, 1}, 1) ==
            RejectReason::None);
    book.drain_events();

    REQUIRE(book.modify_stop_order(10, 90, std::nullopt, 2, 2) ==
            RejectReason::None);
    const auto modified = book.drain_events();
    REQUIRE(modified.size() == 1);
    CHECK(modified[0].kind == EventKind::StopModified);
    CHECK(modified[0].price == 90);
    CHECK(modified[0].quantity == 2);

    // A trade at 95 would have woken the old 98 stop; the new 90 one sleeps.
    book.add_order({20, 20, Side::Sell, OrderType::Limit, 95, 1}, 3);
    CHECK(triggered_ids(book.drain_events()).empty());
    CHECK(book.depth(1).bids[0].quantity == 9);

    // Its old key is really gone: a sweep down to 90 (9 at 95, 1 at 90)
    // fires it exactly once, for 2.
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 90, 5}, 4);
    book.add_order({21, 21, Side::Sell, OrderType::Limit, 90, 10}, 5);
    const auto events = book.drain_events();
    CHECK(triggered_ids(events) == std::vector<OrderId>{10});
    Quantity fired = 0;
    for (const auto &e : events)
        if (e.kind == EventKind::Trade && e.order_id == 10)
            fired += e.quantity;
    CHECK(fired == 2);
    CHECK(book.modify_stop_order(10, 80, std::nullopt, 1, 6) ==
          RejectReason::TooLate);
    CHECK(book.check_invariants());
}

TEST_CASE("Modifying a stop can turn it into a stop-limit") {
    Book book;
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 110, 1}, 0);
    REQUIRE(book.place_stop_order({10, 10, Side::Buy, 100, 1}, 0) ==
            RejectReason::None);
    REQUIRE(book.modify_stop_order(10, 100, 105, 1, 1) == RejectReason::None);

    // As a stop-market it would have bought the 110 ask. As a stop-limit
    // capped at 105 it rests a bid at 105 instead.
    book.add_order({20, 20, Side::Buy, OrderType::Limit, 100, 1}, 2);
    CHECK(book.best_ask() == 110);
    CHECK(book.best_bid() == 105);
    CHECK(book.check_invariants());
}

TEST_CASE("Rejected stop modifies leave the stop unchanged") {
    Book book;
    OrderId next = 500;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 90, 5}, 0);
    print_trade(book, 100, next, 0); // last trade 100
    REQUIRE(book.place_stop_order({10, 10, Side::Sell, 95, 2}, 1) ==
            RejectReason::None);
    book.drain_events();

    CHECK(book.modify_stop_order(10, 100, std::nullopt, 2, 2) ==
          RejectReason::StopWouldTrigger);
    CHECK(book.modify_stop_order(10, 95, std::nullopt, 0, 2) ==
          RejectReason::InvalidQuantity);
    CHECK(book.modify_stop_order(10, 0, std::nullopt, 2, 2) ==
          RejectReason::InvalidPrice);
    CHECK(book.modify_stop_order(10, 95, 0, 2, 2) ==
          RejectReason::InvalidPrice);
    CHECK(book.modify_stop_order(77, 95, std::nullopt, 2, 2) ==
          RejectReason::UnknownOrder);
    CHECK(book.drain_events().empty());

    // Still a 95 stop-market for 2.
    book.add_order({20, 20, Side::Sell, OrderType::Limit, 90, 1}, 3);
    Quantity fired = 0;
    for (const auto &e : book.drain_events())
        if (e.kind == EventKind::Trade && e.order_id == 10)
            fired += e.quantity;
    CHECK(fired == 2);
}

TEST_CASE("Stop modify: only a size reduction keeps its firing place") {
    // Three sell stops at 99, placed A, B, C. They wake together.
    auto run = [](auto &&modify) {
        Book book;
        book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 1}, 0);
        book.add_order({2, 1, Side::Buy, OrderType::Limit, 99, 1}, 0);
        book.add_order({3, 1, Side::Buy, OrderType::Limit, 50, 100}, 0);
        for (OrderId id : {10, 11, 12})
            REQUIRE(book.place_stop_order({id, id, Side::Sell, 99, 3}, 0) ==
                    RejectReason::None);
        modify(book);
        book.drain_events();
        book.add_order({20, 20, Side::Sell, OrderType::Limit, 99, 2}, 1);
        return triggered_ids(book.drain_events());
    };

    CHECK(run([](Book &) {}) == std::vector<OrderId>{10, 11, 12});
    CHECK(run([](Book &b) {
              REQUIRE(b.modify_stop_order(10, 99, std::nullopt, 1, 0) ==
                      RejectReason::None);
          }) == std::vector<OrderId>{10, 11, 12}); // smaller: keeps place
    CHECK(run([](Book &b) {
              REQUIRE(b.modify_stop_order(10, 99, std::nullopt, 5, 0) ==
                      RejectReason::None);
          }) == std::vector<OrderId>{11, 12, 10}); // bigger: to the back
    CHECK(run([](Book &b) {
              REQUIRE(b.modify_stop_order(10, 99, 60, 3, 0) ==
                      RejectReason::None);
          }) == std::vector<OrderId>{11, 12, 10}); // new limit: to the back
}

TEST_CASE("Stops sent back by a halt fire ahead of stops that never woke") {
    Book book(100, 1000, 0, 30000); // grace 0: second breach in a walk halts
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 1}, 1);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 1}, 1); // last 100
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 99, 1}, 2);
    book.add_order({4, 4, Side::Buy, OrderType::Limit, 80, 1}, 2);
    book.add_order({5, 5, Side::Buy, OrderType::Limit, 70, 20}, 2);
    // Stop 9 is placed first, far below; 10 and 11 wake at 99.
    REQUIRE(book.place_stop_order({9, 9, Side::Sell, 75, 1}, 2) ==
            RejectReason::None);
    REQUIRE(book.place_stop_order({10, 10, Side::Sell, 99, 3}, 2) ==
            RejectReason::None);
    REQUIRE(book.place_stop_order({11, 11, Side::Sell, 99, 1}, 2) ==
            RejectReason::None);

    // Trade at 99 wakes 10 and 11. Stop 10 sells at 80 (first breach), then
    // at 70 (second breach): halt. Stop 11 goes back to dormant.
    book.add_order({6, 6, Side::Sell, OrderType::Limit, 99, 1}, 10);
    REQUIRE(triggered_ids(book.drain_events()) == std::vector<OrderId>{10});

    // After the halt (last trade 80, band [72, 88]) a trade at 73 wakes both
    // 9 and 11. 11 already woke once, so it goes first even though 9 is older.
    book.add_order({30, 30, Side::Buy, OrderType::Limit, 73, 1}, 40000);
    book.add_order({31, 31, Side::Sell, OrderType::Limit, 73, 1}, 40001);
    CHECK(triggered_ids(book.drain_events()) == std::vector<OrderId>{11, 9});
    CHECK(book.check_invariants());
}

TEST_CASE("Buy and sell stops woken by one sweep fire in entry order") {
    Book book;
    // Bids at 100 and 90; a market sell sweeps both, printing 100 then 90.
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 1, Side::Buy, OrderType::Limit, 90, 1}, 0);
    book.add_order({3, 1, Side::Sell, OrderType::Limit, 200, 10}, 0);
    book.add_order({4, 1, Side::Buy, OrderType::Limit, 50, 10}, 0);
    // Placed: sell@95, buy@100, sell@91. Sweep low 90 wakes both sells,
    // sweep high 100 wakes the buy.
    REQUIRE(book.place_stop_order({12, 12, Side::Sell, 95, 1}, 0) ==
            RejectReason::None);
    REQUIRE(book.place_stop_order({10, 10, Side::Buy, 100, 1}, 0) ==
            RejectReason::None);
    REQUIRE(book.place_stop_order({11, 11, Side::Sell, 91, 1}, 0) ==
            RejectReason::None);
    book.drain_events();

    book.add_order({20, 20, Side::Sell, OrderType::Market, std::nullopt, 2}, 1);
    CHECK(triggered_ids(book.drain_events()) ==
          std::vector<OrderId>{12, 10, 11});
    CHECK(book.check_invariants());
}
