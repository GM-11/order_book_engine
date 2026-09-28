// Modify/cancel edge cases: modify quantity is the order's TOTAL size
// (FIX OrderQty), so a fill that races a modify can never overfill the
// client (CME In-Flight Mitigation). Finished orders are remembered so a
// late modify/cancel gets TooLate instead of UnknownOrder.
#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

using namespace engine;

TEST_CASE("In-flight fill: modify qty is the TOTAL, not the remaining") {
    // CME's own example: order for 10, 2 filled, replaced with 5 -> 3 left.
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 10}, 0);
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 100, 4},
                   0); // queued behind #1
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 2},
                   1); // fills 2 of #1

    REQUIRE(book.modify_order(1, 100, 5, 2).reject_reason ==
            RejectReason::None);

    // Total 5, filled 2 -> #1 has 3 left and keeps its place (it got smaller).
    // A sell of 4 (not 3: 3 would pass under the old logic too) should take
    // 3 from #1, then 1 from #3.
    const auto s =
        book.add_order({4, 4, Side::Sell, OrderType::Limit, 100, 4}, 3);
    REQUIRE(s.trades.size() == 2);
    CHECK(s.trades[0].passive_id == 1);
    CHECK(s.trades[0].quantity == 3);
    CHECK(s.trades[1].passive_id == 3);
    CHECK(s.trades[1].quantity == 1);
    CHECK(book.final_state(1) == FinalState::Filled);
    CHECK(book.check_invariants());
}

TEST_CASE("Modify total below filled cancels the rest") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 10}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 7}, 1); // 7 filled
    book.drain_events();

    const auto r = book.modify_order(1, 100, 5, 2); // wants 5 total, has 7
    CHECK(r.reject_reason == RejectReason::None);
    CHECK(r.trades.empty());
    CHECK_FALSE(book.best_bid().has_value()); // no more buying at all
    CHECK(book.final_state(1) == FinalState::Cancelled);

    const auto ev = book.drain_events();
    REQUIRE(ev.size() == 1);
    CHECK(ev[0].kind == EventKind::Cancelled);
    CHECK(ev[0].order_id == 1);
    CHECK(ev[0].quantity == 3); // the remainder that was still resting
    CHECK(book.check_invariants());
}

TEST_CASE("Modify total equal to filled also cancels the rest") {
    // CME's docs show total < filled; pin total == filled explicitly.
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 10}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 5}, 1); // 5 filled

    CHECK(book.modify_order(1, 100, 5, 2).reject_reason == RejectReason::None);
    CHECK_FALSE(book.best_bid().has_value());
    CHECK(book.final_state(1) == FinalState::Cancelled);
    CHECK(book.check_invariants());
}

TEST_CASE("Increasing total after a partial fill rests total minus filled "
          "and loses priority") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 10}, 0);
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 100, 4}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 2}, 1); // #1: 2 filled

    REQUIRE(book.modify_order(1, 100, 15, 2).reject_reason ==
            RejectReason::None);

    // #1 now has 15 - 2 = 13 resting; #3 still 4.
    const DepthSnapshot d = book.depth(1);
    REQUIRE(d.bids.size() == 1);
    CHECK(d.bids[0].quantity == 13 + 4);
    CHECK(d.bids[0].order_count == 2);

    // #1 grew, so it went to the back: #3 fills first.
    const auto s =
        book.add_order({4, 4, Side::Sell, OrderType::Limit, 100, 5}, 3);
    REQUIRE(s.trades.size() == 2);
    CHECK(s.trades[0].passive_id == 3);
    CHECK(s.trades[0].quantity == 4);
    CHECK(s.trades[1].passive_id == 1);
    CHECK(s.trades[1].quantity == 1);
    CHECK(book.check_invariants());
}

