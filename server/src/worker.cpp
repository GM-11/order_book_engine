#include "server/worker.hpp"
#include "engine/event.hpp"
#include "engine/order.hpp"
#include "server/commands.hpp"
#include <algorithm>
#include <stdexcept>
namespace server {

template <class> inline constexpr bool always_false = false; // at namespace scope, above run()

Worker::Worker(BlockingQueue<Output> &outbox) : outbox_(outbox) {}
Worker::~Worker() { stop(); }

void Worker::stop() {
    if (!started_ || stopped_)
        return;
    stopped_ = true;
    inbox_.push(Stamped{.seq = 0, .ts = {}, .command = Shutdown{}});
    thread_.join();
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

void Worker::submit(Stamped item) { inbox_.push(std::move(item)); }

bool Worker::owns(SymbolId symbol) const { return books_.contains(symbol); }

void Worker::run() {
    while (true) {
        Stamped item = inbox_.pop();
        if (std::holds_alternative<Shutdown>(item.command))
            return;
        if (const auto *take = std::get_if<TakeSnapshot>(&item.command)) {
            outbox_.push(SnapshotReady{take->symbol, take->subscriber, book_for(take->symbol).snapshot()});
            continue;
        }
        engine::Timestamp now = std::max(item.ts, last_ts_);
        last_ts_ = now;

        std::visit(
            [&](auto &&arg) {
                using T = std::remove_cvref_t<decltype(arg)>;

                if constexpr (std::is_same_v<T, NewOrder>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::OrderResult result = book.add_order(arg.order, now);
                    publish(arg.symbol, book);
                    reply(item, arg.request_id, arg.symbol, result.reject_reason, result.unaccepted_quantity,
                          result.rested_price);

                } else if constexpr (std::is_same_v<T, CancelOrder>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::RejectReason result = book.cancel_order(arg.order_id, arg.requester, now);
                    publish(arg.symbol, book);
                    reply(item, arg.request_id, arg.symbol, result);

                } else if constexpr (std::is_same_v<T, ModifyOrder>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::OrderResult result =
                        book.modify_order(arg.order_id, arg.requester, arg.new_price, arg.new_quantity, now);
                    publish(arg.symbol, book);
                    reply(item, arg.request_id, arg.symbol, result.reject_reason, result.unaccepted_quantity,
                          result.rested_price);

                } else if constexpr (std::is_same_v<T, PlaceStop>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::RejectReason result = book.place_stop_order(arg.stop, now);
                    publish(arg.symbol, book);
                    reply(item, arg.request_id, arg.symbol, result);

                } else if constexpr (std::is_same_v<T, CancelStop>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::RejectReason result = book.cancel_stop_order(arg.order_id, arg.requester, now);
                    publish(arg.symbol, book);
                    reply(item, arg.request_id, arg.symbol, result);

                } else if constexpr (std::is_same_v<T, ModifyStop>) {
                    engine::Book &book = book_for(arg.symbol);
                    engine::RejectReason result = book.modify_stop_order(
                        arg.order_id, arg.requester, arg.new_stop_price, arg.new_limit_price, arg.new_quantity, now);
                    publish(arg.symbol, book);
                    reply(item, arg.request_id, arg.symbol, result);

                } else if constexpr (std::is_same_v<T, Shutdown> || std::is_same_v<T, TakeSnapshot>) {
                    // Handled before std::visit; nothing to do here.

                } else {
                    static_assert(always_false<T>, "unhandled Command type");
                }
            },
            item.command);
    }
}

void Worker::reply(const Stamped &item, RequestId request_id, SymbolId symbol, engine::RejectReason reason,
                   engine::Quantity unaccepted, std::optional<engine::Price> rested_price) {
    outbox_.push(Reply{.request_id = request_id,
                       .symbol = symbol,
                       .reject_reason = reason,
                       .unaccepted_quantity = unaccepted,
                       .rested_price = rested_price,
                       .gateway_id = item.gateway_id});
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
