#pragma once
#include "server/publisher.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace server {

struct FeedServerOptions {
    std::string address = "127.0.0.1:50061"; // port 0 = pick a free port
    std::function<void(const std::string &)> log{};
    std::chrono::milliseconds poll{100};
    std::chrono::milliseconds shutdown_grace{2000};
};

class EngineFeedServer {
  public:
    EngineFeedServer(MarketDataPublisher &publisher, FeedServerOptions options);
    ~EngineFeedServer(); // calls stop()
    EngineFeedServer(const EngineFeedServer &) = delete;
    EngineFeedServer &operator=(const EngineFeedServer &) = delete;

    void start();
    int port() const;
    void stop();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace server
