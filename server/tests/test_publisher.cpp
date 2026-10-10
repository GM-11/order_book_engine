// Tests for MarketDataPublisher: who gets what, in what order, and what
// happens to subscribers that join late or fall behind. Run under TSan too.
#include "seq_of.hpp"
#include "server/publisher.hpp"
#include "server/worker.hpp"

#include "book_replica.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace server;
using namespace std::chrono_literals;
using engine::EventKind;
using engine::RejectReason;

namespace {

// Records snapshot requests instead of sending them to a worker.
struct FakeRequester {
    std::mutex mutex;
    std::vector<std::pair<SymbolId, SubscriberId>> calls;
    MarketDataPublisher::SnapshotRequester fn() {
        return [this](SymbolId s, SubscriberId id) {
            std::lock_guard lock(mutex);
            calls.emplace_back(s, id);
        };
    }
};

MarketEvent event(SymbolId symbol, engine::SequenceNumber seq) {
    engine::EngineEvent e{};
    e.sequence_number = seq;
    e.kind = EventKind::Accepted;
    return {symbol, e};
}

SnapshotReady snapshot_for(SymbolId symbol, SubscriberId id, engine::SequenceNumber as_of) {
    engine::BookSnapshot snap{};
    snap.as_of_sequence = as_of;
    return {symbol, id, snap};
}

Reply reply_to(std::string gateway, RequestId request) {
    Reply r{};
    r.client_request_id = request;
    r.gateway_id = std::move(gateway);
    return r;
}

// Waits for the next item; fails the test if none comes.
FeedItem next(Subscription &sub) {
    auto item = sub.next_for(2s);
    REQUIRE(item.has_value());
    return std::move(*item);
}

// Polls until `done` is true or 5 s pass. For facts the publisher thread
// makes true slightly after the item a test is reading.
template <class Pred> bool eventually(Pred done) {
    for (int i = 0; i < 500; ++i) {
        if (done())
            return true;
        std::this_thread::sleep_for(10ms);
    }
    return done();
}

// Pushes OutboxClosed and joins, so every earlier push has been routed.
void finish(BlockingQueue<Output> &outbox, MarketDataPublisher &pub) {
    outbox.push(OutboxClosed{});
    pub.join();
}

} // namespace

TEST_CASE("subscribe validates its input", "[publisher]") {
    BlockingQueue<Output> outbox;
    FakeRequester req;
    MarketDataPublisher pub(outbox, {1, 2}, req.fn());
    pub.start();

    CHECK(pub.subscribe("", {}).error == SubscribeError::EmptyGatewayId);
    CHECK(pub.subscribe("gw", {3}).error == SubscribeError::UnknownSymbol);
    CHECK(pub.subscribe("gw", {1, 3}).error == SubscribeError::UnknownSymbol);

    auto first = pub.subscribe("gw", {1});
    REQUIRE(first.error == SubscribeError::None);
    CHECK(pub.subscribe("gw", {2}).error == SubscribeError::GatewayAlreadySubscribed);

    // Leaving frees the gateway id.
    pub.unsubscribe(first.subscription->id());
    CHECK(first.subscription->end_reason() == EndReason::Unsubscribed);
    pub.unsubscribe(first.subscription->id()); // twice is fine
    CHECK(pub.subscribe("gw", {2}).error == SubscribeError::None);

    finish(outbox, pub);
    CHECK(pub.subscribe("late", {}).error == SubscribeError::Closed);
}

TEST_CASE("subscribe asks for one snapshot per symbol; empty list means every symbol", "[publisher]") {
    BlockingQueue<Output> outbox;
    FakeRequester req;
    MarketDataPublisher pub(outbox, {1, 2, 3}, req.fn());

    auto all = pub.subscribe("a", {});
    auto some = pub.subscribe("b", {3, 1, 3}); // duplicate counts once
    REQUIRE(all.error == SubscribeError::None);
    REQUIRE(some.error == SubscribeError::None);

    const std::vector<std::pair<SymbolId, SubscriberId>> expected{
        {1, all.subscription->id()},  {2, all.subscription->id()}, {3, all.subscription->id()},
        {1, some.subscription->id()}, {3, some.subscription->id()},
    };
    CHECK(req.calls == expected);
}

