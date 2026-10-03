// Tests for server::SequencerFeed: one sequencer partition streamed into one
// Worker.
//
// The feed talks to a FakeSequencer defined below: a real gRPC server whose
// Subscribe follows a script ("send seq 1 and 3, then hang up with
// UNAVAILABLE"), so the tests can create gaps, junk payloads, disconnects and
// every error code on purpose. The real sequencer lives in its own repo; its
// behaviour is pinned by its own contract tests.
//
// What reaches the Worker is observed through the Worker's outbox: every
// command produces exactly one Reply (with its request_id) plus engine events
// (whose ts is the time the Book was given).

#include "server/codec.hpp"
#include "server/sequencer_feed.hpp"
#include "server/worker.hpp"

#include "exchange/v1/sequencer.grpc.pb.h"
#include <grpcpp/grpcpp.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace server;
using namespace std::chrono_literals;
namespace pb = exchange::v1;

namespace {

constexpr SymbolId MOOG = 1;
constexpr SymbolId BANANA = 2;
constexpr SymbolId NOT_OURS = 99;

// ------------------------------------------------------------------ the fake

// One Subscribe call's script: send these commands, then either keep the
// stream open until the client hangs up, or end it with `end`.
struct Plan {
  std::vector<pb::SequencedCommand> send;
  bool hold_open = true;
  grpc::Status end = grpc::Status::OK;
};

Plan hold(std::vector<pb::SequencedCommand> send = {}) {
  return Plan{std::move(send), true, grpc::Status::OK};
}

Plan end_with(grpc::StatusCode code,
              std::vector<pb::SequencedCommand> send = {}) {
  return Plan{std::move(send), false, grpc::Status(code, "scripted")};
}

class FakeSequencer final : public pb::Sequencer::Service {
public:
  // Scripts are used one per Subscribe call, in order. When they run out,
  // `fallback` is used (default: hold the stream open, send nothing).
  void script(std::vector<Plan> plans,
              std::optional<Plan> fallback = std::nullopt) {
    std::lock_guard lock(mu_);
    plans_.assign(plans.begin(), plans.end());
    fallback_ = std::move(fallback);
  }

  grpc::Status
  Subscribe(grpc::ServerContext *ctx, const pb::SubscribeRequest *req,
            grpc::ServerWriter<pb::SequencedCommand> *writer) override {
    Plan plan;
    {
      std::lock_guard lock(mu_);
      requests_.push_back(*req);
      if (!plans_.empty()) {
        plan = std::move(plans_.front());
        plans_.pop_front();
      } else if (fallback_) {
        plan = *fallback_;
      }
    }
    cv_.notify_all();

    for (const auto &command : plan.send)
      if (!writer->Write(command))
        return grpc::Status::CANCELLED;
    if (!plan.hold_open)
      return plan.end;

    while (!ctx->IsCancelled() && !closing_)
      std::this_thread::sleep_for(2ms);
    if (ctx->IsCancelled()) {
      {
        std::lock_guard lock(mu_);
        ++hang_ups_;
      }
      cv_.notify_all();
    }
    return grpc::Status::CANCELLED;
  }

  std::vector<pb::SubscribeRequest> requests() const {
    std::lock_guard lock(mu_);
    return requests_;
  }

  // Waits until at least n Subscribe calls have arrived.
  bool wait_for_requests(std::size_t n,
                         std::chrono::milliseconds timeout = 3s) {
    std::unique_lock lock(mu_);
    return cv_.wait_for(lock, timeout, [&] { return requests_.size() >= n; });
  }

  // Waits until a held-open stream sees the client hang up.
  bool wait_for_hang_up(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mu_);
    return cv_.wait_for(lock, timeout, [&] { return hang_ups_ > 0; });
  }

  void close() { closing_ = true; }

private:
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Plan> plans_;
  std::optional<Plan> fallback_;
  std::vector<pb::SubscribeRequest> requests_;
  int hang_ups_ = 0;
  std::atomic<bool> closing_{false};
};

