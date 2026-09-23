#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

using namespace engine;

TEST_CASE("The first trade has no reference-price band restriction") {
    Book book{100, 0.10, 10, 50};
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 1000, 1}, 1);

    const auto first_trade = book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 2);

    REQUIRE(first_trade.trades.size() == 1);
    CHECK(first_trade.trades[0].price == 1000);
    CHECK(first_trade.reject_reason == RejectReason::None);
}

TEST_CASE("Current implementation does not establish a reference price or halt") {
    // Documented implementation gap: Book::add_order never sets reference_price_
    // or has_reference_price_. Therefore these trades cannot enter the band/grace/
    // halt state machine, even after the grace interval has elapsed.
    Book book{100, 0.10, 10, 50};

    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 1);
    REQUIRE(book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 2).trades.size() == 1);

    book.add_order({3, 3, Side::Sell, OrderType::Limit, 120, 1}, 3);
    const auto first_breach_candidate =
        book.add_order({4, 4, Side::Buy, OrderType::Market, 0, 1}, 3);
    REQUIRE(first_breach_candidate.trades.size() == 1);
    CHECK(first_breach_candidate.reject_reason == RejectReason::None);

    book.add_order({5, 5, Side::Sell, OrderType::Limit, 120, 1}, 4);
    const auto after_grace = book.add_order({6, 6, Side::Buy, OrderType::Market, 0, 1}, 20);
    REQUIRE(after_grace.trades.size() == 1);
    CHECK(after_grace.reject_reason == RejectReason::None);

    // This also documents that the one-call grace-period trap, halt rejection,
    // recovery, in-band reset, and a stop-on-halt edge cannot currently be
    // reached through the public API. Do not treat this behavior as a contract.
    book.add_order({7, 7, Side::Buy, OrderType::Limit, 90, 1}, 21);
    CHECK(book.cancel_order(7));
    book.place_stop_order({8, 8, Side::Sell, 80, 1});
    CHECK(book.cancel_stop_order(8));
}
