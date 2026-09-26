#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

using namespace engine;

namespace {
void expect_not_crossed(const Book &book);
} // namespace

TEST_CASE("Reducing remaining quantity keeps FIFO priority") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 100}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 500, 50}, 1);

    const auto reduced = book.modify_order(1, 500, 60, 2);
    CHECK(reduced.reject_reason == RejectReason::None);
    CHECK(reduced.unaccepted_quantity == 0);
    CHECK(reduced.trades.empty());

    const auto first =
        book.add_order({3, 3, Side::Sell, OrderType::Limit, 500, 60}, 3);
    REQUIRE(first.trades.size() == 1);
    CHECK(first.trades[0].passive_id == 1);
    CHECK(first.trades[0].quantity == 60);
    const auto second =
        book.add_order({4, 4, Side::Sell, OrderType::Limit, 500, 50}, 4);
    REQUIRE(second.trades.size() == 1);
    CHECK(second.trades[0].passive_id == 2);
    CHECK(second.trades[0].quantity == 50);
    expect_not_crossed(book);
}

TEST_CASE("An unchanged modify retains FIFO position") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 2}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 500, 1}, 1);
    const auto unchanged = book.modify_order(1, 500, 2, 2);
    CHECK(unchanged.reject_reason == RejectReason::None);
    CHECK(unchanged.trades.empty());

    const auto sell =
        book.add_order({3, 3, Side::Sell, OrderType::Limit, 500, 2}, 3);
    REQUIRE(sell.trades.size() == 1);
    CHECK(sell.trades[0].passive_id == 1);
    CHECK(sell.trades[0].quantity == 2);
    expect_not_crossed(book);
}

TEST_CASE("Increasing remaining quantity loses FIFO position") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 100}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 500, 50}, 1);
    CHECK(book.modify_order(1, 500, 150, 2).reject_reason ==
          RejectReason::None);

    const auto sell =
        book.add_order({3, 3, Side::Sell, OrderType::Limit, 500, 50}, 3);
    REQUIRE(sell.trades.size() == 1);
    CHECK(sell.trades[0].passive_id == 2);
    CHECK(sell.trades[0].quantity == 50);
    const auto next =
        book.add_order({4, 4, Side::Sell, OrderType::Limit, 500, 150}, 4);
    REQUIRE(next.trades.size() == 1);
    CHECK(next.trades[0].passive_id == 1);
    expect_not_crossed(book);
}

TEST_CASE("A reprice joins the tail of its new price level") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 502, 1}, 1);
    CHECK(book.modify_order(1, 502, 1, 2).reject_reason == RejectReason::None);

    const auto first =
        book.add_order({3, 3, Side::Sell, OrderType::Limit, 502, 1}, 3);
    REQUIRE(first.trades.size() == 1);
    CHECK(first.trades[0].passive_id == 2);
    const auto second =
        book.add_order({4, 4, Side::Sell, OrderType::Limit, 502, 1}, 4);
    REQUIRE(second.trades.size() == 1);
    CHECK(second.trades[0].passive_id == 1);
    expect_not_crossed(book);
}

TEST_CASE(
    "Less aggressive reprices remove emptied levels and update best prices") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 1}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 510, 1}, 1);
    CHECK(book.modify_order(1, 490, 1, 2).reject_reason == RejectReason::None);
    CHECK(book.best_bid() == 490);
    CHECK(book.modify_order(2, 520, 1, 3).reject_reason == RejectReason::None);
    CHECK(book.best_ask() == 520);
    CHECK(book.cancel_order(1, 0));
    CHECK(book.cancel_order(2, 0));
    CHECK_FALSE(book.best_bid().has_value());
    CHECK_FALSE(book.best_ask().has_value());
    expect_not_crossed(book);
}

