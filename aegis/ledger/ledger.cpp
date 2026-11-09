#include <aegis/ledger/ledger.hpp>

#include <aegis/ledger/fee.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/twophase.hpp>
#include <aegis/ledger/posting.hpp>
#include <aegis/ledger/wal.hpp>
#include <aegis/ledger/wallet.hpp>

#include <utility>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace aegis::ledger {

namespace {

[[nodiscard]] Result<AccountId, LedgerError> find_system_account(
    const std::unordered_map<AccountId, Wallet>& wallets) {
    for (const auto& [id, wallet] : wallets) {
        if (wallet.kind() == WalletKind::System) {
            return Result<AccountId, LedgerError>::ok(id);
        }
    }
    return Result<AccountId, LedgerError>::err(LedgerError::UnknownWallet);
}

[[nodiscard]] Result<void, LedgerError> release_hold(
    std::unordered_map<AccountId, Wallet>& wallets,
    const Hold& hold) {
    const PostingBatch batch{
        PostingLine{hold.cardholder, Bucket::Holds, hold.amount, false},
        PostingLine{hold.cardholder, Bucket::Available, hold.amount, true},
    };
    assert_balanced(batch);

    const auto apply_result = apply_batch(wallets, batch);
    if (!apply_result.has_value()) {
        return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
    }
    return Result<void, LedgerError>::ok();
}

} // namespace

struct Ledger::Impl {
    Impl(std::unordered_map<AccountId, Wallet> initial_wallets, std::size_t stripe_count)
        : wallets(std::move(initial_wallets)), locks(stripe_count) {}

    Impl(std::size_t partition_count)
        : locks(1), mode(LedgerMode::Partitioned), partitioner(partition_count) {}

    Impl(
        std::unordered_map<AccountId, Wallet> initial_wallets,
        std::unordered_map<HoldId, Hold> initial_holds,
        std::uint64_t initial_next_hold_id,
        std::size_t stripe_count)
        : wallets(std::move(initial_wallets)),
          holds(std::move(initial_holds)),
          next_hold_id(initial_next_hold_id),
          locks(stripe_count) {}

    std::unordered_map<AccountId, Wallet> wallets;
    std::unordered_map<HoldId, Hold> holds;
    std::uint64_t next_hold_id{1};
    std::chrono::seconds hold_ttl{std::chrono::hours{24}};
    Wal* wal{nullptr};
    StripedLocks locks;

    // Stage 3. Empty unless the ledger was constructed partitioned, in which case `wallets` above is
    // empty and every verb routes through `partitions` instead of the striped path.
    LedgerMode mode{LedgerMode::Striped};
    WalletPartitioner partitioner{1};
    std::optional<AccountId> system_account;
    std::atomic<std::uint64_t> next_txn{1};
    WalSink wal_sink;
    std::vector<std::unique_ptr<Partition>> partitions;
    // Guards the hold metadata (holds map, next_hold_id, hold_ttl, WAL appends).
    // Lock order rule: this mutex is always taken before any wallet stripe, and stripes are taken
    // in ascending index order -- that total order is what makes multi-stripe verbs deadlock-free.
    mutable std::mutex metadata_mutex;
};

Ledger::Ledger(std::unordered_map<AccountId, Wallet> wallets, std::size_t stripe_count)
    : impl_(std::make_unique<Impl>(std::move(wallets), stripe_count)) {}

Ledger::Ledger(
    std::unordered_map<AccountId, Wallet> wallets,
    LedgerMode mode,
    std::size_t partition_count) {
    if (mode != LedgerMode::Partitioned) {
        impl_ = std::make_unique<Impl>(std::move(wallets), partition_count);
        return;
    }

    impl_ = std::make_unique<Impl>(partition_count);

    const auto system = find_system_account(wallets);
    if (system.has_value()) {
        impl_->system_account = system.value();
    }

    std::vector<std::unordered_map<AccountId, Wallet>> slices(partition_count);
    for (auto& [id, wallet] : wallets) {
        slices[impl_->partitioner.partition_of(id)].emplace(id, std::move(wallet));
    }
    impl_->partitions.reserve(partition_count);
    for (std::size_t index = 0; index < partition_count; ++index) {
        impl_->partitions.push_back(std::make_unique<Partition>(
            index, std::move(slices[index]), impl_->wal_sink));
    }
}

Ledger::Ledger(
    std::unordered_map<AccountId, Wallet> wallets,
    std::unordered_map<HoldId, Hold> holds,
    std::uint64_t next_hold_id)
    : impl_(std::make_unique<Impl>(
          std::move(wallets),
          std::move(holds),
          next_hold_id,
          StripedLocks::kDefaultStripeCount)) {}

Ledger::Ledger(Ledger&& other) noexcept = default;

Ledger& Ledger::operator=(Ledger&& other) noexcept = default;

Ledger::~Ledger() = default;

