#include "server/codec.hpp"

#include "exchange/v1/commands.pb.h"
#include <climits>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace pb = exchange::v1;
namespace server {
namespace {

template <typename> inline constexpr bool always_false = false;

pb::Side to_pb_side(engine::Side side) {
    switch (side) {
    case engine::Side::Buy:
        return pb::SIDE_BUY;
    case engine::Side::Sell:
        return pb::SIDE_SELL;
    }
    throw std::logic_error("unknown engine side");
}

std::optional<engine::Side> from_pb_side(pb::Side side) {
    switch (side) {
    case pb::SIDE_BUY:
        return engine::Side::Buy;
    case pb::SIDE_SELL:
        return engine::Side::Sell;
    case pb::SIDE_UNSPECIFIED:
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

pb::OrderType to_pb_order_type(engine::OrderType type) {
    switch (type) {
    case engine::OrderType::Limit:
        return pb::ORDER_TYPE_LIMIT;
    case engine::OrderType::Market:
        return pb::ORDER_TYPE_MARKET;
    }
    throw std::logic_error("unknown engine order type");
}

std::optional<engine::OrderType> from_pb_order_type(pb::OrderType type) {
    switch (type) {
    case pb::ORDER_TYPE_LIMIT:
        return engine::OrderType::Limit;
    case pb::ORDER_TYPE_MARKET:
        return engine::OrderType::Market;
    case pb::ORDER_TYPE_UNSPECIFIED:
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

DecodeResult error(std::string message) { return {.command = std::nullopt, .error = std::move(message)}; }

} // namespace

std::string encode_body(const Command &command) {
    pb::CommandBody body;

    std::visit(
        [&](auto &&arg) {
            using T = std::remove_cvref_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, NewOrder>) {
                const auto &o = arg.order;
                auto *p = body.mutable_new_order()->mutable_order();
                p->set_id(o.id);
                p->set_owner_id(o.owner_id);
                p->set_side(to_pb_side(o.side));
                p->set_type(to_pb_order_type(o.type));
                p->set_quantity(o.quantity);
                if (o.price)
                    p->set_price(*o.price);
            } else if constexpr (std::is_same_v<T, CancelOrder>) {
                auto *p = body.mutable_cancel_order();
                p->set_requester(arg.requester);
                p->set_order_id(arg.order_id);
            } else if constexpr (std::is_same_v<T, ModifyOrder>) {
                auto *p = body.mutable_modify_order();
                p->set_requester(arg.requester);
                p->set_order_id(arg.order_id);
                p->set_new_price(arg.new_price);
                p->set_new_quantity(arg.new_quantity);
            } else if constexpr (std::is_same_v<T, PlaceStop>) {
                const auto &o = arg.stop;
                auto *p = body.mutable_place_stop()->mutable_stop();
                p->set_id(o.id);
                p->set_owner_id(o.owner_id);
                p->set_side(to_pb_side(o.side));
                p->set_stop_price(o.stop_price);
                p->set_quantity(o.quantity);
                if (o.limit_price)
                    p->set_limit_price(*o.limit_price);
            } else if constexpr (std::is_same_v<T, CancelStop>) {
                auto *p = body.mutable_cancel_stop();
                p->set_requester(arg.requester);
                p->set_order_id(arg.order_id);
            } else if constexpr (std::is_same_v<T, ModifyStop>) {
                auto *p = body.mutable_modify_stop();
                p->set_requester(arg.requester);
                p->set_order_id(arg.order_id);
                p->set_new_stop_price(arg.new_stop_price);
                if (arg.new_limit_price)
                    p->set_new_limit_price(*arg.new_limit_price);
                p->set_new_quantity(arg.new_quantity);
            } else if constexpr (std::is_same_v<T, Shutdown>) {
                throw std::invalid_argument("Shutdown commands cannot be encoded");
            } else if constexpr (std::is_same_v<T, TakeSnapshot>) {
                throw std::invalid_argument("TakeSnapshot commands cannot be encoded");
            } else {
                static_assert(always_false<T>, "unhandled Command type");
            }
        },
        command);

    return body.SerializeAsString();
}

DecodeResult decode_body(SymbolId symbol, RequestId request_id, std::string_view payload) {
    if (payload.size() > static_cast<std::size_t>(INT_MAX))
        return error("payload exceeds maximum protobuf size");

    pb::CommandBody body;
    if (!body.ParseFromArray(payload.data(), static_cast<int>(payload.size())))
        return error("payload does not parse");

    switch (body.body_case()) {
    case pb::CommandBody::kNewOrder: {
        const auto &p = body.new_order().order();
        const auto side = from_pb_side(p.side());
        if (!side)
            return error("invalid order side");
        const auto type = from_pb_order_type(p.type());
        if (!type)
            return error("invalid order type");
        return {.command = Command{NewOrder{
                    .request_id = request_id,
                    .symbol = symbol,
                    .order = engine::Order{.id = p.id(),
                                           .owner_id = p.owner_id(),
                                           .side = *side,
                                           .type = *type,
                                           .price = p.has_price() ? std::optional{p.price()} : std::nullopt,
                                           .quantity = p.quantity()}}},
                .error = {}};
    }
    case pb::CommandBody::kCancelOrder: {
        const auto &p = body.cancel_order();
        return {.command = Command{CancelOrder{
                    .request_id = request_id, .symbol = symbol, .requester = p.requester(), .order_id = p.order_id()}},
                .error = {}};
    }
    case pb::CommandBody::kModifyOrder: {
        const auto &p = body.modify_order();
        return {.command = Command{ModifyOrder{.request_id = request_id,
                                               .symbol = symbol,
                                               .requester = p.requester(),
                                               .order_id = p.order_id(),
                                               .new_price = p.new_price(),
                                               .new_quantity = p.new_quantity()}},
                .error = {}};
    }
    case pb::CommandBody::kPlaceStop: {
        const auto &p = body.place_stop().stop();
        const auto side = from_pb_side(p.side());
        if (!side)
            return error("invalid stop side");
        return {.command = Command{PlaceStop{
                    .request_id = request_id,
                    .symbol = symbol,
                    .stop = engine::StopOrder{.id = p.id(),
                                              .owner_id = p.owner_id(),
                                              .side = *side,
                                              .stop_price = p.stop_price(),
                                              .quantity = p.quantity(),
                                              .limit_price = p.has_limit_price() ? std::optional{p.limit_price()}
                                                                                 : std::nullopt}}},
                .error = {}};
    }
    case pb::CommandBody::kCancelStop: {
        const auto &p = body.cancel_stop();
        return {.command = Command{CancelStop{
                    .request_id = request_id, .symbol = symbol, .requester = p.requester(), .order_id = p.order_id()}},
                .error = {}};
    }
    case pb::CommandBody::kModifyStop: {
        const auto &p = body.modify_stop();
        return {.command = Command{ModifyStop{
                    .request_id = request_id,
                    .symbol = symbol,
                    .requester = p.requester(),
                    .order_id = p.order_id(),
                    .new_stop_price = p.new_stop_price(),
                    .new_limit_price = p.has_new_limit_price() ? std::optional{p.new_limit_price()} : std::nullopt,
                    .new_quantity = p.new_quantity()}},
                .error = {}};
    }
    case pb::CommandBody::BODY_NOT_SET:
        return error("empty command body");
    }

    return error("unknown command body");
}

} // namespace server
