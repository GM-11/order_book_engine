#pragma once

#include "server/commands.hpp"
#include <cstdint>
#include <string>

// Tests count with numbers; a client_order_id is a string. cid(7) == "7".
inline server::ClientOrderId cid(std::uint64_t n) { return std::to_string(n); }