TEST_CASE("A crossing reprice executes at the passive price and rests its "
          "remainder") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 5}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 505, 2}, 1);

    const auto modified = book.modify_order(1, 505, 5, 2);
    REQUIRE(modified.trades.size() == 1);
    CHECK(modified.trades[0].aggressor_id == 1);
    CHECK(modified.trades[0].passive_id == 2);
    CHECK(modified.trades[0].price == 505);
    CHECK(modified.trades[0].quantity == 2);
    CHECK(modified.reject_reason == RejectReason::None);
    CHECK(modified.unaccepted_quantity == 0);
    CHECK(book.best_bid() == 505);

    const auto remainder =
        book.add_order({3, 3, Side::Sell, OrderType::Limit, 505, 3}, 3);
    REQUIRE(remainder.trades.size() == 1);
    CHECK(remainder.trades[0].passive_id == 1);
    CHECK(remainder.trades[0].quantity == 3);
    expect_not_crossed(book);
}

TEST_CASE("Unknown, filled and cancelled ids cannot be modified") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 1}, 0);
    CHECK(book.modify_order(99, 501, 1, 1).reject_reason ==
          RejectReason::UnknownOrder);
    CHECK(book.best_bid() == 500);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 500, 1}, 2);
    CHECK(book.modify_order(1, 501, 1, 3).reject_reason ==
          RejectReason::UnknownOrder);
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 490, 1}, 4);
    CHECK(book.cancel_order(3, 0));
    CHECK(book.modify_order(3, 501, 1, 5).reject_reason ==
          RejectReason::UnknownOrder);
    CHECK_FALSE(book.best_bid().has_value());
    expect_not_crossed(book);
}

TEST_CASE("Invalid quantity and prices preserve the original FIFO order") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 2}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 500, 1}, 1);

    CHECK(book.modify_order(1, 500, 0, 2).reject_reason ==
          RejectReason::InvalidQuantity);
    CHECK(book.modify_order(1, 0, 2, 3).reject_reason ==
          RejectReason::InvalidPrice);
    CHECK(book.modify_order(1, -1, 2, 4).reject_reason ==
          RejectReason::InvalidPrice);
    CHECK(book.best_bid() == 500);
    const auto sell =
        book.add_order({3, 3, Side::Sell, OrderType::Limit, 500, 2}, 5);
    REQUIRE(sell.trades.size() == 1);
    CHECK(sell.trades[0].passive_id == 1);
    CHECK(sell.trades[0].quantity == 2);
    CHECK(book.best_bid() == 500);
    expect_not_crossed(book);
}

TEST_CASE(
    "A reprice is rejected during a halt without losing its old position") {
    Book book(20, 1000, 10, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0);
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 95, 2}, 0);
    book.add_order({8, 8, Side::Buy, OrderType::Limit, 95, 1}, 0);
    book.add_order({4, 4, Side::Sell, OrderType::Limit, 120, 1}, 1);
    REQUIRE(book.add_order({5, 5, Side::Buy, OrderType::Market, 0, 1}, 1)
                .trades.size() == 1);
    book.add_order({6, 6, Side::Sell, OrderType::Limit, 150, 1}, 20);
    REQUIRE(book.add_order({7, 7, Side::Buy, OrderType::Market, 0, 1}, 20)
                .reject_reason == RejectReason::SymbolHalted);

    const auto rejected = book.modify_order(3, 96, 2, 30);
    CHECK(rejected.reject_reason == RejectReason::SymbolHalted);
    CHECK(rejected.unaccepted_quantity == 2);
    CHECK(rejected.trades.empty());
    CHECK(book.best_bid() == 95);
    const auto after =
        book.add_order({9, 9, Side::Sell, OrderType::Limit, 95, 2}, 70);
    REQUIRE(after.trades.size() == 1);
    CHECK(after.trades[0].passive_id == 3);
    CHECK(after.trades[0].price == 95);
    CHECK(after.trades[0].quantity == 2);
    CHECK(book.best_bid() == 95);
    expect_not_crossed(book);
}

