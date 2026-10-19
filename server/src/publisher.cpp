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
                                                                    std::vector<SymbolId> symbols,
                                                                    std::unordered_map<SymbolId, Seq> from_seq) {
    if (gateway_id.empty())
        return {nullptr, SubscribeError::EmptyGatewayId, "gateway_id is empty"};
    if (symbols.empty())
        symbols = symbol_list_;

    std::sort(symbols.begin(), symbols.end());
    symbols.erase(std::unique(symbols.begin(), symbols.end()), symbols.end());

    for (SymbolId s : symbols)
        if (!symbols_.contains(s))
            return {nullptr, SubscribeError::UnknownSymbol, "unknown symbol"};

    for (const auto &[symbol, seq] : from_seq) {
        if (!std::binary_search(symbols.begin(), symbols.end(), symbol) || seq < 1)
            return {nullptr, SubscribeError::BadFromSeq, "from_seq must name a subscribed symbol and be at least 1"};
    }

    std::shared_ptr<Subscription> sub;
    std::vector<SymbolId> snapshots;
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return {nullptr, SubscribeError::Closed, "closed"};
        if (market_by_gateway_.contains(gateway_id))
            return {nullptr, SubscribeError::GatewayAlreadySubscribed, "gateway_id is already subscribed"};

        for (const auto &[symbol, wanted] : from_seq) {
            const Seq last = last_seq_.contains(symbol) ? last_seq_.at(symbol) : 0;
            const auto history = history_.find(symbol);
            const Seq oldest = history == history_.end() || history->second.empty()
                                   ? last + 1
                                   : history->second.front().event.sequence_number;
            if (wanted <= last && wanted < oldest) {
                ++stats_.too_old_refusals;
                return {nullptr, SubscribeError::TooOld,
                        "requested sequence " + std::to_string(wanted) + " for symbol " + std::to_string(symbol) +
                            " is older than the oldest retained event " + std::to_string(oldest)};
            }
        }

        std::size_t replay_count = 0;
        for (const auto &[symbol, wanted] : from_seq) {
            const auto history = history_.find(symbol);
            if (history == history_.end())
                continue;
            for (const MarketEvent &event : history->second)
                if (event.event.sequence_number >= wanted)
                    ++replay_count;
        }

        const SubscriberId id = next_id_++;
        sub = std::make_shared<Subscription>(id, gateway_id, options_.queue_capacity + replay_count);
        for (SymbolId symbol : symbols) {
            const auto from = from_seq.find(symbol);
            if (from == from_seq.end()) {
                snapshots.push_back(symbol);
            } else {
                const auto history = history_.find(symbol);
                if (history == history_.end())
                    continue;
                for (const MarketEvent &event : history->second) {
                    if (event.event.sequence_number >= from->second) {
                        if (!sub->try_push(event))
                            throw std::logic_error("replay does not fit subscription queue");
                        ++stats_.replayed_events;
                    }
                }
            }
        }

        Subscriber entry{sub, SubscriberKind::Market, {}};
        for (SymbolId symbol : symbols) {
            const auto from = from_seq.find(symbol);
            entry.live.emplace(symbol,
                               from == from_seq.end() ? SymbolState{false, 0} : SymbolState{true, from->second});
        }
        subscribers_.emplace(id, std::move(entry));
        market_by_gateway_.emplace(std::move(gateway_id), id);
    }

    // Snapshot symbols are registered before requesting their snapshots, so
    // a snapshot cannot be missed. Replay symbols are already live.
    for (SymbolId symbol : snapshots)
        request_snapshot_(symbol, sub->id());
    return {sub, SubscribeError::None, {}};
}

