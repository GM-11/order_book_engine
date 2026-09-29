#pragma once

#include "engine/book.hpp"
#include "engine/order.hpp"
#include "server/blocking_queue.hpp"
#include "server/commands.hpp"
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
namespace server {
class Worker {
  public:
    using Clock = std::function<engine::Timestamp()>;

    Worker(BlockingQueue<Output> &outbox, Clock clock);
    ~Worker();
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    void add_book(SymbolId symbol,
                  std::unique_ptr<engine::Book> book); // before start() only
    void start();
    void submit(Command command); // callable from any thread

  private:
    void run(); // the thread's loop
    engine::Book &book_for(SymbolId symbol);
    void publish(SymbolId symbol, engine::Book &book); // drain events -> outbox

    BlockingQueue<Command> inbox_;
    std::unordered_map<SymbolId, std::unique_ptr<engine::Book>> books_;
    BlockingQueue<Output> &outbox_;
    Clock clock_;
    bool started_ = false;
    std::jthread thread_; // MUST be the last member
};
} // namespace server
