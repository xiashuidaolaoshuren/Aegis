#pragma once

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/money.hpp>
#include <aegis/result.hpp>

#include <chrono>
#include <cstddef>
#include <unordered_map>

namespace aegis::ledger {

class Wallet;

enum class LedgerError { InsufficientFunds, UnknownWallet, AmountMismatch, UnknownHold };

class Ledger {
public:
    explicit Ledger(std::unordered_map<AccountId, Wallet> wallets);
    Ledger(Ledger&&) noexcept;
    Ledger& operator=(Ledger&&) noexcept;
    ~Ledger();

    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;

    [[nodiscard]] Result<Hold, LedgerError> reserve(
        AccountId cardholder,
        Money amount,
        MerchantId merchant,
        TimePoint now = TimePoint{});
    [[nodiscard]] Result<void, LedgerError> capture(HoldId hold, Money amount);
    [[nodiscard]] Result<void, LedgerError> reverse(HoldId hold);
    [[nodiscard]] std::size_t expire_due(TimePoint now);
    void set_hold_ttl(std::chrono::seconds ttl);
    [[nodiscard]] Result<Money, LedgerError> balance(AccountId id, Bucket bucket) const;
    [[nodiscard]] std::size_t live_hold_count() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace aegis::ledger
