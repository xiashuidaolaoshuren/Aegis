#pragma once

#include <chrono>
#include <cstdint>

namespace aegis::observer {

struct MetricsSnapshot {
    double throughput_per_s{0.0};
    double approval_rate{0.0};
    std::int64_t p50_ns{0};
    std::int64_t p99_ns{0};
    std::int64_t p99_9_ns{0};
    std::uint64_t queue_depth{0};
    std::uint64_t live_holds{0};
    std::uint64_t wal_bytes{0};
    std::uint64_t account_count{0};
    std::uint64_t worker_count{0};
    std::uint64_t partition_count{0};
    std::chrono::milliseconds uptime{0};
    bool ledger_invariant_ok{true};
    bool running{false};
};

} // namespace aegis::observer
