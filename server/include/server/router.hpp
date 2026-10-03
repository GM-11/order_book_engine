#pragma once

#include "server/blocking_queue.hpp"
#include "server/commands.hpp"
#include "server/worker.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
namespace server {

enum class SubmitResult { Queued, UnknownSymbol, NotRunning };

class Router {
  public:
    using Clock = std::function<engine::Timestamp()>;

    Router(std::size_t worker_count, Clock clock);
    ~Router();
    void add_symbol(SymbolId symbol, std::size_t worker_index, std::unique_ptr<engine::Book> book);
    void start();
    SubmitResult submit(Command command);
    void shutdown();
    BlockingQueue<Output> &outbox();

  private:
    BlockingQueue<Output> outbox_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::unordered_map<SymbolId, Worker *> routes_;
    Clock clock_;
    bool started_ = false;
    std::atomic<bool> running_ = false;
};
} // namespace server
