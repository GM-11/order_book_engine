#pragma once

#include "exchange/v1/engine_feed.pb.h"
#include "server/publisher.hpp"

namespace server {
// Snapshot or MarketEvent only; a Reply throws std::logic_error.
void fill_feed_message(const FeedItem &item, exchange::v1::FeedMessage &out);
void fill_reply(const Reply &in, exchange::v1::Reply &out);
} // namespace server
