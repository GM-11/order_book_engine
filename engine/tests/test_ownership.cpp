// Only the account that placed an order may cancel or modify it.
// Anyone else gets UnknownOrder: the same answer as for an id that never
// existed, so a reply never reveals whether another account's order exists,
// rests, filled or was cancelled. (Real venues scope order ids per account:
// another account's order simply cannot be named.)
#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

#include <algorithm>
#include <vector>

using namespace engine;

namespace {
constexpr OwnerId kAlice = 1;
constexpr OwnerId kBob = 2;

bool has_kind(const std::vector<EngineEvent> &ev, EventKind k) {
    return std::any_of(ev.begin(), ev.end(), [k](const EngineEvent &e) { return e.kind == k; });
}

Quantity ask_qty_at(const Book &book, Price p) {
    for (const auto &l : book.depth(10).asks)
        if (l.price == p) return l.quantity;
    return 0;
}
} // namespace

TEST_CASE("a non-owner cannot cancel a resting order; the owner can") {
    Book book;
    book.add_order({1, kAlice, Side::Sell, OrderType::Limit, 100, 10}, 0);
    book.drain_events();

    CHECK(book.cancel_order(1, kBob, 1) == RejectReason::UnknownOrder);
    CHECK(book.drain_events().empty()); // nothing happened, nothing published
    CHECK(ask_qty_at(book, 100) == 10); // still resting, untouched
    CHECK_FALSE(book.final_state(1).has_value());

    CHECK(book.cancel_order(1, kAlice, 2) == RejectReason::None);
    CHECK(has_kind(book.drain_events(), EventKind::Cancelled));
    CHECK(book.check_invariants());
}

TEST_CASE("a non-owner cannot modify a resting order, and the order keeps its "
          "place in the queue") {
    Book book;
    book.add_order({1, kAlice, Side::Sell, OrderType::Limit, 100, 10}, 0);
    book.add_order({2, 3, Side::Sell, OrderType::Limit, 100, 5}, 0); // behind #1
    book.drain_events();

    const auto r = book.modify_order(1, kBob, 100, 3, 1);
    CHECK(r.reject_reason == RejectReason::UnknownOrder);
    CHECK(r.unaccepted_quantity == 3);
    CHECK(book.drain_events().empty());
    CHECK(ask_qty_at(book, 100) == 15);

    // #1 is still first in line: a buy of 10 fills only #1.
    book.add_order({3, 4, Side::Buy, OrderType::Limit, 100, 10}, 2);
    CHECK(book.final_state(1) == FinalState::Filled);
    CHECK_FALSE(book.final_state(2).has_value());
    CHECK(book.check_invariants());
}

TEST_CASE("ownership is checked before anything else, so a bad modify from a "
          "non-owner still says UnknownOrder, not InvalidQuantity") {
    Book book;
    book.add_order({1, kAlice, Side::Buy, OrderType::Limit, 100, 10}, 0);
    CHECK(book.modify_order(1, kBob, 100, 0, 1).reject_reason == RejectReason::UnknownOrder);
    CHECK(book.modify_order(1, kBob, -5, 3, 1).reject_reason == RejectReason::UnknownOrder);
    // The owner gets the real validation error.
    CHECK(book.modify_order(1, kAlice, 100, 0, 1).reject_reason == RejectReason::InvalidQuantity);
}

TEST_CASE("TooLate is only told to the owner: a filled or cancelled order looks "
          "unknown to everyone else") {
    Book book;
    // #1 fills completely.
    book.add_order({1, kAlice, Side::Sell, OrderType::Limit, 100, 4}, 0);
    book.add_order({2, 3, Side::Buy, OrderType::Limit, 100, 4}, 0);
    // #5 is cancelled by its owner.
    book.add_order({5, kAlice, Side::Buy, OrderType::Limit, 90, 1}, 0);
    REQUIRE(book.cancel_order(5, kAlice, 1) == RejectReason::None);

    for (OrderId id : {1, 5}) {
        CHECK(book.cancel_order(id, kAlice, 2) == RejectReason::TooLate);
        CHECK(book.modify_order(id, kAlice, 100, 9, 2).reject_reason == RejectReason::TooLate);
        CHECK(book.cancel_order(id, kBob, 2) == RejectReason::UnknownOrder);
        CHECK(book.modify_order(id, kBob, 100, 9, 2).reject_reason == RejectReason::UnknownOrder);
    }
    // An id that never existed: the same answer for everyone.
    CHECK(book.cancel_order(77, kAlice, 2) == RejectReason::UnknownOrder);
    CHECK(book.cancel_order(77, kBob, 2) == RejectReason::UnknownOrder);
}

TEST_CASE("a non-owner cannot cancel or modify a dormant stop; TooLate after it "
          "is gone goes to the owner only") {
    Book book;
    book.add_order({1, 9, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 8, Side::Buy, OrderType::Limit, 100, 1}, 0); // last trade 100
    REQUIRE(book.place_stop_order({10, kAlice, Side::Sell, 90, 3}, 0) == RejectReason::None);
    book.drain_events();

    CHECK(book.cancel_stop_order(10, kBob, 1) == RejectReason::UnknownOrder);
    CHECK(book.modify_stop_order(10, kBob, 80, std::nullopt, 1, 1) == RejectReason::UnknownOrder);
    CHECK(book.drain_events().empty()); // stop untouched

    CHECK(book.modify_stop_order(10, kAlice, 80, std::nullopt, 2, 2) == RejectReason::None);
    CHECK(book.cancel_stop_order(10, kAlice, 3) == RejectReason::None);

    CHECK(book.cancel_stop_order(10, kAlice, 4) == RejectReason::TooLate);
    CHECK(book.cancel_stop_order(10, kBob, 4) == RejectReason::UnknownOrder);
    CHECK(book.modify_stop_order(10, kBob, 80, std::nullopt, 1, 4) == RejectReason::UnknownOrder);
}

TEST_CASE("a triggered stop-limit that rests keeps its owner: only the owner can "
          "cancel the resting order") {
    Book book;
    book.add_order({1, 9, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 8, Side::Buy, OrderType::Limit, 100, 1}, 0); // last 100
    // Alice: sell stop at 95, limit 97. Nothing bids at 97+, so once it
    // triggers it rests as a sell at 97.
    REQUIRE(book.place_stop_order({10, kAlice, Side::Sell, 95, 2, 97}, 0) == RejectReason::None);
    book.add_order({3, 7, Side::Buy, OrderType::Limit, 95, 1}, 1);
    book.add_order({4, 6, Side::Sell, OrderType::Limit, 95, 1}, 1); // trade at 95: triggers
    REQUIRE(ask_qty_at(book, 97) == 2);

    CHECK(book.cancel_order(10, kBob, 2) == RejectReason::UnknownOrder);
    CHECK(ask_qty_at(book, 97) == 2);
    CHECK(book.cancel_order(10, kAlice, 2) == RejectReason::None);
    CHECK(ask_qty_at(book, 97) == 0);
    CHECK(book.check_invariants());
}
