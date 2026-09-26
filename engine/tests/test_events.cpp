#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

#include <algorithm>

using namespace engine;

namespace {
// Every Accepted must reach exactly one outcome before the id is accepted
// again: fully filled by its own (aggressor) trades, Rested, or Cancelled.
// Rested/Cancelled quantity plus filled quantity must equal what was accepted.
void expect_every_order_terminates(const std::vector<EngineEvent> &events) {
    for (std::size_t i = 0; i < events.size(); ++i) {
        if (events[i].kind != EventKind::Accepted)
            continue;
        const OrderId id = events[i].order_id;
        const Quantity accepted = events[i].quantity;
        Quantity filled = 0;
        bool closed = false;
        for (std::size_t j = i + 1; j < events.size() && !closed; ++j) {
            const EngineEvent &e = events[j];
            if (e.order_id != id)
                continue;
            if (e.kind == EventKind::Accepted)
                break;
            if (e.kind == EventKind::Trade)
                filled += e.quantity;
            if (e.kind == EventKind::Rested || e.kind == EventKind::Cancelled) {
                CHECK(filled + e.quantity == accepted);
                closed = true;
            }
        }
        INFO("order " << id << " accepted at seq "
                      << events[i].sequence_number);
        CHECK((closed || filled == accepted));
    }
}
} // namespace

TEST_CASE("Order lifecycle events are timestamped and sequenced") {
    Book book;

    book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 5}, 10);
    const auto accepted_and_rested = book.drain_events();
    REQUIRE(accepted_and_rested.size() == 2);
    CHECK(accepted_and_rested[0].sequence_number == 1);
    CHECK(accepted_and_rested[0].kind == EventKind::Accepted);
    CHECK(accepted_and_rested[0].ts == 10);
    CHECK(accepted_and_rested[0].order_id == 1);
    CHECK(accepted_and_rested[0].quantity == 5);
    CHECK(accepted_and_rested[1].sequence_number == 2);
    CHECK(accepted_and_rested[1].kind == EventKind::Rested);
    CHECK(accepted_and_rested[1].price == 100);
    CHECK(accepted_and_rested[1].quantity == 5);

    book.modify_order(1, 100, 3, 11);
    const auto modified = book.drain_events();
    REQUIRE(modified.size() == 1);
    CHECK(modified[0].sequence_number == 3);
    CHECK(modified[0].kind == EventKind::Modified);
    CHECK(modified[0].ts == 11);
    CHECK(modified[0].quantity == 3);

    REQUIRE(book.cancel_order(1, 12));
    const auto cancelled = book.drain_events();
    REQUIRE(cancelled.size() == 1);
    CHECK(cancelled[0].sequence_number == 4);
    CHECK(cancelled[0].kind == EventKind::Cancelled);
    CHECK(cancelled[0].ts == 12);
    CHECK(cancelled[0].quantity == 3);
}

TEST_CASE("Trade and stop events retain ownership and triggering order") {
    Book book;
    REQUIRE(book.place_stop_order({3, 33, Side::Buy, 100, 1}, 1) ==
            RejectReason::None);
    const auto stop_accepted = book.drain_events();
    REQUIRE(stop_accepted.size() == 1);
    CHECK(stop_accepted[0].kind == EventKind::StopAccepted);
    CHECK(stop_accepted[0].ts == 1);
    CHECK(stop_accepted[0].order_id == 3);

    book.add_order({1, 11, Side::Sell, OrderType::Limit, 100, 2}, 2);
    book.drain_events();
    const auto result =
        book.add_order({2, 22, Side::Buy, OrderType::Limit, 100, 1}, 3);
    REQUIRE(result.trades.size() == 1);

    const auto events = book.drain_events();
    REQUIRE(events.size() == 5);
    CHECK(events[0].kind == EventKind::Accepted);
    CHECK(events[0].order_id == 2);
    CHECK(events[1].kind == EventKind::Trade);
    CHECK(events[1].ts == 3);
    CHECK(events[1].order_id == 2);
    CHECK(events[1].passive_id == 1);
    CHECK(events[1].owner_id == 22);
    CHECK(events[1].other_owner == 11);
    CHECK(events[1].side == Side::Buy);
    CHECK(events[2].kind == EventKind::StopTriggered);
    CHECK(events[2].order_id == 3);
    CHECK(events[3].kind == EventKind::Accepted);
    CHECK(events[3].order_id == 3);
    CHECK(events[4].kind == EventKind::Trade);
    CHECK(events[4].order_id == 3);
    CHECK(events[4].passive_id == 1);
}

TEST_CASE("Stop cancellation emits its lifecycle event") {
    Book book;
    REQUIRE(book.place_stop_order({1, 11, Side::Sell, 90, 2}, 5) ==
            RejectReason::None);
    book.drain_events();

    REQUIRE(book.cancel_stop_order(1, 6));
    const auto events = book.drain_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EventKind::StopCancelled);
    CHECK(events[0].ts == 6);
    CHECK(events[0].order_id == 1);
    CHECK(events[0].owner_id == 11);
    CHECK(events[0].price == 90);
    CHECK(events[0].quantity == 2);
}

