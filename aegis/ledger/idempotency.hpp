#pragma once

#include <aegis/ids.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/tagged.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace aegis::ledger {

struct MmddTag {};

using Mmdd = Tagged<std::uint32_t, MmddTag>;

struct IdempotencyKey {
    TerminalId terminal;
    Stan stan;
    Mmdd date;

    friend bool operator==(const IdempotencyKey& lhs, const IdempotencyKey& rhs) {
        return lhs.terminal == rhs.terminal && lhs.stan == rhs.stan && lhs.date == rhs.date;
    }
};

struct IdempotencyKeyHash {
    size_t operator()(const IdempotencyKey& key) const {
        size_t seed = std::hash<aegis::TerminalId>{}(key.terminal);
        seed ^= std::hash<aegis::Stan>{}(key.stan) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed ^= std::hash<Mmdd>{}(key.date) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

class IdempotencyStore {
public:
    [[nodiscard]] const Result<Hold, LedgerError>* find_reserve(IdempotencyKey key) const {
        const auto it = reserve_outcomes_.find(key);
        if (it == reserve_outcomes_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    void store_reserve(IdempotencyKey key, Result<Hold, LedgerError> outcome) {
        if (outcome.has_value()) {
            auth_key_to_hold_id_.emplace(key, outcome.value().id);
        }
        reserve_outcomes_.emplace(std::move(key), std::move(outcome));
    }

    [[nodiscard]] const HoldId* find_hold_id(IdempotencyKey original_auth_key) const {
        const auto it = auth_key_to_hold_id_.find(original_auth_key);
        if (it == auth_key_to_hold_id_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    [[nodiscard]] const Result<void, LedgerError>* find_capture(IdempotencyKey key) const {
        const auto it = capture_outcomes_.find(key);
        if (it == capture_outcomes_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    void store_capture(IdempotencyKey key, Result<void, LedgerError> outcome) {
        capture_outcomes_.emplace(std::move(key), std::move(outcome));
    }

    [[nodiscard]] const Result<void, LedgerError>* find_reverse(IdempotencyKey key) const {
        const auto it = reverse_outcomes_.find(key);
        if (it == reverse_outcomes_.end()) {
            return nullptr;
        }
        return &it->second;
    }

    void store_reverse(IdempotencyKey key, Result<void, LedgerError> outcome) {
        reverse_outcomes_.emplace(std::move(key), std::move(outcome));
    }

    [[nodiscard]] std::size_t size() const {
        return reserve_outcomes_.size() + capture_outcomes_.size() + reverse_outcomes_.size();
    }

private:
    std::unordered_map<IdempotencyKey, Result<Hold, LedgerError>, IdempotencyKeyHash>
        reserve_outcomes_;
    std::unordered_map<IdempotencyKey, Result<void, LedgerError>, IdempotencyKeyHash>
        capture_outcomes_;
    std::unordered_map<IdempotencyKey, Result<void, LedgerError>, IdempotencyKeyHash>
        reverse_outcomes_;
    std::unordered_map<IdempotencyKey, HoldId, IdempotencyKeyHash> auth_key_to_hold_id_;
};

} // namespace aegis::ledger
