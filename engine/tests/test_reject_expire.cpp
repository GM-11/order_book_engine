#include <catch2/catch_test_macros.hpp>

#include "book_replica.hpp"
#include "engine/book.hpp"

#include <algorithm>
#include <map>
#include <random>

using namespace engine;

namespace {
std::vector<EngineEvent> of_kind(const std::vector<EngineEvent> &events, EventKind kind) {
    std::vector<EngineEvent> out;
    std::copy_if(events.begin(), events.end(), std::back_inserter(out),
                 [&](const EngineEvent &e) { return e.kind == kind; });
    return out;
}

void expect_one_rejected(Book &book, Order order, RejectReason why) {
    book.drain_events();
    const OrderResult r = book.add_order(order, 5);
    CHECK(r.reject_reason == why);
    const auto events = book.drain_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EventKind::Rejected);
    CHECK(events[0].reject_reason == why);
    CHECK(events[0].order_id == order.id);
    CHECK(events[0].owner_id == order.owner_id);
    CHECK(events[0].side == order.side);
    CHECK(events[0].price == order.price);
    CHECK(events[0].quantity == order.quantity); // the full order quantity
}
} // namespace

TEST_CASE("A rejected new order emits exactly one Rejected and no Accepted") {
    Book book;
    SECTION("invalid price") {
        expect_one_rejected(book, {1, 11, Side::Buy, OrderType::Limit, 0, 7}, RejectReason::InvalidPrice);
    }
    SECTION("invalid quantity") {
        expect_one_rejected(book, {1, 11, Side::Buy, OrderType::Limit, 100, 0}, RejectReason::InvalidQuantity);
    }
    SECTION("duplicate id") {
        book.add_order({1, 11, Side::Buy, OrderType::Limit, 100, 3}, 1);
        expect_one_rejected(book, {1, 22, Side::Sell, OrderType::Limit, 100, 4}, RejectReason::DuplicateOrderId);
    }
    SECTION("halted symbol") {
        Book halting(100, 1000, 10, 50); // 10% band, 10ms grace, 50ms halt
        halting.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
        halting.add_order({2, 2, Side::Buy, OrderType::Market, std::nullopt, 1}, 0); // reference 100
        halting.add_order({3, 3, Side::Sell, OrderType::Limit, 200, 1}, 0);
        halting.add_order({7, 3, Side::Sell, OrderType::Limit, 300, 1}, 0);
        halting.add_order({4, 4, Side::Buy, OrderType::Market, std::nullopt, 1}, 0);  // first breach, clock starts
        halting.add_order({5, 5, Side::Buy, OrderType::Market, std::nullopt, 1}, 20); // past grace: halts
        expect_one_rejected(halting, {6, 6, Side::Buy, OrderType::Limit, 100, 2}, RejectReason::SymbolHalted);
    }
}

TEST_CASE("A rejected stop emits exactly one Rejected with the stop's fields") {
    Book book;
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 1}, 0);
    book.add_order({2, 2, Side::Buy, OrderType::Market, std::nullopt, 1}, 0); // last trade 100
    book.drain_events();

    CHECK(book.place_stop_order({3, 9, Side::Sell, 100, 4, 90}, 1) == RejectReason::StopWouldTrigger);
    const auto events = book.drain_events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == EventKind::Rejected);
    CHECK(events[0].reject_reason == RejectReason::StopWouldTrigger);
    CHECK(events[0].order_id == 3);
    CHECK(events[0].price == 100); // the stop price
    CHECK(events[0].limit_price == 90);
    CHECK(events[0].quantity == 4);
}

TEST_CASE("Rejected events consume a per-symbol seq like every other event") {
    Book book;
    book.add_order({1, 11, Side::Buy, OrderType::Limit, 0, 1}, 1);   // Rejected
    book.add_order({2, 11, Side::Buy, OrderType::Limit, 100, 1}, 1); // Accepted, Rested
    book.add_order({3, 11, Side::Buy, OrderType::Limit, 100, 0}, 1); // Rejected
    const auto events = book.drain_events();
    REQUIRE(events.size() == 4);
    for (std::size_t i = 0; i < events.size(); ++i)
        CHECK(events[i].sequence_number == i + 1);
}

TEST_CASE("A market order larger than the opposite side trades, then Expires the rest") {
    Book book;
    book.add_order({1, 11, Side::Sell, OrderType::Limit, 100, 3}, 1);
    book.drain_events();

    book.add_order({2, 22, Side::Buy, OrderType::Market, std::nullopt, 10}, 2);
    const auto events = book.drain_events();
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == EventKind::Accepted);
    CHECK(events[1].kind == EventKind::Trade);
    CHECK(events[1].quantity == 3);
    CHECK(events[2].kind == EventKind::Expired);
    CHECK(events[2].order_id == 2);
    CHECK(events[2].owner_id == 22);
    CHECK(events[2].side == Side::Buy);
    CHECK(events[2].quantity == 7); // the discarded amount
    CHECK(of_kind(events, EventKind::Cancelled).empty());
}