Result<Hold, LedgerError> Ledger::reserve(
    AccountId cardholder,
    Money amount,
    MerchantId merchant,
    TimePoint now) {
    if (impl_->mode == LedgerMode::Partitioned) {
        const std::size_t index = impl_->partitioner.partition_of(cardholder);
        return impl_->partitions[index]->reserve(std::move(cardholder), amount,
                                                  std::move(merchant), now);
    }

    // The genesis fixture fixes the wallet set, so existence, currency, and kind never change.
    // Reading them needs no stripe; only balances do.
    const auto wallet_it = impl_->wallets.find(cardholder);
    if (wallet_it == impl_->wallets.end()) {
        return Result<Hold, LedgerError>::err(LedgerError::UnknownWallet);
    }

    if (amount.currency() != wallet_it->second.currency()) {
        return Result<Hold, LedgerError>::err(LedgerError::CurrencyMismatch);
    }

    if (amount.minor_units() <= 0) {
        return Result<Hold, LedgerError>::err(LedgerError::InvalidAmount);
    }

    {
        const auto wallet_locks = impl_->locks.lock({cardholder});

        const Money available = wallet_it->second.balance(Bucket::Available);
        if (available.minor_units() < amount.minor_units()) {
            return Result<Hold, LedgerError>::err(LedgerError::InsufficientFunds);
        }

        const PostingBatch batch{
            PostingLine{cardholder, Bucket::Available, amount, false},
            PostingLine{cardholder, Bucket::Holds, amount, true},
        };
        assert_balanced(batch);

        const auto apply_result = apply_batch(impl_->wallets, batch);
        if (!apply_result.has_value()) {
            return Result<Hold, LedgerError>::err(LedgerError::UnknownWallet);
        }
    }

    // The hold only becomes visible to other verbs after its postings exist, and the metadata
    // mutex is never held while a stripe is held, so no lock cycle is possible.
    std::optional<Hold> created;
    {
        std::lock_guard metadata_lock(impl_->metadata_mutex);
        created = Hold{
            HoldId{impl_->next_hold_id++},
            cardholder,
            merchant,
            amount,
            now + impl_->hold_ttl,
        };
        impl_->holds.emplace(created->id, created.value());
        if (impl_->wal != nullptr) {
            impl_->wal->append_reserve(created.value());
        }
    }
    return Result<Hold, LedgerError>::ok(created.value());
}

Result<void, LedgerError> Ledger::capture(HoldId hold_id, Money amount) {
    if (impl_->mode == LedgerMode::Partitioned) {
        return run_capture_2pc(impl_->partitions, impl_->partitioner, impl_->system_account,
                               impl_->wal_sink, impl_->next_txn.fetch_add(1), hold_id, amount);
    }

    std::optional<Hold> claimed;
    {
        std::lock_guard metadata_lock(impl_->metadata_mutex);
        const auto hold_it = impl_->holds.find(hold_id);
        if (hold_it == impl_->holds.end()) {
            return Result<void, LedgerError>::err(LedgerError::UnknownHold);
        }
        if (hold_it->second.amount != amount) {
            return Result<void, LedgerError>::err(LedgerError::AmountMismatch);
        }
        claimed = hold_it->second;
        impl_->holds.erase(hold_it);
        if (impl_->wal != nullptr) {
            impl_->wal->append_capture(hold_id, amount);
        }
    }

    const Hold& hold = claimed.value();

    // Claiming the hold makes a second capture see UnknownHold instead of paying twice, so the
    // postings can run under wallet stripes only. Existence and currency are immutable, which is
    // why the checks below can run without a stripe and a failure can put the hold back.
    const auto restore = [this, &hold] {
        std::lock_guard metadata_lock(impl_->metadata_mutex);
        impl_->holds.emplace(hold.id, hold);
    };

    const AccountId merchant_account{hold.merchant.value()};
    const auto merchant_it = impl_->wallets.find(merchant_account);
    if (merchant_it == impl_->wallets.end()) {
        restore();
        return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
    }
    if (merchant_it->second.currency() != hold.amount.currency()) {
        restore();
        return Result<void, LedgerError>::err(LedgerError::CurrencyMismatch);
    }

    const auto system_account = find_system_account(impl_->wallets);
    if (!system_account.has_value()) {
        restore();
        return Result<void, LedgerError>::err(system_account.error());
    }

    const auto system_it = impl_->wallets.find(system_account.value());
    if (system_it == impl_->wallets.end()) {
        restore();
        return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
    }
    if (system_it->second.currency() != hold.amount.currency()) {
        restore();
        return Result<void, LedgerError>::err(LedgerError::CurrencyMismatch);
    }

    const CaptureSplit split = split_capture(amount);
    const PostingBatch batch{
        PostingLine{hold.cardholder, Bucket::Holds, amount, false},
        PostingLine{merchant_account, Bucket::Payable, split.payable, true},
        PostingLine{system_account.value(), Bucket::Interchange, split.interchange, true},
    };
    assert_balanced(batch);

    const auto wallet_locks =
        impl_->locks.lock({hold.cardholder, merchant_account, system_account.value()});
    const auto apply_result = apply_batch(impl_->wallets, batch);
    if (!apply_result.has_value()) {
        return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
    }

    return Result<void, LedgerError>::ok();
}

