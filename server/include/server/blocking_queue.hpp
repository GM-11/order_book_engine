#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>
namespace server {
template <typename T> class BlockingQueue {
  public:
    // BlockingQueue() = default;
    // BlockingQueue(const BlockingQueue &) = delete;
    // BlockingQueue &operator=(const BlockingQueue &) = delete;
    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            deque_.push_back(std::move(item));
        }

        condition_variable_.notify_one();
    }

    T pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_variable_.wait(lock, [this] { return !deque_.empty(); });
        T item = std::move(deque_.front());
        deque_.pop_front();
        return item;
    }

    std::optional<T> try_pop() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (deque_.empty())
            return std::nullopt;
        T item = std::move(deque_.front());
        deque_.pop_front();
        return item;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deque_.size();
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable condition_variable_;
    std::deque<T> deque_;
};
} // namespace server
