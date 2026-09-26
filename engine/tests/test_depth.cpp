#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"

#include <random>
#include <vector>

using namespace engine;

namespace {
void check_level(const DepthLevel &level, Price price, Quantity quantity,
                 std::uint32_t orders) {
    CHECK(level.price == price);
    CHECK(level.quantity == quantity);
    CHECK(level.order_count == orders);
}
} // namespace

TEST_CASE("Depth aggregates each price level, best price first") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 5}, 1);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 100, 3}, 1);
    book.add_order({3, 3, Side::Buy, OrderType::Limit, 100, 2}, 1);
    book.add_order({4, 4, Side::Buy, OrderType::Limit, 99, 7}, 1);
    book.add_order({5, 5, Side::Sell, OrderType::Limit, 102, 4}, 1);
    book.add_order({6, 6, Side::Sell, OrderType::Limit, 101, 6}, 1);

    const auto d = book.depth(10);
    REQUIRE(d.bids.size() == 2);
    check_level(d.bids[0], 100, 10, 3); // highest bid first
    check_level(d.bids[1], 99, 7, 1);
    REQUIRE(d.asks.size() == 2);
    check_level(d.asks[0], 101, 6, 1); // lowest ask first
    check_level(d.asks[1], 102, 4, 1);
    CHECK(book.check_invariants());
}

TEST_CASE("Fills shrink level totals; empty levels disappear") {
    Book book;
    book.add_order({1, 1, Side::Sell, OrderType::Limit, 100, 5}, 1);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 3}, 1);

    SECTION("a partial fill lowers the total but keeps the count") {
        book.add_order({3, 3, Side::Buy, OrderType::Limit, 100, 2}, 2);
        const auto d = book.depth(10);
        REQUIRE(d.asks.size() == 1);
        check_level(d.asks[0], 100, 6, 2);
        CHECK(book.check_invariants());
    }

    SECTION("a full fill of the front order removes it from the count") {
        book.add_order({3, 3, Side::Buy, OrderType::Limit, 100, 5}, 2);
        const auto d = book.depth(10);
        REQUIRE(d.asks.size() == 1);
        check_level(d.asks[0], 100, 3, 1);
        CHECK(book.check_invariants());
    }

    SECTION("filling the whole level removes it") {
        book.add_order({3, 3, Side::Buy, OrderType::Market, 0, 8}, 2);
        CHECK(book.depth(10).asks.empty());
        CHECK(book.check_invariants());
    }
}

TEST_CASE("Cancel, in-place modify and replace keep totals correct") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 5}, 1);
    book.add_order({2, 2, Side::Buy, OrderType::Limit, 100, 3}, 1);

    SECTION("cancel") {
        REQUIRE(book.cancel_order(1, 2));
        check_level(book.depth(10).bids.at(0), 100, 3, 1);
        CHECK(book.check_invariants());
    }

    SECTION("in-place reduce lowers the total by exactly the difference") {
        REQUIRE(book.modify_order(1, 100, 2, 2).reject_reason ==
                RejectReason::None);
        check_level(book.depth(10).bids.at(0), 100, 5, 2);
        CHECK(book.check_invariants());
    }

    SECTION("replace to a new price moves quantity between levels") {
        REQUIRE(book.modify_order(1, 101, 9, 2).reject_reason ==
                RejectReason::None);
        const auto d = book.depth(10);
        REQUIRE(d.bids.size() == 2);
        check_level(d.bids[0], 101, 9, 1);
        check_level(d.bids[1], 100, 3, 1);
        CHECK(book.check_invariants());
    }

    SECTION("size increase at the same price goes to the back, total grows") {
        REQUIRE(book.modify_order(1, 100, 8, 2).reject_reason ==
                RejectReason::None);
        check_level(book.depth(10).bids.at(0), 100, 11, 2);
        CHECK(book.check_invariants());
    }
}

TEST_CASE("Depth respects max_levels and handles an empty book") {
    Book book;
    const auto empty = book.depth(5);
    CHECK(empty.bids.empty());
    CHECK(empty.asks.empty());
    CHECK(empty.as_of_sequence == 0);

    for (Price p = 90; p < 100; ++p)
        book.add_order(
            {static_cast<OrderId>(p), 1, Side::Buy, OrderType::Limit, p, 1}, 1);
    const auto d = book.depth(3);
    REQUIRE(d.bids.size() == 3);
    CHECK(d.bids[0].price == 99);
    CHECK(d.bids[2].price == 97);
    CHECK(book.depth(0).bids.empty());
}

TEST_CASE("Snapshot as_of_sequence matches the last emitted event") {
    Book book;
    book.add_order({1, 1, Side::Buy, OrderType::Limit, 100, 5}, 1);
    book.add_order({2, 2, Side::Sell, OrderType::Limit, 100, 2}, 2);
    const auto events = book.drain_events();
    REQUIRE_FALSE(events.empty());

    const auto snap = book.depth(10);
    CHECK(snap.as_of_sequence == events.back().sequence_number);

    // A later event must have a higher number than the snapshot.
    book.cancel_order(1, 3);
    const auto later = book.drain_events();
    REQUIRE(later.size() == 1);
    CHECK(later[0].sequence_number == snap.as_of_sequence + 1);
}

// Randomized stress: thousands of mixed operations, auditing the whole book
// after every single one. Fixed seed, so any failure is reproducible.
TEST_CASE("Random operation stream never breaks book invariants") {
    Book book(500, 5000, 5, 20); // wide band, halts still possible
    std::mt19937_64 rng(12345);
    auto pick = [&](int lo, int hi) {
        return std::uniform_int_distribution<int>(lo, hi)(rng);
    };

    OrderId next_id = 1;
    std::vector<OrderId> ids;
    Timestamp now = 0;

    for (int step = 0; step < 5000; ++step) {
        now += pick(0, 3);
        const int op = pick(0, 9);
        const Side side = pick(0, 1) ? Side::Buy : Side::Sell;
        const OwnerId owner = static_cast<OwnerId>(pick(1, 6));

        if (op <= 4) {
            const OrderId id = next_id++;
            book.add_order({id, owner, side, OrderType::Limit,
                            static_cast<Price>(pick(95, 105)),
                            static_cast<Quantity>(pick(1, 10))},
                           now);
            ids.push_back(id);
        } else if (op == 5) {
            book.add_order({next_id++, owner, side, OrderType::Market, 0,
                            static_cast<Quantity>(pick(1, 15))},
                           now);
        } else if (op == 6 && !ids.empty()) {
            book.cancel_order(ids[pick(0, static_cast<int>(ids.size()) - 1)],
                              now);
        } else if (op == 7 && !ids.empty()) {
            book.modify_order(ids[pick(0, static_cast<int>(ids.size()) - 1)],
                              static_cast<Price>(pick(95, 105)),
                              static_cast<Quantity>(pick(1, 10)), now);
        } else if (op == 8) {
            book.place_stop_order({next_id++, owner, side,
                                   static_cast<Price>(pick(95, 105)),
                                   static_cast<Quantity>(pick(1, 5))},
                                  now);
        }
        book.drain_events();

        INFO("step " << step);
        REQUIRE(book.check_invariants());
    }
}
