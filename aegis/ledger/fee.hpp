#pragma once

#include <aegis/money.hpp>

namespace aegis {

struct CaptureSplit {
    Money payable;
    Money interchange;
};

inline CaptureSplit split_capture(Money amount) {
    const std::int64_t interchange_minor =
        (amount.minor_units() * 29) / 1000;
    const std::int64_t payable_minor =
        amount.minor_units() - interchange_minor;

    return CaptureSplit{
        Money{amount.currency(), payable_minor},
        Money{amount.currency(), interchange_minor},
    };
}

} // namespace aegis
