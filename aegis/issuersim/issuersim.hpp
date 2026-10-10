#pragma once

#include <chrono>
#include <functional>

namespace aegis::issuersim {

enum class Decision {
    Approved,
    Declined,
    Timeout,
};

using WaitFn = std::function<void(std::chrono::milliseconds)>;

class IssuerSim {
public:
    std::chrono::milliseconds latency{std::chrono::milliseconds{0}};
    std::chrono::milliseconds timeout{std::chrono::seconds{30}};
    bool inject_decline{false};
    int calls{0};
};

[[nodiscard]] inline Decision await_decision(IssuerSim& sim, WaitFn wait) {
    ++sim.calls;
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
