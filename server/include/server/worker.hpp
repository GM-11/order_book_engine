#pragma once

#include "engine/book.hpp"
#include "engine/order.hpp"
#include "server/blocking_queue.hpp"
#include "server/commands.hpp"
#include <memory>
#include <thread>
#include <unordered_map>
namespace server {
class Worker {
  public:
    explicit Worker(BlockingQueue<Output> &outbox);
    ~Worker();
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    void add_book(SymbolId symbol,
                  std::unique_ptr<engine::Book> book); // before start() only
    void start();
    void submit(Stamped item); // callable from any thread
    // Safe after start(): books_ is never modified after start().
    bool owns(SymbolId symbol) const;
    void stop();

  private:
    void run(); // the thread's loop
    engine::Book &book_for(SymbolId symbol);
    void publish(SymbolId symbol, engine::Book &book); // drain events -> outbox

    BlockingQueue<Stamped> inbox_;
    std::unordered_map<SymbolId, std::unique_ptr<engine::Book>> books_;
    BlockingQueue<Output> &outbox_;
    engine::Timestamp last_ts_{}; // worker-thread only
    bool started_ = false;
    bool stopped_ = false;
    std::jthread thread_; // MUST be the last member
};
} // namespace server