TEST_CASE("Reducing remaining quantity is permitted during a halt") {
    Book book(20, 1000, 10, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0);
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 95, 2}, 0);
    book.add_order({4, 4, Side::Sell, OrderType::Limit, 120, 1}, 1);
    book.add_order({5, 5, Side::Buy, OrderType::Market, 0, 1}, 1);
    book.add_order({6, 6, Side::Sell, OrderType::Limit, 150, 1}, 20);
    REQUIRE(book.add_order({7, 7, Side::Buy, OrderType::Market, 0, 1}, 20)
                .reject_reason == RejectReason::SymbolHalted);
    const auto reduced = book.modify_order(3, 95, 1, 30);
    CHECK(reduced.reject_reason == RejectReason::None);
    CHECK(reduced.trades.empty());
    const auto after =
        book.add_order({8, 8, Side::Sell, OrderType::Limit, 95, 1}, 70);
    REQUIRE(after.trades.size() == 1);
    CHECK(after.trades[0].passive_id == 3);
    CHECK(after.trades[0].quantity == 1);
    expect_not_crossed(book);
}

TEST_CASE(
    "A crossing reprice uses Cancel Newest after retaining earlier fills") {
    Book book;
    book.add_order({1, 7, Side::Buy, OrderType::Limit, 500, 3}, 0);
    book.add_order({2, 8, Side::Sell, OrderType::Limit, 503, 1}, 1);
    book.add_order({3, 7, Side::Sell, OrderType::Limit, 505, 2}, 2);

    const auto modified = book.modify_order(1, 505, 3, 3);
    REQUIRE(modified.trades.size() == 1);
    CHECK(modified.trades[0].aggressor_id == 1);
    CHECK(modified.trades[0].passive_id == 2);
    CHECK(modified.trades[0].quantity == 1);
    CHECK(modified.reject_reason == RejectReason::SelfTrade);
    CHECK(modified.unaccepted_quantity == 2);
    CHECK(book.best_ask() == 505);
    CHECK_FALSE(book.best_bid().has_value());
    const auto other =
        book.add_order({4, 9, Side::Buy, OrderType::Limit, 505, 2}, 4);
    REQUIRE(other.trades.size() == 1);
    CHECK(other.trades[0].passive_id == 3);
    CHECK(other.trades[0].quantity == 2);
    expect_not_crossed(book);
}

TEST_CASE("A partially filled order can reduce its remaining size without "
          "losing priority") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 100}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 500, 10}, 1);
    const auto partial =
        book.add_order({3, 3, Side::Sell, OrderType::Limit, 500, 30}, 2);
    REQUIRE(partial.trades.size() == 1);
    CHECK(partial.trades[0].passive_id == 1);
    CHECK(book.modify_order(1, 500, 50, 3).reject_reason == RejectReason::None);
    const auto next =
        book.add_order({4, 4, Side::Sell, OrderType::Limit, 500, 50}, 4);
    REQUIRE(next.trades.size() == 1);
    CHECK(next.trades[0].passive_id == 1);
    CHECK(next.trades[0].quantity == 50);
    const auto last =
        book.add_order({5, 5, Side::Sell, OrderType::Limit, 500, 10}, 5);
    REQUIRE(last.trades.size() == 1);
    CHECK(last.trades[0].passive_id == 2);
    expect_not_crossed(book);
}

TEST_CASE("Dormant stop ids are not eligible for limit-order modification") {
    Book book;
    book.place_stop_order({1, 1, Side::Sell, 500, 1}, 0);
    CHECK(book.modify_order(1, 501, 2, 0).reject_reason ==
          RejectReason::UnknownOrder);
    CHECK(book.cancel_stop_order(1, 0));
    expect_not_crossed(book);
}

