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

enum class SubscriberKind {
    Market,
    Replies,
};

enum class SubscribeError {
    None,
    EmptyGatewayId,
    GatewayAlreadySubscribed,
    UnknownSymbol,
    Closed,
    BadFromSeq,
    TooOld,
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
    std::size_t replay_window = 100000;
    std::function<void(const Output &)> tap{};
};

struct PublisherStats {
    std::uint64_t events_routed = 0;   // MarketEvents taken from the outbox
    std::uint64_t replies_dropped = 0; // no Replies subscriber for their gateway
    std::uint64_t slow_consumer_drops = 0;
    std::size_t market_subscribers = 0;  // open right now
    std::size_t replies_subscribers = 0; // open right now
    std::uint64_t replayed_events = 0;
    std::uint64_t too_old_refusals = 0;
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
        std::string detail;
    };
    SubscribeResult subscribe(std::string gateway_id, std::vector<SymbolId> symbols,
                              std::unordered_map<SymbolId, Seq> from_seq = {});
    SubscribeResult subscribe_replies(std::string gateway_id);
    void unsubscribe(SubscriberId id);

    PublisherStats stats() const;

  private:
    struct SymbolState {
        bool live = false;  // false = waiting for its snapshot
        Seq skip_below = 0; // drop events with seq < this: a from_seq subscriber asking for a seq not yet produced
    };
    struct Subscriber {
        std::shared_ptr<Subscription> sub;
        SubscriberKind kind = SubscriberKind::Market;
        std::unordered_map<SymbolId, SymbolState> live; // Market only. Per symbol: snapshot/replay state.
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
    std::unordered_map<std::string, SubscriberId> market_by_gateway_;
    std::unordered_map<std::string, SubscriberId> replies_by_gateway_;
    SubscriberId next_id_ = 1;
    bool closed_ = false;
    PublisherStats stats_;

    // Last `replay_window` events per symbol, so a reconnecting subscriber can ask for
    // everything after its last seq (from_seq) instead of a snapshot. Guarded by mutex_.
    std::unordered_map<SymbolId, std::deque<MarketEvent>> history_;
    // Highest seq seen per symbol, kept even when history is empty or trimmed: tells
    // "too old" (below the window) from "ahead" (not happened yet). Guarded by mutex_.
    std::unordered_map<SymbolId, Seq> last_seq_;
    void remember(const MarketEvent &event); // called with mutex_ held

    bool started_ = false;
    std::jthread thread_; // MUST be the last member
};

} // namespace server
