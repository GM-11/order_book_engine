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

std::string encode_body(const Command &command);

DecodeResult decode_body(SymbolId symbol, RequestId request_id, std::string_view payload);

} // namespace server
