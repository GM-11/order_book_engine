#include "cli/console.hpp"
#include "config/instruments.hpp"
#include "server/blocking_queue.hpp"
#include "server/engine_feed_server.hpp"
#include "server/publisher.hpp"
#include "server/sequencer_feed.hpp"
#include "server/worker.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <string>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct ShutdownInOrder {
    std::vector<std::unique_ptr<server::SequencerFeed>> &feeds;
    std::vector<std::unique_ptr<server::Worker>> &workers;
    server::BlockingQueue<server::Output> &outbox;
    server::MarketDataPublisher &publisher;
    server::EngineFeedServer &feed_server;

    ~ShutdownInOrder() {
        // Feeds must not enqueue into stopped workers; workers must finish publishing

        for (auto &feed : feeds)
            feed->stop();
        for (auto &worker : workers)
            worker->stop();
        outbox.push(server::OutboxClosed{});
        publisher.join();
        feed_server.stop();
    }
};

} // namespace

int main(int argc, char *argv[]) {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    const int mask_error = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (mask_error != 0) {
        std::cerr << "engine_node setup failed: " << std::system_error(mask_error, std::generic_category()).what()
                  << '\n';
        return 1;
    }

    try {
        const std::string address = argc > 1 ? argv[1] : "127.0.0.1:50051";
        const std::string feed_address = argc > 2 ? argv[2] : "127.0.0.1:50061";
        const char *quiet_env = std::getenv("ENGINE_NODE_QUIET");
        const bool quiet = quiet_env != nullptr && std::string(quiet_env) == "1";

        // The same instrument list the sequencer reads: partition i is worker i.
        const std::string instruments_path = config::instruments_path();
        const config::InstrumentConfig instruments = config::load_instruments(instruments_path);
        const std::size_t worker_count = instruments.partitions;

        server::BlockingQueue<server::Output> outbox;
        std::vector<std::unique_ptr<server::Worker>> workers;
        workers.reserve(worker_count);
        for (std::size_t i = 0; i < worker_count; ++i)
            workers.push_back(std::make_unique<server::Worker>(outbox));

        std::unordered_map<server::SymbolId, std::string> ticker_of;
        std::unordered_map<server::SymbolId, server::Worker *> worker_of;
        std::vector<server::SymbolId> symbols;
        for (const auto &instrument : instruments.instruments) {
            server::Worker *worker = workers.at(instrument.partition).get();
            worker->add_book(instrument.id, std::make_unique<engine::Book>(100000, instrument.band_bps));
            ticker_of.emplace(instrument.id, instrument.ticker);
            worker_of.emplace(instrument.id, worker);
            symbols.push_back(instrument.id);
        }
        for (auto &worker : workers)
            worker->start();

        server::PublisherOptions publisher_options;
        if (!quiet)
            publisher_options.tap = [&ticker_of](const server::Output &out) {
                if (auto *market = std::get_if<server::MarketEvent>(&out))
                    cli::say(cli::format_event(ticker_of.at(market->symbol), market->event));
                else if (auto *reply = std::get_if<server::Reply>(&out))
                    cli::say(cli::format_reply(*reply));
            };
        server::MarketDataPublisher publisher(
            outbox, symbols,
            [&worker_of](server::SymbolId symbol, server::SubscriberId subscriber) {
                worker_of.at(symbol)->submit(
                    server::Stamped{.seq = 0, .ts = {}, .command = server::TakeSnapshot{symbol, subscriber}});
            },
            std::move(publisher_options));
        publisher.start();
        server::EngineFeedServer feed_server(
            publisher, server::FeedServerOptions{.address = feed_address,
                                                 .log = [](const std::string &message) { cli::say(message); }});

        std::atomic<bool> failed{false};
        {
            std::vector<std::unique_ptr<server::SequencerFeed>> feeds;
            ShutdownInOrder shutdown_guard{feeds, workers, outbox, publisher, feed_server};
            feed_server.start(); // inside the guard: a bind failure still shuts down in order
            feeds.reserve(worker_count);
            for (std::size_t i = 0; i < worker_count; ++i) {
                server::FeedOptions options{
                    .address = address,
                    .partition = static_cast<std::uint32_t>(i),
                    .from_seq = 1,
                    .log = [](const std::string &message) { cli::say(message); },
                    .on_fatal =
                        [&failed](const std::string &reason) {
                            cli::say("FATAL: " + reason);
                            failed = true;
                            kill(getpid(), SIGTERM);
                        },
                };
                feeds.push_back(std::make_unique<server::SequencerFeed>(*workers[i], std::move(options)));
            }

            // Print before starting feeds so retry logs cannot precede the startup line.
            cli::say("engine_node: " + std::to_string(instruments.instruments.size()) + " symbols on " +
                     std::to_string(worker_count) + " workers (instruments v" + std::to_string(instruments.version) +
                     " from " + instruments_path + "), sequencer at " + address + ", engine feed on " + feed_address +
                     " (port " + std::to_string(feed_server.port()) + "). Ctrl-C to stop.");
            for (auto &feed : feeds)
                feed->start();

            int received = 0;
            const int wait_error = sigwait(&signals, &received);
            if (wait_error != 0)
                throw std::system_error(wait_error, std::generic_category(), "sigwait");
            cli::say("engine_node stopping...");
        }
        cli::say("engine_node stopped");
        return failed ? 1 : 0;
    } catch (const std::exception &error) {
        std::cerr << "engine_node setup failed: " << error.what() << '\n';
        return 1;
    }
}
