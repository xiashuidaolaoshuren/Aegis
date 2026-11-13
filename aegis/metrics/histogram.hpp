#pragma once

#include <aegis/result.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace aegis::metrics {

enum class HistogramError {
    Empty,
};

enum class Percentile {
    P50,
    P99,
    P999,
};

class LatencyHistogram {
public:
    void record(std::chrono::nanoseconds latency) {
        std::lock_guard lock(mutex_);
        samples_.push_back(latency.count());
    }

    [[nodiscard]] std::size_t count() const {
        std::lock_guard lock(mutex_);
        return samples_.size();
    }

    [[nodiscard]] Result<std::chrono::nanoseconds, HistogramError> percentile(Percentile p) const {
        std::vector<std::int64_t> copy;
        {
            std::lock_guard lock(mutex_);
            if (samples_.empty()) {
                return Result<std::chrono::nanoseconds, HistogramError>::err(HistogramError::Empty);
            }
            copy = samples_;
        }

        std::sort(copy.begin(), copy.end());

        const std::size_t n = copy.size();
        std::size_t rank = 0;
        switch (p) {
        case Percentile::P50:
            rank = (50U * n + 100U - 1U) / 100U;
            break;
        case Percentile::P99:
            rank = (99U * n + 100U - 1U) / 100U;
            break;
        case Percentile::P999:
            rank = (999U * n + 1000U - 1U) / 1000U;
            break;
        }

        if (rank == 0U) {
            rank = 1U;
        }
        const std::size_t index = rank - 1U;
        return Result<std::chrono::nanoseconds, HistogramError>::ok(
            std::chrono::nanoseconds{copy[index]});
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::int64_t> samples_;
};

} // namespace aegis::metrics
