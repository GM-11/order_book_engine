// encode_feed_item: engine output -> exchange.v1.FeedMessage bytes.
#include "server/feed_codec.hpp"

#include "exchange/v1/engine_feed.pb.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace server;
namespace pb = exchange::v1;
using engine::EventKind;
using engine::RejectReason;

namespace {
pb::FeedMessage decode(const FeedItem &item) {
    pb::FeedMessage msg;
    REQUIRE(msg.ParseFromString(encode_feed_item(item)));
    return msg;
}
} // namespace

TEST_CASE("A trade event keeps every field, owners included", "[feed_codec]") {
    engine::EngineEvent e{};
    e.sequence_number = 41;
    e.kind = EventKind::Trade;
    e.ts = 1234;
    e.order_id = 501;
    e.passive_id = 410;
    e.owner_id = 7;
    e.other_owner = 9;
    e.side = engine::Side::Buy;
    e.price = 100;
    e.quantity = 10;
    const pb::FeedMessage msg = decode(MarketEvent{3, e});

    REQUIRE(msg.has_event());
    const pb::MarketEvent &m = msg.event();
    CHECK(m.symbol() == 3);
    CHECK(m.seq() == 41);
    CHECK(m.kind() == pb::EVENT_KIND_TRADE);
    CHECK(m.ts() == 1234);
    CHECK(m.order_id() == 501);
    CHECK(m.passive_id() == 410);
    CHECK(m.owner_id() == 7);
    CHECK(m.other_owner() == 9);
    CHECK(m.side() == pb::SIDE_BUY);
    REQUIRE(m.has_price());
    CHECK(m.price() == 100);
    CHECK(m.quantity() == 10);
    CHECK_FALSE(m.has_limit_price());
}

TEST_CASE("An absent price stays absent; price 0 is not the same as no price", "[feed_codec]") {
    engine::EngineEvent e{};
    e.kind = EventKind::Resumed;
    e.price = std::nullopt;
    CHECK_FALSE(decode(MarketEvent{1, e}).event().has_price());

    e.kind = EventKind::StopAccepted;
    e.side = engine::Side::Sell;
    e.price = 95;
    e.limit_price = 93;
    const auto m = decode(MarketEvent{1, e}).event();
    CHECK(m.side() == pb::SIDE_SELL);
    CHECK(m.price() == 95);
    REQUIRE(m.has_limit_price());
    CHECK(m.limit_price() == 93);
}

TEST_CASE("Every event kind maps to its own wire value, never UNSPECIFIED", "[feed_codec]") {
    const std::vector<std::pair<EventKind, pb::EventKind>> all{
        {EventKind::Accepted, pb::EVENT_KIND_ACCEPTED},
        {EventKind::Rested, pb::EVENT_KIND_RESTED},
        {EventKind::Trade, pb::EVENT_KIND_TRADE},
        {EventKind::Cancelled, pb::EVENT_KIND_CANCELLED},
        {EventKind::Modified, pb::EVENT_KIND_MODIFIED},
        {EventKind::Replaced, pb::EVENT_KIND_REPLACED},
        {EventKind::StopAccepted, pb::EVENT_KIND_STOP_ACCEPTED},
        {EventKind::StopTriggered, pb::EVENT_KIND_STOP_TRIGGERED},
        {EventKind::StopCancelled, pb::EVENT_KIND_STOP_CANCELLED},
        {EventKind::StopModified, pb::EVENT_KIND_STOP_MODIFIED},
        {EventKind::Halted, pb::EVENT_KIND_HALTED},
        {EventKind::Resumed, pb::EVENT_KIND_RESUMED},
    };
    std::set<int> seen;
    for (const auto &[kind, wire] : all) {
        engine::EngineEvent e{};
        e.kind = kind;
        const auto got = decode(MarketEvent{1, e}).event().kind();
        CHECK(got == wire);
        seen.insert(got);
    }
    CHECK(seen.size() == all.size());
    CHECK(pb::EventKind_ARRAYSIZE == static_cast<int>(all.size()) + 1); // + UNSPECIFIED
}

