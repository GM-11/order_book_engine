#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

using namespace engine;

TEST_CASE("A resting order id cannot be reused while the order is live") {
    Book book;
    REQUIRE(book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 1)
                .reject_reason == RejectReason::None);
    book.drain_events();

    const auto dup =
        book.add_order({1, 22, Side::Buy, OrderType::Limit, 101, 1}, 2);
    CHECK(dup.reject_reason == RejectReason::DuplicateOrderId);
    CHECK(dup.unaccepted_quantity == 1);
    CHECK(book.drain_events().empty()); // rejected: never Accepted
    CHECK(book.best_bid() == 100);      // the original is untouched

    // The original stays reachable: one cancel empties the book.
    CHECK(book.cancel_order(1, 3));
    CHECK_FALSE(book.best_bid().has_value());
}

TEST_CASE("A duplicate id is rejected before it can match") {
    Book book;
    book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 1);
    book.add_order({2, 22, Side::Sell, OrderType::Limit, 105, 1}, 1);

    // Would cross the ask at 105 if it were allowed through.
    const auto dup =
        book.add_order({1, 33, Side::Buy, OrderType::Limit, 105, 1}, 2);
    CHECK(dup.reject_reason == RejectReason::DuplicateOrderId);
    CHECK(dup.trades.empty());
    CHECK(book.best_ask() == 105);
}

TEST_CASE("Stop ids share the id space with resting orders") {
    SECTION("stop cannot reuse a resting order id") {
        Book book;
        book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 1);
        CHECK(book.place_stop_order({1, 22, Side::Sell, 90, 1}, 2) ==
              RejectReason::DuplicateOrderId);
        CHECK_FALSE(book.cancel_stop_order(1, 3));
    }

    SECTION("order cannot reuse a dormant stop id") {
        Book book;
        REQUIRE(book.place_stop_order({1, 22, Side::Sell, 90, 1}, 1) ==
                RejectReason::None);
        CHECK(book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 2)
                  .reject_reason == RejectReason::DuplicateOrderId);
        CHECK_FALSE(book.best_bid().has_value());
    }

    SECTION("stop cannot reuse another dormant stop id") {
        Book book;
        REQUIRE(book.place_stop_order({1, 22, Side::Sell, 90, 1}, 1) ==
                RejectReason::None);
        CHECK(book.place_stop_order({1, 33, Side::Buy, 110, 1}, 2) ==
              RejectReason::DuplicateOrderId);
    }
}

TEST_CASE("Ids are live-only: modify and triggered stops keep their own id") {
    SECTION("a repriced order keeps its id") {
        Book book;
        book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 1);
        CHECK(book.modify_order(1, 101, 2, 2).reject_reason ==
              RejectReason::None);
        CHECK(book.best_bid() == 101);
    }

    SECTION("a triggered stop submits under its own id") {
        Book book;
        book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 2}, 1);
        REQUIRE(book.place_stop_order({5, 55, Side::Sell, 100, 1}, 1) ==
                RejectReason::None);
        book.add_order({2, 22, Side::Sell, OrderType::Limit, 100, 1}, 2);
        // The stop fired and filled against the remaining bid.
        CHECK_FALSE(book.best_bid().has_value());
    }
}
