#pragma once
#include "server/publisher.hpp"

#include <string>

namespace server {

std::string encode_feed_item(const FeedItem &item);
std::string encode_reply(const Reply &reply);

} // namespace server
