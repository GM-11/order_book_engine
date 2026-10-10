// Codec tests: Command <-> payload bytes.
//
// Rules under test:
//   * The payload carries the command body only. symbol and client_request_id come
//     from the envelope, i.e. the arguments of decode_body.
//   * The codec checks the SHAPE of the bytes (parses, body set, enums known),
//     never the MEANING of the numbers. The engine owns that.

#include "server/codec.hpp"

#include "exchange/v1/commands.pb.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

using namespace server;
namespace pb = exchange::v1;

namespace {

// Envelope values used when decoding.
constexpr SymbolId kSymbol = 7;
constexpr RequestId kRequest = 99;
// Envelope values stored in the commands we encode. They differ from the
// decode ones on purpose: a decoder that took them from anywhere but its
// arguments would be caught.
constexpr SymbolId kInSymbol = 2;
constexpr RequestId kInRequest = 1;

// Encode, decode, and check the bytes survive a second encode unchanged
// (nothing was dropped on the way). Returns the decoded command.
Command round_trip(const Command &in) {
    const std::string bytes = encode_body(in);
    DecodeResult out = decode_body(kSymbol, kRequest, bytes);
    INFO(out.error);
    REQUIRE(out.command.has_value());
    REQUIRE(encode_body(*out.command) == bytes);
    return *out.command;
}

template <class T> const T &as(const Command &c) {
    REQUIRE(std::holds_alternative<T>(c));
    return std::get<T>(c);
}

DecodeResult decode(std::string_view bytes) { return decode_body(kSymbol, kRequest, bytes); }

bool mentions(const DecodeResult &r, std::string_view word) { return r.error.find(word) != std::string::npos; }

engine::Order make_order(engine::Side side, engine::OrderType type, std::optional<engine::Price> price) {
    return engine::Order{.id = 5, .owner_id = 11, .side = side, .type = type, .price = price, .quantity = 10};
}

engine::StopOrder make_stop(std::optional<engine::Price> limit) {
    return engine::StopOrder{
        .id = 6, .owner_id = 12, .side = engine::Side::Buy, .stop_price = 105, .quantity = 3, .limit_price = limit};
}

// A valid limit-order body, built with raw protobuf so tests can break one field.
pb::CommandBody valid_limit_body() {
    pb::CommandBody body;
    auto *o = body.mutable_new_order()->mutable_order();
    o->set_id(1);
    o->set_owner_id(2);
    o->set_side(pb::SIDE_BUY);
    o->set_type(pb::ORDER_TYPE_LIMIT);
    o->set_price(100);
    o->set_quantity(5);
    return body;
}

} // namespace

// ---------------------------------------------------------------- round trips

TEST_CASE("a new order survives the round trip, both sides, both types", "[codec]") {
    SECTION("limit sell") {
        const Command out = round_trip(NewOrder{kInRequest, kInSymbol, make_order(engine::Side::Sell, engine::OrderType::Limit, 100)});
        const auto &n = as<NewOrder>(out);
        CHECK(n.order.id == 5);
        CHECK(n.order.owner_id == 11);
        CHECK(n.order.side == engine::Side::Sell);
        CHECK(n.order.type == engine::OrderType::Limit);
        CHECK(n.order.price == std::optional<engine::Price>{100});
        CHECK(n.order.quantity == 10);
        CHECK(n.order.filled == 0);
    }
    SECTION("market buy") {
        const Command out = round_trip(NewOrder{kInRequest, kInSymbol, make_order(engine::Side::Buy, engine::OrderType::Market, std::nullopt)});
        const auto &n = as<NewOrder>(out);
        CHECK(n.order.side == engine::Side::Buy);
        CHECK(n.order.type == engine::OrderType::Market);
        CHECK_FALSE(n.order.price.has_value());
    }
}

TEST_CASE("cancel and modify survive the round trip", "[codec]") {
    SECTION("cancel order") {
        const Command out = round_trip(CancelOrder{.client_request_id = kInRequest, .symbol = kInSymbol, .requester = 3, .order_id = 4});
        const auto &c = as<CancelOrder>(out);
        CHECK(c.requester == 3);
        CHECK(c.order_id == 4);
    }
    SECTION("modify order") {
        const Command out = round_trip(ModifyOrder{.client_request_id = kInRequest,
                                                   .symbol = kInSymbol,
                                                   .requester = 3,
                                                   .order_id = 4,
                                                   .new_price = 250,
                                                   .new_quantity = 60});
        const auto &m = as<ModifyOrder>(out);
        CHECK(m.requester == 3);
        CHECK(m.order_id == 4);
        CHECK(m.new_price == 250);
        CHECK(m.new_quantity == 60);
    }
}

