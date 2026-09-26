#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

using namespace engine;

namespace {
void expect_not_crossed(const Book &book) {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (bid && ask)
        CHECK(*bid < *ask);
}
} // namespace

TEST_CASE("The first trade establishes the reference price without any band "
          "restriction") {
    Book book(100, 1000, 10, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 1000, 1}, 0);

    const auto first_trade =
        book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0);

    REQUIRE(first_trade.trades.size() == 1);
    CHECK(first_trade.trades[0].price == 1000);
    CHECK(first_trade.reject_reason == RejectReason::None);
    expect_not_crossed(book);
}

TEST_CASE("A trade within the band executes normally and never starts a breach "
          "clock") {
    Book book(100, 1000, 10, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1},
                   0); // reference = 100, band [90,110]

    book.add_order({3, 3, Side::Sell, OrderType::Limit, 105, 1}, 1);
    const auto in_band =
        book.add_order({4, 4, Side::Buy, OrderType::Market, 0, 1}, 1);

    REQUIRE(in_band.trades.size() == 1);
    CHECK(in_band.reject_reason == RejectReason::None);

    // Prove no clock is running: an immediate breach right after this is still
    // treated as a first-ever breach and is allowed through, not halted.
    book.add_order({5, 5, Side::Sell, OrderType::Limit, 200, 1}, 1);
    const auto breach =
        book.add_order({6, 6, Side::Buy, OrderType::Market, 0, 1}, 1);
    REQUIRE(breach.trades.size() == 1);
    CHECK(breach.reject_reason == RejectReason::None);
    expect_not_crossed(book);
}

TEST_CASE("A single breach is allowed through immediately and starts the grace "
          "clock") {
    Book book(100, 1000, 10, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1},
                   0); // reference = 100, band [90,110]

    book.add_order({3, 3, Side::Sell, OrderType::Limit, 120, 1},
                   1); // 120 is outside the band
    const auto breach =
        book.add_order({4, 4, Side::Buy, OrderType::Market, 0, 1}, 1);

    REQUIRE(breach.trades.size() == 1);
    CHECK(breach.trades[0].price == 120);
    CHECK(breach.reject_reason == RejectReason::None);
    expect_not_crossed(book);
}

TEST_CASE("A sustained breach past the grace period halts the symbol") {
    Book book(100, 1000, 10, 50); // band 10%, grace 10ms, halt 50ms
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1},
                   0); // reference = 100, band [90,110]

    book.add_order({3, 3, Side::Sell, OrderType::Limit, 120, 1}, 1);
    const auto breach1 =
        book.add_order({4, 4, Side::Buy, OrderType::Market, 0, 1}, 1);
    REQUIRE(
        breach1.trades.size() ==
        1); // first breach passes, starts the clock at t=1, reference -> 120

    // Reference is now 120 (band [108,132]), so this second breach must clear
    // that shifted band too, not just the original one, to genuinely test the
    // halt rather than getting swallowed by the reference having moved.
    book.add_order({5, 5, Side::Sell, OrderType::Limit, 150, 1},
                   20); // t=20, elapsed since t=1 is 19 >= grace(10)
    const auto halted =
        book.add_order({6, 6, Side::Buy, OrderType::Market, 0, 1}, 20);

    CHECK(halted.trades.empty());
    CHECK(halted.unaccepted_quantity == 1);
    CHECK(halted.reject_reason == RejectReason::SymbolHalted);

    // The order that tripped the halt never executed, so it's still resting.
    CHECK(book.best_ask() == 150);
    expect_not_crossed(book);
}

TEST_CASE("While halted, every order is rejected regardless of its own price") {
    Book book(100, 1000, 10, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0);

    book.add_order({3, 3, Side::Sell, OrderType::Limit, 120, 1}, 1);
    book.add_order({4, 4, Side::Buy, OrderType::Market, 0, 1},
                   1); // starts clock

    book.add_order({5, 5, Side::Sell, OrderType::Limit, 150, 1}, 20);
    const auto halted = book.add_order(
        {6, 6, Side::Buy, OrderType::Market, 0, 1}, 20); // trips the halt
    REQUIRE(halted.reject_reason == RejectReason::SymbolHalted);

    // 121 would be a perfectly ordinary, in-band-looking price -- it still
    // gets rejected outright because the whole symbol is halted, not just
    // orders that would themselves breach the band.
    book.add_order({7, 7, Side::Sell, OrderType::Limit, 121, 1}, 30);
    const auto during_halt =
        book.add_order({8, 8, Side::Buy, OrderType::Market, 0, 1}, 30);

    CHECK(during_halt.trades.empty());
    CHECK(during_halt.unaccepted_quantity == 1);
    CHECK(during_halt.reject_reason == RejectReason::SymbolHalted);
    expect_not_crossed(book);
}

