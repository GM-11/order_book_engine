#pragma once
#include "server/publisher.hpp"

#include <string>

namespace server {

std::string encode_feed_item(const FeedItem &item);

} // namespace server
