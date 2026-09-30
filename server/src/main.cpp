#include "server/commands.hpp"
#include "server/router.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace server;

namespace {

// ------------------------------------------------------------ symbol config
// Read once at startup; symbols can only be added before router.start().
// Later this moves to a config file.
struct SymbolConfig {
    std::string_view ticker;
    SymbolId id;
    std::size_t worker;    // which worker thread owns this symbol's Book
    std::int64_t band_bps; // circuit-breaker band, basis points
};

constexpr SymbolConfig kSymbols[] = {
    {"MOOG", 1, 0, 1000},
    {"BANANA", 2, 0, 1000},
    {"TESLO", 3, 1, 1000},
    {"MACROHARD", 4, 1, 1000},
};
constexpr std::size_t kWorkers = 2;

// ------------------------------------------------------------------ printing
// Two threads write to the screen (gateway and publisher). Each printed line
// takes this lock so lines never interleave mid-way.
std::mutex g_print_mu;

void say(const std::string &line) {
    std::lock_guard lock(g_print_mu);
    std::cout << line << '\n' << std::flush;
}

std::string_view to_string(engine::EventKind k) {
    using K = engine::EventKind;
    switch (k) {
    case K::Accepted:
        return "ACCEPTED";
    case K::Rested:
        return "RESTED";
    case K::Trade:
        return "TRADE";
    case K::Cancelled:
        return "CANCELLED";
    case K::Modified:
        return "MODIFIED";
    case K::Replaced:
        return "REPLACED";
    case K::StopAccepted:
        return "STOP_ACCEPTED";
    case K::StopTriggered:
        return "STOP_TRIGGERED";
    case K::StopCancelled:
        return "STOP_CANCELLED";
    case K::StopModified:
        return "STOP_MODIFIED";
    case K::Halted:
        return "HALTED";
    case K::Resumed:
        return "RESUMED";
    }
    return "?";
}

std::string_view to_string(engine::RejectReason r) {
    using R = engine::RejectReason;
    switch (r) {
    case R::None:
        return "None";
    case R::InvalidPrice:
        return "InvalidPrice";
    case R::InvalidQuantity:
        return "InvalidQuantity";
    case R::SelfTrade:
        return "SelfTrade";
    case R::PoolExhausted:
        return "PoolExhausted";
    case R::SymbolHalted:
        return "SymbolHalted";
    case R::UnknownOrder:
        return "UnknownOrder";
    case R::DuplicateOrderId:
        return "DuplicateOrderId";
    case R::TooLate:
        return "TooLate";
    case R::PriceBand:
        return "PriceBand";
    case R::PriceCollar:
        return "PriceCollar";
    case R::StopWouldTrigger:
        return "StopWouldTrigger";
    }
    return "?";
}

std::string format_event(std::string_view ticker, const engine::EngineEvent &e) {
    std::ostringstream s;
    s << "  [" << ticker << " #" << e.sequence_number << "] " << to_string(e.kind);
    switch (e.kind) {
    case engine::EventKind::Trade:
        s << ' ' << e.quantity << " @ " << e.price.value_or(0) << "  (aggressor order " << e.order_id << ' '
          << engine::to_string(e.side) << ", resting order " << e.passive_id << "; buyer " << e.owner_id << ", seller "
          << e.other_owner << ')';
        break;
    case engine::EventKind::Halted:
    case engine::EventKind::Resumed:
        break;
    default:
        s << " order " << e.order_id << ' ' << engine::to_string(e.side) << ' ' << e.quantity;
        if (e.price)
            s << " @ " << *e.price;
        if (e.limit_price)
            s << " limit " << *e.limit_price;
        break;
    }
    return s.str();
}

// ------------------------------------------------------------------- parsing
bool parse_int(std::string_view text, std::int64_t &out) {
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc{} && ptr == text.data() + text.size();
}

std::string upper(std::string s) {
    for (auto &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void print_help() {
    say("commands (prices and quantities are whole numbers; tickers: MOOG BANANA TESLO MACROHARD):\n"
        "  as <trader>                              act as another trader (self-trades are blocked)\n"
        "  buy|sell <TICKER> <qty> [price]          limit order, or market order if no price\n"
        "  cancel <TICKER> <order_id>\n"
        "  modify <TICKER> <order_id> <price> <total_qty>\n"
        "  stop buy|sell <TICKER> <qty> <stop_price> [limit_price]\n"
        "  cancelstop <TICKER> <order_id>\n"
        "  help | quit");
}

} // namespace

int main() {
    // Engine time in milliseconds from a clock that never jumps backwards
    // (steady_clock), so the circuit-breaker timers behave.
    Worker::Clock clock = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

    // --- 1. build and configure the router (single thread, before start) ---
    Router router(kWorkers, clock);
    std::unordered_map<std::string, SymbolId> id_of;     // gateway: ticker -> id
    std::unordered_map<SymbolId, std::string> ticker_of; // publisher: id -> ticker
    for (const auto &cfg : kSymbols) {
        router.add_symbol(cfg.id, cfg.worker, std::make_unique<engine::Book>(100000, cfg.band_bps));
        id_of.emplace(std::string(cfg.ticker), cfg.id);
        ticker_of.emplace(cfg.id, std::string(cfg.ticker));
    }
    // Both maps are only read from here on, so two threads may share them.
    router.start();

    // --- 2. publisher: empties the outbox until the poison pill -------------
    // Declared AFTER router: destroyed (joined) BEFORE router.
    std::jthread publisher([&router, &ticker_of] {
        while (true) {
            Output out = router.outbox().pop();
            if (std::holds_alternative<OutboxClosed>(out))
                return;
            if (auto *m = std::get_if<MarketEvent>(&out)) {
                say(format_event(ticker_of.at(m->symbol), m->event));
            } else if (auto *r = std::get_if<Reply>(&out)) {
                std::ostringstream s;
                s << "  reply to request " << r->request_id << ": ";
                if (r->reject_reason == engine::RejectReason::None)
                    s << "OK";
                else
                    s << "REJECTED (" << to_string(r->reject_reason) << ')';
                if (r->unaccepted_quantity > 0)
                    s << ", unaccepted qty " << r->unaccepted_quantity;
                if (r->rested_price)
                    s << ", rested at band edge " << *r->rested_price;
                say(s.str());
            }
        }
    });

    // --- 4. shutdown sequence, run by a destructor --------------------------
    // Declared after `publisher`, so it is destroyed first: on a normal exit
    // AND if anything below throws. Without it, an exception would make
    // ~jthread join a publisher that is asleep in pop() forever.
    struct ShutdownInOrder {
        Router &router;
        std::jthread &publisher;
        ~ShutdownInOrder() {
            router.shutdown();                    // workers drain + join
            router.outbox().push(OutboxClosed{}); // pill goes in last
            if (publisher.joinable())
                publisher.join();
        }
    } shutdown_guard{router, publisher};

    // --- 3. gateway: keyboard -> Command -> router.submit -------------------
    // The gateway assigns every id. Clients never pick engine ids.
    RequestId next_request = 1;
    engine::OrderId next_order = 1;
    engine::OwnerId trader = 1;

    say("exchange_server: " + std::to_string(std::size(kSymbols)) + " symbols on " + std::to_string(kWorkers) +
        " workers. Type 'help'.");

    auto submit = [&](Command cmd, RequestId req) {
        switch (router.submit(std::move(cmd))) {
        case SubmitResult::Queued:
            say("request " + std::to_string(req) + " queued");
            break;
        case SubmitResult::UnknownSymbol:
            say("unknown symbol");
            break;
        case SubmitResult::NotRunning:
            say("server is not running");
            break;
        }
    };

    std::string line;
    while (true) {
        {
            std::lock_guard lock(g_print_mu);
            std::cout << "trader " << trader << "> " << std::flush;
        }
        if (!std::getline(std::cin, line))
            break; // Ctrl-D: same as quit

        std::istringstream in(line);
        std::vector<std::string> w;
        for (std::string tok; in >> tok;)
            w.push_back(tok);
        if (w.empty())
            continue;
        const std::string verb = w[0];

        // ticker -> SymbolId at the edge. An unknown ticker never reaches the
        // router (the router would also say UnknownSymbol, with id 0).
        auto symbol_of = [&](const std::string &t) -> std::optional<SymbolId> {
            auto it = id_of.find(upper(t));
            if (it == id_of.end())
                return std::nullopt;
            return it->second;
        };

        std::int64_t a = 0, b = 0, c = 0;
        if (verb == "quit" || verb == "exit") {
            break;
        } else if (verb == "help") {
            print_help();
        } else if (verb == "as" && w.size() == 2 && parse_int(w[1], a) && a > 0) {
            trader = static_cast<engine::OwnerId>(a);
        } else if ((verb == "buy" || verb == "sell") && (w.size() == 3 || w.size() == 4) && parse_int(w[2], a) &&
                   (w.size() == 3 || parse_int(w[3], b))) {
            auto sym = symbol_of(w[1]);
            if (!sym) {
                say("unknown ticker " + w[1]);
                continue;
            }
            engine::Order o{};
            o.id = next_order++;
            o.owner_id = trader;
            o.side = verb == "buy" ? engine::Side::Buy : engine::Side::Sell;
            o.quantity = a;
            if (w.size() == 4) {
                o.type = engine::OrderType::Limit;
                o.price = b;
            } else {
                o.type = engine::OrderType::Market; // no price: rejected if one is set
            }
            say("order id " + std::to_string(o.id));
            RequestId req = next_request++;
            submit(NewOrder{req, *sym, o}, req);
        } else if (verb == "cancel" && w.size() == 3 && parse_int(w[2], a)) {
            auto sym = symbol_of(w[1]);
            if (!sym) {
                say("unknown ticker " + w[1]);
                continue;
            }
            RequestId req = next_request++;
            submit(CancelOrder{.request_id = req, .symbol = *sym, .requester = trader,
                               .order_id = static_cast<engine::OrderId>(a)},
                   req);
        } else if (verb == "modify" && w.size() == 5 && parse_int(w[2], a) && parse_int(w[3], b) &&
                   parse_int(w[4], c)) {
            auto sym = symbol_of(w[1]);
            if (!sym) {
                say("unknown ticker " + w[1]);
                continue;
            }
            RequestId req = next_request++;
            submit(ModifyOrder{.request_id = req, .symbol = *sym, .requester = trader,
                               .order_id = static_cast<engine::OrderId>(a), .new_price = b, .new_quantity = c},
                   req);
        } else if (verb == "stop" && (w.size() == 5 || w.size() == 6) && (w[1] == "buy" || w[1] == "sell") &&
                   parse_int(w[3], a) && parse_int(w[4], b) && (w.size() == 5 || parse_int(w[5], c))) {
            auto sym = symbol_of(w[2]);
            if (!sym) {
                say("unknown ticker " + w[2]);
                continue;
            }
            engine::StopOrder s{};
            s.id = next_order++;
            s.owner_id = trader;
            s.side = w[1] == "buy" ? engine::Side::Buy : engine::Side::Sell;
            s.quantity = a;
            s.stop_price = b;
            if (w.size() == 6)
                s.limit_price = c;
            say("stop order id " + std::to_string(s.id));
            RequestId req = next_request++;
            submit(PlaceStop{req, *sym, s}, req);
        } else if (verb == "cancelstop" && w.size() == 3 && parse_int(w[2], a)) {
            auto sym = symbol_of(w[1]);
            if (!sym) {
                say("unknown ticker " + w[1]);
                continue;
            }
            RequestId req = next_request++;
            submit(CancelStop{.request_id = req, .symbol = *sym, .requester = trader,
                              .order_id = static_cast<engine::OrderId>(a)},
                   req);
        } else {
            say("could not parse that. Type 'help'.");
        }
    }

    say("shutting down...");
    return 0; // ~ShutdownInOrder runs here: shutdown, pill, join
}