TEST_CASE("stop commands survive the round trip", "[codec]") {
    SECTION("place a stop-market") {
        const Command out = round_trip(PlaceStop{kInRequest, kInSymbol, make_stop(std::nullopt)});
        const auto &p = as<PlaceStop>(out);
        CHECK(p.stop.id == 6);
        CHECK(p.stop.owner_id == 12);
        CHECK(p.stop.side == engine::Side::Buy);
        CHECK(p.stop.stop_price == 105);
        CHECK(p.stop.quantity == 3);
        CHECK_FALSE(p.stop.limit_price.has_value());
    }
    SECTION("place a stop-limit") {
        const Command out = round_trip(PlaceStop{kInRequest, kInSymbol, make_stop(110)});
        CHECK(as<PlaceStop>(out).stop.limit_price == std::optional<engine::Price>{110});
    }
    SECTION("cancel a stop") {
        const Command out = round_trip(CancelStop{.client_request_id = kInRequest, .symbol = kInSymbol, .requester = 3, .order_id = 6});
        const auto &c = as<CancelStop>(out);
        CHECK(c.requester == 3);
        CHECK(c.order_id == 6);
    }
    SECTION("modify a stop, limit price absent") {
        const Command out = round_trip(ModifyStop{.client_request_id = kInRequest,
                                                  .symbol = kInSymbol,
                                                  .requester = 3,
                                                  .order_id = 6,
                                                  .new_stop_price = 120,
                                                  .new_limit_price = std::nullopt,
                                                  .new_quantity = 9});
        const auto &m = as<ModifyStop>(out);
        CHECK(m.requester == 3);
        CHECK(m.order_id == 6);
        CHECK(m.new_stop_price == 120);
        CHECK_FALSE(m.new_limit_price.has_value());
        CHECK(m.new_quantity == 9);
    }
    SECTION("modify a stop, limit price present") {
        const Command out = round_trip(ModifyStop{.client_request_id = kInRequest,
                                                  .symbol = kInSymbol,
                                                  .requester = 3,
                                                  .order_id = 6,
                                                  .new_stop_price = 120,
                                                  .new_limit_price = 125,
                                                  .new_quantity = 9});
        CHECK(as<ModifyStop>(out).new_limit_price == std::optional<engine::Price>{125});
    }
}

TEST_CASE("a price of zero stays present; an absent price stays absent", "[codec]") {
    // proto3 would silently turn 0 into "not set" without the optional keyword.
    SECTION("order price") {
        const Command out = round_trip(NewOrder{kInRequest, kInSymbol, make_order(engine::Side::Buy, engine::OrderType::Limit, 0)});
        const auto &n = as<NewOrder>(out);
        REQUIRE(n.order.price.has_value());
        CHECK(*n.order.price == 0);
    }
    SECTION("stop limit price") {
        const Command out = round_trip(PlaceStop{kInRequest, kInSymbol, make_stop(0)});
        const auto &p = as<PlaceStop>(out);
        REQUIRE(p.stop.limit_price.has_value());
        CHECK(*p.stop.limit_price == 0);
    }
    SECTION("modify-stop limit price") {
        const Command out = round_trip(ModifyStop{.client_request_id = kInRequest,
                                                  .symbol = kInSymbol,
                                                  .requester = 3,
                                                  .order_id = 6,
                                                  .new_stop_price = 120,
                                                  .new_limit_price = 0,
                                                  .new_quantity = 9});
        const auto &m = as<ModifyStop>(out);
        REQUIRE(m.new_limit_price.has_value());
        CHECK(*m.new_limit_price == 0);
    }
}

// ------------------------------------------------------------------- envelope

TEST_CASE("symbol and client_request_id come from the envelope, not the payload", "[codec]") {
    SECTION("decode uses its arguments") {
        const Command out = round_trip(CancelOrder{.client_request_id = kInRequest, .symbol = kInSymbol, .requester = 3, .order_id = 4});
        const auto &c = as<CancelOrder>(out);
        CHECK(c.symbol == kSymbol);
        CHECK(c.client_request_id == kRequest);
    }
    SECTION("encode ignores them: same body, same bytes") {
        const CancelOrder a{.client_request_id = 1, .symbol = 2, .requester = 3, .order_id = 4};
        const CancelOrder b{.client_request_id = 100, .symbol = 200, .requester = 3, .order_id = 4};
        CHECK(encode_body(a) == encode_body(b));
    }
}

