#include "server/publisher.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace server {

Subscription::Subscription(SubscriberId id, std::string gateway_id, std::size_t capacity)
    : id_(id), gateway_id_(std::move(gateway_id)), capacity_(capacity) {}

bool Subscription::try_push(FeedItem item) {
    {
        std::lock_guard lock(mutex_);
        if (end_reason_ != EndReason::None)
            return true; // already ended: nothing to deliver, not "full"
        if (items_.size() >= capacity_)
            return false;
        items_.push_back(std::move(item));
    }
    ready_.notify_one();
    return true;
}

void Subscription::end(EndReason reason) {
    {
        std::lock_guard lock(mutex_);
        if (end_reason_ != EndReason::None)
            return;
        end_reason_ = reason;
        if (reason == EndReason::SlowConsumer)
            items_.clear(); // it restarts from a new snapshot; free the memory now
    }
    ready_.notify_all();
}

std::optional<FeedItem> Subscription::next_for(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    ready_.wait_for(lock, timeout, [this] { return !items_.empty() || end_reason_ != EndReason::None; });
    if (items_.empty())
        return std::nullopt;
    FeedItem item = std::move(items_.front());
    items_.pop_front();
    return item;
}

bool Subscription::ended() const {
    std::lock_guard lock(mutex_);
    return end_reason_ != EndReason::None;
}

EndReason Subscription::end_reason() const {
    std::lock_guard lock(mutex_);
    return end_reason_;
}

MarketDataPublisher::MarketDataPublisher(BlockingQueue<Output> &outbox, std::vector<SymbolId> symbols,
                                         SnapshotRequester request_snapshot, PublisherOptions options)
    : outbox_(outbox), symbols_(symbols.begin(), symbols.end()), symbol_list_(std::move(symbols)),
      request_snapshot_(std::move(request_snapshot)), options_(std::move(options)) {
    if (!request_snapshot_)
        throw std::invalid_argument("MarketDataPublisher needs a snapshot requester");
    if (options_.queue_capacity == 0)
        throw std::invalid_argument("queue_capacity must be > 0");
    if (symbols_.size() != symbol_list_.size())
        throw std::invalid_argument("duplicate symbol in publisher symbol list");
}

MarketDataPublisher::~MarketDataPublisher() { join(); }

void MarketDataPublisher::start() {
    if (started_)
        throw std::logic_error("MarketDataPublisher already started");
    started_ = true;
    thread_ = std::jthread([this] { run(); });
}

void MarketDataPublisher::join() {
    if (thread_.joinable())
        thread_.join();
}

MarketDataPublisher::SubscribeResult MarketDataPublisher::subscribe(std::string gateway_id,
                                                                    std::vector<SymbolId> symbols) {
    if (gateway_id.empty())
        return {nullptr, SubscribeError::EmptyGatewayId};
    if (symbols.empty())
        symbols = symbol_list_;
    std::sort(symbols.begin(), symbols.end());
    symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());
    for (SymbolId s : symbols)
        if (!symbols_.contains(s))
            return {nullptr, SubscribeError::UnknownSymbol};

    std::shared_ptr<Subscription> sub;
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return {nullptr, SubscribeError::Closed};
        if (by_gateway_.contains(gateway_id))
            return {nullptr, SubscribeError::GatewayAlreadySubscribed};
        const SubscriberId id = next_id_++;
        sub = std::make_shared<Subscription>(id, gateway_id, options_.queue_capacity);
        Subscriber entry{sub, {}};
        for (SymbolId s : symbols)
            entry.live.emplace(s, false); // waiting for its snapshot
        subscribers_.emplace(id, std::move(entry));
        by_gateway_.emplace(std::move(gateway_id), id);
    }
    // Registered first, then asked: the snapshot can only reach the outbox
    // after this subscriber exists, so it can never be missed.
    for (SymbolId s : symbols)
        request_snapshot_(s, sub->id());
    return {sub, SubscribeError::None};
}

void MarketDataPublisher::unsubscribe(SubscriberId id) {
    std::lock_guard lock(mutex_);
    drop(id, EndReason::Unsubscribed);
}

PublisherStats MarketDataPublisher::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void MarketDataPublisher::drop(SubscriberId id, EndReason reason) {
    const auto it = subscribers_.find(id);
    if (it == subscribers_.end())
        return;
    it->second.sub->end(reason);
    by_gateway_.erase(it->second.sub->gateway_id());
    subscribers_.erase(it);
}

void MarketDataPublisher::run() {
    while (true) {
        Output out = outbox_.pop();
        if (std::holds_alternative<OutboxClosed>(out)) {
            std::lock_guard lock(mutex_);
            closed_ = true;
            for (auto &[id, entry] : subscribers_)
                entry.sub->end(EndReason::Shutdown);
            subscribers_.clear();
            by_gateway_.clear();
            return;
        }
        if (options_.tap)
            options_.tap(out);
        std::lock_guard lock(mutex_);
        route(out);
    }
}

void MarketDataPublisher::route(Output &out) {
    if (auto *event = std::get_if<MarketEvent>(&out)) {
        ++stats_.events_routed;
        std::vector<SubscriberId> too_slow;
        for (auto &[id, entry] : subscribers_) {
            const auto symbol = entry.live.find(event->symbol);
            if (symbol == entry.live.end() || !symbol->second)
                continue; // not subscribed, or still waiting for the snapshot
            if (!entry.sub->try_push(*event))
                too_slow.push_back(id);
        }
        for (SubscriberId id : too_slow) {
            ++stats_.slow_consumer_drops;
            drop(id, EndReason::SlowConsumer);
        }
    } else if (auto *reply = std::get_if<Reply>(&out)) {
        const auto it = by_gateway_.find(reply->gateway_id);
        if (it == by_gateway_.end()) {
            ++stats_.replies_dropped; // that gateway isn't connected
            return;
        }
        const SubscriberId id = it->second;
        if (!subscribers_.at(id).sub->try_push(std::move(*reply))) {
            ++stats_.slow_consumer_drops;
            drop(id, EndReason::SlowConsumer);
        }
    } else if (auto *ready = std::get_if<SnapshotReady>(&out)) {
        const auto it = subscribers_.find(ready->subscriber);
        if (it == subscribers_.end())
            return; // it left before its snapshot was ready
        const auto symbol = it->second.live.find(ready->symbol);
        if (symbol == it->second.live.end() || symbol->second)
            return; // not asked for, or already live: ignore
        symbol->second = true;
        const SubscriberId id = ready->subscriber;
        if (!it->second.sub->try_push(std::move(*ready))) {
            ++stats_.slow_consumer_drops;
            drop(id, EndReason::SlowConsumer);
        }
    }
}

} // namespace server
