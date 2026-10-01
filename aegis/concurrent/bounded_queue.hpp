#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace aegis::concurrent {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] bool push(T value) {
        {
            std::lock_guard lock(mutex_);
            if (items_.size() >= capacity_) {
                return false;
            }
            items_.push_back(std::move(value));
        }
        not_empty_.notify_one();
        return true;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] std::optional<T> pop(std::stop_token stop_token) {
        const std::stop_callback callback(stop_token, [this] { not_empty_.notify_all(); });
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this, &stop_token] {
            return !items_.empty() || stop_token.stop_requested();
        });
        if (items_.empty()) {
            return std::nullopt;
        }
        T value = std::move(items_.front());
        items_.pop_front();
        return value;
    }

private:
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<T> items_;
};

} // namespace aegis::concurrent
