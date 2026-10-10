#include "cli/console.hpp"

#include <iostream>
#include <sstream>

namespace cli {

// Publishers, feeds and the foreground thread share the screen; lock each line
// so they never interleave mid-way.
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

std::string format_reply(const server::Reply &reply) {
    std::ostringstream s;
    s << "  reply to request " << reply.client_request_id << " (order " << reply.order_id << "): ";
    if (reply.reject_reason == engine::RejectReason::None)
        s << "OK";
    else
        s << "REJECTED (" << to_string(reply.reject_reason) << ')';
    if (reply.unaccepted_quantity > 0)
        s << ", unaccepted qty " << reply.unaccepted_quantity;
    if (reply.rested_price)
        s << ", rested at band edge " << *reply.rested_price;
    return s.str();
}

} // namespace cli
