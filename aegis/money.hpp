#pragma once

#include <aegis/result.hpp>

#include <cstdint>

namespace aegis {

enum class Currency { Usd, Eur };

enum class MoneyError { MismatchedCurrency };

class Money {
public:
    explicit Money(Currency currency, std::int64_t minor_units)
        : currency_(currency), minor_units_(minor_units) {}

    [[nodiscard]] Currency currency() const {
        return currency_;
    }

    [[nodiscard]] std::int64_t minor_units() const {
        return minor_units_;
    }

    friend bool operator==(const Money& lhs, const Money& rhs) {
        return lhs.currency_ == rhs.currency_ && lhs.minor_units_ == rhs.minor_units_;
    }

    friend Result<Money, MoneyError> operator+(Money lhs, Money rhs) {
        if (lhs.currency_ != rhs.currency_) {
            return Result<Money, MoneyError>::err(MoneyError::MismatchedCurrency);
        }
        return Result<Money, MoneyError>::ok(
            Money{lhs.currency_, lhs.minor_units_ + rhs.minor_units_});
    }

private:
    Currency currency_;
    std::int64_t minor_units_;
};

} // namespace aegis