TEST_CASE("A full pool still allows replacement because the old node is freed "
          "first") {
    Book book(2);
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 490, 1}, 1);
    const auto modified = book.modify_order(1, 501, 1, 2);
    CHECK(modified.reject_reason == RejectReason::None);
    CHECK(modified.unaccepted_quantity == 0);
    CHECK(book.best_bid() == 501);
    const auto sell =
        book.add_order({3, 3, Side::Sell, OrderType::Limit, 501, 1}, 3);
    REQUIRE(sell.trades.size() == 1);
    CHECK(sell.trades[0].passive_id == 1);
    CHECK(book.best_bid() == 490);
    expect_not_crossed(book);
}

TEST_CASE("A crossing reprice triggers and executes an eligible dormant stop") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 500, 1}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 505, 2}, 1);
    book.place_stop_order({3, 3, Side::Buy, 505, 1}, 0);

    const auto modified = book.modify_order(1, 505, 1, 2);
    REQUIRE(modified.trades.size() == 1);
    CHECK(modified.trades[0].passive_id == 2);
    CHECK(modified.trades[0].price == 505);
    CHECK(modified.reject_reason == RejectReason::None);
    CHECK_FALSE(
        book.best_ask()
            .has_value()); // the stop's market buy consumed the other unit
    CHECK_FALSE(book.cancel_stop_order(3, 0));
    expect_not_crossed(book);
}

TEST_CASE("A reprice at halt expiry clears the stale halt state") {
    Book book(20, 1000, 10, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0);
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 95, 2}, 0);
    book.add_order({4, 4, Side::Sell, OrderType::Limit, 120, 1}, 1);
    REQUIRE(book.add_order({5, 5, Side::Buy, OrderType::Market, 0, 1}, 1)
                .trades.size() == 1);
    book.add_order({6, 6, Side::Sell, OrderType::Limit, 150, 1}, 20);
    REQUIRE(book.add_order({7, 7, Side::Buy, OrderType::Market, 0, 1}, 20)
                .reject_reason ==
            RejectReason::SymbolHalted); // halt_until = 70

    const auto resumed = book.modify_order(3, 96, 2, 70);
    CHECK(resumed.reject_reason == RejectReason::None);
    CHECK(resumed.trades.empty());
    CHECK(book.best_bid() == 96);
    const auto sell =
        book.add_order({8, 8, Side::Sell, OrderType::Limit, 96, 2}, 70);
    REQUIRE(sell.trades.size() == 1);
    CHECK(sell.trades[0].passive_id == 3);
    CHECK(sell.trades[0].price == 96);
}

namespace {
void seed_band_halt_sweep(Book &book) {
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    const auto reference =
        book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0);
    REQUIRE(reference.trades.size() == 1); // reference = 100, band [90, 110]
    book.add_order({20, 20, Side::Sell, OrderType::Limit, 150, 1}, 1);
    book.add_order({21, 21, Side::Sell, OrderType::Limit, 160, 1}, 1);
    book.add_order({22, 22, Side::Sell, OrderType::Limit, 170, 1}, 1);
}

void expect_not_crossed(const Book &book) {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (bid && ask)
        CHECK(*bid < *ask);
}

void check_band_halt_sweep(Book &book, const OrderResult &result) {
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].price == 150);
    CHECK(result.trades[0].quantity == 1);
    CHECK(result.trades[0].aggressor_id == 10);
    CHECK(result.trades[0].passive_id == 20);
    CHECK(result.unaccepted_quantity == 0);
    CHECK(result.reject_reason == RejectReason::None);
    CHECK(result.rested_price == 110);
    CHECK(book.best_bid() == 110);
    CHECK(book.best_ask() == 160); // the breaching fill was not executed

    const auto during_halt =
        book.add_order({30, 30, Side::Buy, OrderType::Limit, 100, 1}, 2);
    CHECK(during_halt.trades.empty());
    CHECK(during_halt.unaccepted_quantity == 1);
    CHECK(during_halt.reject_reason == RejectReason::SymbolHalted);
    expect_not_crossed(book);
}
} // namespace

