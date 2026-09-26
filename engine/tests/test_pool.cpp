#include <catch2/catch_test_macros.hpp>

#include "engine/book.hpp"
#include "engine/pool.hpp"

#include <set>
#include <type_traits>

using namespace engine;

// Both hold raw Node* into the pool's storage; a copy would point into the
// original. Copying must not compile.
static_assert(!std::is_copy_constructible_v<NodePool>);
static_assert(!std::is_copy_assignable_v<NodePool>);
static_assert(!std::is_copy_constructible_v<Book>);

TEST_CASE("A pool hands out exactly its capacity in distinct nodes") {
    NodePool pool(3);
    std::set<Node *> seen;
    for (int i = 0; i < 3; ++i) {
        Node *node = pool.acquire();
        REQUIRE(node != nullptr);
        seen.insert(node);
    }
    CHECK(seen.size() == 3);
    CHECK(pool.acquire() == nullptr);
}

TEST_CASE("A zero-capacity pool is immediately exhausted") {
    NodePool pool(0);
    CHECK(pool.acquire() == nullptr);
}

TEST_CASE("Released nodes are reused last-in, first-out") {
    NodePool pool(3);
    Node *a = pool.acquire();
    Node *b = pool.acquire();
    Node *c = pool.acquire();
    REQUIRE(pool.acquire() == nullptr);

    pool.release(b);
    pool.release(a);
    CHECK(pool.acquire() == a); // most recently released comes back first
    CHECK(pool.acquire() == b);
    CHECK(pool.acquire() == nullptr);

    pool.release(c);
    CHECK(pool.acquire() == c);
}

TEST_CASE("A reacquired node carries no stale links or order data") {
    NodePool pool(2);
    Node *node = pool.acquire();
    Node *other = pool.acquire();
    REQUIRE(node != nullptr);
    REQUIRE(other != nullptr);

    // Dirty everything a previous owner might have left behind.
    node->next = other;
    node->prev = other;
    node->order.id = 99;
    node->order.quantity = 42;
    node->order.price = 100;

    pool.release(node);
    Node *again = pool.acquire();
    REQUIRE(again == node);
    CHECK(again->next == nullptr); // free-list link must not leak out
    CHECK(again->prev == nullptr);
    CHECK(again->order.id == 0);
    CHECK(again->order.quantity == 0);
    CHECK(again->order.price == 0);
}

TEST_CASE("Release and reacquire cycles never lose or duplicate nodes") {
    NodePool pool(4);
    Node *nodes[4];
    for (int round = 0; round < 3; ++round) {
        std::set<Node *> seen;
        for (auto &n : nodes) {
            n = pool.acquire();
            REQUIRE(n != nullptr);
            seen.insert(n);
        }
        CHECK(seen.size() == 4);
        CHECK(pool.acquire() == nullptr);
        for (Node *n : nodes)
            pool.release(n);
    }
}
