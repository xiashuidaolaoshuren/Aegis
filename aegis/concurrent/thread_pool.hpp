#pragma once

#include <aegis/concurrent/bounded_queue.hpp>

#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace aegis::concurrent {

class ThreadPool {
public:
    ThreadPool(std::size_t workers, std::size_t queue_capacity) : queue_(queue_capacity) {
        workers_.reserve(workers);
        for (std::size_t i = 0; i < workers; ++i) {
            workers_.emplace_back([this](std::stop_token stop) { worker_loop(stop); });
        }
    }

    ~ThreadPool() {
        shutdown();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    [[nodiscard]] bool submit(std::function<void()> job) {
        std::lock_guard lock(shutdown_mutex_);
        if (closed_) {
            return false;
        }
        return queue_.push(std::move(job));
    }

    void shutdown() {
        shutdown_impl();
    }

private:
    void worker_loop(std::stop_token stop) {
        while (true) {
            const auto job = queue_.pop(stop);
            if (job.has_value()) {
                job.value()();
                continue;
            }
            if (stop.stop_requested()) {
                return;
            }
        }
    }

    void shutdown_impl() {
        std::lock_guard lock(shutdown_mutex_);
        if (shutdown_started_) {
            return;
        }
        shutdown_started_ = true;
        closed_ = true;

        for (std::jthread& worker : workers_) {
            worker.request_stop();
        }
        for (std::jthread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();
    }

    BoundedQueue<std::function<void()>> queue_;
    std::vector<std::jthread> workers_;
    std::mutex shutdown_mutex_;
    bool shutdown_started_{false};
    bool closed_{false};
};

} // namespace aegis::concurrent
