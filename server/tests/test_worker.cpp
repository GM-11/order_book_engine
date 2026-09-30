// Tests for server::Worker: one thread that owns some Books and processes
// their commands strictly in inbox order.
//
// Pattern used throughout: run the worker, then destroy it (its destructor
// pushes Shutdown and joins), then read everything it produced from the
// outbox. After the join, no other thread touches the outbox, so the reads
// are race-free. Run under -fsanitize=thread to check the concurrent parts.

#include "server/worker.hpp"

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

engine::Order limit(engine::OrderId id, engine::OwnerId owner, engine::Side side,
                    engine::Price price, engine::Quantity qty) {
    engine::Order o{};
    o.id = id;
    o.owner_id = owner;
    o.side = side;
    o.type = engine::OrderType::Limit;
    o.price = price;
    o.quantity = qty;
    return o;
}

Worker::Clock fixed_clock(engine::Timestamp t) {
    return [t] { return t; };
}

std::vector<Output> drain(BlockingQueue<Output>& q) {
    std::vector<Output> out;
    while (auto x = q.try_pop()) out.push_back(std::move(*x));
    return out;
}

std::vector<MarketEvent> events_of(const std::vector<Output>& out) {
    std::vector<MarketEvent> ev;
    for (const auto& o : out)
        if (auto* m = std::get_if<MarketEvent>(&o)) ev.push_back(*m);
    return ev;
}

std::vector<Reply> replies_of(const std::vector<Output>& out) {
    std::vector<Reply> r;
    for (const auto& o : out)
        if (auto* p = std::get_if<Reply>(&o)) r.push_back(*p);
    return r;
}

}  // namespace

// ---------------------------------------------------------------- setup rules

TEST_CASE("constructor rejects an empty clock", "[worker]") {
    BlockingQueue<Output> out;
    REQUIRE_THROWS_AS(Worker(out, Worker::Clock{}), std::invalid_argument);
}

TEST_CASE("add_book rejects duplicates, null books, and anything after start",
          "[worker]") {
    BlockingQueue<Output> out;
    Worker w(out, fixed_clock(0));
    w.add_book(1, std::make_unique<engine::Book>());

    REQUIRE_THROWS_AS(w.add_book(1, std::make_unique<engine::Book>()),
                      std::logic_error);
    REQUIRE_THROWS_AS(w.add_book(2, nullptr), std::invalid_argument);

    w.start();
    REQUIRE_THROWS_AS(w.add_book(3, std::make_unique<engine::Book>()),
                      std::logic_error);
    REQUIRE_THROWS_AS(w.start(), std::logic_error);  // second start
}

TEST_CASE("a worker that was never started is destroyed without hanging",
          "[worker]") {
    BlockingQueue<Output> out;
    { Worker w(out, fixed_clock(0)); }  // no Shutdown needed, nothing to join
    SUCCEED();
}

// ------------------------------------------------------------ single command

TEST_CASE("one resting limit order: Accepted, Rested, then a clean Reply",
          "[worker]") {
    BlockingQueue<Output> out;
    {
        Worker w(out, fixed_clock(5));
        w.add_book(42, std::make_unique<engine::Book>());
        w.start();
        w.submit(NewOrder{77, 42, limit(10, 1, engine::Side::Buy, 100, 5)});
    }
    auto all = drain(out);
    REQUIRE(all.size() == 3);

    auto ev = events_of(all);
    REQUIRE(ev.size() == 2);
    REQUIRE(ev[0].symbol == 42);
    REQUIRE(ev[0].event.kind == EventKind::Accepted);
    REQUIRE(ev[1].event.kind == EventKind::Rested);
    REQUIRE(ev[1].event.quantity == 5);

    // The worker's clock, not the sender, sets the time.
    REQUIRE(ev[0].event.ts == 5);

    // Events for a command come before its Reply.
    REQUIRE(std::holds_alternative<Reply>(all.back()));
    auto r = std::get<Reply>(all.back());
    REQUIRE(r.request_id == 77);
    REQUIRE(r.symbol == 42);
    REQUIRE(r.reject_reason == RejectReason::None);
    REQUIRE(r.unaccepted_quantity == 0);
}