TEST_CASE("events before the snapshot are skipped; events after it are delivered in order", "[publisher]") {
    BlockingQueue<Output> outbox;
    FakeRequester req;
    MarketDataPublisher pub(outbox, {1, 2}, req.fn());
    pub.start();
    auto sub = pub.subscribe("gw", {1}).subscription;

    outbox.push(event(1, 1));                         // already in the snapshot
    outbox.push(event(1, 2));                         // already in the snapshot
    outbox.push(snapshot_for(1, sub->id(), 2));
    outbox.push(event(1, 3));
    outbox.push(event(2, 1));                         // a symbol it didn't ask for
    outbox.push(event(1, 4));
    finish(outbox, pub);

    const FeedItem first = next(*sub);
    REQUIRE(std::holds_alternative<SnapshotReady>(first));
    CHECK(std::get<SnapshotReady>(first).snapshot.as_of_sequence == 2);
    CHECK(std::get<MarketEvent>(next(*sub)).event.sequence_number == 3);
    CHECK(std::get<MarketEvent>(next(*sub)).event.sequence_number == 4);
    CHECK_FALSE(sub->next_for(10ms).has_value());
    CHECK(sub->end_reason() == EndReason::Shutdown);
}

TEST_CASE("each symbol goes live on its own snapshot", "[publisher]") {
    BlockingQueue<Output> outbox;
    FakeRequester req;
    MarketDataPublisher pub(outbox, {1, 2}, req.fn());
    pub.start();
    auto sub = pub.subscribe("gw", {}).subscription;

    outbox.push(snapshot_for(1, sub->id(), 0));
    outbox.push(event(1, 1)); // symbol 1 is live
    outbox.push(event(2, 5)); // symbol 2 still waiting: skipped
    outbox.push(snapshot_for(2, sub->id(), 5));
    outbox.push(event(2, 6));
    finish(outbox, pub);

    CHECK(std::get<SnapshotReady>(next(*sub)).symbol == 1);
    CHECK(std::get<MarketEvent>(next(*sub)).symbol == 1);
    CHECK(std::get<SnapshotReady>(next(*sub)).symbol == 2);
    const auto last = std::get<MarketEvent>(next(*sub));
    CHECK(last.symbol == 2);
    CHECK(last.event.sequence_number == 6);
}

TEST_CASE("stray snapshots are ignored", "[publisher]") {
    BlockingQueue<Output> outbox;
    FakeRequester req;
    MarketDataPublisher pub(outbox, {1, 2}, req.fn());
    pub.start();
    auto sub = pub.subscribe("gw", {1}).subscription;

    outbox.push(snapshot_for(1, 999, 0));       // unknown subscriber
    outbox.push(snapshot_for(2, sub->id(), 0)); // symbol it didn't ask for
    outbox.push(snapshot_for(1, sub->id(), 0));
    outbox.push(snapshot_for(1, sub->id(), 0)); // a second one for a live symbol
    finish(outbox, pub);

    CHECK(std::holds_alternative<SnapshotReady>(next(*sub)));
    CHECK_FALSE(sub->next_for(10ms).has_value());
}

