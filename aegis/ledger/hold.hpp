#pragma once

#include <aegis/ids.hpp>
#include <aegis/money.hpp>
#include <aegis/tagged.hpp>

#include <cstdint>

namespace aegis::ledger {

struct HoldIdTag {};

using HoldId = Tagged<std::uint64_t, HoldIdTag>;

struct Hold {
    HoldId id;
    AccountId cardholder;
    MerchantId merchant;
    Money amount;
};

} // namespace aegis::ledger
