#include "server/router.hpp"
#include "server/commands.hpp"
#include <stdexcept>
#include <type_traits>
#include <variant>
namespace server {
template <class> inline constexpr bool always_false = false; // at namespace scope, above run()

Router::Router(std::size_t worker_count, Clock clock) : clock_(std::move(clock)) {
    if (worker_count <= 0)
        throw std::invalid_argument("Router needs at least one worker");
    if (!clock_)
        throw std::invalid_argument("Router needs a clock");
    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i)
        workers_.push_back(std::make_unique<Worker>(outbox_));
}

Router::~Router() { shutdown(); }

void Router::start() {
    if (started_) {
        throw std::runtime_error("Router already started");
    }
    started_ = true;
    for (auto &worker : workers_)
        worker->start();

    running_ = true;
}

void Router::shutdown() {
    if (!started_) {
        return;
    }
    running_ = false;
    for (auto &worker : workers_)
        worker->stop();
}

void Router::add_symbol(SymbolId symbol, std::size_t worker_index, std::unique_ptr<engine::Book> book) {
    if (started_) {
        throw std::runtime_error("Cannot add symbol after router has started");
    }
    if (worker_index >= workers_.size()) {
        throw std::runtime_error("Worker index out of bounds");
    }
    if (routes_.contains(symbol))
        throw std::logic_error("Symbol already routed to a worker");
    Worker *worker = workers_[worker_index].get();
    worker->add_book(symbol, std::move(book));

    routes_[symbol] = worker;
}

SubmitResult Router::submit(Command command) {
    if (!running_)
        return SubmitResult::NotRunning;

    if (std::holds_alternative<Shutdown>(command))
        throw std::invalid_argument("Shutdown command cannot be submitted");

    SymbolId symbol = std::visit(
        [&](auto &&arg) -> SymbolId {
            using T = std::remove_cvref_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, Shutdown>) {
                throw std::invalid_argument("Shutdown command cannot be submitted");
            } else {
                return arg.symbol;
            }
        },
        command);
    auto it = routes_.find(symbol);
    if (it == routes_.end())
        return SubmitResult::UnknownSymbol;
    it->second->submit(Stamped{.seq = 0, .ts = clock_(), .command = std::move(command)});

    return SubmitResult::Queued;
}

BlockingQueue<Output> &Router::outbox() { return outbox_; }
} // namespace server