TEST_CASE("An aggressor that partly fills and then rests remembers its fills") {
    Book book;
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 3}, 0);
    // Buys 10: 3 fill on arrival, 7 rest. Its filled count must be 3.
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 100, 10}, 1);

    REQUIRE(book.modify_order(2, 100, 5, 2).reject_reason ==
            RejectReason::None);

    // Total 5, filled 3 -> 2 resting (not 5).
    const DepthSnapshot d = book.depth(1);
    REQUIRE(d.bids.size() == 1);
    CHECK(d.bids[0].quantity == 2);
    CHECK(book.check_invariants());
}

TEST_CASE("Modify and cancel after a full fill are TooLate, not UnknownOrder") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 5}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 5}, 1);
    book.drain_events();

    CHECK(book.modify_order(1, 100, 3, 2).reject_reason ==
          RejectReason::TooLate);
    CHECK(book.cancel_order(1, 2) == RejectReason::TooLate);
    CHECK(book.final_state(1) == FinalState::Filled);
    CHECK(book.final_state(2) == FinalState::Filled); // the aggressor too
    CHECK(book.drain_events().empty()); // rejections emit nothing

    CHECK(book.modify_order(99, 100, 3, 2).reject_reason ==
          RejectReason::UnknownOrder);
    CHECK(book.cancel_order(99, 2) == RejectReason::UnknownOrder);
    CHECK_FALSE(book.final_state(99).has_value());
}

TEST_CASE("A live order has no final state") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 5}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 2}, 1); // partial
    CHECK_FALSE(book.final_state(1).has_value());
}

TEST_CASE("Finished order ids cannot be reused") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 5}, 0);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 5}, 1); // #1 filled
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 90, 5}, 2);
    REQUIRE(book.cancel_order(3, 3) == RejectReason::None); // #3 cancelled
    book.drain_events();

    SECTION("filled id") {
        const auto r =
            book.add_order({1, 9, Side::Buy, OrderType::Limit, 95, 1}, 4);
        CHECK(r.reject_reason == RejectReason::DuplicateOrderId);
    }
    SECTION("cancelled id") {
        const auto r =
            book.add_order({3, 9, Side::Buy, OrderType::Limit, 95, 1}, 4);
        CHECK(r.reject_reason == RejectReason::DuplicateOrderId);
    }
    SECTION("stop reusing a finished id") {
        CHECK(book.place_stop_order({1, 9, Side::Sell, 80, 1}, 4) ==
              RejectReason::DuplicateOrderId);
    }
    CHECK(book.drain_events().empty());
    CHECK_FALSE(book.best_bid().has_value());
}

TEST_CASE("Modify below filled cancels the rest even during a halt") {
    Book book(20, 1000, 10, 50); // 10% band, 10ms grace, 50ms halt
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, std::nullopt, 1}, 0); // ref 100
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 95, 2}, 0);
    book.add_order({9, 9, Side::Sell, OrderType::Limit, 95, 1}, 0); // #3: 1 filled
    book.add_order({4, 4, Side::Sell, OrderType::Limit, 120, 1}, 1);
    book.add_order({5, 5, Side::Buy, OrderType::Market, std::nullopt, 1}, 1); // breach 1
    book.add_order({6, 6, Side::Sell, OrderType::Limit, 150, 1}, 20);
    REQUIRE(book.add_order({7, 7, Side::Buy, OrderType::Market, std::nullopt, 1}, 20)
                .reject_reason == RejectReason::SymbolHalted); // until 70

    // Total 1, already filled 1 -> cancel the rest. A cancel only reduces
    // risk, so the halt does not block it.
    const auto r = book.modify_order(3, 95, 1, 30);
    CHECK(r.reject_reason == RejectReason::None);
    CHECK_FALSE(book.best_bid().has_value());
    CHECK(book.final_state(3) == FinalState::Cancelled);

    // Still halted: the cancel did not resume trading.
    CHECK(book.add_order({8, 8, Side::Buy, OrderType::Limit, 95, 1}, 30)
              .reject_reason == RejectReason::SymbolHalted);
    CHECK(book.check_invariants());
}
