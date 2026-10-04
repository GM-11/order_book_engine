#pragma once

#include "server/commands.hpp"

#include <mutex>
#include <string>
#include <string_view>

namespace cli {

extern std::mutex g_print_mu;
void say(const std::string &line);
std::string_view to_string(engine::EventKind kind);
std::string_view to_string(engine::RejectReason reason);
std::string format_event(std::string_view ticker, const engine::EngineEvent &event);
std::string format_reply(const server::Reply &reply);

} // namespace cli
