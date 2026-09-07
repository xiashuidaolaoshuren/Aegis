#pragma once

#include <aegis/ids.hpp>
#include <aegis/money.hpp>
#include <aegis/tagged.hpp>

#include <chrono>
#include <cstdint>

namespace aegis::ledger {

using TimePoint = std::chrono::steady_clock::time_point;

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
