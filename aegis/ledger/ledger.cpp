#include <aegis/ledger/ledger.hpp>

#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/posting.hpp>
#include <aegis/ledger/wallet.hpp>

#include <utility>
#include <vector>

namespace aegis::ledger {

struct Ledger::Impl {
    std::unordered_map<AccountId, Wallet> wallets;
    std::vector<Hold> holds;
    std::uint64_t next_hold_id{1};
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
    MerchantId merchant) {
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
    };
    impl_->holds.push_back(hold);
    return Result<Hold, LedgerError>::ok(hold);
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
