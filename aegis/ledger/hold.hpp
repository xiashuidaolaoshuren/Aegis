#pragma once

#include <aegis/ids.hpp>
#include <aegis/money.hpp>
#include <aegis/tagged.hpp>

#include <chrono>
#include <cstdint>

namespace aegis::ledger {

// Wall clock so hold expiry survives process restarts; may shift with NTP adjustments.
using TimePoint = std::chrono::system_clock::time_point;

struct HoldIdTag {};

using HoldId = Tagged<std::uint64_t, HoldIdTag>;

struct Hold {
    HoldId id;
    AccountId cardholder;
    MerchantId merchant;
    Money amount;
    TimePoint expires_at;
};

} // namespace aegis::ledger
