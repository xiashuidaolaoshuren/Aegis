#pragma once

#include <aegis/ledger/fee.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/ledger/posting.hpp>
#include <aegis/ledger/wallet.hpp>
#include <aegis/result.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegis::ledger {

enum class WalError { IoFailure, CorruptRecord };

class Wal {
public:
    explicit Wal(std::filesystem::path path) : path_(std::move(path)) {}

    ~Wal() = default;

    void append_reserve(const Hold& hold) {
        const auto expires_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    hold.expires_at.time_since_epoch())
                                    .count();
        buffer_.append("R|");
        buffer_.append(std::to_string(hold.id.value()));
        buffer_.push_back('|');
        buffer_.append(hold.cardholder.value());
        buffer_.push_back('|');
        buffer_.append(hold.merchant.value());
        buffer_.push_back('|');
        buffer_.append(std::to_string(hold.amount.minor_units()));
        buffer_.push_back('|');
        buffer_.append(currency_to_string(hold.amount.currency()));
        buffer_.push_back('|');
        buffer_.append(std::to_string(expires_ns));
        buffer_.push_back('\n');
    }

    void append_capture(HoldId hold, Money amount) {
        buffer_.append("C|");
        buffer_.append(std::to_string(hold.value()));
        buffer_.push_back('|');
        buffer_.append(std::to_string(amount.minor_units()));
        buffer_.push_back('\n');
    }

    void append_reverse(HoldId hold) {
        buffer_.append("V|");
        buffer_.append(std::to_string(hold.value()));
        buffer_.push_back('\n');
    }

    void append_expire(TimePoint now, const std::vector<HoldId>& expired) {
        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                now.time_since_epoch())
                                .count();
        buffer_.append("X|");
        buffer_.append(std::to_string(now_ns));
        buffer_.push_back('|');
        for (std::size_t i = 0; i < expired.size(); ++i) {
            if (i > 0) {
                buffer_.push_back(',');
            }
            buffer_.append(std::to_string(expired[i].value()));
        }
        buffer_.push_back('\n');
    }

    void flush() {
        if (buffer_.empty()) {
            return;
        }

        std::ofstream out(path_, std::ios::app | std::ios::binary);
        if (!out) {
            return;
        }
        out.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
        buffer_.clear();
    }

    static Result<Ledger, WalError> replay(
        const std::filesystem::path& path,
        std::unordered_map<AccountId, Wallet> genesis) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            return Result<Ledger, WalError>::err(WalError::IoFailure);
        }

        std::string content(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());

        if (!content.empty() && content.back() != '\n') {
            const auto last_newline = content.rfind('\n');
            if (last_newline == std::string::npos) {
                content.clear();
            } else {
                content.resize(last_newline + 1);
            }
        }

        std::unordered_map<HoldId, Hold> holds;
        std::uint64_t next_hold_id = 1;
        auto wallets = std::move(genesis);

        std::size_t pos = 0;
        while (pos < content.size()) {
            const auto end = content.find('\n', pos);
            const std::string line = content.substr(pos, end - pos);
            pos = (end == std::string::npos) ? content.size() : end + 1;

            if (line.empty()) {
                continue;
            }

            const auto fields = split_fields(line, '|');
            if (fields.empty()) {
                return Result<Ledger, WalError>::err(WalError::CorruptRecord);
            }

            const std::string_view kind = fields[0];
            if (kind == "R") {
                if (fields.size() != 7) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const auto hold_id = parse_u64(fields[1]);
                const AccountId cardholder{std::string{fields[2]}};
                const MerchantId merchant{std::string{fields[3]}};
                const auto minor = parse_i64(fields[4]);
                const auto currency = parse_currency(fields[5]);
                const auto expires_ns = parse_i64(fields[6]);
                if (!hold_id.has_value() || !minor.has_value() || !currency.has_value() ||
                    !expires_ns.has_value()) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const Money amount{currency.value(), minor.value()};
                const TimePoint expires_at{
                    std::chrono::nanoseconds{expires_ns.value()}};

                const PostingBatch batch{
                    PostingLine{cardholder, Bucket::Available, amount, false},
                    PostingLine{cardholder, Bucket::Holds, amount, true},
                };
                assert_balanced(batch);
                if (!apply_batch(wallets, batch).has_value()) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const Hold hold{
                    HoldId{hold_id.value()},
                    cardholder,
                    merchant,
                    amount,
                    expires_at,
                };
                holds.emplace(hold.id, hold);
                next_hold_id = std::max(next_hold_id, hold_id.value() + 1);
            } else if (kind == "C") {
                if (fields.size() != 3) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const auto hold_id = parse_u64(fields[1]);
                const auto minor = parse_i64(fields[2]);
                if (!hold_id.has_value() || !minor.has_value()) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const auto hold_it = holds.find(HoldId{hold_id.value()});
                if (hold_it == holds.end()) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const Hold& hold = hold_it->second;
                const Money amount{hold.amount.currency(), minor.value()};
                if (hold.amount != amount) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const AccountId merchant_account{hold.merchant.value()};
                const auto system_account = find_system_account(wallets);
                if (!system_account.has_value()) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const CaptureSplit split = split_capture(amount);
                const PostingBatch batch{
                    PostingLine{hold.cardholder, Bucket::Holds, amount, false},
                    PostingLine{merchant_account, Bucket::Payable, split.payable, true},
                    PostingLine{
                        system_account.value(),
                        Bucket::Interchange,
                        split.interchange,
                        true},
                };
                assert_balanced(batch);
                if (!apply_batch(wallets, batch).has_value()) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                holds.erase(hold_it);
            } else if (kind == "V") {
                if (fields.size() != 2) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const auto hold_id = parse_u64(fields[1]);
                if (!hold_id.has_value()) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const auto hold_it = holds.find(HoldId{hold_id.value()});
                if (hold_it == holds.end()) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const Hold& hold = hold_it->second;
                const PostingBatch batch{
                    PostingLine{hold.cardholder, Bucket::Holds, hold.amount, false},
                    PostingLine{hold.cardholder, Bucket::Available, hold.amount, true},
                };
                assert_balanced(batch);
                if (!apply_batch(wallets, batch).has_value()) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                holds.erase(hold_it);
            } else if (kind == "X") {
                if (fields.size() != 3) {
                    return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                }

                const auto expired_ids = split_fields(fields[2], ',');
                for (const std::string_view id_field : expired_ids) {
                    const auto hold_id = parse_u64(id_field);
                    if (!hold_id.has_value()) {
                        return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                    }

                    const auto hold_it = holds.find(HoldId{hold_id.value()});
                    if (hold_it == holds.end()) {
                        return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                    }

                    const Hold& hold = hold_it->second;
                    const PostingBatch batch{
                        PostingLine{hold.cardholder, Bucket::Holds, hold.amount, false},
                        PostingLine{hold.cardholder, Bucket::Available, hold.amount, true},
                    };
                    assert_balanced(batch);
                    if (!apply_batch(wallets, batch).has_value()) {
                        return Result<Ledger, WalError>::err(WalError::CorruptRecord);
                    }

                    holds.erase(hold_it);
                }
            } else {
                return Result<Ledger, WalError>::err(WalError::CorruptRecord);
            }
        }

        return Result<Ledger, WalError>::ok(
            Ledger(std::move(wallets), std::move(holds), next_hold_id));
    }