namespace {
void check_contiguous_sequences(const std::vector<EngineEvent> &events) {
    REQUIRE_FALSE(events.empty());
    for (std::size_t i = 0; i < events.size(); ++i)
        CHECK(events[i].sequence_number == i + 1);
}

void check_equal_events(const std::vector<EngineEvent> &left,
                        const std::vector<EngineEvent> &right) {
    REQUIRE(left.size() == right.size());
    for (std::size_t i = 0; i < left.size(); ++i) {
        CHECK(left[i].sequence_number == right[i].sequence_number);
        CHECK(left[i].kind == right[i].kind);
        CHECK(left[i].ts == right[i].ts);
        CHECK(left[i].order_id == right[i].order_id);
        CHECK(left[i].passive_id == right[i].passive_id);
        CHECK(left[i].owner_id == right[i].owner_id);
        CHECK(left[i].other_owner == right[i].other_owner);
        CHECK(left[i].side == right[i].side);
        CHECK(left[i].price == right[i].price);
        CHECK(left[i].quantity == right[i].quantity);
    }
}

void run_replay_scenario(Book &book) {
    book.place_stop_order({9, 90, Side::Buy, 100, 1}, 0);
    book.add_order({1, 10, Side::Sell, OrderType::Limit, 100, 2}, 1);
    book.add_order({2, 20, Side::Buy, OrderType::Limit, 100, 1}, 2);
    book.add_order({3, 30, Side::Buy, OrderType::Limit, 95, 2}, 3);
    book.modify_order(3, 95, 1, 4);
    book.cancel_order(3, 5);
}
} // namespace

TEST_CASE("Mixed event streams use contiguous sequence numbers") {
    Book book(100, 1000, 10, 50);
    book.place_stop_order({90, 900, Side::Buy, 100, 1}, 0);
    book.add_order({1, 10, Side::Sell, OrderType::Limit, 100, 2}, 0);
    book.add_order({2, 20, Side::Buy, OrderType::Limit, 100, 1}, 0);
    book.add_order({3, 30, Side::Buy, OrderType::Limit, 95, 2}, 1);
    book.modify_order(3, 95, 1, 2);
    book.cancel_order(3, 3);

    book.add_order({4, 40, Side::Sell, OrderType::Limit, 120, 1}, 4);
    book.add_order({5, 50, Side::Buy, OrderType::Market, 0, 1}, 4);
    book.add_order({6, 60, Side::Sell, OrderType::Limit, 150, 1}, 20);
    const auto halted =
        book.add_order({7, 70, Side::Buy, OrderType::Market, 0, 1}, 20);
    REQUIRE(halted.reject_reason == RejectReason::SymbolHalted);

    const auto events = book.drain_events();
    check_contiguous_sequences(events);
    expect_every_order_terminates(events);
    CHECK(
        std::any_of(events.begin(), events.end(), [](const EngineEvent &event) {
            return event.kind == EventKind::Halted;
        }));
}

TEST_CASE("Identical books produce identical replayable event streams") {
    Book first;
    Book second;

    run_replay_scenario(first);
    run_replay_scenario(second);

    const auto first_events = first.drain_events();
    const auto second_events = second.drain_events();
    check_equal_events(first_events, second_events);
    expect_every_order_terminates(first_events);
}

TEST_CASE(
    "An order that cannot rest because the pool is full still terminates") {
    Book book(1);
    book.add_order({1, 11, Side::Sell, OrderType::Limit, 101, 1}, 1);
    book.drain_events();

    const auto result =
        book.add_order({2, 22, Side::Buy, OrderType::Limit, 100, 5}, 2);
    REQUIRE(result.reject_reason == RejectReason::PoolExhausted);

    const auto events = book.drain_events();
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == EventKind::Accepted);
    CHECK(events[1].kind == EventKind::Cancelled);
    CHECK(events[1].order_id == 2);
    CHECK(events[1].quantity == 5);
    expect_every_order_terminates(events);
}

TEST_CASE("A reprice modify emits Replaced, never a terminal Cancelled") {
    Book book;
    book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 5}, 1);
    book.drain_events();

    REQUIRE(book.modify_order(1, 101, 7, 2).reject_reason ==
            RejectReason::None);
    const auto events = book.drain_events();
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == EventKind::Replaced);
    CHECK(events[0].order_id == 1);
    CHECK(events[0].price == 100); // what left the book
    CHECK(events[0].quantity == 5);
    CHECK(events[1].kind == EventKind::Accepted);
    CHECK(events[1].price == 101);
    CHECK(events[1].quantity == 7);
    CHECK(events[2].kind == EventKind::Rested);
    CHECK(std::none_of(events.begin(), events.end(), [](const EngineEvent &e) {
        return e.kind == EventKind::Cancelled;
    }));
    expect_every_order_terminates(events);

    // A user cancel is still a Cancelled.
    REQUIRE(book.cancel_order(1, 3));
    const auto cancelled = book.drain_events();
    REQUIRE(cancelled.size() == 1);
    CHECK(cancelled[0].kind == EventKind::Cancelled);
}

TEST_CASE("Resumed is a symbol-level event with no order fields") {
    Book book(100, 1000, 0, 50); // grace 0: second breach in one walk halts
    book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 1}, 1);
    book.add_order({2, 22, Side::Sell, OrderType::Limit, 100, 1}, 1);
    book.add_order({3, 33, Side::Sell, OrderType::Limit, 120, 1}, 2);
    book.add_order({4, 44, Side::Sell, OrderType::Limit, 130, 1}, 2);
    book.add_order({5, 55, Side::Buy, OrderType::Market, 0, 2}, 3);
    book.drain_events();

    book.add_order({6, 66, Side::Buy, OrderType::Limit, 90, 1}, 100);
    const auto events = book.drain_events();
    REQUIRE_FALSE(events.empty());
    REQUIRE(events[0].kind == EventKind::Resumed);
    CHECK(events[0].ts == 100);
    CHECK(events[0].order_id == 0);
    CHECK(events[0].owner_id == 0);
    CHECK(events[0].quantity == 0);
}