MarketDataPublisher::SubscribeResult MarketDataPublisher::subscribe_replies(std::string gateway_id) {
    if (gateway_id.empty())
        return {nullptr, SubscribeError::EmptyGatewayId, "gateway_id is empty"};
    std::lock_guard lock(mutex_);
    if (closed_)
        return {nullptr, SubscribeError::Closed, "closed"};
    if (replies_by_gateway_.contains(gateway_id))
        return {nullptr, SubscribeError::GatewayAlreadySubscribed, "gateway_id is already subscribed"};
    const SubscriberId id = next_id_++;
    auto sub = std::make_shared<Subscription>(id, gateway_id, options_.queue_capacity);
    // No live map: route() never gives it a MarketEvent or a snapshot.
    subscribers_.emplace(id, Subscriber{sub, SubscriberKind::Replies, {}});
    replies_by_gateway_.emplace(std::move(gateway_id), id);
    return {sub, SubscribeError::None, {}};
}

void MarketDataPublisher::unsubscribe(SubscriberId id) {
    std::lock_guard lock(mutex_);
    drop(id, EndReason::Unsubscribed);
}

PublisherStats MarketDataPublisher::stats() const {
    std::lock_guard lock(mutex_);
    PublisherStats s = stats_;
    s.market_subscribers = market_by_gateway_.size();
    s.replies_subscribers = replies_by_gateway_.size();
    return s;
}

void MarketDataPublisher::drop(SubscriberId id, EndReason reason) {
    const auto it = subscribers_.find(id);
    if (it == subscribers_.end())
        return;
    it->second.sub->end(reason);
    auto &by_gateway = it->second.kind == SubscriberKind::Replies ? replies_by_gateway_ : market_by_gateway_;
    by_gateway.erase(it->second.sub->gateway_id());
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
            market_by_gateway_.clear();
            replies_by_gateway_.clear();
            return;
        }
        if (options_.tap)
            options_.tap(out);
        std::lock_guard lock(mutex_);
        route(out);
    }
}

void MarketDataPublisher::route(Output &out) {
    if (auto *event = std::get_if<MarketEvent>(&out)) { // public
        remember(*event);
        ++stats_.events_routed;
        std::vector<SubscriberId> too_slow;
        for (auto &[id, entry] : subscribers_) {
            const auto symbol = entry.live.find(event->symbol);
            if (symbol == entry.live.end() || !symbol->second.live)
                continue;
            if (event->event.sequence_number < symbol->second.skip_below)
                continue;
            if (!entry.sub->try_push(*event))
                too_slow.push_back(id);
        }
        for (SubscriberId id : too_slow) {
            ++stats_.slow_consumer_drops;
            drop(id, EndReason::SlowConsumer);
        }
    } else if (auto *reply = std::get_if<Reply>(&out)) { // private: Replies subscribers only
        const auto it = replies_by_gateway_.find(reply->gateway_id);
        if (it == replies_by_gateway_.end()) {
            ++stats_.replies_dropped; // that gateway has no SubscribeReplies stream
            return;
        }
        const SubscriberId id = it->second;
        if (!subscribers_.at(id).sub->try_push(std::move(*reply))) {
            ++stats_.slow_consumer_drops;
            drop(id, EndReason::SlowConsumer);
        }
    } else if (auto *ready = std::get_if<SnapshotReady>(&out)) { // snapshot
        const auto it = subscribers_.find(ready->subscriber);
        if (it == subscribers_.end())
            return; // it left before its snapshot was ready
        const auto symbol = it->second.live.find(ready->symbol);
        if (symbol == it->second.live.end() || symbol->second.live)
            return; // not asked for, or already live: ignore
        symbol->second.live = true;
        const SubscriberId id = ready->subscriber;
        if (!it->second.sub->try_push(std::move(*ready))) {
            ++stats_.slow_consumer_drops;
            drop(id, EndReason::SlowConsumer);
        }
    }
}

void MarketDataPublisher::remember(const MarketEvent &event) {
    auto &kept = history_[event.symbol];
    kept.push_back(event);
    if (options_.replay_window == 0)
        kept.clear();
    else if (kept.size() > options_.replay_window)
        kept.pop_front();
    last_seq_[event.symbol] = event.event.sequence_number;
}
} // namespace server
