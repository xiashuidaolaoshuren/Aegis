#pragma once

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/money.hpp>
#include <aegis/result.hpp>

#include <cstddef>
#include <unordered_map>

namespace aegis::ledger {

class Wallet;
struct Hold;

enum class LedgerError { InsufficientFunds, UnknownWallet };

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
        MerchantId merchant);
    [[nodiscard]] Result<Money, LedgerError> balance(AccountId id, Bucket bucket) const;
    [[nodiscard]] std::size_t live_hold_count() const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace aegis::ledger