TEST_CASE("Shutdown is never put on the wire", "[codec]") {
    CHECK_THROWS_AS(encode_body(Shutdown{}), std::invalid_argument);
}

// ---------------------------------------------- shape is checked, meaning is not

TEST_CASE("the codec passes odd numbers through; the engine judges them", "[codec]") {
    SECTION("a market order that carries a price") {
        const Command out = round_trip(NewOrder{kInRequest, kInSymbol, make_order(engine::Side::Buy, engine::OrderType::Market, 100)});
        CHECK(as<NewOrder>(out).order.price == std::optional<engine::Price>{100});
    }
    SECTION("a limit order with no price") {
        const Command out = round_trip(NewOrder{kInRequest, kInSymbol, make_order(engine::Side::Buy, engine::OrderType::Limit, std::nullopt)});
        CHECK_FALSE(as<NewOrder>(out).order.price.has_value());
    }
    SECTION("zero and negative quantities") {
        engine::Order o = make_order(engine::Side::Buy, engine::OrderType::Limit, 100);
        o.quantity = -5;
        const Command out = round_trip(NewOrder{kInRequest, kInSymbol, o});
        CHECK(as<NewOrder>(out).order.quantity == -5);
    }
}

// -------------------------------------------------------------------- failures

TEST_CASE("a control body decodes, so the failure tests below break only one thing", "[codec]") {
    const DecodeResult r = decode(valid_limit_body().SerializeAsString());
    INFO(r.error);
    REQUIRE(r.command.has_value());
}

TEST_CASE("bytes that do not parse are rejected with a reason", "[codec]") {
    SECTION("a field that promises 5 bytes and has none") {
        const DecodeResult r = decode(std::string("\x0a\x05", 2));
        CHECK_FALSE(r.command.has_value());
        CHECK(mentions(r, "parse"));
    }
    SECTION("a varint that never ends") {
        const DecodeResult r = decode(std::string("\xff\xff\xff\xff", 4));
        CHECK_FALSE(r.command.has_value());
        CHECK(mentions(r, "parse"));
    }
}

TEST_CASE("an empty payload has no command body", "[codec]") {
    const DecodeResult r = decode("");
    CHECK_FALSE(r.command.has_value());
    CHECK(mentions(r, "empty"));
}

TEST_CASE("an unspecified or unknown enum is rejected, never guessed", "[codec]") {
    SECTION("order side unspecified") {
        pb::CommandBody body = valid_limit_body();
        body.mutable_new_order()->mutable_order()->set_side(pb::SIDE_UNSPECIFIED);
        const DecodeResult r = decode(body.SerializeAsString());
        CHECK_FALSE(r.command.has_value());
        CHECK(mentions(r, "side"));
    }
    SECTION("order type unspecified") {
        pb::CommandBody body = valid_limit_body();
        body.mutable_new_order()->mutable_order()->set_type(pb::ORDER_TYPE_UNSPECIFIED);
        const DecodeResult r = decode(body.SerializeAsString());
        CHECK_FALSE(r.command.has_value());
        CHECK(mentions(r, "type"));
    }
    SECTION("order side from a future schema (value 99)") {
        pb::CommandBody body = valid_limit_body();
        body.mutable_new_order()->mutable_order()->set_side(static_cast<pb::Side>(99));
        const DecodeResult r = decode(body.SerializeAsString());
        CHECK_FALSE(r.command.has_value());
        CHECK(mentions(r, "side"));
    }
    SECTION("order type from a future schema (value 99)") {
        pb::CommandBody body = valid_limit_body();
        body.mutable_new_order()->mutable_order()->set_type(static_cast<pb::OrderType>(99));
        const DecodeResult r = decode(body.SerializeAsString());
        CHECK_FALSE(r.command.has_value());
        CHECK(mentions(r, "type"));
    }
    SECTION("stop side unspecified") {
        pb::CommandBody body;
        auto *s = body.mutable_place_stop()->mutable_stop();
        s->set_id(1);
        s->set_owner_id(2);
        s->set_stop_price(100);
        s->set_quantity(5);
        const DecodeResult r = decode(body.SerializeAsString());
        CHECK_FALSE(r.command.has_value());
        CHECK(mentions(r, "side"));
    }
}
