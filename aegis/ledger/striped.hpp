#pragma once

#include <aegis/ids.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <vector>

namespace aegis::ledger {

// Stage 2 of the ledger concurrency arc.
//
// Wallets hash into a fixed set of stripes, so postings on unrelated wallets proceed in parallel
// instead of queueing behind one mutex. A capture touches the cardholder, the merchant, and the
// system wallet at once, which means a verb can need several stripes. Acquiring them in ascending
// index order gives every verb the same total order -- that is what keeps multi-stripe verbs
// deadlock-free, and it is the lesson stage 2 exists to teach.
class StripedLocks {
public:
    static constexpr std::size_t kDefaultStripeCount = 16;

    explicit StripedLocks(std::size_t stripe_count = kDefaultStripeCount) : stripes_(stripe_count) {
        assert(stripe_count > 0);
    }

    StripedLocks(const StripedLocks&) = delete;
    StripedLocks& operator=(const StripedLocks&) = delete;

    [[nodiscard]] std::size_t size() const {
        return stripes_.size();
    }

    [[nodiscard]] std::size_t stripe_of(const AccountId& account) const {
        return std::hash<AccountId>{}(account) % stripes_.size();
    }

    // Distinct stripe indices for these accounts, ascending. Pure: takes no locks.
    [[nodiscard]] std::vector<std::size_t> stripe_indices(
        std::initializer_list<AccountId> accounts) const {
        std::vector<std::size_t> indices;
        indices.reserve(accounts.size());
        for (const AccountId& account : accounts) {
            indices.push_back(stripe_of(account));
        }
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    }

    // Locks every distinct stripe these accounts map to, in ascending index order.
    [[nodiscard]] std::vector<std::unique_lock<std::mutex>> lock(
        std::initializer_list<AccountId> accounts) const {
        const std::vector<std::size_t> indices = stripe_indices(accounts);
        std::vector<std::unique_lock<std::mutex>> held;
        held.reserve(indices.size());
        for (const std::size_t index : indices) {
            held.emplace_back(stripes_[index]);
        }
        return held;
    }

private:
    mutable std::vector<std::mutex> stripes_;
};

} // namespace aegis::ledger