TEST_CASE("replies go only to their own gateway, even before its snapshot", "[publisher]") {
    BlockingQueue<Output> outbox;
    FakeRequester req;
    MarketDataPublisher pub(outbox, {1}, req.fn());
    pub.start();
    auto a = pub.subscribe("gw-a", {}).subscription;
    auto b = pub.subscribe("gw-b", {}).subscription;

    outbox.push(reply_to("gw-a", 1));
    outbox.push(reply_to("gw-b", 2));
    outbox.push(reply_to("gw-gone", 3)); // nobody to tell
    outbox.push(reply_to("", 4));        // legacy path: no gateway
    finish(outbox, pub);

    CHECK(std::get<Reply>(next(*a)).client_request_id == 1);
    CHECK_FALSE(a->next_for(10ms).has_value());
    CHECK(std::get<Reply>(next(*b)).client_request_id == 2);
    CHECK_FALSE(b->next_for(10ms).has_value());
    CHECK(pub.stats().replies_dropped == 2);
}

TEST_CASE("a subscriber that falls behind is dropped; the others are not affected", "[publisher]") {
    BlockingQueue<Output> outbox;
    FakeRequester req;
    MarketDataPublisher pub(outbox, {1}, req.fn(), PublisherOptions{.queue_capacity = 3});
    pub.start();
    auto slow = pub.subscribe("slow", {}).subscription;
    auto fast = pub.subscribe("fast", {}).subscription;

    outbox.push(snapshot_for(1, slow->id(), 0));
    outbox.push(snapshot_for(1, fast->id(), 0));
    // "fast" keeps reading; "slow" never does. Snapshot + 2 events fill
    // slow's queue of 3; the third event overflows it.
    CHECK(std::holds_alternative<SnapshotReady>(next(*fast)));
    for (engine::SequenceNumber seq = 1; seq <= 3; ++seq) {
        outbox.push(event(1, seq));
        CHECK(std::get<MarketEvent>(next(*fast)).event.sequence_number == seq);
    }

    // "fast" may read event 3 a moment before the publisher reaches "slow".
    REQUIRE(eventually([&] { return slow->ended(); }));
    CHECK(slow->end_reason() == EndReason::SlowConsumer);
    CHECK_FALSE(slow->next_for(10ms).has_value()); // queue cleared: restart from a snapshot
    CHECK(pub.stats().slow_consumer_drops == 1);
    // Its gateway id is free again, so it can resubscribe.
    CHECK(pub.subscribe("slow", {}).error == SubscribeError::None);
    CHECK(fast->end_reason() == EndReason::None);
    finish(outbox, pub);
}

TEST_CASE("shutdown ends every subscription but leaves queued items readable", "[publisher]") {
    BlockingQueue<Output> outbox;
    FakeRequester req;
    MarketDataPublisher pub(outbox, {1}, req.fn());
    pub.start();
    auto sub = pub.subscribe("gw", {}).subscription;
    outbox.push(snapshot_for(1, sub->id(), 0));
    outbox.push(event(1, 1));
    finish(outbox, pub);

    CHECK(sub->ended());
    CHECK(sub->end_reason() == EndReason::Shutdown);
    CHECK(std::holds_alternative<SnapshotReady>(next(*sub)));
    CHECK(std::holds_alternative<MarketEvent>(next(*sub)));
    CHECK_FALSE(sub->next_for(10ms).has_value());
}

TEST_CASE("the tap sees every outbox item, routed or not", "[publisher]") {
    BlockingQueue<Output> outbox;
    FakeRequester req;
    std::atomic<int> seen{0};
    MarketDataPublisher pub(outbox, {1}, req.fn(), PublisherOptions{.tap = [&](const Output &) { ++seen; }});
    pub.start();
    outbox.push(event(1, 1));
    outbox.push(reply_to("nobody", 1));
    finish(outbox, pub);
    CHECK(seen == 2);
}