class FakeServer {
public:
  FakeServer() {
    int port = 0;
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(),
                             &port);
    builder.RegisterService(&fake);
    server_ = builder.BuildAndStart();
    if (!server_ || port == 0)
      throw std::runtime_error("fake sequencer failed to start");
    address = "127.0.0.1:" + std::to_string(port);
  }
  ~FakeServer() {
    fake.close();
    server_->Shutdown(std::chrono::system_clock::now() + 1s);
    server_->Wait();
  }
  FakeServer(const FakeServer &) = delete;
  FakeServer &operator=(const FakeServer &) = delete;

  FakeSequencer fake; // declared before server_: outlives it
  std::string address;

private:
  std::unique_ptr<grpc::Server> server_;
};

// ------------------------------------------------------------- the commands

engine::Order buy(engine::OrderId id, engine::Price price = 100) {
  engine::Order o{};
  o.id = id;
  o.owner_id = 7;
  o.side = engine::Side::Buy;
  o.type = engine::OrderType::Limit;
  o.price = price;
  o.quantity = 1;
  return o;
}

// A sequenced NewOrder whose request_id and order id both equal `request`.
pb::SequencedCommand order(std::uint64_t seq, std::int64_t ts,
                           std::uint64_t request, SymbolId symbol = MOOG) {
  pb::SequencedCommand c;
  c.set_seq(seq);
  c.set_ts(ts);
  c.set_symbol(symbol);
  c.set_request_id(request);
  c.set_gateway_id("gw-1");
  c.set_payload(encode_body(NewOrder{request, symbol, buy(request)}));
  return c;
}

pb::SequencedCommand junk(std::uint64_t seq, std::uint64_t request) {
  pb::SequencedCommand c = order(seq, 0, request);
  c.set_payload(""); // decodes as an empty command body: rejected by the codec
  return c;
}

// --------------------------------------------------------- the engine side

struct Seen {
  std::vector<Reply> replies;
  std::vector<MarketEvent> events;
};

// Collects outbox items until `n` replies have arrived (or the timeout passes).
Seen wait_for_replies(BlockingQueue<Output> &outbox, std::size_t n,
                      std::chrono::milliseconds timeout = 3s) {
  Seen seen;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (seen.replies.size() < n &&
         std::chrono::steady_clock::now() < deadline) {
    auto item = outbox.try_pop();
    if (!item) {
      std::this_thread::sleep_for(1ms);
      continue;
    }
    if (auto *reply = std::get_if<Reply>(&*item))
      seen.replies.push_back(*reply);
    else if (auto *event = std::get_if<MarketEvent>(&*item))
      seen.events.push_back(*event);
  }
  return seen;
}

bool eventually(const std::function<bool()> &condition,
                std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition())
      return true;
    std::this_thread::sleep_for(2ms);
  }
  return condition();
}

// A Worker that owns MOOG and BANANA (not NOT_OURS), plus the feed options
// every test starts from. Destruction order: feed (declared last by the test),
// then worker, then outbox, then the fake server.
struct Rig {
  FakeServer server;
  BlockingQueue<Output> outbox;
  Worker worker{outbox};
  std::vector<std::string> log_lines;
  std::mutex log_mu;
  std::atomic<int> fatal_calls{0};

  Rig() {
    worker.add_book(MOOG, std::make_unique<engine::Book>());
    worker.add_book(BANANA, std::make_unique<engine::Book>());
    worker.start();
  }

  FeedOptions options() {
    FeedOptions o;
    o.address = server.address;
    o.partition = 0;
    o.retry_delay = 20ms;
    o.log = [this](const std::string &line) {
      std::lock_guard lock(log_mu);
      log_lines.push_back(line);
    };
    o.on_fatal = [this](const std::string &) { ++fatal_calls; };
    return o;
  }

