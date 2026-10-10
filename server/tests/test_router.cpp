// Tests for server::Router: the front door that owns the workers, routes each
// command to the worker that owns its symbol, and runs startup/shutdown.
//
// Pattern: submit, call shutdown() (every worker drains its inbox and joins),
// then read the outbox. After the joins no other thread touches the outbox,
// so the reads are race-free. Run under -fsanitize=thread for the
// concurrent parts.

#include "server/router.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace server;
using engine::EventKind;
using engine::RejectReason;

namespace {

engine::Order limit(engine::OrderId id, engine::OwnerId owner, engine::Side side, engine::Price price,
                    engine::Quantity qty) {
    engine::Order o{};
    o.id = id;
    o.owner_id = owner;
    o.side = side;
    o.type = engine::OrderType::Limit;
    o.price = price;
    o.quantity = qty;
    return o;
}

Router::Clock fixed_clock(engine::Timestamp t) {
    return [t] { return t; }; // no shared state: safe to copy into every worker
}

std::vector<Output> drain(BlockingQueue<Output> &q) {
    std::vector<Output> out;
    while (auto x = q.try_pop())
        out.push_back(std::move(*x));
    return out;
}

std::vector<MarketEvent> events_of(const std::vector<Output> &out) {
    std::vector<MarketEvent> ev;
    for (const auto &o : out)
        if (auto *m = std::get_if<MarketEvent>(&o))
            ev.push_back(*m);
    return ev;
}

std::vector<Reply> replies_of(const std::vector<Output> &out) {
    std::vector<Reply> r;
    for (const auto &o : out)
        if (auto *p = std::get_if<Reply>(&o))
            r.push_back(*p);
    return r;
}

NewOrder buy(RequestId req, SymbolId sym, engine::OrderId id) {
    return NewOrder{req, sym, limit(id, 1, engine::Side::Buy, 100, 10)};
}

} // namespace

// ---------------------------------------------------------------- setup rules

TEST_CASE("constructor rejects an empty clock", "[router]") {
    // The Router stamps legacy (keyboard) commands with this clock; without it every submit would crash.
    REQUIRE_THROWS_AS(Router(1, Router::Clock{}), std::invalid_argument);
}

TEST_CASE("constructor rejects zero workers", "[router]") {
    REQUIRE_THROWS_AS(Router(0, fixed_clock(0)), std::invalid_argument);
}

TEST_CASE("add_symbol rejects a bad worker index, any duplicate symbol, and "
          "anything after start",
          "[router]") {
    Router r(2, fixed_clock(0));
    r.add_symbol(7, 0, std::make_unique<engine::Book>());

    REQUIRE_THROWS(r.add_symbol(8, 2, std::make_unique<engine::Book>()));
    // Same worker: the worker itself would catch it.
    REQUIRE_THROWS(r.add_symbol(7, 0, std::make_unique<engine::Book>()));
    // Different worker: only the router can see this duplicate.
    REQUIRE_THROWS_AS(r.add_symbol(7, 1, std::make_unique<engine::Book>()), std::logic_error);

    r.start();
    REQUIRE_THROWS(r.add_symbol(9, 0, std::make_unique<engine::Book>()));
    REQUIRE_THROWS(r.start()); // twice
}

TEST_CASE("a rejected duplicate leaves the original route working", "[router]") {
    Router r(2, fixed_clock(0));
    r.add_symbol(7, 0, std::make_unique<engine::Book>());
    REQUIRE_THROWS(r.add_symbol(7, 1, std::make_unique<engine::Book>()));
    r.start();
    REQUIRE(r.submit(buy(1, 7, 1)) == SubmitResult::Queued);
    r.shutdown();
    auto replies = replies_of(drain(r.outbox()));
    REQUIRE(replies.size() == 1);
    REQUIRE(replies[0].reject_reason == RejectReason::None);
}

TEST_CASE("a router that was never started is destroyed without crashing or "
          "hanging",
          "[router]") {
    {
        Router r(3, fixed_clock(0));
    }
    {
        Router r(3, fixed_clock(0));
        r.add_symbol(1, 0, std::make_unique<engine::Book>());
        r.shutdown(); // explicit shutdown before start is a no-op too
    }
    SUCCEED();
}

// --------------------------------------------------------------- submit rules

TEST_CASE("submit before start returns NotRunning and queues nothing", "[router]") {
    Router r(1, fixed_clock(0));
    r.add_symbol(1, 0, std::make_unique<engine::Book>());
    REQUIRE(r.submit(buy(1, 1, 1)) == SubmitResult::NotRunning);
    r.start();
    r.shutdown();
    REQUIRE(drain(r.outbox()).empty());
}

