#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

#include <vector>

using namespace engine;

namespace {
void expect_sane(const Book &book) {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (bid && ask)
        CHECK(*bid < *ask);
    CHECK(book.check_invariants());
}

// reference = 100, band 10% -> [90, 110]; asks at 100, 150, 300, 1000.
void thin_ask_ladder(Book &book) {
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    REQUIRE(book.add_order({2, 2, Side::Buy, OrderType::Limit, 100, 1}, 0)
                .trades.size() == 1);
    const Price prices[] = {100, 150, 300, 1000};
    OrderId id = 10;
    for (const Price p : prices)
        book.add_order({id++, 3, Side::Sell, OrderType::Limit, p, 1}, 1);
    book.drain_events();
}
} // namespace

TEST_CASE("One market order cannot sweep past its first out-of-band level") {
    Book book(1000, 1000, 2000, 30000);
    thin_ask_ladder(book);

    const auto r =
        book.add_order({20, 4, Side::Buy, OrderType::Market, 0, 4}, 5);

    // 100 is in band; 150 is the one allowed breach print; 300 and 1000 are
    // never touched.
    REQUIRE(r.trades.size() == 2);
    CHECK(r.trades[0].price == 100);
    CHECK(r.trades[1].price == 150);
    CHECK(r.unaccepted_quantity == 2);
    CHECK(r.reject_reason == RejectReason::PriceBand);
    CHECK(book.best_ask() == 300);

    // The market remainder must end in a Cancelled event, and no halt.
    bool cancelled = false;
    for (const auto &e : book.drain_events()) {
        CHECK(e.kind != EventKind::Halted);
        if (e.kind == EventKind::Cancelled && e.order_id == 20) {
            cancelled = true;
            CHECK(e.quantity == 2);
        }
    }
    CHECK(cancelled);
    expect_sane(book);
}

TEST_CASE("A limit order stopped by the sweep limit rests at the band edge") {
    Book book(1000, 1000, 2000, 30000);
    thin_ask_ladder(book);

    const auto r =
        book.add_order({20, 4, Side::Buy, OrderType::Limit, 1000, 4}, 5);

    REQUIRE(r.trades.size() == 2);
    CHECK(r.trades[1].price == 150);
    // Remainder rests at the upper band edge of the reference in force during
    // the walk (100 -> 110), not at its own 1000 limit (that would cross).
    CHECK(r.reject_reason == RejectReason::None);
    CHECK(r.unaccepted_quantity == 0);
    REQUIRE(r.rested_price.has_value());
    CHECK(*r.rested_price == 110);
    CHECK(book.best_bid() == 110);
    CHECK(book.best_ask() == 300);
    expect_sane(book);
}

TEST_CASE("The sweep limit lets the whole breach level trade, several orders "
          "deep") {
    Book book(1000, 1000, 2000, 30000);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 100, 1}, 0);
    book.add_order({10, 3, Side::Sell, OrderType::Limit, 150, 2}, 1);
    book.add_order({11, 5, Side::Sell, OrderType::Limit, 150, 3}, 1);
    book.add_order({12, 6, Side::Sell, OrderType::Limit, 200, 1}, 1);

    const auto r =
        book.add_order({20, 4, Side::Buy, OrderType::Market, 0, 6}, 5);

    // Both orders at 150 fill (same level); 200 is a further level.
    REQUIRE(r.trades.size() == 2);
    CHECK(r.trades[0].quantity == 2);
    CHECK(r.trades[1].quantity == 3);
    CHECK(r.unaccepted_quantity == 1);
    CHECK(r.reject_reason == RejectReason::PriceBand);
    CHECK(book.best_ask() == 200);
    expect_sane(book);
}

TEST_CASE("The sweep limit also applies to a sell walking down the bids") {
    Book book(1000, 1000, 2000, 30000);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 100, 1}, 0);
    book.add_order({10, 3, Side::Buy, OrderType::Limit, 95, 1}, 1);
    book.add_order({11, 3, Side::Buy, OrderType::Limit, 60, 1}, 1);
    book.add_order({12, 3, Side::Buy, OrderType::Limit, 10, 1}, 1);

    const auto r =
        book.add_order({20, 4, Side::Sell, OrderType::Market, 0, 3}, 5);

    REQUIRE(r.trades.size() == 2);
    CHECK(r.trades[0].price == 95);
    CHECK(r.trades[1].price == 60);
    CHECK(r.reject_reason == RejectReason::PriceBand);
    CHECK(book.best_bid() == 10);
    expect_sane(book);
}

TEST_CASE("Market collar stops a market order at reference +/- collar") {
    // band 10%, collar 5%: reference 100 -> market buys fill only up to 105.
    Book book(1000, 1000, 2000, 30000, 500);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 100, 1}, 0);
    book.add_order({10, 3, Side::Sell, OrderType::Limit, 104, 1}, 1);
    book.add_order({11, 3, Side::Sell, OrderType::Limit, 106, 1}, 1);
    book.drain_events();

    const auto r =
        book.add_order({20, 4, Side::Buy, OrderType::Market, 0, 2}, 2);

    REQUIRE(r.trades.size() == 1);
    CHECK(r.trades[0].price == 104);
    CHECK(r.unaccepted_quantity == 1);
    CHECK(r.reject_reason == RejectReason::PriceCollar);
    CHECK(book.best_ask() == 106);

    bool cancelled = false;
    for (const auto &e : book.drain_events())
        if (e.kind == EventKind::Cancelled && e.order_id == 20)
            cancelled = true;
    CHECK(cancelled);
    expect_sane(book);
}

TEST_CASE("Market collar does not apply to limit orders or before the first "
          "trade") {
    Book book(1000, 1000, 2000, 30000, 500);
    // No reference yet: a market order is not collared.
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    REQUIRE(book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0)
                .trades.size() == 1);

    // Reference 100. A limit buy at 106 is the trader's own price bound; the
    // collar only gives a bound to orders that have none.
    book.add_order({10, 3, Side::Sell, OrderType::Limit, 106, 1}, 1);
    const auto r =
        book.add_order({20, 4, Side::Buy, OrderType::Limit, 106, 1}, 2);
    REQUIRE(r.trades.size() == 1);
    CHECK(r.reject_reason == RejectReason::None);
    expect_sane(book);
}

TEST_CASE("Collar setting is validated") {
    CHECK_THROWS_AS(Book(1, 1000, 0, 0, -1), std::invalid_argument);
    CHECK_THROWS_AS(Book(1, 1000, 0, 0, 10000), std::invalid_argument);
    CHECK_NOTHROW(Book(1, 1000, 0, 0, 0));
}