TEST_CASE("each command kind reaches the right Book call", "[worker]") {
    BlockingQueue<Output> out;
    {
        Worker w(out, fixed_clock(1));
        w.add_book(1, std::make_unique<engine::Book>());
        w.start();
        // 1: sell 10 @ 100 rests. 2: buy 4 @ 100 trades against it.
        w.submit(NewOrder{1, 1, limit(1, 1, engine::Side::Sell, 100, 10)});
        w.submit(NewOrder{2, 1, limit(2, 2, engine::Side::Buy, 100, 4)});
        // 3: modify the resting sell to total 8 (4 filled, so 4 left).
        w.submit(ModifyOrder{.request_id = 3, .symbol = 1, .requester = 1, .order_id = 1,
                             .new_price = 100, .new_quantity = 8});
        // 4: sell stop at 90 (last trade 100, so it stays dormant).
        engine::StopOrder s{};
        s.id = 5;
        s.owner_id = 3;
        s.side = engine::Side::Sell;
        s.stop_price = 90;
        s.quantity = 1;
        w.submit(PlaceStop{4, 1, s});
        // 5: move the stop to 80. 6: cancel it. 7: cancel it again (too late).
        w.submit(ModifyStop{.request_id = 5, .symbol = 1, .requester = 3, .order_id = 5,
                            .new_stop_price = 80, .new_limit_price = std::nullopt, .new_quantity = 1});
        w.submit(CancelStop{.request_id = 6, .symbol = 1, .requester = 3, .order_id = 5});
        w.submit(CancelStop{.request_id = 7, .symbol = 1, .requester = 3, .order_id = 5});
        // 8: cancel the resting sell. 9: cancel an id that never existed.
        w.submit(CancelOrder{.request_id = 8, .symbol = 1, .requester = 1, .order_id = 1});
        w.submit(CancelOrder{.request_id = 9, .symbol = 1, .requester = 1, .order_id = 999});
    }
    auto all = drain(out);
    auto replies = replies_of(all);
    REQUIRE(replies.size() == 9);
    for (std::size_t i = 0; i < replies.size(); ++i)
        REQUIRE(replies[i].request_id == i + 1);  // replies in submit order

    REQUIRE(replies[0].reject_reason == RejectReason::None);
    REQUIRE(replies[1].reject_reason == RejectReason::None);
    REQUIRE(replies[2].reject_reason == RejectReason::None);
    REQUIRE(replies[3].reject_reason == RejectReason::None);
    REQUIRE(replies[4].reject_reason == RejectReason::None);
    REQUIRE(replies[5].reject_reason == RejectReason::None);
    REQUIRE(replies[6].reject_reason == RejectReason::TooLate);
    REQUIRE(replies[7].reject_reason == RejectReason::None);
    REQUIRE(replies[8].reject_reason == RejectReason::UnknownOrder);

    auto ev = events_of(all);
    int trades = 0;
    bool modified = false, stop_accepted = false, stop_modified = false,
         stop_cancelled = false, cancelled = false;
    for (auto& m : ev) {
        switch (m.event.kind) {
        case EventKind::Trade:
            ++trades;
            REQUIRE(m.event.quantity == 4);
            break;
        case EventKind::Modified: modified = true; break;
        case EventKind::StopAccepted: stop_accepted = true; break;
        case EventKind::StopModified: stop_modified = true; break;
        case EventKind::StopCancelled: stop_cancelled = true; break;
        case EventKind::Cancelled:
            cancelled = true;
            REQUIRE(m.event.order_id == 1);
            REQUIRE(m.event.quantity == 4);  // 8 total - 4 filled
            break;
        default: break;
        }
    }
    REQUIRE(trades == 1);
    REQUIRE(modified);
    REQUIRE(stop_accepted);
    REQUIRE(stop_modified);
    REQUIRE(stop_cancelled);
    REQUIRE(cancelled);
}

TEST_CASE("time comes from the worker's clock, once per command, in inbox order",
          "[worker]") {
    BlockingQueue<Output> out;
    std::atomic<engine::Timestamp> ticks{0};
    {
        // Only the worker thread calls the clock; atomic just keeps TSan
        // honest if that ever changes.
        Worker w(out, [&ticks] { return ++ticks; });
        w.add_book(1, std::make_unique<engine::Book>());
        w.start();
        for (engine::OrderId id = 1; id <= 5; ++id)
            w.submit(NewOrder{id, 1, limit(id, id, engine::Side::Buy, 100, 1)});
    }
    REQUIRE(ticks.load() == 5);  // one clock read per command
    engine::Timestamp expected = 1;
    for (auto& m : events_of(drain(out))) {
        if (m.event.kind == EventKind::Accepted) {
            REQUIRE(m.event.ts == expected);
            ++expected;
        }
    }
    REQUIRE(expected == 6);
}

