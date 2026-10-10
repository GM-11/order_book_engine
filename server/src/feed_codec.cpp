#include "server/feed_codec.hpp"
#include "feed_proto.hpp"

#include <stdexcept>

namespace server {
namespace pb = exchange::v1;
namespace {

template <class> inline constexpr bool always_false = false;

pb::Side to_pb(engine::Side side) {
    switch (side) {
    case engine::Side::Buy:
        return pb::SIDE_BUY;
    case engine::Side::Sell:
        return pb::SIDE_SELL;
    }
    throw std::invalid_argument("unknown Side");
}

pb::EventKind to_pb(engine::EventKind kind) {
    using K = engine::EventKind;
    switch (kind) {
    case K::Accepted:
        return pb::EVENT_KIND_ACCEPTED;
    case K::Rested:
        return pb::EVENT_KIND_RESTED;
    case K::Trade:
        return pb::EVENT_KIND_TRADE;
    case K::Cancelled:
        return pb::EVENT_KIND_CANCELLED;
    case K::Modified:
        return pb::EVENT_KIND_MODIFIED;
    case K::Replaced:
        return pb::EVENT_KIND_REPLACED;
    case K::StopAccepted:
        return pb::EVENT_KIND_STOP_ACCEPTED;
    case K::StopTriggered:
        return pb::EVENT_KIND_STOP_TRIGGERED;
    case K::StopCancelled:
        return pb::EVENT_KIND_STOP_CANCELLED;
    case K::StopModified:
        return pb::EVENT_KIND_STOP_MODIFIED;
    case K::Halted:
        return pb::EVENT_KIND_HALTED;
    case K::Resumed:
        return pb::EVENT_KIND_RESUMED;
    }
    throw std::invalid_argument("unknown EventKind");
}

pb::RejectReason to_pb(engine::RejectReason reason) {
    using R = engine::RejectReason;
    switch (reason) {
    case R::None:
        return pb::REJECT_REASON_NONE;
    case R::InvalidPrice:
        return pb::REJECT_REASON_INVALID_PRICE;
    case R::InvalidQuantity:
        return pb::REJECT_REASON_INVALID_QUANTITY;
    case R::SelfTrade:
        return pb::REJECT_REASON_SELF_TRADE;
    case R::PoolExhausted:
        return pb::REJECT_REASON_POOL_EXHAUSTED;
    case R::SymbolHalted:
        return pb::REJECT_REASON_SYMBOL_HALTED;
    case R::UnknownOrder:
        return pb::REJECT_REASON_UNKNOWN_ORDER;
    case R::DuplicateOrderId:
        return pb::REJECT_REASON_DUPLICATE_ORDER_ID;
    case R::TooLate:
        return pb::REJECT_REASON_TOO_LATE;
    case R::PriceBand:
        return pb::REJECT_REASON_PRICE_BAND;
    case R::PriceCollar:
        return pb::REJECT_REASON_PRICE_COLLAR;
    case R::StopWouldTrigger:
        return pb::REJECT_REASON_STOP_WOULD_TRIGGER;
    }
    throw std::invalid_argument("unknown RejectReason");
}

void fill(const MarketEvent &in, pb::MarketEvent &out) {
    const engine::EngineEvent &e = in.event;
    out.set_symbol(in.symbol);
    out.set_seq(e.sequence_number);
    out.set_kind(to_pb(e.kind));
    out.set_ts(e.ts);
    out.set_order_id(e.order_id);
    out.set_passive_id(e.passive_id);
    out.set_owner_id(e.owner_id);
    out.set_other_owner(e.other_owner);
    out.set_side(to_pb(e.side));
    if (e.price)
        out.set_price(*e.price);
    out.set_quantity(e.quantity);
    if (e.limit_price)
        out.set_limit_price(*e.limit_price);
}

void fill(const Reply &in, pb::Reply &out) {
    out.set_gateway_id(in.gateway_id);
    out.set_client_request_id(in.client_request_id);
    out.set_account_id(in.account_id);
    out.set_order_id(in.order_id);
    out.set_symbol(in.symbol);
    out.set_reject_reason(to_pb(in.reject_reason));
    out.set_unaccepted_quantity(in.unaccepted_quantity);
    if (in.rested_price)
        out.set_rested_price(*in.rested_price);
}

void fill(const SnapshotReady &in, pb::BookSnapshot &out) {
    const engine::BookSnapshot &s = in.snapshot;
    out.set_symbol(in.symbol);
    out.set_as_of_seq(s.as_of_sequence);
    auto orders = [](const std::vector<engine::RestingOrder> &from, auto *to) {
        to->Reserve(static_cast<int>(from.size()));
        for (const engine::RestingOrder &o : from) {
            pb::RestingOrder *p = to->Add();
            p->set_order_id(o.order_id);
            p->set_owner_id(o.owner_id);
            p->set_price(o.price);
            p->set_quantity(o.quantity);
            p->set_filled(o.filled);
        }
    };
    orders(s.bids, out.mutable_bids());
    orders(s.asks, out.mutable_asks());
    for (const engine::DormantStop &d : s.stops) {
        pb::DormantStop *p = out.add_stops();
        p->set_order_id(d.order_id);
        p->set_owner_id(d.owner_id);
        p->set_side(to_pb(d.side));
        p->set_stop_price(d.stop_price);
        if (d.limit_price)
            p->set_limit_price(*d.limit_price);
        p->set_quantity(d.quantity);
    }
    if (s.last_trade_price)
        out.set_last_trade_price(*s.last_trade_price);
    out.set_halted(s.halted);
}

} // namespace

void fill_feed_message(const FeedItem &item, pb::FeedMessage &out) {
    std::visit(
        [&](const auto &x) {
            using T = std::remove_cvref_t<decltype(x)>;
            if constexpr (std::is_same_v<T, SnapshotReady>)
                fill(x, *out.mutable_snapshot());
            else if constexpr (std::is_same_v<T, MarketEvent>)
                fill(x, *out.mutable_event());
            else if constexpr (std::is_same_v<T, Reply>)
                fill(x, *out.mutable_reply());
            else
                static_assert(always_false<T>, "unhandled FeedItem type");
        },
        item);
}

std::string encode_feed_item(const FeedItem &item) {
    pb::FeedMessage msg;
    fill_feed_message(item, msg);
    return msg.SerializeAsString();
}

} // namespace server
