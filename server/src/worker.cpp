#include "server/worker.hpp"
#include "engine/event.hpp"
#include "engine/order.hpp"
#include "server/commands.hpp"
#include <stdexcept>
namespace server {

template <class> inline constexpr bool always_false = false; // at namespace scope, above run()

Worker::Worker(BlockingQueue<Output> &outbox, Clock clock) : outbox_(outbox), clock_(std::move(clock)) {
    if (!clock_) {
        throw std::invalid_argument("Worker needs a clock");
    }
}
Worker::~Worker() {
    if (started_)
        inbox_.push(Shutdown{});
}

void Worker::add_book(SymbolId symbol, std::unique_ptr<engine::Book> book) {
    if (started_)
        throw std::logic_error("Cannot add book after worker has started");
    if (!book)
        throw std::invalid_argument("Book must not be null");
    auto it = books_.find(symbol);
    if (it != books_.end())
        throw std::logic_error("Book already exists");

    books_.emplace(symbol, std::move(book));
}

void Worker::start() {
    if (started_)
        throw std::logic_error("Cannot start worker twice");
    started_ = true;
    thread_ = std::jthread([this] { run(); });
}

void Worker::submit(Command command) { inbox_.push(std::move(command)); }

void Worker::run() {
    while (true) {
        Command command = inbox_.pop();
        if (std::holds_alternative<Shutdown>(command))
            return;
        engine::Timestamp now = clock_();

        std::visit(
            [&](auto &&arg) {
                using T = std::remove_cvref_t<decltype(arg)>;

                if constexpr (std::is_same_v<T, NewOrder>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::OrderResult result = book.add_order(arg.order, now);
                    publish(arg.symbol, book);
                    outbox_.push(Reply{arg.request_id, arg.symbol, result.reject_reason, result.unaccepted_quantity,
                                       result.rested_price});

                } else if constexpr (std::is_same_v<T, CancelOrder>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::RejectReason result = book.cancel_order(arg.order_id, now);
                    publish(arg.symbol, book);
                    outbox_.push(Reply{arg.request_id, arg.symbol, result});

                } else if constexpr (std::is_same_v<T, ModifyOrder>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::OrderResult result = book.modify_order(arg.order_id, arg.new_price, arg.new_quantity, now);
                    publish(arg.symbol, book);
                    outbox_.push(Reply{arg.request_id, arg.symbol, result.reject_reason, result.unaccepted_quantity,
                                       result.rested_price});

                } else if constexpr (std::is_same_v<T, PlaceStop>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::RejectReason result = book.place_stop_order(arg.stop, now);
                    publish(arg.symbol, book);
                    outbox_.push(Reply{arg.request_id, arg.symbol, result});

                } else if constexpr (std::is_same_v<T, CancelStop>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::RejectReason result = book.cancel_stop_order(arg.order_id, now);
                    publish(arg.symbol, book);
                    outbox_.push(Reply{arg.request_id, arg.symbol, result});

                } else if constexpr (std::is_same_v<T, ModifyStop>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::RejectReason result = book.modify_stop_order(arg.order_id, arg.new_stop_price,
                                                                         arg.new_limit_price, arg.new_quantity, now);
                    publish(arg.symbol, book);
                    outbox_.push(Reply{arg.request_id, arg.symbol, result});

                } else if constexpr (std::is_same_v<T, Shutdown>) {
                    // Handled before std::visit; nothing to do here.

                } else {
                    static_assert(always_false<T>, "unhandled Command type");
                }
            },
            command);
    }
}

void Worker::publish(SymbolId symbol, engine::Book &book) {
    for (engine::EngineEvent &event : book.drain_events()) {
        outbox_.push(MarketEvent{symbol, event});
    }
}

engine::Book &Worker::book_for(SymbolId symbol) {
    auto it = books_.find(symbol);
    if (it == books_.end())
        throw std::logic_error("No book for symbol");
    return *it->second;
}
} // namespace server
