#pragma once

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/fee.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/ledger/wallet.hpp>
#include <aegis/money.hpp>
#include <aegis/result.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <unordered_map>

namespace aegis::ledger {

class ShadowLedger {
public:
    explicit ShadowLedger(std::unordered_map<AccountId, Wallet> genesis_wallets) {
        for (const auto& [id, wallet] : genesis_wallets) {
            ShadowWallet shadow{
                wallet.currency(),
                wallet.balance(Bucket::Available).minor_units(),
                0,
                0,
                0,
            };
            wallets_.emplace(id, shadow);
            if (wallet.kind() == WalletKind::System) {
                system_account_ = id;
            }
        }
    }

    void set_hold_ttl(std::chrono::seconds ttl) {
        hold_ttl_ = ttl;
    }

    [[nodiscard]] Result<Hold, LedgerError> reserve(
        AccountId cardholder,
        Money amount,
        MerchantId merchant,
        TimePoint now = TimePoint{}) {
        const auto it = wallets_.find(cardholder);
        if (it == wallets_.end()) {
            return Result<Hold, LedgerError>::err(LedgerError::UnknownWallet);
        }

        if (it->second.available < amount.minor_units()) {
            return Result<Hold, LedgerError>::err(LedgerError::InsufficientFunds);
        }

        it->second.available -= amount.minor_units();
        it->second.holds += amount.minor_units();

        const Hold hold{
            HoldId{next_hold_id_++},
            cardholder,
            merchant,
            amount,
            now + hold_ttl_,
        };
        holds_.emplace(hold.id, hold);
        return Result<Hold, LedgerError>::ok(hold);
    }

    [[nodiscard]] Result<void, LedgerError> capture(HoldId hold_id, Money amount) {
        const auto hold_it = holds_.find(hold_id);
        if (hold_it == holds_.end()) {
            return Result<void, LedgerError>::err(LedgerError::UnknownHold);
        }

        const Hold& hold = hold_it->second;
        if (hold.amount != amount) {
            return Result<void, LedgerError>::err(LedgerError::AmountMismatch);
        }

        const AccountId merchant_account{hold.merchant.value()};
        const auto merchant_it = wallets_.find(merchant_account);
        if (merchant_it == wallets_.end()) {
            return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
        }

        if (!system_account_.has_value()) {
            return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
        }
        const AccountId system_account = system_account_.value();

        const CaptureSplit split = split_capture(amount);
        auto& cardholder_wallet = wallets_.at(hold.cardholder);
        cardholder_wallet.holds -= amount.minor_units();
        merchant_it->second.payable += split.payable.minor_units();
        wallets_.at(system_account).interchange += split.interchange.minor_units();

        holds_.erase(hold_it);
        return Result<void, LedgerError>::ok();
    }

    [[nodiscard]] Result<void, LedgerError> reverse(HoldId hold_id) {
        const auto hold_it = holds_.find(hold_id);
        if (hold_it == holds_.end()) {
            return Result<void, LedgerError>::err(LedgerError::UnknownHold);
        }

        const Hold& hold = hold_it->second;
        auto& cardholder_wallet = wallets_.at(hold.cardholder);
        cardholder_wallet.holds -= hold.amount.minor_units();
        cardholder_wallet.available += hold.amount.minor_units();
        holds_.erase(hold_it);
        return Result<void, LedgerError>::ok();
    }

    [[nodiscard]] std::size_t expire_due(TimePoint now) {
        std::size_t expired_count = 0;

        for (auto it = holds_.begin(); it != holds_.end();) {
            if (it->second.expires_at <= now) {
                auto& cardholder_wallet = wallets_.at(it->second.cardholder);
                cardholder_wallet.holds -= it->second.amount.minor_units();
                cardholder_wallet.available += it->second.amount.minor_units();
                it = holds_.erase(it);
                ++expired_count;
            } else {
                ++it;
            }
        }

        return expired_count;
    }

    [[nodiscard]] Result<Money, LedgerError> balance(AccountId id, Bucket bucket) const {
        const auto it = wallets_.find(id);
        if (it == wallets_.end()) {
            return Result<Money, LedgerError>::err(LedgerError::UnknownWallet);
        }

        const Currency currency = it->second.currency;
        switch (bucket) {
        case Bucket::Available:
            return Result<Money, LedgerError>::ok(Money{currency, it->second.available});
        case Bucket::Holds:
            return Result<Money, LedgerError>::ok(Money{currency, it->second.holds});
        case Bucket::Payable:
            return Result<Money, LedgerError>::ok(Money{currency, it->second.payable});
        case Bucket::Interchange:
            return Result<Money, LedgerError>::ok(Money{currency, it->second.interchange});
        }
        return Result<Money, LedgerError>::ok(Money{currency, 0});
    }

    [[nodiscard]] std::size_t live_hold_count() const {
        return holds_.size();
    }

private:
    struct ShadowWallet {
        Currency currency;
        std::int64_t available;
        std::int64_t holds;
        std::int64_t payable;
        std::int64_t interchange;
    };

    std::unordered_map<AccountId, ShadowWallet> wallets_;
    std::unordered_map<HoldId, Hold> holds_;
    std::optional<AccountId> system_account_;
    std::uint64_t next_hold_id_{1};
    std::chrono::seconds hold_ttl_{std::chrono::hours{24}};
};

} // namespace aegis::ledger
