#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

using namespace engine;

TEST_CASE("Cancel Newest preserves resting self liquidity and its queue position") {
    Book book;
    book.add_order({1, 7, Side::Sell, OrderType::Limit, 100, 5}, 1);

    const auto rejected = book.add_order({2, 7, Side::Buy, OrderType::Limit, 100, 5}, 2);
    CHECK(rejected.trades.empty());
    CHECK(rejected.unaccepted_quantity == 5);
    CHECK(rejected.reject_reason == RejectReason::SelfTrade);
    CHECK(book.best_ask() == 100);

    const auto later = book.add_order({3, 8, Side::Buy, OrderType::Limit, 100, 5}, 3);
    REQUIRE(later.trades.size() == 1);
    CHECK(later.trades[0].passive_id == 1);
    CHECK_FALSE(book.best_ask().has_value());
}

TEST_CASE("Trades before a self-trade are retained and only the remainder is rejected") {
    Book book;
    book.add_order({1, 8, Side::Sell, OrderType::Limit, 100, 2}, 1);
    book.add_order({2, 7, Side::Sell, OrderType::Limit, 100, 5}, 2);

    const auto result = book.add_order({3, 7, Side::Buy, OrderType::Limit, 100, 6}, 3);

    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].passive_id == 1);
    CHECK(result.trades[0].quantity == 2);
    CHECK(result.unaccepted_quantity == 4);
    CHECK(result.reject_reason == RejectReason::SelfTrade);

    const auto later = book.add_order({4, 9, Side::Buy, OrderType::Limit, 100, 5}, 4);
    REQUIRE(later.trades.size() == 1);
    CHECK(later.trades[0].passive_id == 2);
    CHECK(later.trades[0].quantity == 5);
}
