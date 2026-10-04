// EngineFeedServer over real gRPC on localhost: a test client subscribes the
// way a gateway would.
#include "server/engine_feed_server.hpp"
#include "server/publisher.hpp"
#include "server/worker.hpp"

#include "exchange/v1/engine_feed.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <thread>

using namespace server;
using namespace std::chrono_literals;
namespace pb = exchange::v1;

namespace {

// A worker with one or two books, a publisher and a feed server on a free port.
struct Rig {
    BlockingQueue<Output> outbox;
    Worker worker{outbox};
    std::unique_ptr<MarketDataPublisher> publisher;
    std::unique_ptr<EngineFeedServer> server;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<pb::EngineFeed::Stub> stub;
    bool closed = false;

    explicit Rig(PublisherOptions options = {}) {
        worker.add_book(1, std::make_unique<engine::Book>());
        worker.add_book(2, std::make_unique<engine::Book>());
        worker.start();
        publisher = std::make_unique<MarketDataPublisher>(
            outbox, std::vector<SymbolId>{1, 2},
            [this](SymbolId s, SubscriberId id) {
                worker.submit(Stamped{.seq = 0, .ts = {}, .command = TakeSnapshot{s, id}});
            },
            std::move(options));
        publisher->start();
        server = std::make_unique<EngineFeedServer>(
            *publisher, FeedServerOptions{.address = "127.0.0.1:0", .poll = 20ms, .shutdown_grace = 500ms});
        server->start();
        channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(server->port()),
                                      grpc::InsecureChannelCredentials());
        stub = pb::EngineFeed::NewStub(channel);
    }

    // The engine_node shutdown order.
    void close() {
        if (closed)
            return;
        closed = true;
        worker.stop();
        outbox.push(OutboxClosed{});
        publisher->join();
        server->stop();
    }
    ~Rig() { close(); }

    void order(RequestId request, const std::string &gateway, engine::OrderId id, engine::OwnerId owner,
               engine::Side side, engine::Price price, engine::Quantity qty, SymbolId symbol = 1) {
        engine::Order o{};
        o.id = id;
        o.owner_id = owner;
        o.side = side;
        o.type = engine::OrderType::Limit;
        o.price = price;
        o.quantity = qty;
        worker.submit(Stamped{.seq = 0, .ts = 1, .command = NewOrder{request, symbol, o}, .gateway_id = gateway});
    }
};

struct Stream {
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReader<pb::FeedMessage>> reader;

    Stream(pb::EngineFeed::Stub &stub, const std::string &gateway, std::vector<std::uint32_t> symbols = {}) {
        pb::FeedSubscribeRequest request;
        request.set_gateway_id(gateway);
        for (auto s : symbols)
            request.add_symbols(s);
        context.set_deadline(std::chrono::system_clock::now() + 20s);
        reader = stub.Subscribe(&context, request);
    }
    pb::FeedMessage read() {
        pb::FeedMessage msg;
        REQUIRE(reader->Read(&msg));
        return msg;
    }
    grpc::Status finish() { return reader->Finish(); }
};

// Reads until the stream ends and returns its final status.
grpc::Status drain_to_end(Stream &stream) {
    pb::FeedMessage msg;
    while (stream.reader->Read(&msg)) {
    }
    return stream.finish();
}

} // namespace

TEST_CASE("A gateway gets a snapshot, then events, then its reply", "[engine_feed]") {
    Rig rig;
    rig.order(1, "other", 100, 9, engine::Side::Sell, 101, 10); // before the subscribe
    Stream stream(*rig.stub, "gw", {1});

    const pb::FeedMessage first = stream.read();
    REQUIRE(first.has_snapshot());
    const pb::BookSnapshot &snap = first.snapshot();
    CHECK(snap.symbol() == 1);
    REQUIRE(snap.asks_size() == 1);
    CHECK(snap.asks(0).order_id() == 100);
    CHECK(snap.asks(0).quantity() == 10);

    rig.order(7, "gw", 200, 4, engine::Side::Buy, 101, 3);
    // Accepted, Trade, then our reply (the buy filled completely).
    const auto accepted = stream.read();
    REQUIRE(accepted.has_event());
    CHECK(accepted.event().kind() == pb::EVENT_KIND_ACCEPTED);
    CHECK(accepted.event().seq() == snap.as_of_seq() + 1);

    const auto trade = stream.read();
    REQUIRE(trade.has_event());
    CHECK(trade.event().kind() == pb::EVENT_KIND_TRADE);
    CHECK(trade.event().seq() == snap.as_of_seq() + 2);
    CHECK(trade.event().owner_id() == 4);    // buyer
    CHECK(trade.event().other_owner() == 9); // seller
    CHECK(trade.event().price() == 101);
    CHECK(trade.event().quantity() == 3);

    const auto reply = stream.read();
    REQUIRE(reply.has_reply());
    CHECK(reply.reply().gateway_id() == "gw");
    CHECK(reply.reply().request_id() == 7);
    CHECK(reply.reply().reject_reason() == pb::REJECT_REASON_NONE);

    rig.close();
    CHECK(drain_to_end(stream).error_code() == grpc::StatusCode::UNAVAILABLE);
}

