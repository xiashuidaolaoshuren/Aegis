#pragma once

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/striped.hpp>
#include <aegis/money.hpp>
#include <aegis/result.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <unordered_map>

namespace aegis::ledger {

class Wallet;
class Wal;
class Partition;

enum class LedgerError {
    InsufficientFunds,
    UnknownWallet,
    AmountMismatch,
    UnknownHold,
    CurrencyMismatch,
    InvalidAmount,
};

// Striped is the stage-2 ledger: one shared wallet map guarded by hashed locks. Partitioned is
// stage 3: each wallet lives on exactly one writer thread, and the verbs route to it.
enum class LedgerMode { Striped, Partitioned };

class Ledger {
    friend class Wal;

public:
    explicit Ledger(std::unordered_map<AccountId, Wallet> wallets,
                    std::size_t stripe_count = StripedLocks::kDefaultStripeCount);
    Ledger(std::unordered_map<AccountId, Wallet> wallets,
           LedgerMode mode,
           std::size_t partition_count);
    Ledger(Ledger&&) noexcept;
    Ledger& operator=(Ledger&&) noexcept;
    ~Ledger();

    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;

    [[nodiscard]] Result<Hold, LedgerError> reserve(
        AccountId cardholder,
        Money amount,
        MerchantId merchant,
        TimePoint now = std::chrono::system_clock::now());
    [[nodiscard]] Result<void, LedgerError> capture(HoldId hold, Money amount);
    [[nodiscard]] Result<void, LedgerError> reverse(HoldId hold);
    [[nodiscard]] std::size_t expire_due(TimePoint now);
    void set_hold_ttl(std::chrono::seconds ttl);
    void set_wal(Wal* wal);
    [[nodiscard]] Result<Money, LedgerError> balance(AccountId id, Bucket bucket) const;
    [[nodiscard]] std::size_t live_hold_count() const;

    // Only meaningful for a partitioned ledger; a striped one reports a single partition.
    [[nodiscard]] std::size_t partition_count() const;
    [[nodiscard]] std::size_t partition_of(AccountId id) const;

private:
    Ledger(
        std::unordered_map<AccountId, Wallet> wallets,
        std::unordered_map<HoldId, Hold> holds,
        std::uint64_t next_hold_id);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegis::ledger