TEST_CASE("unknown symbol returns UnknownSymbol and reaches no worker", "[router]") {
    Router r(2, fixed_clock(0));
    r.add_symbol(1, 0, std::make_unique<engine::Book>());
    r.start();
    REQUIRE(r.submit(buy(1, 42, 1)) == SubmitResult::UnknownSymbol);
    r.shutdown();
    REQUIRE(drain(r.outbox()).empty());
}

TEST_CASE("submitting Shutdown directly is a programmer error", "[router]") {
    Router r(1, fixed_clock(0));
    r.add_symbol(1, 0, std::make_unique<engine::Book>());
    r.start();
    REQUIRE_THROWS_AS(r.submit(Shutdown{}), std::invalid_argument);
    // ...and it did not shut the worker down: normal orders still work.
    REQUIRE(r.submit(buy(1, 1, 1)) == SubmitResult::Queued);
    r.shutdown();
    REQUIRE(replies_of(drain(r.outbox())).size() == 1);
}

// -------------------------------------------------------------------- routing

TEST_CASE("each symbol's commands reach its own Book", "[router]") {
    // Symbols 1 and 2 share worker 0; symbol 3 is alone on worker 1.
    Router r(2, fixed_clock(0));
    r.add_symbol(1, 0, std::make_unique<engine::Book>());
    r.add_symbol(2, 0, std::make_unique<engine::Book>());
    r.add_symbol(3, 1, std::make_unique<engine::Book>());
    r.start();

    // The same order id on three symbols. If two symbols shared a Book, the
    // second would be rejected as DuplicateOrderId.
    REQUIRE(r.submit(buy(10, 1, 5)) == SubmitResult::Queued);
    REQUIRE(r.submit(buy(20, 2, 5)) == SubmitResult::Queued);
    REQUIRE(r.submit(buy(30, 3, 5)) == SubmitResult::Queued);
    r.shutdown();

    auto all = drain(r.outbox());
    std::map<RequestId, Reply> by_req;
    for (auto &rep : replies_of(all))
        by_req.emplace(rep.client_request_id, rep);
    REQUIRE(by_req.size() == 3);
    REQUIRE(by_req.at(10).symbol == 1);
    REQUIRE(by_req.at(20).symbol == 2);
    REQUIRE(by_req.at(30).symbol == 3);
    for (auto &[req, rep] : by_req)
        REQUIRE(rep.reject_reason == RejectReason::None);

    // Each Book numbers its own events from 1 (Accepted, Rested).
    std::map<SymbolId, std::vector<engine::SequenceNumber>> seqs;
    for (auto &m : events_of(all))
        seqs[m.symbol].push_back(m.event.sequence_number);
    REQUIRE(seqs.size() == 3);
    for (auto &[sym, s] : seqs)
        REQUIRE(s == std::vector<engine::SequenceNumber>{1, 2});
}

TEST_CASE("orders on one symbol trade against each other through the router", "[router]") {
    Router r(2, fixed_clock(0));
    r.add_symbol(1, 1, std::make_unique<engine::Book>());
    r.start();
    r.submit(NewOrder{1, 1, limit(1, 1, engine::Side::Sell, 100, 10)});
    r.submit(NewOrder{2, 1, limit(2, 2, engine::Side::Buy, 100, 4)});
    r.shutdown();

    int trades = 0;
    for (auto &m : events_of(drain(r.outbox())))
        if (m.event.kind == EventKind::Trade)
            ++trades;
    REQUIRE(trades == 1);
}

// ------------------------------------------------------------------- shutdown

TEST_CASE("shutdown drains everything already queued, then closes the door", "[router]") {
    constexpr int kOrders = 2000;
    Router r(2, fixed_clock(0));
    r.add_symbol(1, 0, std::make_unique<engine::Book>());
    r.add_symbol(2, 1, std::make_unique<engine::Book>());
    r.start();
    for (int i = 0; i < kOrders; ++i) {
        auto sym = static_cast<SymbolId>(1 + i % 2);
        REQUIRE(r.submit(buy(i + 1, sym, i + 1)) == SubmitResult::Queued);
    }
    r.shutdown();

    REQUIRE(replies_of(drain(r.outbox())).size() == kOrders);
    REQUIRE(r.submit(buy(99999, 1, 99999)) == SubmitResult::NotRunning);
    r.shutdown(); // second call is a no-op
    REQUIRE(drain(r.outbox()).empty());
}

