# Order Book Engine

A C++20 limit order book library with price-time priority, market orders, dormant stop and stop-limit orders, cancellation, bounded node allocation, and Cancel Newest self-trade prevention.

## Features

- Price-time priority: bids are ordered highest-first; asks are ordered lowest-first.
- Limit orders match crossing liquidity and rest when quantity remains.
- Market orders consume available opposing liquidity and never rest.
- Stop and stop-limit orders remain dormant until a trade price reaches their inclusive trigger.
- Cancel Newest self-trade prevention rejects an incoming order's unmatched remainder when it reaches liquidity from the same owner.
- A fixed-capacity node pool bounds the number of resting orders.
- `OrderResult` reports fills, rejected quantity, and the rejection reason.

## Requirements

- CMake 3.20 or newer
- A C++20 compiler

## Build

```sh
cmake -S . -B build
cmake --build build
```

The static library target is `engine`.

## Running engine_node

From `trading-project/`, build and start the sequencer first, then the engine node:

```sh
cmake -S sequencer -B sequencer/build
cmake --build sequencer/build -j2
cmake -S order_book_engine -B order_book_engine/build-node
cmake --build order_book_engine/build-node -j2
./sequencer/build/sequencer                        # in one terminal
./order_book_engine/build-node/server/engine_node  # in another; Ctrl-C to stop
```

`engine_node [sequencer_address] [feed_address]` defaults to `127.0.0.1:50051` and `127.0.0.1:50061`. It reads commands from the sequencer and serves its results on the engine feed (below). It prints startup and shutdown messages, reconnect attempts when the sequencer is unavailable, gateways subscribing and leaving, and the resulting market events and replies (set `ENGINE_NODE_QUIET=1` to stop printing events and replies, e.g. for a load test). If a feed gives up, it shuts down cleanly and exits with code 1.

Orders will come from the gateway (not built yet); for now, tests send them. A fresh node needs **every** command starting at sequence 1, while the relay keeps no history: start the node before sending any orders. Restarting the sequencer after commands have reached a running node causes its feed to stop with `OUT_OF_RANGE` by design; without those commands, it can reconnect.

### Instruments come from one shared file

Which symbols exist, and which worker owns each one, is read at startup from
`trading-project/config/instruments.json` — the same file the sequencer reads (see `../config/README.md`).
Nothing is hardcoded. Override the location with `EXCHANGE_INSTRUMENTS=/path/to/instruments.json`.
Worker `i` owns the instruments whose `partition` is `i`. A bad file stops `engine_node` and `exchange_server` at startup
with the file, entry and field in the message. If the sequencer runs a different version of the file and sends a symbol
this engine does not own, the feed stops with "config mismatch" instead of guessing.

## Core types

```cpp
using OrderId = std::uint64_t;
using OwnerId = std::uint64_t;
using Price = std::int64_t;
using Quantity = std::int64_t;

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };
```

`Price` is represented as an integer. Choose and consistently apply an external scale (for example, cents or ticks).

## Usage

```cpp
#include "engine/book.hpp"

engine::Book book;

// Rest 10 units offered at price 10,100.
engine::OrderResult ask = book.add_order({
    1,                    // order ID
    42,                   // owner ID
    engine::Side::Sell,
    engine::OrderType::Limit,
    10100,                // price in ticks/cents
    10                    // quantity
}, 0);                    // timestamp

// Buy 4 units. This executes at the resting ask price.
engine::OrderResult buy = book.add_order({
    2,
    7,
    engine::Side::Buy,
    engine::OrderType::Limit,
    10100,
    4
}, 1);

for (const engine::Trade& trade : buy.trades) {
    // trade.price == 10100; trade.quantity == 4
}
```

An `Order` aggregate has this field order:

```cpp
OrderId id;
OwnerId owner_id;
Side side;
OrderType type;
std::optional<Price> price;
Quantity quantity;
Quantity filled = 0;      // engine-maintained
```

