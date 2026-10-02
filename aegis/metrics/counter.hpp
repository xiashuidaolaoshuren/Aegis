#pragma once

#include <atomic>
#include <cstdint>

namespace aegis::metrics {

class AuthCounter {
public:
    void increment() {
        count_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t snapshot_count() const {
        return count_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t> count_{0};
};

} // namespace aegis::metrics
