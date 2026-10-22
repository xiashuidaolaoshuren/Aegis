#pragma once

#include <cstdint>

namespace aegis::observer {

enum class CommandKind {
    Start,
    Stop,
    SetWorkerCount,
    SetPartitionCount,
    InjectSlowIssuer,
    ForceTimeouts,
    DropConnections,
};

struct Command {
    CommandKind kind{CommandKind::Start};
    std::uint64_t count{0};
};

} // namespace aegis::observer
