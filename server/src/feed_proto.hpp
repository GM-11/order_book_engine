#pragma once

#include "exchange/v1/engine_feed.pb.h"
#include "server/publisher.hpp"

namespace server {
void fill_feed_message(const FeedItem &item, exchange::v1::FeedMessage &out);
}
