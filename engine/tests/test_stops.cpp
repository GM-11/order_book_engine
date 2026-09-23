#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

using namespace engine;

TEST_CASE("Stop sells and buys fire inclusively and submit market orders") {
    SECTION("sell stop fires at its exact stop price") {
        Book book;
        book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 1);
        book.add_order({2, 12, Side::Buy, OrderType::Limit, 100, 3}, 2);
        book.place_stop_order({3, 13, Side::Sell, 100, 2});

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
        book.place_stop_order({3, 13, Side::Buy, 100, 2});

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
    book.place_stop_order({3, 13, Side::Sell, 100, 1});
    book.place_stop_order({4, 14, Side::Sell, 90, 1});

    const auto trigger = book.add_order({5, 15, Side::Sell, OrderType::Limit, 100, 1}, 3);

    REQUIRE(trigger.trades.size() == 1);
    CHECK(trigger.trades[0].price == 100);
    CHECK_FALSE(book.best_bid().has_value());
    CHECK_FALSE(book.cancel_stop_order(3));
    CHECK_FALSE(book.cancel_stop_order(4));
}

TEST_CASE("Cancelling a dormant stop prevents it from firing") {
    Book book;
    book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 1);
    book.add_order({2, 12, Side::Buy, OrderType::Limit, 100, 3}, 2);
    book.place_stop_order({3, 13, Side::Sell, 100, 2});
    CHECK(book.cancel_stop_order(3));

    book.add_order({4, 14, Side::Sell, OrderType::Limit, 100, 1}, 3);
    const auto remainder = book.add_order({5, 15, Side::Sell, OrderType::Market, 0, 3}, 4);

    REQUIRE(remainder.trades.size() == 1);
    CHECK(remainder.trades[0].quantity == 3);
}

TEST_CASE("Stop cancellation does not consume resting-order pool capacity") {
    Book book{1};
    book.add_order({1, 11, Side::Buy, OrderType::Limit, 90, 1}, 1);
    book.place_stop_order({2, 12, Side::Sell, 80, 1});

    CHECK(book.cancel_stop_order(2));
    CHECK(book.cancel_order(1));
    const auto replacement = book.add_order({3, 13, Side::Buy, OrderType::Limit, 90, 1}, 2);
    CHECK(replacement.reject_reason == RejectReason::None);
    CHECK(book.best_bid() == 90);
}
