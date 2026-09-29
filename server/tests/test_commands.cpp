// Compile-time and basic checks for the server's command/output types.
// Mostly guards against accidental changes: commands must stay cheap value
// types that can travel through BlockingQueue by move.

#include "server/blocking_queue.hpp"
#include "server/commands.hpp"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <variant>

using namespace server;

// Every command and output must be movable into the queue.
static_assert(std::is_nothrow_move_constructible_v<Command>);
static_assert(std::is_nothrow_move_constructible_v<Output>);
// Shutdown is addressed to a worker, not a book: it carries nothing.
static_assert(std::is_empty_v<Shutdown>);

TEST_CASE("commands survive a trip through the queue with their alternative intact",
          "[commands]") {
    BlockingQueue<Command> q;
    engine::Order o{};
    o.id = 7;
    o.owner_id = 1;
    o.side = engine::Side::Buy;
    o.type = engine::OrderType::Limit;
    o.price = 100;
    o.quantity = 10;

    q.push(NewOrder{1, 42, o});
    q.push(CancelOrder{2, 42, 7});
    q.push(Shutdown{});

    Command a = q.pop();
    REQUIRE(std::holds_alternative<NewOrder>(a));
    const auto& n = std::get<NewOrder>(a);
    REQUIRE(n.request_id == 1);
    REQUIRE(n.symbol == 42);
    REQUIRE(n.order.id == 7);
    REQUIRE(n.order.price == 100);

    Command b = q.pop();
    REQUIRE(std::holds_alternative<CancelOrder>(b));
    REQUIRE(std::get<CancelOrder>(b).order_id == 7);

    REQUIRE(std::holds_alternative<Shutdown>(q.pop()));
}

TEST_CASE("std::visit reaches the right handler for each command", "[commands]") {
    // The worker will dispatch this way; make sure every alternative is
    // distinguishable (no two commands collapse into the same type).
    auto name = [](const Command& c) {
        return std::visit(
            [](const auto& cmd) -> int {
                using T = std::decay_t<decltype(cmd)>;
                if constexpr (std::is_same_v<T, NewOrder>) return 0;
                else if constexpr (std::is_same_v<T, CancelOrder>) return 1;
                else if constexpr (std::is_same_v<T, ModifyOrder>) return 2;
                else if constexpr (std::is_same_v<T, PlaceStop>) return 3;
                else if constexpr (std::is_same_v<T, CancelStop>) return 4;
                else if constexpr (std::is_same_v<T, ModifyStop>) return 5;
                else return 6;
            },
            c);
    };
    REQUIRE(name(NewOrder{}) == 0);
    REQUIRE(name(CancelOrder{}) == 1);
    REQUIRE(name(ModifyOrder{}) == 2);
    REQUIRE(name(PlaceStop{}) == 3);
    REQUIRE(name(CancelStop{}) == 4);
    REQUIRE(name(ModifyStop{}) == 5);
    REQUIRE(name(Shutdown{}) == 6);
}
