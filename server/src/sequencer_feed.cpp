#include "server/sequencer_feed.hpp"

#include "server/codec.hpp"
#include "server/worker.hpp"

#include "exchange/v1/sequencer.grpc.pb.h"
#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace pb = exchange::v1;

namespace server {

struct SequencerFeed::Impl {
    Impl(Worker &worker_in, FeedOptions options_in)
        : worker(worker_in), options(std::move(options_in)), last(options.from_seq - 1) {
        if (!options.log) {
            options.log = [](const std::string &message) { std::cerr << message << std::endl; };
        }
    }

    void start() {
        std::lock_guard lock(state_mutex);
        if (started)
            throw std::logic_error("SequencerFeed already started");
        started = true;
        thread = std::thread([this] { run(); });
    }

    void stop() {
        {
            std::lock_guard lock(context_mutex);
            stopping = true;
            if (current_context)
                current_context->TryCancel();
        }
        retry_cv.notify_all();
        // stop() may be called from on_fatal, which runs on the feed thread itself. A thread
        // cannot join itself, so then we only request the stop; the destructor joins later.
        if (thread.joinable() && thread.get_id() != std::this_thread::get_id())
            thread.join();
    }

    Seq last_seq_value() const { return last.load(); }

    bool has_failed() const { return failed.load(); }

    std::string failure_message() const {
        std::lock_guard lock(failure_mutex);
        return failure_reason;
    }

    bool is_stopping() const {
        std::lock_guard lock(context_mutex);
        return stopping;
    }

    bool wait_to_retry() {
        std::unique_lock lock(context_mutex);
        return retry_cv.wait_for(lock, options.retry_delay, [this] { return stopping; });
    }

    void fail(std::string reason) {
        {
            std::lock_guard lock(failure_mutex);
            failure_reason = std::move(reason);
        }
        failed = true;
        const std::string message = failure_message();
        options.log(message);
        if (options.on_fatal)
            options.on_fatal(message);
    }

    void clear_context(grpc::ClientContext *context) {
        std::lock_guard lock(context_mutex);
        if (current_context == context)
            current_context = nullptr;
    }

    void run() {
        auto channel = grpc::CreateChannel(options.address, grpc::InsecureChannelCredentials());
        auto stub = pb::Sequencer::NewStub(channel);
        int already_exists_retries = 0;

        while (!is_stopping() && !failed.load()) {
            grpc::ClientContext context;
            {
                std::lock_guard lock(context_mutex);
                if (stopping)
                    return;
                // Register before calling so stop() can cancel this stream.
                current_context = &context;
            }

            pb::SubscribeRequest request;
            request.set_partition(options.partition);
            request.set_from_seq(last.load() + 1);
            std::unique_ptr<grpc::ClientReader<pb::SequencedCommand>> reader = stub->Subscribe(&context, request);

            pb::SequencedCommand command;
            bool fatal = false;
            while (reader->Read(&command)) {
                const Seq expected = last.load() + 1;
                if (command.seq() != expected) {
                    fail("partition " + std::to_string(options.partition) + ": gap, expected seq " +
                         std::to_string(expected) + " but got " + std::to_string(command.seq()));
                    fatal = true;
                    break;
                }
                if (!worker.owns(command.symbol())) {
                    fail("partition " + std::to_string(options.partition) + ": symbol " +
                         std::to_string(command.symbol()) + " is not owned by this worker (config mismatch)");
                    fatal = true;
                    break;
                }

                DecodeResult decoded = decode_body(command.symbol(), command.client_request_id(), command.payload());
                if (!decoded) {
                    options.log("partition " + std::to_string(options.partition) + ": skipping seq " +
                                std::to_string(command.seq()) + " from gateway " + command.gateway_id() + " request " +
                                std::to_string(command.client_request_id()) + ": " + decoded.error);
                    // A seen, deliberately skipped command still counts during replay.
                    last = command.seq();
                    already_exists_retries = 0;
                    continue;
                }

                worker.submit(Stamped{.seq = command.seq(),
                                      .ts = static_cast<engine::Timestamp>(command.ts()),
                                      .command = std::move(*decoded.command),
                                      .gateway_id = command.gateway_id(),
                                      .account_id = command.account_id()});
                last = command.seq();
                already_exists_retries = 0;
            }
            if (fatal)
                context.TryCancel();
            grpc::Status status = reader->Finish();
            clear_context(&context);
            if (fatal || failed.load())
                return;
            if (is_stopping())
                return;

            const auto code = status.error_code();
            const std::string message = status.error_message();
            if (code == grpc::StatusCode::OUT_OF_RANGE) {
                // Fatal until the journal exists: retrying would miss commands.
                fail("sequencer cannot serve from seq " + std::to_string(last.load() + 1) + ": " + message);
                return;
            }
            if (code == grpc::StatusCode::NOT_FOUND || code == grpc::StatusCode::INVALID_ARGUMENT) {
                fail("partition " + std::to_string(options.partition) + ": sequencer configuration error: " + message);
                return;
            }
            if (code == grpc::StatusCode::ALREADY_EXISTS) {
                // This is usually our old stream holding the seat briefly.
                ++already_exists_retries;
                if (already_exists_retries > options.max_already_exists_retries) {
                    fail("another subscriber holds partition " + std::to_string(options.partition));
                    return;
                }
                options.log("partition " + std::to_string(options.partition) + ": subscriber already exists; retrying");
            } else {
                already_exists_retries = 0;
                options.log("partition " + std::to_string(options.partition) + ": subscribe ended (" +
                            status.error_message() + "); retrying");
            }

            if (wait_to_retry())
                return;
        }
    }

    Worker &worker;
    FeedOptions options;
    std::atomic<Seq> last;
    std::atomic<bool> failed{false};
    mutable std::mutex failure_mutex;
    std::string failure_reason;
    mutable std::mutex context_mutex;
    std::condition_variable retry_cv;
    grpc::ClientContext *current_context = nullptr;
    bool stopping = false;
    std::mutex state_mutex;
    bool started = false;
    std::thread thread;
};

SequencerFeed::SequencerFeed(Worker &worker, FeedOptions options) {
    if (options.address.empty())
        throw std::invalid_argument("SequencerFeed needs an address");
    if (options.from_seq == 0)
        throw std::invalid_argument("SequencerFeed from_seq must not be zero");
    impl_ = std::make_unique<Impl>(worker, std::move(options));
}

SequencerFeed::~SequencerFeed() { stop(); }

void SequencerFeed::start() { impl_->start(); }

void SequencerFeed::stop() { impl_->stop(); }

Seq SequencerFeed::last_seq() const { return impl_->last_seq_value(); }

bool SequencerFeed::failed() const { return impl_->has_failed(); }

std::string SequencerFeed::failure() const { return impl_->failure_message(); }

} // namespace server
