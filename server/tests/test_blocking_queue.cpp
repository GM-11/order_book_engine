// Tests for server::BlockingQueue.
// The multi-threaded cases are only meaningful when built with
// -fsanitize=thread (separate build dir; TSan cannot be combined with ASan):
// a missing lock may still produce correct counts by luck, but TSan
// reports the race itself.

#include "server/blocking_queue.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

using server::BlockingQueue;

// The queue owns a mutex, so copying or moving it would be meaningless.
static_assert(!std::is_copy_constructible_v<BlockingQueue<int>>);
static_assert(!std::is_copy_assignable_v<BlockingQueue<int>>);
static_assert(!std::is_move_constructible_v<BlockingQueue<int>>);

TEST_CASE("single thread: items come out in the order they went in", "[queue]") {
    BlockingQueue<int> q;
    q.push(1);
    q.push(2);
    q.push(3);
    REQUIRE(q.size() == 3);
    REQUIRE(q.pop() == 1);
    REQUIRE(q.pop() == 2);
    REQUIRE(q.pop() == 3);
    REQUIRE(q.size() == 0);
}

TEST_CASE("try_pop on an empty queue returns nothing and does not block", "[queue]") {
    BlockingQueue<int> q;
    REQUIRE_FALSE(q.try_pop().has_value());
    q.push(7);
    auto item = q.try_pop();
    REQUIRE(item.has_value());
    REQUIRE(*item == 7);
    REQUIRE_FALSE(q.try_pop().has_value());
}

TEST_CASE("move-only items work (no hidden copies)", "[queue]") {
    // Commands will carry move-only payloads later; this must compile and work.
    BlockingQueue<std::unique_ptr<int>> q;
    q.push(std::make_unique<int>(42));
    auto p = q.pop();
    REQUIRE(p != nullptr);
    REQUIRE(*p == 42);
}

TEST_CASE("pop blocks until something is pushed", "[queue][threads]") {
    BlockingQueue<int> q;
    std::atomic<bool> got{false};
    int value = 0;

    std::thread consumer([&] {
        value = q.pop(); // must sleep here, not return garbage
        got.store(true);
    });

    // Give the consumer time to reach pop() and go to sleep.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(got.load()); // still waiting: nothing was pushed

    q.push(99);
    consumer.join(); // join makes the write to `value` visible here
    REQUIRE(got.load());
    REQUIRE(value == 99);
}

namespace {
struct Tagged {
    int producer;
    int seq;
};
} // namespace

TEST_CASE("many producers, one consumer: nothing lost, nothing duplicated, "
          "each producer's items stay in order",
          "[queue][threads]") {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 50'000;

    BlockingQueue<Tagged> q;
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int s = 0; s < kPerProducer; ++s)
                q.push({p, s});
        });
    }

    // One consumer = the worker thread in the real server.
    std::vector<int> next_expected(kProducers, 0);
    bool in_order = true;
    for (int i = 0; i < kProducers * kPerProducer; ++i) {
        Tagged t = q.pop();
        if (t.seq != next_expected[t.producer])
            in_order = false;
        next_expected[t.producer] = t.seq + 1;
    }
    for (auto &t : producers)
        t.join();

    REQUIRE(in_order);
    for (int p = 0; p < kProducers; ++p)
        REQUIRE(next_expected[p] == kPerProducer);
    REQUIRE(q.size() == 0);
}

TEST_CASE("two consumers: every item is delivered exactly once", "[queue][threads]") {
    constexpr int kItems = 100'000;
    constexpr int kDone = -1; // one "stop" marker per consumer (poison pill)

    BlockingQueue<int> q;
    std::vector<int> seen_a, seen_b;

    auto consume = [&q](std::vector<int> &out) {
        for (;;) {
            int v = q.pop();
            if (v == kDone)
                return;
            out.push_back(v);
        }
    };
    std::thread a(consume, std::ref(seen_a));
    std::thread b(consume, std::ref(seen_b));

    for (int i = 0; i < kItems; ++i)
        q.push(i);
    q.push(kDone);
    q.push(kDone);
    a.join();
    b.join();

    std::vector<int> count(kItems, 0);
    for (int v : seen_a)
        ++count[v];
    for (int v : seen_b)
        ++count[v];
    bool exactly_once = true;
    for (int c : count)
        if (c != 1)
            exactly_once = false;

    REQUIRE(seen_a.size() + seen_b.size() == static_cast<std::size_t>(kItems));
    REQUIRE(exactly_once);
}
