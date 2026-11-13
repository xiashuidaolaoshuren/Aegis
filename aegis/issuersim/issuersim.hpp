#pragma once

#include <atomic>
#include <chrono>
#include <functional>

namespace aegis::issuersim {

enum class Decision {
    Approved,
    Declined,
    Timeout,
};

using WaitFn = std::function<void(std::chrono::milliseconds)>;

// A fake network with no balances. Stage 2 calls this from several workers at once, so the call
// counter is atomic; the configuration fields stay read-only for the duration of a run.
class IssuerSim {
public:
    std::chrono::milliseconds latency{std::chrono::milliseconds{0}};
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};
    bool inject_decline{false};
    std::atomic<int> calls{0};
};

[[nodiscard]] inline Decision await_decision(IssuerSim& sim, WaitFn wait) {
    sim.calls.fetch_add(1, std::memory_order_relaxed);
    if (sim.latency >= sim.timeout) {
        return Decision::Timeout;
    }
    if (sim.inject_decline) {
        return Decision::Declined;
    }
    if (wait) {
        wait(sim.latency);
    }
    return Decision::Approved;
}

} // namespace aegis::issuersim