  bool logged(const std::string &needle) {
    std::lock_guard lock(log_mu);
    for (const auto &line : log_lines)
      if (line.find(needle) != std::string::npos)
        return true;
    return false;
  }
};

} // namespace

// ------------------------------------------------------------------- normal
// flow

TEST_CASE("feed: commands reach the worker in order, with the sequencer's ts",
          "[feed]") {
  Rig rig;
  rig.server.fake.script(
      {hold({order(1, 1000, 11), order(2, 1005, 12), order(3, 1010, 13)})});
  SequencerFeed feed(rig.worker, rig.options());
  feed.start();

  const auto seen = wait_for_replies(rig.outbox, 3);
  REQUIRE(seen.replies.size() == 3);
  CHECK(seen.replies[0].request_id == 11);
  CHECK(seen.replies[1].request_id == 12);
  CHECK(seen.replies[2].request_id == 13);

  std::vector<engine::Timestamp> accepted_ts;
  for (const auto &m : seen.events)
    if (m.event.kind == engine::EventKind::Accepted)
      accepted_ts.push_back(m.event.ts);
  CHECK(accepted_ts ==
        std::vector<engine::Timestamp>{
            1000, 1005, 1010}); // the sequencer's time, not a clock

  CHECK(eventually([&] { return feed.last_seq() == 3; }));
  CHECK_FALSE(feed.failed());

  const auto requests = rig.server.fake.requests();
  REQUIRE(requests.size() == 1);
  CHECK(requests[0].partition() == 0);
  CHECK(requests[0].from_seq() == 1); // fresh engine: everything from the start
}

TEST_CASE("feed: subscribes from options.from_seq and expects that seq first",
          "[feed]") {
  Rig rig;
  rig.server.fake.script({hold({order(5, 1, 5), order(6, 2, 6)})});
  auto options = rig.options();
  options.from_seq = 5;
  SequencerFeed feed(rig.worker, options);
  CHECK(feed.last_seq() == 4); // "dealt with everything before 5"
  feed.start();

  CHECK(wait_for_replies(rig.outbox, 2).replies.size() == 2);
  CHECK(eventually([&] { return feed.last_seq() == 6; }));
  CHECK(rig.server.fake.requests().at(0).from_seq() == 5);
  CHECK_FALSE(feed.failed());
}

TEST_CASE("feed: the partition number is passed to the sequencer", "[feed]") {
  Rig rig;
  rig.server.fake.script({hold({order(1, 1, 1)})});
  auto options = rig.options();
  options.partition = 3;
  SequencerFeed feed(rig.worker, options);
  feed.start();
  REQUIRE(rig.server.fake.wait_for_requests(1));
  CHECK(rig.server.fake.requests()[0].partition() == 3);
}

// ---------------------------------------------------------------- fatal: data

TEST_CASE("feed: a gap stops the feed for good and nothing after it reaches "
          "the worker",
          "[feed]") {
  Rig rig;
  rig.server.fake.script(
      {hold({order(1, 1, 1), order(3, 3, 3)})}); // seq 2 never arrives
  SequencerFeed feed(rig.worker, rig.options());
  feed.start();

  REQUIRE(eventually([&] { return feed.failed(); }));
  CHECK(feed.failure().find("gap, expected seq 2 but got 3") !=
        std::string::npos);
  CHECK(feed.last_seq() == 1);
  CHECK(rig.fatal_calls == 1);

  const auto seen = wait_for_replies(rig.outbox, 2, 300ms);
  REQUIRE(seen.replies.size() == 1); // seq 1 only; seq 3 was never handed over
  CHECK(seen.replies[0].request_id == 1);

  std::this_thread::sleep_for(200ms);            // 10 retry delays
  CHECK(rig.server.fake.requests().size() == 1); // a gap is never retried
}

