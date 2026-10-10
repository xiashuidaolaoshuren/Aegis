#pragma once

#include <aegis/ids.hpp>
#include <aegis/iso8583/fields.hpp>
#include <aegis/iso8583/message.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/money.hpp>
#include <aegis/result.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

namespace aegis::authorizer {

struct ScreenConfig {
    std::optional<Money> max_amount;
};

enum class ScreenError {
    Declined,
};

namespace detail {

[[nodiscard]] inline std::int64_t parse_amount_minor(std::string_view digits) {
    std::int64_t value = 0;
    for (const char c : digits) {
        value = value * 10 + static_cast<std::int64_t>(c - '0');
    }
    return value;
}

[[nodiscard]] inline std::optional<Currency> parse_currency_code(std::string_view code) {
    if (code == "840") {
        return Currency::Usd;
    }
    if (code == "978") {
        return Currency::Eur;
    }
    return std::nullopt;
}

} // namespace detail

[[nodiscard]] inline Result<void, ScreenError> screen(
    const iso8583::Message& request,
    const ledger::Ledger& ledger,
    ScreenConfig config) {
    const auto pan = request.get(iso8583::FieldId::Pan);
    const auto amount_field = request.get(iso8583::FieldId::Amount);
    const auto currency_field = request.get(iso8583::FieldId::Currency);
    if (!pan.has_value() || !amount_field.has_value() || !currency_field.has_value()) {
        return Result<void, ScreenError>::err(ScreenError::Declined);
    }

    const std::int64_t minor = detail::parse_amount_minor(amount_field.value());
    if (minor <= 0) {
        return Result<void, ScreenError>::err(ScreenError::Declined);
    }

    const auto currency = detail::parse_currency_code(currency_field.value());
    if (!currency.has_value()) {
        return Result<void, ScreenError>::err(ScreenError::Declined);
    }

    const AccountId cardholder{pan.value()};
    const auto available = ledger.balance(cardholder, ledger::Bucket::Available);
    if (!available.has_value()) {
        return Result<void, ScreenError>::err(ScreenError::Declined);
    }
    if (available.value().currency() != currency.value()) {
        return Result<void, ScreenError>::err(ScreenError::Declined);
    }

    const Money amount{currency.value(), minor};
    if (config.max_amount.has_value()) {
        if (config.max_amount->currency() != amount.currency() ||
            amount.minor_units() > config.max_amount->minor_units()) {
            return Result<void, ScreenError>::err(ScreenError::Declined);
        }
    }

    return Result<void, ScreenError>::ok();
}

} // namespace aegis::authorizer
