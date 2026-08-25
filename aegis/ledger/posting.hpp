#pragma once

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/wallet.hpp>
#include <aegis/money.hpp>
#include <aegis/result.hpp>

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <unordered_map>
#include <vector>

namespace aegis::ledger {

enum class PostingError { Unbalanced, UnknownWallet };

struct PostingLine {
    AccountId account;
    Bucket bucket;
    Money amount;
    bool is_credit;
};

class PostingBatch {
public:
    PostingBatch() = default;

    PostingBatch(std::initializer_list<PostingLine> lines) : lines_(lines) {}

    [[nodiscard]] std::size_t size() const {
        return lines_.size();
    }

    [[nodiscard]] const PostingLine& operator[](std::size_t index) const {
        return lines_[index];
    }

    [[nodiscard]] std::vector<PostingLine>::const_iterator begin() const {
        return lines_.begin();
    }

    [[nodiscard]] std::vector<PostingLine>::const_iterator end() const {
        return lines_.end();
    }

private:
    std::vector<PostingLine> lines_;
};

[[nodiscard]] inline bool is_balanced(const PostingBatch& batch) {
    std::int64_t total = 0;
    for (const PostingLine& line : batch) {
        const std::int64_t signed_amount =
            line.is_credit ? line.amount.minor_units() : -line.amount.minor_units();
        total += signed_amount;
    }
    return total == 0;
}

[[nodiscard]] inline Result<void, PostingError> validate(const PostingBatch& batch) {
    if (!is_balanced(batch)) {
        return Result<void, PostingError>::err(PostingError::Unbalanced);
    }
    return Result<void, PostingError>::ok();
}

inline void assert_balanced(const PostingBatch& batch) {
    assert(is_balanced(batch));
}

[[nodiscard]] inline Result<void, PostingError> apply_batch(
    std::unordered_map<AccountId, Wallet>& wallets,
    const PostingBatch& batch) {
    if (!is_balanced(batch)) {
        return Result<void, PostingError>::err(PostingError::Unbalanced);
    }

    for (const PostingLine& line : batch) {
        if (wallets.find(line.account) == wallets.end()) {
            return Result<void, PostingError>::err(PostingError::UnknownWallet);
        }
    }

    for (const PostingLine& line : batch) {
        const Money delta = line.is_credit ? line.amount : -line.amount;
        wallets.at(line.account).adjust(line.bucket, delta);
    }

    return Result<void, PostingError>::ok();
}

} // namespace aegis::ledger
