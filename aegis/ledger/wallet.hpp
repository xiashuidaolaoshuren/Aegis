#pragma once

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/money.hpp>
#include <aegis/result.hpp>

#include <cassert>

namespace aegis::ledger {

enum class WalletKind { Cardholder, Merchant, System };

enum class PostingError;

class PostingBatch;

class Wallet {
public:
    Wallet(AccountId id, WalletKind kind, Currency currency)
        : id_(std::move(id)),
          kind_(kind),
          currency_(currency),
          available_(currency, 0),
          holds_(currency, 0),
          payable_(currency, 0),
          interchange_(currency, 0) {}

    [[nodiscard]] const AccountId& id() const {
        return id_;
    }

    [[nodiscard]] WalletKind kind() const {
        return kind_;
    }

    [[nodiscard]] Currency currency() const {
        return currency_;
    }

    [[nodiscard]] Money balance(Bucket bucket) const {
        switch (bucket) {
        case Bucket::Available:
            return available_;
        case Bucket::Holds:
            return holds_;
        case Bucket::Payable:
            return payable_;
        case Bucket::Interchange:
            return interchange_;
        }
        return available_;
    }

    void adjust(Bucket bucket, Money delta) {
        Money* target = nullptr;
        switch (bucket) {
        case Bucket::Available:
            target = &available_;
            break;
        case Bucket::Holds:
            target = &holds_;
            break;
        case Bucket::Payable:
            target = &payable_;
            break;
        case Bucket::Interchange:
            target = &interchange_;
            break;
        }
        assert(target != nullptr);

        const auto result = *target + delta;
        assert(result.has_value());
        *target = result.value();
    }

private:
    AccountId id_;
    WalletKind kind_;
    Currency currency_;
    Money available_;
    Money holds_;
    Money payable_;
    Money interchange_;
};

} // namespace aegis::ledger