TEST_CASE("The halt clears once the cooldown elapses and matching resumes") {
    Book book(100, 1000, 10, 50); // halt_duration = 50ms
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0);

    book.add_order({3, 3, Side::Sell, OrderType::Limit, 120, 1}, 1);
    book.add_order({4, 4, Side::Buy, OrderType::Market, 0, 1}, 1);

    book.add_order({5, 5, Side::Sell, OrderType::Limit, 150, 1}, 20);
    const auto halted =
        book.add_order({6, 6, Side::Buy, OrderType::Market, 0, 1}, 20);
    REQUIRE(halted.reject_reason ==
            RejectReason::SymbolHalted); // halt_until_ = 20 + 50 = 70

    // Still inside the cooldown window.
    book.add_order({7, 7, Side::Sell, OrderType::Limit, 121, 1}, 69);
    CHECK(book.add_order({8, 8, Side::Buy, OrderType::Market, 0, 1}, 69)
              .reject_reason == RejectReason::SymbolHalted);

    // At/after halt_until_, the halt lifts and matching resumes normally.
    // The unmatched breaching order (150) is still resting from before the
    // halt, so a buy for 121 matches the order placed at t=69 instead --
    // the reference is still 120 from before the halt, so 121 is in-band.
    book.add_order({9, 9, Side::Sell, OrderType::Limit, 121, 1}, 70);
    const auto resumed =
        book.add_order({10, 10, Side::Buy, OrderType::Market, 0, 1}, 70);

    REQUIRE(resumed.trades.size() == 1);
    CHECK(resumed.reject_reason == RejectReason::None);
    expect_not_crossed(book);
}

TEST_CASE("An in-band trade between two breaches resets the grace clock") {
    Book book(100, 1000, 10, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1},
                   0); // reference = 100, band [90,110]

    book.add_order({3, 3, Side::Sell, OrderType::Limit, 120, 1}, 1);
    const auto breach1 =
        book.add_order({4, 4, Side::Buy, OrderType::Market, 0, 1}, 1);
    REQUIRE(breach1.reject_reason ==
            RejectReason::None); // clock starts at t=1, reference -> 120, band
                                 // [108,132]

    // In-band relative to the new reference -- this clears outside_band_since_.
    book.add_order({5, 5, Side::Sell, OrderType::Limit, 125, 1}, 5);
    const auto in_band =
        book.add_order({6, 6, Side::Buy, OrderType::Market, 0, 1}, 5);
    REQUIRE(in_band.reject_reason ==
            RejectReason::None); // reference -> 125, band [112,137]

    // t=30 is 29ms after the ORIGINAL breach at t=1 -- comfortably past the
    // 10ms grace period if that clock were still running. It isn't: the
    // in-band trade at t=5 reset it, so this breach is treated as brand new
    // and is allowed through rather than halting.
    book.add_order({7, 7, Side::Sell, OrderType::Limit, 200, 1}, 30);
    const auto fresh_breach =
        book.add_order({8, 8, Side::Buy, OrderType::Market, 0, 1}, 30);

    REQUIRE(fresh_breach.trades.size() == 1);
    CHECK(fresh_breach.reject_reason == RejectReason::None);
    expect_not_crossed(book);
}

TEST_CASE("Cancellation is never blocked by an active halt") {
    Book book(100, 1000, 10, 50);
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0);

    // A resting order and a dormant stop that the upcoming halt has no reason
    // to touch (they're on the bid side / far from the trigger price).
    book.add_order({100, 100, Side::Buy, OrderType::Limit, 95, 1}, 0);
    book.place_stop_order({200, 200, Side::Sell, 100, 1}, 0);

    book.add_order({3, 3, Side::Sell, OrderType::Limit, 120, 1}, 1);
    book.add_order({4, 4, Side::Buy, OrderType::Market, 0, 1}, 1);

    book.add_order({5, 5, Side::Sell, OrderType::Limit, 150, 1}, 20);
    const auto halted =
        book.add_order({6, 6, Side::Buy, OrderType::Market, 0, 1}, 20);
    REQUIRE(halted.reject_reason == RejectReason::SymbolHalted);

    // Still well within the halt window (halt_until_ = 70).
    CHECK(book.cancel_order(100, 0));
    CHECK(book.cancel_stop_order(200, 0));
    expect_not_crossed(book);
}

TEST_CASE("A stop that can't fire because the symbol is halted stays dormant, "
          "not lost") {
    Book book(100, 1000, 0,
              50); // grace=0 to force the halt within one add_order call
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1},
                   0); // reference = 100, band [90,110]

    book.add_order({10, 100, Side::Sell, OrderType::Limit, 150, 1},
                   1); // breach level 1
    book.add_order({11, 101, Side::Sell, OrderType::Limit, 160, 1},
                   1); // breach level 2
    book.place_stop_order({20, 200, Side::Sell, 150, 1},
                          0); // fires when last_trade_price <= 150

    const auto sweep =
        book.add_order({30, 300, Side::Buy, OrderType::Market, 0, 2}, 1);

    REQUIRE(sweep.trades.size() == 1); // only the 150 level executed
    CHECK(sweep.trades[0].price == 150);
    CHECK(sweep.unaccepted_quantity == 1);
    CHECK(sweep.reject_reason == RejectReason::SymbolHalted);

    // The 160 order never matched -- it's still resting.
    CHECK(book.best_ask() == 160);

    // The stop must still be cancellable -- proof it was never erased, unlike
    // the old (buggy) behavior where it vanished the instant the halt engaged.
    CHECK(book.cancel_stop_order(20, 0));
    expect_not_crossed(book);
}