private:
    static std::vector<std::string_view> split_fields(
        std::string_view text,
        char delimiter) {
        std::vector<std::string_view> fields;
        std::size_t start = 0;
        while (start <= text.size()) {
            const auto end = text.find(delimiter, start);
            if (end == std::string_view::npos) {
                fields.emplace_back(text.substr(start));
                break;
            }
            fields.emplace_back(text.substr(start, end - start));
            start = end + 1;
        }
        return fields;
    }

    static std::optional<std::uint64_t> parse_u64(std::string_view text) {
        if (text.empty()) {
            return std::nullopt;
        }
        try {
            std::size_t consumed = 0;
            const unsigned long long value = std::stoull(std::string{text}, &consumed);
            if (consumed != text.size()) {
                return std::nullopt;
            }
            return static_cast<std::uint64_t>(value);
        } catch (...) {
            return std::nullopt;
        }
    }

    static std::optional<std::int64_t> parse_i64(std::string_view text) {
        if (text.empty()) {
            return std::nullopt;
        }
        try {
            std::size_t consumed = 0;
            const long long value = std::stoll(std::string{text}, &consumed);
            if (consumed != text.size()) {
                return std::nullopt;
            }
            return static_cast<std::int64_t>(value);
        } catch (...) {
            return std::nullopt;
        }
    }

    static std::optional<Currency> parse_currency(std::string_view text) {
        if (text == "Usd") {
            return Currency::Usd;
        }
        if (text == "Eur") {
            return Currency::Eur;
        }
        return std::nullopt;
    }

    static const char* currency_to_string(Currency currency) {
        switch (currency) {
        case Currency::Usd:
            return "Usd";
        case Currency::Eur:
            return "Eur";
        }
        return "Usd";
    }

    static Result<AccountId, WalError> find_system_account(
        const std::unordered_map<AccountId, Wallet>& wallets) {
        for (const auto& [id, wallet] : wallets) {
            if (wallet.kind() == WalletKind::System) {
                return Result<AccountId, WalError>::ok(id);
            }
        }
        return Result<AccountId, WalError>::err(WalError::CorruptRecord);
    }

    std::filesystem::path path_;
    std::string buffer_;
};

} // namespace aegis::ledger
