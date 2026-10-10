#include "cli/console.hpp"
#include "config/instruments.hpp"
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
using cli::say;

namespace {

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

void print_help(const std::string &tickers) {
    say("commands (prices and quantities are whole numbers; tickers: " + tickers +
        "):\n"
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
    // --- 0. the instrument list: the same file the sequencer reads ----------
    const std::string instruments_path = config::instruments_path();
    config::InstrumentConfig instruments;
    try {
        instruments = config::load_instruments(instruments_path);
    } catch (const config::ConfigError &error) {
        std::cerr << "exchange_server: " << error.what() << '\n';
        return 1;
    }
    std::string all_tickers; // for 'help'
    for (const auto &instrument : instruments.instruments)
        all_tickers += (all_tickers.empty() ? "" : " ") + instrument.ticker;

    // Engine time in milliseconds from a clock that never jumps backwards
    // (steady_clock), so the circuit-breaker timers behave.
    Router::Clock clock = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };

    // --- 1. build and configure the router (single thread, before start) ---
    Router router(instruments.partitions, clock);        // one worker per partition
    std::unordered_map<std::string, SymbolId> id_of;     // gateway: ticker -> id
    std::unordered_map<SymbolId, std::string> ticker_of; // publisher: id -> ticker
    for (const auto &instrument : instruments.instruments) {
        router.add_symbol(instrument.id, instrument.partition,
                          std::make_unique<engine::Book>(100000, instrument.band_bps));
        id_of.emplace(instrument.ticker, instrument.id);
        ticker_of.emplace(instrument.id, instrument.ticker);
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
                say(cli::format_event(ticker_of.at(m->symbol), m->event));
            } else if (auto *r = std::get_if<Reply>(&out)) {
                say(cli::format_reply(*r));
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

    say("exchange_server: " + std::to_string(instruments.instruments.size()) + " symbols on " +
        std::to_string(instruments.partitions) + " workers (instruments v" + std::to_string(instruments.version) +
        " from " + instruments_path + "). Type 'help'.");

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
            std::lock_guard lock(cli::g_print_mu);
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
            print_help(all_tickers);
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
            submit(CancelOrder{.client_request_id = req,
                               .symbol = *sym,
                               .requester = trader,
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
            submit(ModifyOrder{.client_request_id = req,
                               .symbol = *sym,
                               .requester = trader,
                               .order_id = static_cast<engine::OrderId>(a),
                               .new_price = b,
                               .new_quantity = c},
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
            submit(CancelStop{.client_request_id = req,
                              .symbol = *sym,
                              .requester = trader,
                              .order_id = static_cast<engine::OrderId>(a)},
                   req);
        } else {
            say("could not parse that. Type 'help'.");
        }
    }

    say("shutting down...");
    return 0; // ~ShutdownInOrder runs here: shutdown, pill, join
}