Result<void, LedgerError> Ledger::reverse(HoldId hold_id) {
    if (impl_->mode == LedgerMode::Partitioned) {
        const std::size_t index = hold_partition(hold_id);
        if (index >= impl_->partitions.size()) {
            return Result<void, LedgerError>::err(LedgerError::UnknownHold);
        }
        return impl_->partitions[index]->reverse(hold_id);
    }

    std::optional<Hold> claimed;
    {
        std::lock_guard metadata_lock(impl_->metadata_mutex);
        const auto hold_it = impl_->holds.find(hold_id);
        if (hold_it == impl_->holds.end()) {
            return Result<void, LedgerError>::err(LedgerError::UnknownHold);
        }
        claimed = hold_it->second;
        impl_->holds.erase(hold_it);
        if (impl_->wal != nullptr) {
            impl_->wal->append_reverse(hold_id);
        }
    }

    const Hold& hold = claimed.value();
    const auto wallet_locks = impl_->locks.lock({hold.cardholder});
    const auto release_result = release_hold(impl_->wallets, hold);
    if (!release_result.has_value()) {
        std::lock_guard metadata_lock(impl_->metadata_mutex);
        impl_->holds.emplace(hold.id, hold);
        return release_result;
    }

    return Result<void, LedgerError>::ok();
}

std::size_t Ledger::expire_due(TimePoint now) {
    if (impl_->mode == LedgerMode::Partitioned) {
        std::size_t released = 0;
        for (const auto& partition : impl_->partitions) {
            released += partition->expire_due(now);
        }
        return released;
    }

    std::vector<Hold> expired;
    {
        std::lock_guard metadata_lock(impl_->metadata_mutex);
        std::vector<HoldId> expired_ids;
        for (auto it = impl_->holds.begin(); it != impl_->holds.end();) {
            if (it->second.expires_at <= now) {
                expired.push_back(it->second);
                expired_ids.push_back(it->second.id);
                it = impl_->holds.erase(it);
            } else {
                ++it;
            }
        }
        if (impl_->wal != nullptr && !expired_ids.empty()) {
            impl_->wal->append_expire(now, expired_ids);
        }
    }

    // Each released hold touches exactly one cardholder wallet, and never more than one stripe at
    // a time, so the sweep cannot cycle with a capture that is holding several stripes.
    std::size_t expired_count = 0;
    for (const Hold& hold : expired) {
        const auto wallet_locks = impl_->locks.lock({hold.cardholder});
        if (release_hold(impl_->wallets, hold).has_value()) {
            ++expired_count;
        }
    }

    return expired_count;
}

void Ledger::set_hold_ttl(std::chrono::seconds ttl) {
    if (impl_->mode == LedgerMode::Partitioned) {
        for (const auto& partition : impl_->partitions) {
            partition->set_hold_ttl(ttl);
        }
        return;
    }

    std::lock_guard metadata_lock(impl_->metadata_mutex);
    impl_->hold_ttl = ttl;
}

void Ledger::set_wal(Wal* wal) {
    if (impl_->mode == LedgerMode::Partitioned) {
        impl_->wal_sink.set(wal);
        return;
    }

    std::lock_guard metadata_lock(impl_->metadata_mutex);
    impl_->wal = wal;
}

Result<Money, LedgerError> Ledger::balance(AccountId id, Bucket bucket) const {
    if (impl_->mode == LedgerMode::Partitioned) {
        const std::size_t index = impl_->partitioner.partition_of(id);
        return impl_->partitions[index]->balance(std::move(id), bucket);
    }

    const auto it = impl_->wallets.find(id);
    if (it == impl_->wallets.end()) {
        return Result<Money, LedgerError>::err(LedgerError::UnknownWallet);
    }

    const auto wallet_locks = impl_->locks.lock({id});
    return Result<Money, LedgerError>::ok(it->second.balance(bucket));
}

std::size_t Ledger::live_hold_count() const {
    if (impl_->mode == LedgerMode::Partitioned) {
        std::size_t count = 0;
        for (const auto& partition : impl_->partitions) {
            count += partition->live_hold_count();
        }
        return count;
    }

    std::lock_guard metadata_lock(impl_->metadata_mutex);
    return impl_->holds.size();
}

std::size_t Ledger::partition_count() const {
    return impl_->partitioner.size();
}

std::size_t Ledger::partition_of(AccountId id) const {
    return impl_->partitioner.partition_of(id);
}

} // namespace aegis::ledger