TEST_CASE("Every reject reason maps to its own wire value, never UNSPECIFIED", "[feed_codec]") {
    const std::vector<std::pair<RejectReason, pb::RejectReason>> all{
        {RejectReason::None, pb::REJECT_REASON_NONE},
        {RejectReason::InvalidPrice, pb::REJECT_REASON_INVALID_PRICE},
        {RejectReason::InvalidQuantity, pb::REJECT_REASON_INVALID_QUANTITY},
        {RejectReason::SelfTrade, pb::REJECT_REASON_SELF_TRADE},
        {RejectReason::PoolExhausted, pb::REJECT_REASON_POOL_EXHAUSTED},
        {RejectReason::SymbolHalted, pb::REJECT_REASON_SYMBOL_HALTED},
        {RejectReason::UnknownOrder, pb::REJECT_REASON_UNKNOWN_ORDER},
        {RejectReason::DuplicateOrderId, pb::REJECT_REASON_DUPLICATE_ORDER_ID},
        {RejectReason::TooLate, pb::REJECT_REASON_TOO_LATE},
        {RejectReason::PriceBand, pb::REJECT_REASON_PRICE_BAND},
        {RejectReason::PriceCollar, pb::REJECT_REASON_PRICE_COLLAR},
        {RejectReason::StopWouldTrigger, pb::REJECT_REASON_STOP_WOULD_TRIGGER},
    };
    std::set<int> seen;
    for (const auto &[reason, wire] : all) {
        Reply r{};
        r.reject_reason = reason;
        const auto got = decode(r).reply().reject_reason();
        CHECK(got == wire);
        seen.insert(got);
    }
    CHECK(seen.size() == all.size());
    CHECK(pb::RejectReason_ARRAYSIZE == static_cast<int>(all.size()) + 1);
}

TEST_CASE("A reply keeps its gateway, request and outcome", "[feed_codec]") {
    Reply r{};
    r.gateway_id = "gw-a";
    r.request_id = 88;
    r.symbol = 2;
    r.reject_reason = RejectReason::PriceBand;
    r.unaccepted_quantity = 4;
    r.rested_price = 110;
    const auto m = decode(r).reply();
    CHECK(m.gateway_id() == "gw-a");
    CHECK(m.request_id() == 88);
    CHECK(m.symbol() == 2);
    CHECK(m.reject_reason() == pb::REJECT_REASON_PRICE_BAND);
    CHECK(m.unaccepted_quantity() == 4);
    REQUIRE(m.has_rested_price());
    CHECK(m.rested_price() == 110);

    r.rested_price = std::nullopt;
    CHECK_FALSE(decode(r).reply().has_rested_price());
}

TEST_CASE("A snapshot keeps every order in order, stops, last trade and halt", "[feed_codec]") {
    SnapshotReady ready{};
    ready.symbol = 5;
    ready.subscriber = 1;
    ready.snapshot.as_of_sequence = 77;
    ready.snapshot.bids = {{1, 11, 101, 3, 0}, {2, 12, 100, 5, 2}};
    ready.snapshot.asks = {{3, 13, 102, 6, 0}};
    ready.snapshot.stops = {{9, 19, engine::Side::Sell, 90, std::nullopt, 1},
                            {8, 18, engine::Side::Buy, 110, engine::Price{112}, 3}};
    ready.snapshot.last_trade_price = 101;
    ready.snapshot.halted = true;

    const pb::FeedMessage msg = decode(ready);
    REQUIRE(msg.has_snapshot());
    const pb::BookSnapshot &s = msg.snapshot();
    CHECK(s.symbol() == 5);
    CHECK(s.as_of_seq() == 77);
    REQUIRE(s.bids_size() == 2);
    CHECK(s.bids(0).order_id() == 1);
    CHECK(s.bids(0).price() == 101);
    CHECK(s.bids(1).order_id() == 2);
    CHECK(s.bids(1).owner_id() == 12);
    CHECK(s.bids(1).quantity() == 5);
    CHECK(s.bids(1).filled() == 2);
    REQUIRE(s.asks_size() == 1);
    CHECK(s.asks(0).price() == 102);
    REQUIRE(s.stops_size() == 2);
    CHECK(s.stops(0).side() == pb::SIDE_SELL);
    CHECK_FALSE(s.stops(0).has_limit_price());
    CHECK(s.stops(1).side() == pb::SIDE_BUY);
    CHECK(s.stops(1).limit_price() == 112);
    CHECK(s.stops(1).quantity() == 3);
    REQUIRE(s.has_last_trade_price());
    CHECK(s.last_trade_price() == 101);
    CHECK(s.halted());

    SnapshotReady empty{};
    CHECK_FALSE(decode(empty).snapshot().has_last_trade_price());
}
