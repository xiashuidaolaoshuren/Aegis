#include <aegis/ledger/ledger.hpp>

#include <aegis/ledger/fee.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/posting.hpp>
#include <aegis/ledger/wallet.hpp>

#include <utility>
#include <unordered_map>
#include <chrono>

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
    std::unordered_map<AccountId, Wallet> wallets;
    std::unordered_map<HoldId, Hold> holds;
    std::uint64_t next_hold_id{1};
    std::chrono::seconds hold_ttl{std::chrono::hours{24}};
};

Ledger::Ledger(std::unordered_map<AccountId, Wallet> wallets)
    : impl_(new Impl{std::move(wallets), {}, 1}) {}

Ledger::Ledger(Ledger&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

Ledger& Ledger::operator=(Ledger&& other) noexcept {
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

Ledger::~Ledger() {
    delete impl_;
}

Result<Hold, LedgerError> Ledger::reserve(
    AccountId cardholder,
    Money amount,
    MerchantId merchant,
    TimePoint now) {
    const auto it = impl_->wallets.find(cardholder);
    if (it == impl_->wallets.end()) {
        return Result<Hold, LedgerError>::err(LedgerError::UnknownWallet);
    }

    const Money available = it->second.balance(Bucket::Available);
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

    const Hold hold{
        HoldId{impl_->next_hold_id++},
        cardholder,
        merchant,
        amount,
        now + impl_->hold_ttl,
    };
    impl_->holds.emplace(hold.id, hold);
    return Result<Hold, LedgerError>::ok(hold);
}

Result<void, LedgerError> Ledger::capture(HoldId hold_id, Money amount) {
    const auto hold_it = impl_->holds.find(hold_id);
    if (hold_it == impl_->holds.end()) {
        return Result<void, LedgerError>::err(LedgerError::UnknownHold);
    }

    const Hold& hold = hold_it->second;
    if (hold.amount != amount) {
        return Result<void, LedgerError>::err(LedgerError::AmountMismatch);
    }

    const AccountId merchant_account{hold.merchant.value()};

    if (impl_->wallets.find(merchant_account) == impl_->wallets.end()) {
        return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
    }

    const auto system_account = find_system_account(impl_->wallets);
    if (!system_account.has_value()) {
        return Result<void, LedgerError>::err(system_account.error());
    }

    const CaptureSplit split = split_capture(amount);
    const PostingBatch batch{
        PostingLine{hold.cardholder, Bucket::Holds, amount, false},
        PostingLine{merchant_account, Bucket::Payable, split.payable, true},
        PostingLine{system_account.value(), Bucket::Interchange, split.interchange, true},
    };
    assert_balanced(batch);

    const auto apply_result = apply_batch(impl_->wallets, batch);
    if (!apply_result.has_value()) {
        return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
    }

    impl_->holds.erase(hold_it);
    return Result<void, LedgerError>::ok();
}

Result<void, LedgerError> Ledger::reverse(HoldId hold_id) {
    const auto hold_it = impl_->holds.find(hold_id);
    if (hold_it == impl_->holds.end()) {
        return Result<void, LedgerError>::err(LedgerError::UnknownHold);
    }

    const Hold& hold = hold_it->second;
    const auto release_result = release_hold(impl_->wallets, hold);
    if (!release_result.has_value()) {
        return release_result;
    }

    impl_->holds.erase(hold_it);
    return Result<void, LedgerError>::ok();
}

std::size_t Ledger::expire_due(TimePoint now) {
    std::size_t expired_count = 0;

    for (auto it = impl_->holds.begin(); it != impl_->holds.end();) {
        if (it->second.expires_at <= now) {
            const Hold& hold = it->second;
            const auto release_result = release_hold(impl_->wallets, hold);
            if (!release_result.has_value()) {
                ++it;
                continue;
            }
            it = impl_->holds.erase(it);
            ++expired_count;
        } else {
            ++it;
        }
    }

    return expired_count;
}

void Ledger::set_hold_ttl(std::chrono::seconds ttl) {
    impl_->hold_ttl = ttl;
}

Result<Money, LedgerError> Ledger::balance(AccountId id, Bucket bucket) const {
    const auto it = impl_->wallets.find(id);
    if (it == impl_->wallets.end()) {
        return Result<Money, LedgerError>::err(LedgerError::UnknownWallet);
    }
    return Result<Money, LedgerError>::ok(it->second.balance(bucket));
}

std::size_t Ledger::live_hold_count() const {
    return impl_->holds.size();
}

} // namespace aegis::ledger
