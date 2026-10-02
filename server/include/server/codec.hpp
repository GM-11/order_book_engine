#pragma once
#include "server/commands.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace server {

struct DecodeResult {
    std::optional<Command> command; // set on success
    std::string error;              // set on failure
    explicit operator bool() const { return command.has_value(); }
};

// Serializes the command body only, never request_id or symbol. Throws std::invalid_argument for Shutdown.
std::string encode_body(const Command &command);

// The codec checks shape, the engine checks meaning, and this never returns Shutdown.
DecodeResult decode_body(SymbolId symbol, RequestId request_id, std::string_view payload);

} // namespace server