TEST_CASE("A remainder dropped by self-trade prevention mid-match Expires") {
    Book book;
    book.add_order({1, 11, Side::Sell, OrderType::Limit, 100, 2}, 1);
    book.add_order({2, 22, Side::Sell, OrderType::Limit, 100, 5}, 1);
    book.drain_events();

    // Buys 2 from owner 11, then meets its own order and stops.
    book.add_order({3, 22, Side::Buy, OrderType::Limit, 100, 6}, 2);
    const auto events = book.drain_events();
    const auto expired = of_kind(events, EventKind::Expired);
    REQUIRE(expired.size() == 1);
    CHECK(expired[0].order_id == 3);
    CHECK(expired[0].quantity == 4);
}

TEST_CASE("Rejected and Expired do not change the book; a replica stays equal") {
    Book book;
    book.add_order({1, 11, Side::Sell, OrderType::Limit, 100, 3}, 1);
    book.add_order({2, 22, Side::Buy, OrderType::Market, std::nullopt, 10}, 2); // Expires 7
    book.add_order({3, 22, Side::Buy, OrderType::Limit, 0, 1}, 3);              // Rejected
    book.add_order({4, 33, Side::Buy, OrderType::Limit, 95, 2}, 4);

    test_support::BookReplica replica;
    for (const EngineEvent &e : book.drain_events())
        replica.apply(e);
    const BookSnapshot real = book.snapshot();
    const BookSnapshot copy = replica.to_snapshot();
    CHECK(copy.as_of_sequence == real.as_of_sequence);
    CHECK(copy.bids == real.bids);
    CHECK(copy.asks == real.asks);
}

// Property: over a random command sequence, every new order's quantity splits
// exactly into traded + rested + expired, or it has a single Rejected carrying
// the full quantity.
TEST_CASE("Quantity conservation: traded + rested + expired, or one Rejected") {
    for (std::uint32_t seed = 1; seed <= 30; ++seed) {
        INFO("seed " << seed);
        std::mt19937 rng(seed);
        Book book(64, 1000, 10, 50); // small pool so PoolExhausted also happens
        std::vector<EngineEvent> all;
        std::map<OrderId, Quantity> submitted;
        OrderId next_id = 1;
        std::vector<OrderId> live;

        for (int step = 0; step < 400; ++step) {
            const Timestamp now = step;
            const int roll = static_cast<int>(rng() % 10);
            if (roll < 7) {
                const OrderId id = next_id++;
                const OwnerId owner = 1 + rng() % 4; // few owners: self-trade happens
                const Side side = rng() % 2 ? Side::Buy : Side::Sell;
                const Quantity qty = static_cast<Quantity>(rng() % 9); // 0 is invalid
                submitted[id] = qty;
                if (rng() % 4 == 0) {
                    book.add_order({id, owner, side, OrderType::Market, std::nullopt, qty}, now);
                } else {
                    const Price price = static_cast<Price>(95 + rng() % 11 - (rng() % 20 == 0 ? 95 : 0)); // some <= 0
                    book.add_order({id, owner, side, OrderType::Limit, price, qty}, now);
                }
                live.push_back(id);
            } else if (roll < 9 && !live.empty()) {
                const OrderId id = live[rng() % live.size()];
                for (OwnerId o = 1; o <= 4; ++o)
                    book.cancel_order(id, o, now);
            } else {
                const OrderId id = next_id++;
                book.place_stop_order({id, 1 + rng() % 4, rng() % 2 ? Side::Buy : Side::Sell,
                                       static_cast<Price>(90 + rng() % 20), static_cast<Quantity>(1 + rng() % 5)},
                                      now);
            }
            for (const EngineEvent &e : book.drain_events())
                all.push_back(e);
        }

        // Stops that fire re-enter as new orders under the stop's id, so only
        // check orders submitted as NewOrder.
        for (const auto &[id, qty] : submitted) {
            Quantity traded = 0, rested_or_cancelled = 0, expired = 0;
            int rejected = 0;
            Quantity rejected_qty = 0;
            bool accepted = false;
            Quantity resting_qty = 0;
            for (const EngineEvent &e : all) {
                if (e.order_id != id && e.passive_id != id)
                    continue;
                if (e.kind == EventKind::Rejected && e.order_id == id) {
                    ++rejected;
                    rejected_qty = e.quantity;
                } else if (e.kind == EventKind::Accepted && e.order_id == id) {
                    accepted = true;
                } else if (e.kind == EventKind::Trade && e.order_id == id) {
                    traded += e.quantity;
                } else if (e.kind == EventKind::Rested && e.order_id == id) {
                    resting_qty = e.quantity;
                    rested_or_cancelled += e.quantity;
                } else if (e.kind == EventKind::Expired && e.order_id == id) {
                    expired += e.quantity;
                }
            }
            if (rejected > 0) {
                CHECK(rejected == 1);
                CHECK_FALSE(accepted);
                CHECK(rejected_qty == qty);
                CHECK(traded == 0);
                CHECK(expired == 0);
                continue;
            }
            REQUIRE(accepted);
            (void)resting_qty;
            // An aggressor's quantity ends up traded, resting, or expired.
            CHECK(traded + rested_or_cancelled + expired == qty);
        }
        CHECK(book.check_invariants());
    }
}
