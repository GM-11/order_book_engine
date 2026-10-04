#include "server/engine_feed_server.hpp"
#include "feed_proto.hpp"

#include "exchange/v1/engine_feed.grpc.pb.h"

#include <grpcpp/grpcpp.h>

#include <mutex>
#include <stdexcept>
#include <vector>

namespace server {
namespace pb = exchange::v1;

namespace {

class FeedService final : public pb::EngineFeed::Service {
  public:
    FeedService(MarketDataPublisher &publisher, const FeedServerOptions &options)
        : publisher_(publisher), options_(options) {}

    grpc::Status Subscribe(grpc::ServerContext *context, const pb::FeedSubscribeRequest *request,
                           grpc::ServerWriter<pb::FeedMessage> *writer) override {
        std::vector<SymbolId> symbols(request->symbols().begin(), request->symbols().end());
        auto result = publisher_.subscribe(request->gateway_id(), std::move(symbols));
        switch (result.error) {
        case SubscribeError::None:
            break;
        case SubscribeError::EmptyGatewayId:
            return {grpc::StatusCode::INVALID_ARGUMENT, "gateway_id must not be empty"};
        case SubscribeError::GatewayAlreadySubscribed:
            return {grpc::StatusCode::ALREADY_EXISTS, "gateway " + request->gateway_id() + " already has a stream"};
        case SubscribeError::UnknownSymbol:
            return {grpc::StatusCode::NOT_FOUND, "unknown symbol in request"};
        case SubscribeError::Closed:
            return {grpc::StatusCode::UNAVAILABLE, "engine is shutting down"};
        }

        const std::shared_ptr<Subscription> sub = result.subscription;
        // However this handler ends, the gateway id is freed.
        struct Leave {
            MarketDataPublisher &publisher;
            SubscriberId id;
            ~Leave() { publisher.unsubscribe(id); }
        } leave{publisher_, sub->id()};
        log("gateway " + sub->gateway_id() + " subscribed");

        pb::FeedMessage message;
        while (true) {
            if (context->IsCancelled()) {
                log("gateway " + sub->gateway_id() + " went away");
                return {grpc::StatusCode::CANCELLED, "subscriber went away"};
            }
            std::optional<FeedItem> item = sub->next_for(options_.poll);
            if (!item) {
                if (sub->ended())
                    break; // ended and fully drained
                continue;
            }
            message.Clear();
            fill_feed_message(*item, message);
            // Blocks while the client's receive window is full. Only this
            // handler waits: the publisher thread never does, it just drops
            // this subscriber once its queue overflows.
            if (!writer->Write(message)) {
                log("gateway " + sub->gateway_id() + " stream broke");
                return {grpc::StatusCode::CANCELLED, "stream broken"};
            }
        }

        switch (sub->end_reason()) {
        case EndReason::SlowConsumer:
            log("gateway " + sub->gateway_id() + " dropped: fell behind");
            return {grpc::StatusCode::RESOURCE_EXHAUSTED, "fell behind; subscribe again for a fresh snapshot"};
        case EndReason::Shutdown:
            return {grpc::StatusCode::UNAVAILABLE, "engine is shutting down"};
        case EndReason::Unsubscribed:
        case EndReason::None:
            break;
        }
        return {grpc::StatusCode::CANCELLED, "unsubscribed"};
    }

  private:
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
