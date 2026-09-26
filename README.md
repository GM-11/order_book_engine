# Order Book Engine

A C++20 limit order book library with price-time priority, market orders, dormant stop orders, cancellation, bounded node allocation, and Cancel Newest self-trade prevention.

## Features

- Price-time priority: bids are ordered highest-first; asks are ordered lowest-first.
- Limit orders match crossing liquidity and rest when quantity remains.
- Market orders consume available opposing liquidity and never rest.
- Stop orders remain dormant until a trade price reaches their inclusive trigger.
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
    10                     // quantity
});

// Buy 4 units. This executes at the resting ask price.
engine::OrderResult buy = book.add_order({
    2,
    7,
    engine::Side::Buy,
    engine::OrderType::Limit,
    10100,
    4
});

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
Price price;
Quantity quantity;
```

For market orders, `price` is unused and should conventionally be `0`.

## Order results and rejections

`Book::add_order` returns:

```cpp
struct OrderResult {
    std::vector<Trade> trades;
    Quantity unaccepted_quantity;
    RejectReason reject_reason;
};
```

`RejectReason` can be `None`, `InvalidPrice`, `InvalidQuantity`, `SelfTrade`, `PoolExhausted`, `SymbolHalted`, `UnknownOrder`, or `DuplicateOrderId`.

- A limit price must be positive.
- Quantity must be positive.
- A limit order that cannot be stored because the pool is exhausted returns `PoolExhausted`.
- An order or stop whose id belongs to a live resting order or a dormant stop is rejected with `DuplicateOrderId` before matching. Ids are checked against live orders only; a filled or cancelled id can be reused. (Long term the gateway should assign engine ids and keep the client's own id separately.)
- If self-trade prevention stops matching, already-executed trades are retained and the unmatched amount is returned with `SelfTrade`.
- On the normal accepted path for a limit order, `unaccepted_quantity` is `0`; any unfilled quantity has been placed on the book. For a market order, quantity with no liquidity left is cancelled and reported in `unaccepted_quantity` (with `RejectReason::None`, since cancelling the rest is not a rejection).

## Self-trade prevention

The engine applies the **Cancel Newest** policy. While walking opposing liquidity, if the incoming order encounters a resting order with the same `owner_id`, matching stops immediately. The resting order stays untouched and preserves its queue position. Any quantity not already executed is rejected with `RejectReason::SelfTrade`; it does not rest on the book.

## Stop orders

Stop orders are submitted separately and become market orders only when a trade triggers them:

```cpp
book.place_stop_order({
    3,                    // stop order ID
    99,                   // owner ID
    engine::Side::Sell,
    9900,                // stop price
    5
}, 0);
```

- A stop-sell triggers when the lowest price traded in a match `<= stop_price`.
- A stop-buy triggers when the highest price traded in a match `>= stop_price`.
  (A single sweep can trade through several prices; a stop fires if any of them touches it.)
- Trigger checks occur only after a real trade.
- Triggered stops are removed from the dormant list before their market orders are submitted. This prevents duplicate triggering and makes recursive cascades safe.
- `cancel_stop_order(order_id, now)` cancels a dormant stop; `cancel_order(order_id, now)` cancels a resting limit order.
- While the symbol is halted, stops are not checked. If a batch of triggered stops trips the halt part-way through, the stops that had not fired yet go back to the front of the dormant list. After the halt they are re-evaluated against post-halt trades like any other stop, so a stop can stay dormant if the market reopens on the other side of its stop price. This is a deliberate policy, pinned by a test; revisit it with the reopening auction.

## Event stream

`drain_events()` returns every state change since the last drain, each with a per-book `sequence_number` (1, 2, 3... with no gaps). `OrderResult` is the private reply to the sender; the event stream is the public record for the ledger, market data and replay.

- Every `Accepted` ends in exactly one outcome: filled by its own trades, `Rested`, or `Cancelled`.
- A price change or size increase via `modify_order` emits `Replaced` (carrying the price and quantity that left the book), then `Accepted` for the new version. `Replaced` is not terminal; only `Cancelled` is.
- `Halted` and `Resumed` are symbol-level. `Resumed` carries no order fields.

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

Only resting limit orders consume pool nodes. Stop orders are stored separately.