TEST_CASE("feed: a symbol this worker does not own stops the feed", "[feed]") {
  Rig rig;
  rig.server.fake.script({hold({order(1, 1, 1, NOT_OURS)})});
  SequencerFeed feed(rig.worker, rig.options());
  feed.start();

  REQUIRE(eventually([&] { return feed.failed(); }));
  CHECK(feed.failure().find("symbol 99 is not owned") != std::string::npos);
  CHECK(feed.last_seq() == 0);
  CHECK(wait_for_replies(rig.outbox, 1, 300ms)
            .replies.empty()); // never handed to the worker
}

TEST_CASE("feed: a fatal error hangs up the stream by itself, without stop()",
          "[feed]") {
  // A sequencer stream never ends on its own. If the feed broke out of its
  // read loop and called Finish() without cancelling, it would sit there
  // holding the partition until someone called stop().
  Rig rig;
  rig.server.fake.script({hold({order(1, 1, 1), order(2, 2, 2, NOT_OURS)})});
  SequencerFeed feed(rig.worker, rig.options());
  feed.start();

  REQUIRE(eventually([&] { return feed.failed(); }));
  CHECK(rig.server.fake.wait_for_hang_up(2s));
}

TEST_CASE("feed: on_fatal may call stop() from the feed thread", "[feed]") {
  Rig rig;
  rig.server.fake.script({hold({order(1, 1, NOT_OURS, NOT_OURS)})});
  std::unique_ptr<SequencerFeed> feed;
  std::atomic<bool> called{false};
  auto options = rig.options();
  options.on_fatal = [&](const std::string &) {
    feed->stop(); // runs on the feed thread: must not try to join itself
    called = true;
  };
  feed = std::make_unique<SequencerFeed>(rig.worker, options);
  feed->start();

  REQUIRE(eventually([&] { return called.load(); }));
  CHECK(feed->failed());
  feed.reset(); // the destructor joins, from this thread
}

// ------------------------------------------------------------ skip, still
// count

TEST_CASE("feed: a junk payload is skipped but still counts as dealt with",
          "[feed]") {
  Rig rig;
  rig.server.fake.script({hold({order(1, 1, 1), junk(2, 2), order(3, 3, 3)})});
  SequencerFeed feed(rig.worker, rig.options());
  feed.start();

  const auto seen = wait_for_replies(rig.outbox, 2);
  REQUIRE(seen.replies.size() == 2);
  CHECK(seen.replies[0].request_id == 1);
  CHECK(seen.replies[1].request_id == 3); // seq 3 was accepted: no false "gap"

  CHECK(eventually([&] { return feed.last_seq() == 3; }));
  CHECK_FALSE(feed.failed());
  CHECK(rig.logged("skipping seq 2 from gateway gw-1 request 2"));
}

// ------------------------------------------------------------ reconnect rules

TEST_CASE("feed: after a disconnect it reconnects from the next seq",
          "[feed]") {
  Rig rig;
  rig.server.fake.script({end_with(grpc::StatusCode::UNAVAILABLE,
                                   {order(1, 1, 1), order(2, 2, 2)}),
                          hold({order(3, 3, 3)})});
  SequencerFeed feed(rig.worker, rig.options());
  feed.start();

  const auto seen = wait_for_replies(rig.outbox, 3);
  REQUIRE(seen.replies.size() == 3);
  CHECK(seen.replies[2].request_id == 3);
  CHECK(eventually([&] { return feed.last_seq() == 3; }));
  CHECK_FALSE(feed.failed());

  const auto requests = rig.server.fake.requests();
  REQUIRE(requests.size() == 2);
  CHECK(requests[0].from_seq() == 1);
  CHECK(requests[1].from_seq() ==
        3); // resumes right after the last seq dealt with
}