TEST_CASE("A dormant stop that survived a halt fires normally on the first "
          "qualifying trade after resumption") {
    Book book(100, 1000, 0, 50); // halt_duration = 50ms
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1},
                   0); // reference = 100

    book.add_order({10, 100, Side::Sell, OrderType::Limit, 150, 1}, 1);
    book.add_order({11, 101, Side::Sell, OrderType::Limit, 160, 1}, 1);
    book.place_stop_order({20, 200, Side::Sell, 150, 1},
                          0); // fires on last_trade_price <= 150

    const auto sweep =
        book.add_order({30, 300, Side::Buy, OrderType::Market, 0, 2}, 1);
    REQUIRE(sweep.reject_reason ==
            RejectReason::SymbolHalted); // halt_until_ = 1 + 50 = 51

    // now = 51 clears the halt as a side effect of this call. Rest TWO bids:
    // A is what the direct triggering trade below will consume; B is bait
    // that only gets consumed if the stop actually fires and its resulting
    // market sell walks the book -- it's the thing that makes this test
    // falsifiable, unlike checking cancel_stop_order() alone (which reads
    // false in both the correct case AND the still-buggy case, since either
    // way the stop is gone by the end -- just for different reasons).
    const auto bidA =
        book.add_order({40, 400, Side::Buy, OrderType::Limit, 145, 1}, 51);
    REQUIRE(bidA.reject_reason ==
            RejectReason::None); // proves the halt is cleared
    const auto bidB =
        book.add_order({42, 402, Side::Buy, OrderType::Limit, 140, 1}, 52);
    REQUIRE(bidB.reject_reason == RejectReason::None);

    // A crossing sell at 145 consumes bidA directly and prints a trade at
    // 145, which is <= the stop's 150 trigger -- this is the first
    // qualifying trade since the halt lifted.
    const auto cross =
        book.add_order({43, 403, Side::Sell, OrderType::Limit, 145, 1}, 53);
    REQUIRE(cross.trades.size() == 1);
    CHECK(cross.trades[0].price == 145);

    // If the stop survived the halt and fired here, its market sell walks
    // the book and consumes bidB too -- best_bid() goes empty. If the stop
    // was lost during the halt (the bug), bidB is never touched and
    // best_bid() still reports 140.
    CHECK_FALSE(book.best_bid().has_value());
    expect_not_crossed(book);
}

TEST_CASE("An exact integer upper band edge is in-band") {
    Book book(100, 1500, 0, 50); // reference 100, band [85, 115]
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    REQUIRE(book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0)
                .trades.size() == 1);

    book.add_order({3, 3, Side::Sell, OrderType::Limit, 115, 1}, 1);
    const auto at_upper =
        book.add_order({4, 4, Side::Buy, OrderType::Market, 0, 1}, 1);
    REQUIRE(at_upper.trades.size() == 1);
    CHECK(at_upper.reject_reason == RejectReason::None);

    // If 115 were misclassified as outside (as a double-floor bug can do),
    // this second trade at grace=0 would trip the halt instead of executing.
    book.add_order({5, 5, Side::Sell, OrderType::Limit, 130, 1}, 1);
    const auto first_breach =
        book.add_order({6, 6, Side::Buy, OrderType::Market, 0, 1}, 1);
    REQUIRE(first_breach.trades.size() == 1);
    CHECK(first_breach.reject_reason == RejectReason::None);
    expect_not_crossed(book);
}

TEST_CASE("Book rejects invalid basis-point bands") {
    CHECK_THROWS_AS(Book(1, 0), std::invalid_argument);
    CHECK_THROWS_AS(Book(1, 10000), std::invalid_argument);
}

TEST_CASE("Integer band limits agree with exact rational arithmetic") {
    constexpr std::int64_t bps_values[] = {1, 10, 333, 1500, 9999};
    for (const auto bps : bps_values) {
        for (Price reference = 1; reference <= 5000; ++reference) {
            Book book(1, bps);
            book.add_order({1, 1, Side::Sell, OrderType::Limit, reference, 1},
                           0);
            REQUIRE(
                book.add_order({2, 2, Side::Buy, OrderType::Market, 0, 1}, 0)
                    .trades.size() == 1);

            const Price expected_lower =
                (reference * (10000 - bps) + 9999) / 10000;
            const Price expected_upper = reference * (10000 + bps) / 10000;
            CHECK(book.within_band(expected_lower));
            CHECK(book.within_band(expected_upper));
            if (expected_lower > 0)
                CHECK_FALSE(book.within_band(expected_lower - 1));
            CHECK_FALSE(book.within_band(expected_upper + 1));
            expect_not_crossed(book);
        }
    }
}