A limit order must carry a price. A market order has none: pass `std::nullopt`. A market order that carries a price is rejected with `InvalidPrice` rather than having the price silently ignored.

## Order results and rejections

`Book::add_order` returns:

```cpp
struct OrderResult {
    std::vector<Trade> trades;
    Quantity unaccepted_quantity;
    RejectReason reject_reason;
};
```

`RejectReason` can be `None`, `InvalidPrice`, `InvalidQuantity`, `SelfTrade`, `PoolExhausted`, `SymbolHalted`, `UnknownOrder`, `DuplicateOrderId`, `TooLate`, `PriceBand`, `PriceCollar`, or `StopWouldTrigger`.

- A limit price must be positive.
- Quantity must be positive.
- A limit order that cannot be stored because the pool is exhausted returns `PoolExhausted`.
- An order or stop whose id belongs to a live resting order, a dormant stop, or an order that already finished this session is rejected with `DuplicateOrderId` before matching. A finished id cannot be reused within a session. (Long term the gateway should assign engine ids and keep the client's own id separately.)
- If self-trade prevention stops matching, already-executed trades are retained and the unmatched amount is returned with `SelfTrade`.
- On the normal accepted path for a limit order, `unaccepted_quantity` is `0`; any unfilled quantity has been placed on the book. For a market order, quantity with no liquidity left is cancelled and reported in `unaccepted_quantity` (with `RejectReason::None`, since cancelling the rest is not a rejection).

## Order ownership

`cancel_order`, `modify_order`, `cancel_stop_order` and `modify_stop_order` all take the id of the account making the request, right after the order id (e.g. `cancel_order(id, requester, now)`). The engine records the owner of every id when it is accepted (`Accepted` / `StopAccepted`) and keeps it for the session.

- Only the owner may cancel or modify an order or stop.
- Anyone else gets `UnknownOrder`, exactly as if the id had never existed. That includes finished orders: `TooLate` is only ever told to the owner, so a reply never reveals whether another account's order exists, rests, filled or was cancelled.
- The ownership check runs before any other validation, so a non-owner's malformed modify still gets `UnknownOrder`, not `InvalidQuantity`.
- The requester id must come from the authenticated connection (the gateway), never from the client's message. Real venues go further and scope order ids per account (FIX `ClOrdID`, Nasdaq OUCH order tokens), so another account's order cannot even be named; this engine check is the backstop behind that.

## Self-trade prevention

The engine applies the **Cancel Newest** policy. While walking opposing liquidity, if the incoming order encounters a resting order with the same `owner_id`, matching stops immediately. The resting order stays untouched and preserves its queue position. Any quantity not already executed is rejected with `RejectReason::SelfTrade`; it does not rest on the book.

## Stop orders

Stop orders are submitted separately and sit dormant until a trade reaches their stop price. A stop-market then fires a market order; a stop-limit fires a limit order at its limit price, which may rest if it cannot fill.

```cpp
book.place_stop_order({
    3,                    // stop order ID
    99,                   // owner ID
    engine::Side::Sell,
    9900,                 // stop (trigger) price
    5,                    // quantity
    9850                  // optional limit price; omit for a stop-market
}, 0);
```

- A stop-sell triggers when the lowest price traded in a match `<= stop_price`.
- A stop-buy triggers when the highest price traded in a match `>= stop_price`.
  (A single sweep can trade through several prices; a stop fires if any of them touches it.)
- Trigger checks occur only after a real trade.
- **Entry check.** A stop whose trigger the last trade has already reached (sell: last `<=` stop, buy: last `>=` stop) is rejected with `StopWouldTrigger`, like Binance's "Order would trigger immediately". Before the first trade there is nothing to compare against, so any stop is accepted. The same check applies to `modify_stop_order`.
- A triggered stop fires under its own id. A stop-limit that rests is an ordinary resting order from then on: cancel or modify it with `cancel_order` / `modify_order`.
- `cancel_stop_order(id, requester, now)` returns `None` if it cancelled a dormant stop, `TooLate` if that id was a stop that has already triggered or been cancelled, and `UnknownOrder` if no stop was ever placed with that id (or the requester is not its owner, see Order ownership).
- `modify_stop_order(id, requester, new_stop_price, new_limit_price, new_qty, now)` replaces all three fields (an empty limit price makes it a stop-market). Same return rules as `cancel_stop_order`; on any rejection the stop is unchanged. Only a pure size reduction keeps the stop's place in the firing order; any other change moves it to the back.
- **Firing order.** Stops woken by the same sweep fire in entry order, across both sides. Stops fire in the order they **triggered** (breadth-first): if stops A and B trigger on the same trade and A's fill then triggers C, the order is A, B, C. Only the outermost call drains the queue; a triggered stop's own fills only append to it, so a long cascade never nests calls.
- While the symbol is halted, stops are not checked. If a batch of triggered stops trips the halt part-way through, the stops that had not fired yet go back to dormant, ahead of every stop that never triggered. After the halt they are re-evaluated against post-halt trades like any other stop, so a stop can stay dormant if the market reopens on the other side of its stop price. This is a deliberate policy, pinned by a test; revisit it with the reopening auction.

**Storage.** Dormant stops live in two `std::map`s (one per side) keyed by `(stop price, priority)`, plus an id index. A trade touches only the stops it wakes: `O(log n + k log k)` for `k` woken stops out of `n`, instead of scanning all `n`. Duplicate-id checks, cancel and modify are `O(1)` lookups plus `O(log n)` map updates. The priority is the entry sequence; stops sent back by a halt get priorities from a second counter that counts down from the middle of the range, which puts them ahead of all other dormant stops.

## Price protection

The book has a LULD-style price band (`band_bps`, a percentage of the last trade price in basis points), plus two per-order protections.

- **Grace print, then halt.** The first fill outside the band is allowed and starts a grace clock. An out-of-band fill after `grace_period_ms` halts the symbol for `halt_duration_ms`. An in-band fill resets the clock.
- **One breach level per sweep.** One order may trade at only one out-of-band price level (all resting orders at that price). It may not walk on to a further level, so a single large order cannot move the price 10x in one call. The walk stops; a limit remainder rests at the band edge (`rested_price`), and a market remainder is cancelled with `RejectReason::PriceBand`.
- **Market-order collar** (`market_collar_bps`, 5th constructor argument, `0` = off). A market order never fills beyond the last trade price +/- the collar; its remainder is cancelled with `RejectReason::PriceCollar`. Limit orders are not collared, because their limit price is already a bound. There is no collar before the first trade.

Real LULD is stricter (no trades outside the band at all; a limit state, then a pause and a reopening auction). That model is deferred to the call-auction work.

## Event stream

`drain_events()` returns every state change since the last drain, each with a per-book `sequence_number` (1, 2, 3... with no gaps). `OrderResult` is the private reply to the sender; the event stream is the public record for the ledger, market data and replay.

- Every `Accepted` ends in exactly one outcome: filled by its own trades, `Rested`, or `Cancelled`.
- A price change or size increase via `modify_order` emits `Replaced` (carrying the price and quantity that left the book), then `Accepted` for the new version. `Replaced` is not terminal; only `Cancelled` is.
- `Halted` and `Resumed` are symbol-level. `Resumed` carries no order fields.
- `price` is a `std::optional<Price>`: empty for a market order's `Accepted` / `Cancelled` and for `Resumed`. Stop events (`StopAccepted`, `StopModified`, `StopTriggered`, `StopCancelled`) carry the stop price in `price` and a stop-limit's limit in `limit_price`.

## Engine feed (the output side)

Two gRPC streams in `../proto/exchange/v1/engine_feed.proto`, one for public data and one for
private answers. This link is internal: events and replies keep owner/account ids, and gateways
must strip them before anything goes public.

- `EngineFeed.Subscribe(gateway_id, symbols)` (empty `symbols` = every symbol), used by the
  market-data gateway. For each symbol a subscriber first gets one `BookSnapshot`, then every event
  for that symbol numbered `as_of_seq + 1`, `+ 2`, ... with no gaps. No replies.
- `EngineFeed.SubscribeReplies(gateway_id)`, used by the order gateway: only the `Reply`s whose
  `gateway_id` is its own (the sequencer stamps `gateway_id` on each command and the worker copies
  it into the reply). No snapshots, no events. Replies are not numbered and never resent; a reply
  for a gateway with no replies stream is dropped and counted.

Why two streams: each subscriber has its own bounded queue. When replies shared a queue with market
events, a burst of events (a crash on a busy symbol) could fill it, get the gateway dropped, and
lose the replies queued behind the events. Now a market burst can only drop a market stream.

- **Snapshot = the whole book, order by order** (`Book::snapshot()`): every resting order in queue
  order with remaining and filled quantity, dormant stops, last trade price, halted flag, and
  `as_of_sequence`. Per-price totals would not be enough: the events name individual orders, and a
  gateway needs each owner's open orders after it restarts.
- **Only the worker thread touches a Book.** A snapshot is requested with an internal `TakeSnapshot`
  command in the worker's inbox. The worker answers into the same outbox as its events, so every
  event after the snapshot in the outbox is newer than it. `TakeSnapshot` changes nothing, so it
  needs no sequence number and is never journaled; replaying inputs still gives the same events.
- **No replay.** To recover from anything (a dropped connection, a gap, being dropped), subscribe
  again and start from fresh snapshots.
- **Slow gateways are dropped, never waited for.** `MarketDataPublisher` (one thread) drains the
  outbox into one bounded queue per subscriber (65,536 items). A full queue ends that subscription
  (`RESOURCE_EXHAUSTED`); everyone else is unaffected and the engine never slows down.
- **One stream of each kind per gateway id** (`ALREADY_EXISTS` otherwise); replies need one
  destination. The two kinds have separate id namespaces. Other codes: `INVALID_ARGUMENT` empty id,
  `NOT_FOUND` unknown symbol (Subscribe), `UNAVAILABLE` shutting down. A dropped replies stream
  (`RESOURCE_EXHAUSTED`) loses its queued replies: the gateway treats those outcomes as unknown.
- Shutdown order: sequencer feeds -> workers -> outbox pill -> publisher (ends every subscription)
  -> feed server.

Tested by a replica (`engine/tests/book_replica.hpp`) that rebuilds a book from snapshot + events
only: in a 6,000-step random run (halts and stop cascades included) it equals `Book::snapshot()`
after every step, both from the start and when joining halfway; and with 2 workers, 4 producer
threads and gateways joining mid-stream, every subscriber's replica of every book equals the final
book, no market stream carries a reply, and each gateway's replies stream got exactly one reply per
request.

## Book queries

```cpp
std::optional<engine::Price> bid = book.best_bid();
std::optional<engine::Price> ask = book.best_ask();
```

An empty optional means that side of the book has no resting liquidity.

## Capacity

`Book` defaults to a 100,000-node resting-order pool. Supply a smaller or larger capacity when constructing the book:

```cpp
engine::Book book{1000};
```

Only resting limit orders consume pool nodes. Stop orders are stored separately. Each node is 80 bytes (the order, list links, and a pointer to its price level, which saves a map lookup on every fill and cancel).

## Source layout

- `engine/src/book.cpp`: order entry, matching, modify, cancel.
- `engine/src/stops.cpp`: stop and stop-limit entry, cancel, modify, trigger and firing.
- `engine/src/book_levels.cpp`: price levels (linking, totals), depth and full snapshots, validation helpers, `check_invariants`.
