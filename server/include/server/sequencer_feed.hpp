#pragma once

#include "server/commands.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace server {

class Worker;

struct FeedOptions {
    std::string address;
    std::uint32_t partition = 0;
    Seq from_seq = 1;
    std::chrono::milliseconds retry_delay{200};
    int max_already_exists_retries = 50;
    std::function<void(const std::string &)> log;
    std::function<void(const std::string &)> on_fatal;
};

class SequencerFeed {
  public:
    SequencerFeed(Worker &worker, FeedOptions options);
    ~SequencerFeed();
    SequencerFeed(const SequencerFeed &) = delete;
    SequencerFeed &operator=(const SequencerFeed &) = delete;

    void start();
    void stop();
    Seq last_seq() const;
    bool failed() const;
    std::string failure() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace server