// The real thing: workers matching random orders from several threads while
// gateways subscribe at different moments. Every subscriber rebuilds every
// book from its snapshot + events (BookReplica throws on any gap or event
// that doesn't fit), and at the end every replica equals the real book.
TEST_CASE("late joiners under load rebuild every book exactly", "[publisher][concurrency]") {
    constexpr SymbolId kSymbols = 4;
    constexpr int kProducers = 4;
    constexpr int kOrdersPerProducer = 1500;

    BlockingQueue<Output> outbox;
    std::vector<std::unique_ptr<Worker>> workers;
    for (int w = 0; w < 2; ++w)
        workers.push_back(std::make_unique<Worker>(outbox));
    std::map<SymbolId, Worker *> owner;
    std::vector<SymbolId> symbols;
    for (SymbolId s = 1; s <= kSymbols; ++s) {
        Worker *w = workers[s % 2].get();
        w->add_book(s, std::make_unique<engine::Book>(100000, 2000));
        owner[s] = w;
        symbols.push_back(s);
    }
    for (auto &w : workers)
        w->start();

    MarketDataPublisher pub(outbox, symbols, [&](SymbolId s, SubscriberId id) {
        owner.at(s)->submit(Stamped{.seq = 0, .ts = {}, .command = TakeSnapshot{s, id}});
    });
    pub.start();

    // A reader thread per subscriber: rebuilds books, counts replies.
    struct Reader {
        std::shared_ptr<Subscription> sub;
        std::map<SymbolId, test_support::BookReplica> books;
        std::map<SymbolId, engine::SequenceNumber> started_at; // snapshot as_of per symbol
        std::atomic<int> replies{0};
        std::string error; // first problem found, if any
        std::jthread thread;
    };
    auto start_reader = [](Reader &r) {
        r.thread = std::jthread([&r] {
            while (true) {
                auto item = r.sub->next_for(50ms);
                if (!item) {
                    if (r.sub->ended())
                        return;
                    continue;
                }
                try {
                    if (auto *snap = std::get_if<SnapshotReady>(&*item)) {
                        if (r.books.contains(snap->symbol))
                            throw std::runtime_error("second snapshot for a symbol");
                        r.books[snap->symbol].load(snap->snapshot);
                        r.started_at[snap->symbol] = snap->snapshot.as_of_sequence;
                    } else if (auto *m = std::get_if<MarketEvent>(&*item)) {
                        const auto book = r.books.find(m->symbol);
                        if (book == r.books.end())
                            throw std::runtime_error("event before its snapshot");
                        book->second.apply(m->event);
                    } else {
                        ++r.replies;
                    }
                } catch (const std::exception &e) {
                    if (r.error.empty())
                        r.error = e.what();
                }
            }
        });
    };

    // Gateways that send orders subscribe before any order, so they must get
    // a reply to every request.
    std::vector<std::unique_ptr<Reader>> readers;
    for (int p = 0; p < kProducers; ++p) {
        auto r = std::make_unique<Reader>();
        auto result = pub.subscribe("gw-" + std::to_string(p), {});
        REQUIRE(result.error == SubscribeError::None);
        r->sub = result.subscription;
        start_reader(*r);
        readers.push_back(std::move(r));
    }

    std::atomic<int> joined{0};
    {
        std::vector<std::jthread> producers;
        for (int p = 0; p < kProducers; ++p)
            producers.emplace_back([&, p] {
                std::mt19937_64 rng(1000 + p);
                auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
                std::vector<std::pair<SymbolId, engine::OrderId>> mine;
                for (int i = 0; i < kOrdersPerProducer; ++i) {
                    const SymbolId s = static_cast<SymbolId>(pick(1, kSymbols));
                    const auto id = static_cast<engine::OrderId>(p) * 1'000'000 + i + 1;
                    const engine::OwnerId who = static_cast<engine::OwnerId>(p * 10 + pick(1, 3));
                    Command c;
                    const int op = pick(0, 9);
                    if (op <= 6 || mine.empty()) {
                        engine::Order o{};
                        o.id = id;
                        o.owner_id = who;
                        o.side = pick(0, 1) ? engine::Side::Buy : engine::Side::Sell;
                        o.type = op == 6 ? engine::OrderType::Market : engine::OrderType::Limit;
                        if (o.type == engine::OrderType::Limit)
                            o.price = pick(95, 105);
                        o.quantity = pick(1, 10);
                        c = NewOrder{static_cast<RequestId>(i), s, o};
                        if (o.type == engine::OrderType::Limit)
                            mine.emplace_back(s, id);
                    } else {
                        const auto [sym, oid] = mine[pick(0, static_cast<int>(mine.size()) - 1)];
                        // Ownership is checked by the engine; the requester may be wrong
                        // (owner chosen at random), which only produces UnknownOrder.
                        c = CancelOrder{static_cast<RequestId>(i), sym, who, oid};
                    }
                    const SymbolId target = std::visit(
                        [](auto &&a) -> SymbolId {
                            if constexpr (requires { a.symbol; })
                                return a.symbol;
                            else
                                return 0;
                        },
                        c);
                    const Seq seq = seq_of(c);
                    owner.at(target)->submit(Stamped{.seq = seq,
                                                     .ts = i,
                                                     .command = std::move(c),
                                                     .gateway_id = "gw-" + std::to_string(p)});
                }
            });
        // Late joiners subscribe while orders are flowing.
        for (int j = 0; j < 4; ++j) {
            std::this_thread::sleep_for(2ms);
            auto r = std::make_unique<Reader>();
            auto result = pub.subscribe("late-" + std::to_string(j), {});
            REQUIRE(result.error == SubscribeError::None);
            r->sub = result.subscription;
            start_reader(*r);
            readers.push_back(std::move(r));
            ++joined;
        }
    } // producers joined: every order is in some worker's inbox

    // A final subscriber's snapshots are queued behind every order, so they
    // show each book's final state.
    auto checker = pub.subscribe("checker", {});
    REQUIRE(checker.error == SubscribeError::None);
    std::map<SymbolId, engine::BookSnapshot> final_books;
    while (final_books.size() < kSymbols) {
        auto item = checker.subscription->next_for(5s);
        REQUIRE(item.has_value());
        if (auto *snap = std::get_if<SnapshotReady>(&*item))
            final_books[snap->symbol] = snap->snapshot;
    }

    // Stop in the engine_node order: workers, then the outbox pill.
    for (auto &w : workers)
        w->stop();
    outbox.push(OutboxClosed{});
    pub.join();
    for (auto &r : readers)
        r->thread.join();

    CHECK(joined == 4);
    // At least one late joiner really joined mid-stream: its snapshot was
    // neither empty nor already final, so it had to splice in live events.
    bool joined_mid_stream = false;
    for (std::size_t i = kProducers; i < readers.size(); ++i)
        for (const auto &[symbol, as_of] : readers[i]->started_at)
            joined_mid_stream |= as_of > 0 && as_of < final_books.at(symbol).as_of_sequence;
    CHECK(joined_mid_stream);
    for (std::size_t i = 0; i < readers.size(); ++i) {
        const Reader &r = *readers[i];
        INFO("reader " << i << " (" << r.sub->gateway_id() << ")");
        CHECK(r.error.empty());
        CHECK(r.sub->end_reason() == EndReason::Shutdown);
        REQUIRE(r.books.size() == kSymbols);
        for (const auto &[symbol, replica] : r.books) {
            INFO("symbol " << symbol);
            auto got = replica.to_snapshot();
            auto want = final_books.at(symbol);
            test_support::BookReplica::sort_stops(want.stops);
            CHECK(got.as_of_sequence == want.as_of_sequence);
            CHECK(got.bids == want.bids);
            CHECK(got.asks == want.asks);
            CHECK(got.last_trade_price == want.last_trade_price);
            CHECK(got.halted == want.halted);
        }
        if (i < kProducers)
            CHECK(r.replies == kOrdersPerProducer); // exactly one reply per request
        else
            CHECK(r.replies == 0); // late joiners sent nothing
    }
    CHECK(pub.stats().slow_consumer_drops == 0);
}