TEST_CASE("feed: transient errors are retried until the sequencer comes back",
          "[feed]") {
  Rig rig;
  rig.server.fake.script({end_with(grpc::StatusCode::UNAVAILABLE),
                          end_with(grpc::StatusCode::UNKNOWN),
                          end_with(grpc::StatusCode::DEADLINE_EXCEEDED),
                          hold({order(1, 1, 1)})});
  SequencerFeed feed(rig.worker, rig.options());
  feed.start();

  CHECK(wait_for_replies(rig.outbox, 1).replies.size() == 1);
  CHECK_FALSE(feed.failed());
  CHECK(rig.server.fake.requests().size() == 4);
}

TEST_CASE("feed: OUT_OF_RANGE is fatal (the relay cannot replay)", "[feed]") {
  Rig rig;
  rig.server.fake.script({end_with(grpc::StatusCode::OUT_OF_RANGE)});
  SequencerFeed feed(rig.worker, rig.options());
  feed.start();

  REQUIRE(eventually([&] { return feed.failed(); }));
  CHECK(feed.failure().find("cannot serve from seq 1") != std::string::npos);
  CHECK(rig.fatal_calls == 1);
  std::this_thread::sleep_for(200ms);
  CHECK(rig.server.fake.requests().size() == 1); // not retried
}

TEST_CASE("feed: NOT_FOUND and INVALID_ARGUMENT are configuration errors, not "
          "retried",
          "[feed]") {
  for (auto code :
       {grpc::StatusCode::NOT_FOUND, grpc::StatusCode::INVALID_ARGUMENT}) {
    Rig rig;
    rig.server.fake.script({end_with(code)});
    SequencerFeed feed(rig.worker, rig.options());
    feed.start();

    REQUIRE(eventually([&] { return feed.failed(); }));
    CHECK(feed.failure().find("configuration error") != std::string::npos);
    std::this_thread::sleep_for(200ms);
    CHECK(rig.server.fake.requests().size() == 1);
  }
}

TEST_CASE("feed: ALREADY_EXISTS is retried (usually our own old stream), then "
          "it connects",
          "[feed]") {
  Rig rig;
  rig.server.fake.script({end_with(grpc::StatusCode::ALREADY_EXISTS),
                          end_with(grpc::StatusCode::ALREADY_EXISTS),
                          end_with(grpc::StatusCode::ALREADY_EXISTS),
                          hold({order(1, 1, 1)})});
  auto options = rig.options();
  options.max_already_exists_retries = 3;
  SequencerFeed feed(rig.worker, options);
  feed.start();

  CHECK(wait_for_replies(rig.outbox, 1).replies.size() == 1);
  CHECK_FALSE(feed.failed());
  CHECK(rig.server.fake.requests().size() == 4);
}

TEST_CASE(
    "feed: ALREADY_EXISTS that never clears means a second engine: give up",
    "[feed]") {
  Rig rig;
  rig.server.fake.script({}, end_with(grpc::StatusCode::ALREADY_EXISTS));
  auto options = rig.options();
  options.max_already_exists_retries = 3;
  SequencerFeed feed(rig.worker, options);
  feed.start();

  REQUIRE(eventually([&] { return feed.failed(); }));
  CHECK(feed.failure().find("another subscriber holds partition 0") !=
        std::string::npos);
  std::this_thread::sleep_for(200ms);
  CHECK(rig.server.fake.requests().size() == 4); // first try + 3 retries
}

TEST_CASE("feed: a working connection between refusals resets the "
          "ALREADY_EXISTS count",
          "[feed]") {
  // 3 refusals, a working stream that drops, then 3 more refusals: never 4 in a
  // row.
  Rig rig;
  rig.server.fake.script(
      {end_with(grpc::StatusCode::ALREADY_EXISTS),
       end_with(grpc::StatusCode::ALREADY_EXISTS),
       end_with(grpc::StatusCode::ALREADY_EXISTS),
       end_with(grpc::StatusCode::UNAVAILABLE, {order(1, 1, 1)}),
       end_with(grpc::StatusCode::ALREADY_EXISTS),
       end_with(grpc::StatusCode::ALREADY_EXISTS),
       end_with(grpc::StatusCode::ALREADY_EXISTS), hold({order(2, 2, 2)})});
  auto options = rig.options();
  options.max_already_exists_retries = 3;
  SequencerFeed feed(rig.worker, options);
  feed.start();

  CHECK(wait_for_replies(rig.outbox, 2).replies.size() == 2);
  CHECK_FALSE(feed.failed());
}

