#include "server/engine_feed_server.hpp"
#include "feed_proto.hpp"

#include "exchange/v1/engine_feed.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace server {
namespace pb = exchange::v1;

namespace {

class FeedService final : public pb::EngineFeed::Service {
  public:
    FeedService(MarketDataPublisher &publisher, const FeedServerOptions &options)
        : publisher_(publisher), options_(options) {}

    // Public stream: snapshots + market events. Never replies.
    grpc::Status Subscribe(grpc::ServerContext *context, const pb::FeedSubscribeRequest *request,
                           grpc::ServerWriter<pb::FeedMessage> *writer) override {
        std::vector<SymbolId> symbols(request->symbols().begin(), request->symbols().end());
        auto result = publisher_.subscribe(request->gateway_id(), std::move(symbols));
        if (result.error != SubscribeError::None)
            return refusal(result.error, request->gateway_id(), "Subscribe");
        return pump(
            context, *result.subscription, writer, "market",
            [](const FeedItem &item, pb::FeedMessage &out) {
                if (std::holds_alternative<Reply>(item))
                    return false; // cannot happen: market subscribers get no replies
                fill_feed_message(item, out);
                return true;
            },
            "fell behind; subscribe again for a fresh snapshot");
    }

    // Private stream: only this gateway's replies.
    grpc::Status SubscribeReplies(grpc::ServerContext *context, const pb::RepliesSubscribeRequest *request,
                                  grpc::ServerWriter<pb::Reply> *writer) override {
        auto result = publisher_.subscribe_replies(request->gateway_id());
        if (result.error != SubscribeError::None)
            return refusal(result.error, request->gateway_id(), "SubscribeReplies");
        return pump(
            context, *result.subscription, writer, "replies",
            [](const FeedItem &item, pb::Reply &out) {
                const auto *reply = std::get_if<Reply>(&item);
                if (!reply)
                    return false; // cannot happen: replies subscribers get nothing else
                fill_reply(*reply, out);
                return true;
            },
            "fell behind; queued replies were lost, treat their outcomes as unknown");
    }

  private:
    static grpc::Status refusal(SubscribeError error, const std::string &gateway_id, const char *rpc) {
        switch (error) {
        case SubscribeError::EmptyGatewayId:
            return {grpc::StatusCode::INVALID_ARGUMENT, "gateway_id must not be empty"};
        case SubscribeError::GatewayAlreadySubscribed:
            return {grpc::StatusCode::ALREADY_EXISTS, "gateway " + gateway_id + " already has a " + rpc + " stream"};
        case SubscribeError::UnknownSymbol:
            return {grpc::StatusCode::NOT_FOUND, "unknown symbol in request"};
        case SubscribeError::Closed:
            return {grpc::StatusCode::UNAVAILABLE, "engine is shutting down"};
        case SubscribeError::None:
            break;
        }
        return {grpc::StatusCode::INTERNAL, "unexpected subscribe result"};
    }

    template <class Message, class Fill>
    grpc::Status pump(grpc::ServerContext *context, Subscription &sub, grpc::ServerWriter<Message> *writer,
                      const std::string &stream, Fill fill, const char *slow_message) {
        // However this handler ends, the gateway id is freed.
        struct Leave {
            MarketDataPublisher &publisher;
            SubscriberId id;
            ~Leave() { publisher.unsubscribe(id); }
        } leave{publisher_, sub.id()};
        const std::string who = "gateway " + sub.gateway_id() + " (" + stream + ")";
        log(who + " subscribed");

        Message message;
        while (true) {
            if (context->IsCancelled()) {
                log(who + " went away");
                return {grpc::StatusCode::CANCELLED, "subscriber went away"};
            }
            std::optional<FeedItem> item = sub.next_for(options_.poll);
            if (!item) {
                if (sub.ended())
                    break; // ended and fully drained
                continue;
            }
            message.Clear();
            if (!fill(*item, message))
                continue;
            // Blocks while the client's receive window is full. Only this
            // handler waits: the publisher thread never does, it just drops
            // this subscriber once its queue overflows.
            if (!writer->Write(message)) {
                log(who + " stream broke");
                return {grpc::StatusCode::CANCELLED, "stream broken"};
            }
        }

        switch (sub.end_reason()) {
        case EndReason::SlowConsumer:
            log(who + " dropped: fell behind");
            return {grpc::StatusCode::RESOURCE_EXHAUSTED, slow_message};
        case EndReason::Shutdown:
            return {grpc::StatusCode::UNAVAILABLE, "engine is shutting down"};
        case EndReason::Unsubscribed:
        case EndReason::None:
            break;
        }
        return {grpc::StatusCode::CANCELLED, "unsubscribed"};
    }

    void log(const std::string &message) const {
        if (options_.log)
            options_.log(message);
    }

    MarketDataPublisher &publisher_;
    const FeedServerOptions &options_;
};

} // namespace

struct EngineFeedServer::Impl {
    Impl(MarketDataPublisher &publisher, FeedServerOptions opts)
        : options(std::move(opts)), service(publisher, options) {}

    FeedServerOptions options;
    FeedService service;
    std::unique_ptr<grpc::Server> server;
    int port = 0;
    std::mutex mutex; // start/stop
    bool stopped = false;
};

EngineFeedServer::EngineFeedServer(MarketDataPublisher &publisher, FeedServerOptions options)
    : impl_(std::make_unique<Impl>(publisher, std::move(options))) {}

EngineFeedServer::~EngineFeedServer() { stop(); }

void EngineFeedServer::start() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->server)
        throw std::logic_error("EngineFeedServer already started");
    grpc::ServerBuilder builder;
    builder.AddListeningPort(impl_->options.address, grpc::InsecureServerCredentials(), &impl_->port);
    builder.RegisterService(&impl_->service);
    impl_->server = builder.BuildAndStart();
    if (!impl_->server || impl_->port == 0)
        throw std::runtime_error("engine feed: cannot listen on " + impl_->options.address);
}

int EngineFeedServer::port() const { return impl_->port; }

void EngineFeedServer::stop() {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->server || impl_->stopped)
        return;
    impl_->stopped = true;
    // Open streams get the grace period to finish (normally they already
    // have, because the publisher ended them); after it they are cancelled.
    impl_->server->Shutdown(std::chrono::system_clock::now() + impl_->options.shutdown_grace);
    impl_->server->Wait();
}

} // namespace server
