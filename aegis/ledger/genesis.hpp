#pragma once

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/wallet.hpp>
#include <aegis/money.hpp>
#include <aegis/result.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace aegis::ledger {

enum class GenesisError { IoFailure, ParseError, DuplicateWallet };

struct GenesisRecord {
    WalletKind kind;
    AccountId id;
    Currency currency;
    std::int64_t available_minor;
};

namespace detail {

[[nodiscard]] inline std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] inline Result<WalletKind, GenesisError> parse_wallet_kind(const std::string& kind) {
    if (kind == "cardholder") {
        return Result<WalletKind, GenesisError>::ok(WalletKind::Cardholder);
    }
    if (kind == "merchant") {
        return Result<WalletKind, GenesisError>::ok(WalletKind::Merchant);
    }
    if (kind == "system") {
        return Result<WalletKind, GenesisError>::ok(WalletKind::System);
    }
    return Result<WalletKind, GenesisError>::err(GenesisError::ParseError);
}

[[nodiscard]] inline Result<Currency, GenesisError> parse_currency(const std::string& currency) {
    if (currency == "Usd") {
        return Result<Currency, GenesisError>::ok(Currency::Usd);
    }
    if (currency == "Eur") {
        return Result<Currency, GenesisError>::ok(Currency::Eur);
    }
    return Result<Currency, GenesisError>::err(GenesisError::ParseError);
}

[[nodiscard]] inline Result<std::int64_t, GenesisError> parse_available_minor(
    const std::string& available_minor) {
    try {
        std::size_t consumed = 0;
        const long long value = std::stoll(available_minor, &consumed);
        if (consumed != available_minor.size()) {
            return Result<std::int64_t, GenesisError>::err(GenesisError::ParseError);
        }
        return Result<std::int64_t, GenesisError>::ok(static_cast<std::int64_t>(value));
    } catch (const std::exception&) {
        return Result<std::int64_t, GenesisError>::err(GenesisError::ParseError);
    }
}

[[nodiscard]] inline Result<GenesisRecord, GenesisError> parse_genesis_line(
    const std::string& line) {
    std::string trimmed = trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
        return Result<GenesisRecord, GenesisError>::err(GenesisError::ParseError);
    }

    std::vector<std::string> fields;
    std::stringstream stream(trimmed);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(trim(field));
    }
    if (fields.size() != 4) {
        return Result<GenesisRecord, GenesisError>::err(GenesisError::ParseError);
    }

    const auto kind = parse_wallet_kind(fields[0]);
    if (!kind.has_value()) {
        return Result<GenesisRecord, GenesisError>::err(kind.error());
    }

    const auto currency = parse_currency(fields[2]);
    if (!currency.has_value()) {
        return Result<GenesisRecord, GenesisError>::err(currency.error());
    }

    const auto available_minor = parse_available_minor(fields[3]);
    if (!available_minor.has_value()) {
        return Result<GenesisRecord, GenesisError>::err(available_minor.error());
    }

    return Result<GenesisRecord, GenesisError>::ok(GenesisRecord{
        kind.value(),
        AccountId{fields[1]},
        currency.value(),
        available_minor.value(),
    });
}

} // namespace detail

[[nodiscard]] inline Result<std::unordered_map<AccountId, Wallet>, GenesisError>
load_genesis(std::span<const GenesisRecord> records) {
    std::unordered_map<AccountId, Wallet> wallets;

    for (const GenesisRecord& record : records) {
        if (wallets.find(record.id) != wallets.end()) {
            return Result<std::unordered_map<AccountId, Wallet>, GenesisError>::err(
                GenesisError::DuplicateWallet);
        }

        Wallet wallet{record.id, record.kind, record.currency};
        wallet.adjust(Bucket::Available, Money{record.currency, record.available_minor});
        wallets.emplace(record.id, std::move(wallet));
    }

    return Result<std::unordered_map<AccountId, Wallet>, GenesisError>::ok(std::move(wallets));
}

[[nodiscard]] inline Result<std::unordered_map<AccountId, Wallet>, GenesisError>
load_genesis(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return Result<std::unordered_map<AccountId, Wallet>, GenesisError>::err(
            GenesisError::IoFailure);
    }

    std::vector<GenesisRecord> records;
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = detail::trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        const auto record = detail::parse_genesis_line(line);
        if (!record.has_value()) {
            return Result<std::unordered_map<AccountId, Wallet>, GenesisError>::err(record.error());
        }
        records.push_back(record.value());
    }

    return load_genesis(records);
}

} // namespace aegis::ledger