// ------------------------------------------------------------------ shutdown

TEST_CASE("shutdown drains every command queued before it", "[worker]") {
    constexpr int kOrders = 2000;
    BlockingQueue<Output> out;
    {
        Worker w(out, fixed_clock(0));
        w.add_book(1, std::make_unique<engine::Book>());
        w.start();
        for (int i = 1; i <= kOrders; ++i)
            w.submit(NewOrder{static_cast<RequestId>(i), 1,
                              limit(i, 1, engine::Side::Buy, 100, 1)});
        // Destructor runs here, while most of these are still queued.
    }
    REQUIRE(replies_of(drain(out)).size() == kOrders);
}

// ------------------------------------------------------ many threads at once

TEST_CASE("two workers, four symbols, four producer threads: every command "
          "answered once, every symbol's events gap-free and in order",
          "[worker][threads]") {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 2000;
    const SymbolId symbols[] = {1, 2, 3, 4};

    BlockingQueue<Output> out;  // declared first: outlives both workers
    std::atomic<engine::OrderId> next_order_id{1};
    {
        Worker w1(out, fixed_clock(0));
        Worker w2(out, fixed_clock(0));
        w1.add_book(1, std::make_unique<engine::Book>());
        w1.add_book(2, std::make_unique<engine::Book>());
        w2.add_book(3, std::make_unique<engine::Book>());
        w2.add_book(4, std::make_unique<engine::Book>());
        w1.start();
        w2.start();

        // Static symbol -> worker table, as the Router will have.
        auto route = [&](SymbolId s) -> Worker& { return s <= 2 ? w1 : w2; };

        std::vector<std::thread> producers;
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&, p] {
                for (int i = 0; i < kPerProducer; ++i) {
                    SymbolId sym = symbols[(p + i) % 4];
                    engine::OrderId id = next_order_id.fetch_add(1);
                    // Alternate sides around 100 so orders cross and trade.
                    auto side = (i % 2) ? engine::Side::Sell : engine::Side::Buy;
                    engine::Price px = (i % 2) ? 99 + (i % 3) : 100 + (i % 3);
                    RequestId req =
                        static_cast<RequestId>(p) * kPerProducer + i + 1;
                    route(sym).submit(NewOrder{
                        req, sym,
                        limit(id, static_cast<engine::OwnerId>(p * 10 + i % 7),
                              side, px, 1 + i % 5)});
                }
            });
        }
        for (auto& t : producers) t.join();
    }  // both workers shut down and join here

    auto all = drain(out);

    // Every request answered exactly once.
    std::set<RequestId> seen;
    for (auto& r : replies_of(all)) REQUIRE(seen.insert(r.request_id).second);
    REQUIRE(seen.size() ==
            static_cast<std::size_t>(kProducers * kPerProducer));

    // Per symbol: sequence numbers are 1, 2, 3, ... in outbox order.
    std::map<SymbolId, engine::SequenceNumber> next_seq;
    int trades = 0;
    bool gap_free = true;
    for (auto& m : events_of(all)) {
        auto& n = next_seq[m.symbol];
        if (m.event.sequence_number != n + 1) gap_free = false;
        n = m.event.sequence_number;
        if (m.event.kind == EventKind::Trade) ++trades;
    }
    REQUIRE(gap_free);
    REQUIRE(next_seq.size() == 4);
    REQUIRE(trades > 0);  // the load actually exercised matching
}

TEST_CASE("stop() drains, joins, and is safe to call again or before start",
          "[worker]") {
    BlockingQueue<Output> out;
    Worker idle(out, fixed_clock(0));
    idle.stop();  // never started: no-op

    Worker w(out, fixed_clock(0));
    w.add_book(1, std::make_unique<engine::Book>());
    w.start();
    for (int i = 0; i < 500; ++i)
        w.submit(NewOrder{static_cast<RequestId>(i + 1), 1,
                          limit(i + 1, 1, engine::Side::Buy, 100, 1)});
    w.stop();  // returns only after all 500 are processed
    REQUIRE(replies_of(drain(out)).size() == 500);
    w.stop();  // second call: no second pill, no second join
    REQUIRE(drain(out).empty());
}
