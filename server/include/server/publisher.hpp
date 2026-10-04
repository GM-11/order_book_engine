#pragma once
#include "server/blocking_queue.hpp"
#include "server/commands.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace server {

using FeedItem = std::variant<SnapshotReady, MarketEvent, Reply>;

enum class EndReason {
    None,
    Unsubscribed,
    SlowConsumer,
    Shutdown,
};

enum class SubscribeError {
    None,
    EmptyGatewayId,
    GatewayAlreadySubscribed,
    UnknownSymbol,
    Closed,
};

class Subscription {
  public:
    Subscription(SubscriberId id, std::string gateway_id, std::size_t capacity);
    std::optional<FeedItem> next_for(std::chrono::milliseconds timeout);
    bool ended() const;
    EndReason end_reason() const;
    SubscriberId id() const { return id_; }
    const std::string &gateway_id() const { return gateway_id_; }

  private:
    friend class MarketDataPublisher;
    bool try_push(FeedItem item); // false = queue full; never blocks
    void end(EndReason reason);   // first reason wins

    const SubscriberId id_;
    const std::string gateway_id_;
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<FeedItem> items_;
    EndReason end_reason_ = EndReason::None;
};

struct PublisherOptions {
    std::size_t queue_capacity = 65536; // per subscriber, in items
    std::function<void(const Output &)> tap{};
};

struct PublisherStats {
    std::uint64_t events_routed = 0;   // MarketEvents taken from the outbox
    std::uint64_t replies_dropped = 0; // no subscriber for their gateway
    std::uint64_t slow_consumer_drops = 0;
};

class MarketDataPublisher {
  public:
    using SnapshotRequester = std::function<void(SymbolId, SubscriberId)>;

    MarketDataPublisher(BlockingQueue<Output> &outbox, std::vector<SymbolId> symbols,
                        SnapshotRequester request_snapshot, PublisherOptions options = {});
    ~MarketDataPublisher(); // joins the thread; push OutboxClosed first
    MarketDataPublisher(const MarketDataPublisher &) = delete;
    MarketDataPublisher &operator=(const MarketDataPublisher &) = delete;

    void start();
    void join();

    struct SubscribeResult {
        std::shared_ptr<Subscription> subscription; // set when error == None
        SubscribeError error = SubscribeError::None;
    };
    SubscribeResult subscribe(std::string gateway_id, std::vector<SymbolId> symbols);
    void unsubscribe(SubscriberId id);

    PublisherStats stats() const;

  private:
    struct Subscriber {
        std::shared_ptr<Subscription> sub;
        std::unordered_map<SymbolId, bool> live; // Per symbol: false = waiting for its snapshot, true = live.
    };

    void run();
    void route(Output &out);
    void drop(SubscriberId id, EndReason reason);

    BlockingQueue<Output> &outbox_;
    const std::unordered_set<SymbolId> symbols_;
    const std::vector<SymbolId> symbol_list_;
    const SnapshotRequester request_snapshot_;
    const PublisherOptions options_;

    mutable std::mutex mutex_; // guards everything below
    std::unordered_map<SubscriberId, Subscriber> subscribers_;
    std::unordered_map<std::string, SubscriberId> by_gateway_;
    SubscriberId next_id_ = 1;
    bool closed_ = false;
    PublisherStats stats_;

    bool started_ = false;
    std::jthread thread_; // MUST be the last member
};

} // namespace server
