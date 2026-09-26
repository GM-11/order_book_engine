#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

using namespace engine;

TEST_CASE("Cancellation removes a resting order exactly once") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 1}, 1);

    CHECK(book.cancel_order(1));
    CHECK_FALSE(book.cancel_order(1));
    CHECK_FALSE(book.best_bid().has_value());
}

TEST_CASE("Best prices are empty initially and update as levels disappear") {
    Book book;
    CHECK_FALSE(book.best_bid().has_value());
    CHECK_FALSE(book.best_ask().has_value());

    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 1}, 1);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 99, 1}, 2);
    CHECK(book.best_bid() == 100);
    CHECK(book.cancel_order(1));
    CHECK(book.best_bid() == 99);
    CHECK(book.cancel_order(2));
    CHECK_FALSE(book.best_bid().has_value());

    book.add_order({3, 3, Side::Sell, OrderType::Limit, 101, 1}, 3);
    book.add_order({4, 4, Side::Sell, OrderType::Limit, 102, 1}, 4);
    CHECK(book.best_ask() == 101);
    book.add_order({5, 5, Side::Buy, OrderType::Limit, 101, 1}, 5);
    CHECK(book.best_ask() == 102);
    CHECK(book.cancel_order(4));
    CHECK_FALSE(book.best_ask().has_value());
}

TEST_CASE("A full node pool rejects a new resting order without partial application") {
    Book book{2};
    CHECK(book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 1}, 1).reject_reason ==
          RejectReason::None);
    CHECK(book.add_order({2, 2, Side::Buy, OrderType::Limit, 99, 1}, 2).reject_reason ==
          RejectReason::None);

    const auto exhausted = book.add_order({3, 3, Side::Buy, OrderType::Limit, 98, 7}, 3);
    CHECK(exhausted.reject_reason == RejectReason::PoolExhausted);
    CHECK(exhausted.unaccepted_quantity == 7);
    CHECK(book.best_bid() == 100);
    CHECK(book.cancel_order(3) == false);
}
