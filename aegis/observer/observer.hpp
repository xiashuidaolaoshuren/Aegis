#pragma once

#include <aegis/observer/commands.hpp>
#include <aegis/observer/ring.hpp>
#include <aegis/observer/snapshot.hpp>

#include <deque>
#include <mutex>
#include <vector>

namespace aegis::observer {

class Observer {
public:
    explicit Observer(std::size_t ring_capacity)
        : ring_(ring_capacity) {}

    void publish(MetricsSnapshot snapshot) {
        std::lock_guard lock(mutex_);
        snapshot_ = snapshot;
    }

    [[nodiscard]] MetricsSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    void push(TransactionEvent event) {
        std::lock_guard lock(mutex_);
        ring_.push(std::move(event));
    }

    [[nodiscard]] std::vector<TransactionEvent> drain() {
        std::lock_guard lock(mutex_);
        return ring_.drain();
    }

    void post(Command command) {
        std::lock_guard lock(mutex_);
        commands_.push_back(command);
    }

    [[nodiscard]] std::vector<Command> take_commands() {
        std::lock_guard lock(mutex_);
        std::vector<Command> out;
        out.reserve(commands_.size());
        for (const Command& command : commands_) {
            out.push_back(command);
        }
        commands_.clear();
        return out;
    }

private:
    mutable std::mutex mutex_;
    MetricsSnapshot snapshot_{};
    TransactionRing ring_;
    std::deque<Command> commands_;
};

} // namespace aegis::observer