TEST_CASE("destroying a running router drains and joins without an explicit "
          "shutdown",
          "[router]") {
    std::vector<Output> all;
    {
        Router r(2, fixed_clock(0));
        r.add_symbol(1, 0, std::make_unique<engine::Book>());
        r.start();
        for (int i = 0; i < 100; ++i)
            r.submit(buy(i + 1, 1, i + 1));
        // No shutdown(): ~Router must do it. We cannot read the outbox after
        // the router is gone, so this case only proves "no hang, no crash".
    }
    SUCCEED();
}

// -------------------------------------------------------------------- threads

TEST_CASE("three workers, six symbols, four producer threads through the "
          "router: every queued request answered once, unknown symbols never "
          "queued, every symbol gap-free",
          "[router][threads]") {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 2000;
    // 1..6 are real; 7 is unknown and must bounce with UnknownSymbol.
    constexpr SymbolId kSymbols = 7;

    Router r(3, fixed_clock(0));
    for (SymbolId s = 1; s <= 6; ++s)
        r.add_symbol(s, (s - 1) % 3, std::make_unique<engine::Book>());
    r.start();

    std::atomic<engine::OrderId> next_order_id{1};
    std::atomic<int> queued{0}, unknown{0}, other{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                SymbolId sym = 1 + static_cast<SymbolId>((p + i) % kSymbols);
                engine::OrderId id = next_order_id.fetch_add(1);
                auto side = (i % 2) ? engine::Side::Sell : engine::Side::Buy;
                engine::Price px = (i % 2) ? 99 + (i % 3) : 100 + (i % 3);
                RequestId req = static_cast<RequestId>(p) * kPerProducer + i + 1;
                auto res = r.submit(
                    NewOrder{req, sym, limit(id, static_cast<engine::OwnerId>(p * 10 + i % 7), side, px, 1 + i % 5)});
                if (res == SubmitResult::Queued)
                    ++queued;
                else if (res == SubmitResult::UnknownSymbol)
                    ++unknown;
                else
                    ++other;
            }
        });
    }
    for (auto &t : producers)
        t.join(); // rule (c): intake stops first...
    r.shutdown(); // ...then the router shuts down

    REQUIRE(other == 0);
    REQUIRE(unknown > 0);
    REQUIRE(queued + unknown == kProducers * kPerProducer);

    auto all = drain(r.outbox());
    std::set<RequestId> seen;
    for (auto &rep : replies_of(all)) {
        REQUIRE(seen.insert(rep.client_request_id).second); // answered once
        REQUIRE(rep.symbol != 7);                    // unknown never queued
    }
    REQUIRE(seen.size() == static_cast<std::size_t>(queued.load()));

    std::map<SymbolId, engine::SequenceNumber> next_seq;
    int trades = 0;
    bool gap_free = true;
    for (auto &m : events_of(all)) {
        auto &n = next_seq[m.symbol];
        if (m.event.sequence_number != n + 1)
            gap_free = false;
        n = m.event.sequence_number;
        if (m.event.kind == EventKind::Trade)
            ++trades;
    }
    REQUIRE(gap_free);
    REQUIRE(next_seq.size() == 6);
    REQUIRE(trades > 0);
}

// ------------------------------------------------------------------ ownership

TEST_CASE("through the router, one trader cannot cancel another trader's "
          "resting order",
          "[router]") {
    Router r(1, fixed_clock(0));
    r.add_symbol(1, 0, std::make_unique<engine::Book>());
    r.start();
    r.submit(NewOrder{1, 1, limit(1, /*owner*/ 1, engine::Side::Sell, 100, 10)});
    r.submit(CancelOrder{.client_request_id = 2, .symbol = 1, .requester = 2, .order_id = 1});
    r.submit(CancelOrder{.client_request_id = 3, .symbol = 1, .requester = 1, .order_id = 1});
    r.shutdown();

    auto all = drain(r.outbox());
    std::map<RequestId, RejectReason> reason;
    for (auto &rep : replies_of(all))
        reason[rep.client_request_id] = rep.reject_reason;
    REQUIRE(reason.at(2) == RejectReason::UnknownOrder); // trader 2: refused
    REQUIRE(reason.at(3) == RejectReason::None);         // trader 1: cancelled

    int cancelled = 0;
    for (auto &m : events_of(all))
        if (m.event.kind == EventKind::Cancelled)
            ++cancelled;
    REQUIRE(cancelled == 1); // only the owner's cancel did anything
}
