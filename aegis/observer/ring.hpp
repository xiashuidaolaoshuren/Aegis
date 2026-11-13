#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace aegis::observer {

struct TransactionEvent {
    std::chrono::system_clock::time_point at{};
    std::string terminal_id;
    std::string masked_pan;
    std::string response_code;
    std::int64_t amount_minor{0};
    std::int64_t latency_ns{0};
};

class TransactionRing {
public:
    explicit TransactionRing(std::size_t capacity) : capacity_(capacity) {}

    void push(TransactionEvent event) {
        if (capacity_ == 0) {
            return;
        }
        if (events_.size() >= capacity_) {
            events_.pop_front();
        }
        events_.push_back(std::move(event));
    }

    [[nodiscard]] std::vector<TransactionEvent> drain() {
        std::vector<TransactionEvent> out;
        out.reserve(events_.size());
        for (TransactionEvent& event : events_) {
            out.push_back(std::move(event));
        }
        events_.clear();
        return out;
    }

    [[nodiscard]] std::size_t size() const {
        return events_.size();
    }

private:
    std::size_t capacity_;
    std::deque<TransactionEvent> events_;
};

} // namespace aegis::observer