// ------------------------------------------------------------------ stopping

TEST_CASE("feed: stop() ends an open stream promptly", "[feed]") {
  Rig rig;
  rig.server.fake.script({hold({order(1, 1, 1)})});
  SequencerFeed feed(rig.worker, rig.options());
  feed.start();
  REQUIRE(wait_for_replies(rig.outbox, 1).replies.size() == 1);

  const auto start = std::chrono::steady_clock::now();
  feed.stop();
  CHECK(std::chrono::steady_clock::now() - start < 1s);
  CHECK_FALSE(feed.failed()); // stopping is not a failure
  CHECK(rig.fatal_calls == 0);
  feed.stop(); // idempotent
}

TEST_CASE("feed: stop() wakes a retry wait instead of sleeping it out",
          "[feed]") {
  Rig rig;
  rig.server.fake.script({}, end_with(grpc::StatusCode::UNAVAILABLE));
  auto options = rig.options();
  options.retry_delay = 30s;
  SequencerFeed feed(rig.worker, options);
  feed.start();
  REQUIRE(rig.server.fake.wait_for_requests(1));
  std::this_thread::sleep_for(50ms); // now inside the 30 s wait

  const auto start = std::chrono::steady_clock::now();
  feed.stop();
  CHECK(std::chrono::steady_clock::now() - start < 1s);
}

TEST_CASE(
    "feed: a sequencer that is not running yet is retried until it appears",
    "[feed]") {
  // Nothing listens on this address at first: every attempt fails with
  // UNAVAILABLE.
  std::string address;
  {
    FakeServer probe; // grab a free port, then let it go
    address = probe.address;
  }
  BlockingQueue<Output> outbox;
  Worker worker(outbox);
  worker.add_book(MOOG, std::make_unique<engine::Book>());
  worker.start();
  FeedOptions options;
  options.address = address;
  options.retry_delay = 20ms;
  options.log = [](const std::string &) {};
  SequencerFeed feed(worker, options);
  feed.start();

  std::this_thread::sleep_for(200ms);
  CHECK_FALSE(feed.failed()); // down is not fatal: keep trying
  CHECK(feed.last_seq() == 0);
  feed.stop();
}

// ------------------------------------------------------------- construction

TEST_CASE("feed: bad options are rejected at construction; start() only once",
          "[feed]") {
  BlockingQueue<Output> outbox;
  Worker worker(outbox);

  FeedOptions no_address;
  CHECK_THROWS_AS(SequencerFeed(worker, no_address), std::invalid_argument);

  FeedOptions zero_seq;
  zero_seq.address = "127.0.0.1:1";
  zero_seq.from_seq = 0;
  CHECK_THROWS_AS(SequencerFeed(worker, zero_seq), std::invalid_argument);

  FeedOptions ok;
  ok.address = "127.0.0.1:1";
  ok.retry_delay = 20ms;
  ok.log = [](const std::string &) {};
  SequencerFeed feed(worker, ok);
  feed.stop(); // before start: harmless
  SequencerFeed second(worker, ok);
  second.start();
  CHECK_THROWS_AS(second.start(), std::logic_error);
}

TEST_CASE("worker: owns() answers for added books only", "[worker][feed]") {
  BlockingQueue<Output> outbox;
  Worker worker(outbox);
  worker.add_book(MOOG, std::make_unique<engine::Book>());
  worker.start();
  CHECK(worker.owns(MOOG));
  CHECK_FALSE(worker.owns(BANANA));
}