TEST_CASE("A repriced bid rests its halt remainder at the upper band edge") {
    Book book(10, 1000, 0, 50); // zero grace: second breach in one call halts
    seed_band_halt_sweep(book);
    book.add_order({10, 10, Side::Buy, OrderType::Limit, 90, 4}, 0);

    const auto result = book.modify_order(10, 170, 4, 1);
    check_band_halt_sweep(book, result);
    const auto after_halt =
        book.add_order({31, 31, Side::Sell, OrderType::Limit, 110, 3}, 51);
    REQUIRE(after_halt.trades.size() == 1);
    CHECK(after_halt.trades[0].passive_id == 10);
    CHECK(after_halt.trades[0].price == 110);
    expect_not_crossed(book);
    expect_not_crossed(book);
}

TEST_CASE("A new bid rests its halt remainder at the upper band edge") {
    Book book(10, 1000, 0, 50);
    seed_band_halt_sweep(book);

    const auto result =
        book.add_order({10, 10, Side::Buy, OrderType::Limit, 170, 4}, 1);
    check_band_halt_sweep(book, result);
    const auto after_halt =
        book.add_order({31, 31, Side::Sell, OrderType::Limit, 110, 3}, 51);
    REQUIRE(after_halt.trades.size() == 1);
    CHECK(after_halt.trades[0].passive_id == 10);
    CHECK(after_halt.trades[0].price == 110);
    expect_not_crossed(book);
    expect_not_crossed(book);
}

TEST_CASE("A sell remainder rests at the lower band edge when a halt trips") {
    Book book(10, 1000, 0, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0);
    book.add_order({20, 20, Side::Buy, OrderType::Limit, 50, 1}, 1);
    book.add_order({21, 21, Side::Buy, OrderType::Limit, 40, 1}, 1);

    const auto result =
        book.add_order({10, 10, Side::Sell, OrderType::Limit, 30, 4}, 1);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].price == 50);
    CHECK(result.unaccepted_quantity == 0);
    CHECK(result.reject_reason == RejectReason::None);
    CHECK(result.rested_price == 90);
    CHECK(book.best_bid() == 40);
    CHECK(book.best_ask() == 90);
    const auto after_halt =
        book.add_order({30, 30, Side::Buy, OrderType::Limit, 90, 3}, 51);
    REQUIRE(after_halt.trades.size() == 1);
    CHECK(after_halt.trades[0].passive_id == 10);
    CHECK(after_halt.trades[0].price == 90);
    expect_not_crossed(book);
    expect_not_crossed(book);
}

TEST_CASE("Band-edge prices round inward to whole ticks") {
    Book book(10, 1000, 0, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 101, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0);
    book.add_order({20, 20, Side::Sell, OrderType::Limit, 150, 1}, 1);
    book.add_order({21, 21, Side::Sell, OrderType::Limit, 160, 1}, 1);

    const auto result =
        book.add_order({10, 10, Side::Buy, OrderType::Limit, 170, 4}, 1);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.rested_price == 111); // floor(101 * 1.10) = 111
    CHECK(book.best_bid() == 111);
    CHECK(book.best_ask() == 160);
    expect_not_crossed(book);
    expect_not_crossed(book);
}

TEST_CASE("A market order does not rest a remainder when a halt trips") {
    Book book(10, 1000, 0, 50);
    seed_band_halt_sweep(book);

    const auto result =
        book.add_order({10, 10, Side::Buy, OrderType::Market, 0, 4}, 1);
    REQUIRE(result.trades.size() == 1);
    CHECK(result.trades[0].price == 150);
    CHECK(result.unaccepted_quantity == 3);
    CHECK(result.reject_reason == RejectReason::SymbolHalted);
    CHECK_FALSE(result.rested_price.has_value());
    CHECK_FALSE(book.cancel_order(10, 0));
    CHECK(book.best_ask() == 160);
    expect_not_crossed(book);
    expect_not_crossed(book);
}
