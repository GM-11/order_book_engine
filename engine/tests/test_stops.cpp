#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

#include <algorithm>

using namespace engine;

TEST_CASE("Stop sells and buys fire inclusively and submit market orders") {
    SECTION("sell stop fires at its exact stop price") {
        Book book;
        book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 1);
        book.add_order({2, 12, Side::Buy, OrderType::Limit, 100, 3}, 2);
        book.place_stop_order({3, 13, Side::Sell, 100, 2}, 0);

        const auto trigger = book.add_order({4, 14, Side::Sell, OrderType::Limit, 100, 1}, 3);

        REQUIRE(trigger.trades.size() == 1);
        CHECK(trigger.trades[0].price == 100);
        CHECK(book.best_bid() == 100);
        const auto remainder = book.add_order({5, 15, Side::Sell, OrderType::Market, 0, 3}, 4);
        REQUIRE(remainder.trades.size() == 1);
        CHECK(remainder.trades[0].quantity == 1);
    }

    SECTION("buy stop fires at its exact stop price") {
        Book book;
        book.add_order({1, 11, Side::Sell, OrderType::Limit, 100, 1}, 1);
        book.add_order({2, 12, Side::Sell, OrderType::Limit, 100, 3}, 2);
        book.place_stop_order({3, 13, Side::Buy, 100, 2}, 0);

        const auto trigger = book.add_order({4, 14, Side::Buy, OrderType::Limit, 100, 1}, 3);

        REQUIRE(trigger.trades.size() == 1);
        CHECK(trigger.trades[0].price == 100);
        CHECK(book.best_ask() == 100);
        const auto remainder = book.add_order({5, 15, Side::Buy, OrderType::Market, 0, 3}, 4);
        REQUIRE(remainder.trades.size() == 1);
        CHECK(remainder.trades[0].quantity == 1);
    }
}

TEST_CASE("Triggered stops can cascade and are removed before firing") {
    Book book;
    book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 1);
    book.add_order({2, 12, Side::Buy, OrderType::Limit, 90, 1}, 2);
    book.place_stop_order({3, 13, Side::Sell, 100, 1}, 0);
    book.place_stop_order({4, 14, Side::Sell, 90, 1}, 0);

    const auto trigger = book.add_order({5, 15, Side::Sell, OrderType::Limit, 100, 1}, 3);

    REQUIRE(trigger.trades.size() == 1);
    CHECK(trigger.trades[0].price == 100);
    CHECK_FALSE(book.best_bid().has_value());
    CHECK_FALSE(book.cancel_stop_order(3, 0));
    CHECK_FALSE(book.cancel_stop_order(4, 0));
}

TEST_CASE("Cancelling a dormant stop prevents it from firing") {
    Book book;
    book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 1);
    book.add_order({2, 12, Side::Buy, OrderType::Limit, 100, 3}, 2);
    book.place_stop_order({3, 13, Side::Sell, 100, 2}, 0);
    CHECK(book.cancel_stop_order(3, 0));

    book.add_order({4, 14, Side::Sell, OrderType::Limit, 100, 1}, 3);
    const auto remainder = book.add_order({5, 15, Side::Sell, OrderType::Market, 0, 3}, 4);

    REQUIRE(remainder.trades.size() == 1);
    CHECK(remainder.trades[0].quantity == 3);
}

TEST_CASE("Stop cancellation does not consume resting-order pool capacity") {
    Book book{1};
    book.add_order({1, 11, Side::Buy, OrderType::Limit, 90, 1}, 1);
    book.place_stop_order({2, 12, Side::Sell, 80, 1}, 0);

    CHECK(book.cancel_stop_order(2, 0));
    CHECK(book.cancel_order(1, 0));
    const auto replacement = book.add_order({3, 13, Side::Buy, OrderType::Limit, 90, 1}, 2);
    CHECK(replacement.reject_reason == RejectReason::None);
    CHECK(book.best_bid() == 90);
}

// Policy pin (see README): a stop that triggers in the same call that trips
// the halt goes back to dormant. After the halt it is re-evaluated against
// post-halt trades like any other stop, so it may not fire if the market
// reopens on the other side of its stop price.
TEST_CASE("A stop put back by a halt is re-evaluated against post-halt trades") {
    Book book(100, 1000, 0, 30000); // grace 0: second breach in a walk halts
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 1}, 1);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 1}, 1);
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 99, 1}, 2);
    book.add_order({4, 4, Side::Buy, OrderType::Limit, 80, 1}, 2);
    book.add_order({5, 5, Side::Buy, OrderType::Limit, 70, 5}, 2);
    REQUIRE(book.place_stop_order({10, 10, Side::Sell, 99, 3}, 2) ==
            RejectReason::None);
    REQUIRE(book.place_stop_order({11, 11, Side::Sell, 99, 1}, 2) ==
            RejectReason::None);

    // Trade at 99 triggers both; stop 10 walks out of band and halts.
    book.add_order({6, 6, Side::Sell, OrderType::Limit, 99, 1}, 10);
    book.drain_events();

    const auto fired = [](const std::vector<EngineEvent> &events, OrderId id) {
        return std::any_of(events.begin(), events.end(), [id](const EngineEvent &e) {
            return e.kind == EventKind::StopTriggered && e.order_id == id;
        });
    };

    // Reopen with a trade at 100, above the sell stop: it stays dormant.
    book.add_order({20, 20, Side::Sell, OrderType::Limit, 100, 1}, 40000);
    book.add_order({21, 21, Side::Buy, OrderType::Limit, 100, 1}, 40001);
    CHECK_FALSE(fired(book.drain_events(), 11));

    // A post-halt trade at 99 fires it.
    book.add_order({22, 22, Side::Buy, OrderType::Limit, 99, 1}, 40002);
    book.add_order({23, 23, Side::Sell, OrderType::Limit, 99, 1}, 40003);
    const auto events = book.drain_events();
    CHECK(fired(events, 11));
    CHECK(std::any_of(events.begin(), events.end(), [](const EngineEvent &e) {
        return e.kind == EventKind::Trade && e.order_id == 11;
    }));
}
