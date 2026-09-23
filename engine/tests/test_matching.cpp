#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

using namespace engine;

TEST_CASE("A non-crossing limit order rests and updates the best bid") {
    Book book;
    const auto result = book.add_order({1, 10, Side::Buy, OrderType::Limit, 100, 5}, 1);

    CHECK(result.remaining_quantity == 0);
    CHECK(result.reject_reason == RejectReason::None);
    CHECK(result.trades.empty());
    CHECK(book.best_bid() == 100);
    CHECK_FALSE(book.best_ask().has_value());
}

TEST_CASE("A crossing limit order executes at the passive price and removes it") {
    Book book;
    book.add_order({1, 10, Side::Sell, OrderType::Limit, 100, 5}, 1);

    const auto result = book.add_order({2, 20, Side::Buy, OrderType::Limit, 105, 5}, 2);

    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].price == 100);
    CHECK(result.trades[0].quantity == 5);
    CHECK(result.trades[0].aggressor_id == 2);
    CHECK(result.trades[0].passive_id == 1);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.reject_reason == RejectReason::None);
    CHECK_FALSE(book.best_ask().has_value());
}

TEST_CASE("A partial fill leaves only the resting remainder") {
    Book book;
    book.add_order({1, 10, Side::Sell, OrderType::Limit, 100, 10}, 1);

    const auto first = book.add_order({2, 20, Side::Buy, OrderType::Limit, 100, 4}, 2);
    const auto second = book.add_order({3, 30, Side::Buy, OrderType::Limit, 100, 6}, 3);

    REQUIRE(first.trades.size() == 1);
    CHECK(first.trades[0].quantity == 4);
    REQUIRE(second.trades.size() == 1);
    CHECK(second.trades[0].passive_id == 1);
    CHECK(second.trades[0].quantity == 6);
    CHECK_FALSE(book.best_ask().has_value());
}

TEST_CASE("Matching walks price levels and same-level orders in FIFO order") {
    Book book;
    book.add_order({1, 11, Side::Sell, OrderType::Limit, 100, 2}, 1);
    book.add_order({2, 12, Side::Sell, OrderType::Limit, 100, 3}, 2);
    book.add_order({3, 13, Side::Sell, OrderType::Limit, 100, 4}, 3);
    book.add_order({4, 14, Side::Sell, OrderType::Limit, 101, 5}, 4);

    const auto result = book.add_order({5, 99, Side::Buy, OrderType::Limit, 101, 12}, 5);

    REQUIRE(result.trades.size() == 4);
    CHECK(result.trades[0].passive_id == 1);
    CHECK(result.trades[0].quantity == 2);
    CHECK(result.trades[1].passive_id == 2);
    CHECK(result.trades[1].quantity == 3);
    CHECK(result.trades[2].passive_id == 3);
    CHECK(result.trades[2].quantity == 4);
    CHECK(result.trades[3].passive_id == 4);
    CHECK(result.trades[3].price == 101);
    CHECK(result.trades[3].quantity == 3);
    CHECK(book.best_ask() == 101);
}

TEST_CASE("A market order consumes liquidity without resting its excess") {
    Book book;
    book.add_order({1, 10, Side::Sell, OrderType::Limit, 100, 2}, 1);
    book.add_order({2, 20, Side::Sell, OrderType::Limit, 101, 3}, 2);

    const auto result = book.add_order({3, 30, Side::Buy, OrderType::Market, 0, 8}, 3);

    REQUIRE(result.trades.size() == 2);
    CHECK(result.trades[0].quantity == 2);
    CHECK(result.trades[1].quantity == 3);
    CHECK(result.remaining_quantity == 0);
    CHECK(result.reject_reason == RejectReason::None);
    CHECK_FALSE(book.best_ask().has_value());
    CHECK_FALSE(book.best_bid().has_value());
}

TEST_CASE("Invalid limit prices and quantities are rejected before matching") {
    Book book;

    CHECK(book.add_order({1, 1, Side::Buy, OrderType::Limit, 0, 1}, 1).reject_reason ==
          RejectReason::InvalidPrice);
    CHECK(book.add_order({2, 1, Side::Buy, OrderType::Limit, -1, 1}, 2).reject_reason ==
          RejectReason::InvalidPrice);
    CHECK(book.add_order({3, 1, Side::Buy, OrderType::Limit, 100, 0}, 3).reject_reason ==
          RejectReason::InvalidQuantity);
    CHECK(book.add_order({4, 1, Side::Buy, OrderType::Market, 0, -1}, 4).reject_reason ==
          RejectReason::InvalidQuantity);
}