TEST_CASE("Only the asked-for symbols are streamed", "[engine_feed]") {
    Rig rig;
    Stream stream(*rig.stub, "gw", {2});
    REQUIRE(stream.read().snapshot().symbol() == 2);
    rig.order(1, "x", 1, 1, engine::Side::Buy, 100, 1, /*symbol=*/1);
    rig.order(2, "x", 2, 1, engine::Side::Buy, 100, 1, /*symbol=*/2);
    const auto msg = stream.read();
    REQUIRE(msg.has_event());
    CHECK(msg.event().symbol() == 2);
}

TEST_CASE("Bad subscribe requests get clear status codes", "[engine_feed]") {
    Rig rig;
    {
        Stream s(*rig.stub, "");
        CHECK(drain_to_end(s).error_code() == grpc::StatusCode::INVALID_ARGUMENT);
    }
    {
        Stream s(*rig.stub, "gw", {1, 99});
        CHECK(drain_to_end(s).error_code() == grpc::StatusCode::NOT_FOUND);
    }
    {
        Stream held(*rig.stub, "gw");
        REQUIRE(held.read().has_snapshot()); // the first stream is open
        Stream second(*rig.stub, "gw");
        CHECK(drain_to_end(second).error_code() == grpc::StatusCode::ALREADY_EXISTS);
    }
}

TEST_CASE("When a gateway goes away, its id is freed for the next stream", "[engine_feed]") {
    Rig rig;
    {
        Stream first(*rig.stub, "gw");
        REQUIRE(first.read().has_snapshot());
        first.context.TryCancel();
        drain_to_end(first);
    }
    // The handler notices within one poll interval; retry until it has.
    bool resubscribed = false;
    for (int attempt = 0; attempt < 100 && !resubscribed; ++attempt) {
        Stream again(*rig.stub, "gw");
        pb::FeedMessage msg;
        if (again.reader->Read(&msg)) {
            resubscribed = msg.has_snapshot();
            again.context.TryCancel();
        }
        drain_to_end(again);
        if (!resubscribed)
            std::this_thread::sleep_for(10ms);
    }
    CHECK(resubscribed);
}

TEST_CASE("Subscribing after shutdown is refused", "[engine_feed]") {
    Rig rig;
    rig.worker.stop();
    rig.outbox.push(OutboxClosed{});
    rig.publisher->join();
    Stream s(*rig.stub, "gw");
    CHECK(drain_to_end(s).error_code() == grpc::StatusCode::UNAVAILABLE);
}

TEST_CASE("A gateway that stops reading is dropped with RESOURCE_EXHAUSTED", "[engine_feed]") {
    Rig rig(PublisherOptions{.queue_capacity = 8});
    Stream slow(*rig.stub, "slow", {1});
    REQUIRE(slow.read().has_snapshot());

    // Flood events without reading. gRPC buffers some, the handler blocks in
    // Write, the 8-item queue fills, and the publisher drops the subscriber.
    std::atomic<bool> stop{false};
    std::jthread flood([&] {
        engine::OrderId id = 1;
        while (!stop && id < 2'000'000) {
            rig.order(id, "flood", id, 1, engine::Side::Buy, 100, 1);
            ++id;
            if (id % 1000 == 0)
                std::this_thread::sleep_for(1ms);
        }
    });
    for (int i = 0; i < 1000 && rig.publisher->stats().slow_consumer_drops == 0; ++i)
        std::this_thread::sleep_for(10ms);
    stop = true;
    flood.join();
    REQUIRE(rig.publisher->stats().slow_consumer_drops == 1);

    // Reading again unblocks the handler, which then reports why it ended.
    CHECK(drain_to_end(slow).error_code() == grpc::StatusCode::RESOURCE_EXHAUSTED);
}
