#pragma once

#include <aegis/ids.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/posting.hpp>
#include <aegis/ledger/wal.hpp>
#include <aegis/ledger/wallet.hpp>
#include <aegis/money.hpp>
#include <aegis/result.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegis::ledger {

// Stage 3 of the ledger concurrency arc.
//
// Wallets hash into a fixed set of partitions, and each partition owns a disjoint slice of them.
// The mapping is pure: it takes no locks and never changes for a given partition count, so a verb
// can decide which writer to route to before it touches any wallet.
class WalletPartitioner {
public:
    explicit WalletPartitioner(std::size_t partition_count) : count_(partition_count) {
        assert(partition_count > 0);
    }

    [[nodiscard]] std::size_t size() const {
        return count_;
    }

    [[nodiscard]] std::size_t partition_of(const AccountId& account) const {
        return std::hash<AccountId>{}(account) % count_;
    }

private:
    std::size_t count_;
};

// A hold id carries the partition that owns it, so capture and reverse can route from the id alone
// without a directory of every live hold. The high half is the partition, the low half a counter
// local to that partition; the two halves never overlap, so ids minted on different partitions
// cannot collide.
inline constexpr unsigned kHoldPartitionShift = 32;
inline constexpr std::uint64_t kHoldLocalMask = (std::uint64_t{1} << kHoldPartitionShift) - 1;

[[nodiscard]] inline HoldId make_hold_id(std::size_t partition, std::uint64_t local) {
    assert(partition <= kHoldLocalMask);
    assert(local != 0 && local <= kHoldLocalMask);
    return HoldId{(static_cast<std::uint64_t>(partition) << kHoldPartitionShift) | local};
}

[[nodiscard]] inline std::size_t hold_partition(HoldId id) {
    return static_cast<std::size_t>(id.value() >> kHoldPartitionShift);
}

[[nodiscard]] inline std::uint64_t hold_local(HoldId id) {
    return id.value() & kHoldLocalMask;
}

// A capture's identity is not a hold id. The tag stops a partition index, a hold, or a raw counter
// from being passed where the two-phase transaction belongs.
struct CaptureTxnIdTag {};

using CaptureTxnId = Tagged<std::uint64_t, CaptureTxnIdTag>;

// What one partition is holding for a capture that has prepared but not yet committed. The claimed
// hold is only set on the partition that owns it; the lines are the postings this partition will
// apply if the capture commits.
struct PreparedCapture {
    std::optional<Hold> claimed;
    std::vector<PostingLine> lines;
};

// Every partition appends through one sink, so the single WAL buffer is never written from two
// writer threads at once. A null sink is the benchmark path: nothing is durable and nothing is locked.
class WalSink {
public:
    void set(Wal* wal) {
        wal_.store(wal, std::memory_order_release);
    }

    template <class F>
    void with(F&& append) {
        if (wal_.load(std::memory_order_acquire) == nullptr) {
            return;
        }
        std::lock_guard lock(mutex_);
        if (Wal* wal = wal_.load(std::memory_order_acquire)) {
            append(*wal);
        }
    }

private:
    std::atomic<Wal*> wal_{nullptr};
    std::mutex mutex_;
};

// One writer thread owns everything this partition holds. Callers hand it work through submit and
// wait; the writer never shares its state, so there is nothing inside the partition to lock.
class Partition {
public:
    Partition(std::size_t index, std::unordered_map<AccountId, Wallet> wallets, WalSink& wal)
        : index_(index), wallets_(std::move(wallets)), wal_(&wal) {
        writer_ = std::jthread([this](std::stop_token stop) { loop(stop); });
    }

    ~Partition() {
        close();
    }

    Partition(const Partition&) = delete;
    Partition& operator=(const Partition&) = delete;
    Partition(Partition&&) = delete;
    Partition& operator=(Partition&&) = delete;

    [[nodiscard]] std::size_t index() const {
        return index_;
    }

    // The hold is only visible once its postings exist, and both happen on this one writer, so a
    // concurrent capture can never see the money move without the hold that explains it.
    [[nodiscard]] Result<Hold, LedgerError> reserve(AccountId cardholder,
                                                     Money amount,
                                                     MerchantId merchant,
                                                     TimePoint now) {
        return submit([this, cardholder = std::move(cardholder), amount,
                        merchant = std::move(merchant), now] {
                   return reserve_local(cardholder, amount, merchant, now, hold_ttl_);
               })
            .get();
    }

    [[nodiscard]] Result<Money, LedgerError> balance(AccountId id, Bucket bucket) {
        return submit([this, id = std::move(id), bucket] { return balance_local(id, bucket); }).get();
    }

    [[nodiscard]] std::size_t live_hold_count() {
        return submit([this] { return holds_.size(); }).get();
    }

    void set_hold_ttl(std::chrono::seconds ttl) {
        submit([this, ttl] {
            hold_ttl_ = ttl;
        }).get();
    }

    // A copy of a live hold, without claiming it. Capture uses this to learn the merchant before
    // any partition is fenced; the prepare that follows still revalidates the hold.
    [[nodiscard]] Result<Hold, LedgerError> find_hold(HoldId hold_id) {
        return submit([this, hold_id] {
                   const auto it = holds_.find(hold_id);
                   if (it == holds_.end()) {
                       return Result<Hold, LedgerError>::err(LedgerError::UnknownHold);
                   }
                   return Result<Hold, LedgerError>::ok(it->second);
               })
            .get();
    }

    // Releases every hold that is already due and returns how many it released. A hold touches only
    // its own cardholder, and that cardholder lives on this partition, so the sweep never leaves it.
    // Lifts the hold out of the live set and remembers it against the transaction. A second capture
    // of the same hold now finds nothing, which is what stops it paying twice. A successful prepare
    // owns the writer until commit or rollback, so a balance read cannot see this debit alone.
    [[nodiscard]] Result<Hold, LedgerError> prepare_capture(CaptureTxnId txn,
                                                             HoldId hold_id,
                                                             Money amount) {
        const auto claimed = submit_control(txn, [this, txn, hold_id, amount] {
                                 const auto it = holds_.find(hold_id);
                                 if (it == holds_.end()) {
                                     return Result<Hold, LedgerError>::err(LedgerError::UnknownHold);
                                 }
                                 if (it->second.amount != amount) {
                                     return Result<Hold, LedgerError>::err(
                                         LedgerError::AmountMismatch);
                                 }
                                 PreparedCapture& staged = staged_[txn];
                                 staged.claimed = it->second;
                                 staged.lines.push_back(
                                     PostingLine{it->second.cardholder, Bucket::Holds, amount, false});
                                 holds_.erase(it);
                                 note_owner(txn);
                                 return Result<Hold, LedgerError>::ok(staged.claimed.value());
                             })
                                 .get();
        return claimed;
    }

    // Remembers a credit to apply when the transaction commits. The line is not posted yet, so a
    // later refusal can still discard it.
    [[nodiscard]] Result<void, LedgerError> prepare_credit(CaptureTxnId txn,
                                                            AccountId account,
                                                            Bucket bucket,
                                                            Money amount) {
        return submit_control(txn, [this, txn, account = std::move(account), bucket, amount] {
                   const auto wallet = wallets_.find(account);
                   if (wallet == wallets_.end()) {
                       return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
                   }
                   if (wallet->second.currency() != amount.currency()) {
                       return Result<void, LedgerError>::err(LedgerError::CurrencyMismatch);
                   }
                   PreparedCapture& staged = staged_[txn];
                   if (staged.claimed.has_value() &&
                       staged.claimed.value().amount.currency() != amount.currency()) {
                       return Result<void, LedgerError>::err(LedgerError::CurrencyMismatch);
                   }
                   staged.lines.push_back(PostingLine{account, bucket, amount, true});
                   note_owner(txn);
                   return Result<void, LedgerError>::ok();
               })
            .get();
    }

    // Gives a live hold back to Available. The hold id names its partition, so this only makes
    // sense on the partition that owns it.
    [[nodiscard]] Result<void, LedgerError> reverse(HoldId hold_id) {
        return submit([this, hold_id] {
                   const auto it = holds_.find(hold_id);
                   if (it == holds_.end()) {
                       return Result<void, LedgerError>::err(LedgerError::UnknownHold);
                   }
                   const Hold hold = it->second;
                   if (!apply_hold_release(wallets_, hold).has_value()) {
                       return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
                   }
                   holds_.erase(it);
                   wal_->with([&](Wal& wal) { wal.append_reverse(hold.id); });
                   return Result<void, LedgerError>::ok();
               })
            .get();
    }

    void commit(CaptureTxnId txn) {
        submit_control(txn, [this, txn] {
            const auto staged = staged_.find(txn);
            if (staged != staged_.end()) {
                for (const PostingLine& line : staged->second.lines) {
                    const Money delta = line.is_credit ? line.amount : -line.amount;
                    const auto applied = wallets_.at(line.account).adjust(line.bucket, delta);
                    assert(applied.has_value());
                }
                staged_.erase(staged);
            }
            release_owner(txn);
        }).get();
    }

    void rollback(CaptureTxnId txn) {
        submit_control(txn, [this, txn] {
            const auto staged = staged_.find(txn);
            if (staged != staged_.end()) {
                if (staged->second.claimed.has_value()) {
                    const Hold& hold = staged->second.claimed.value();
                    holds_.emplace(hold.id, hold);
                }
                staged_.erase(staged);
            }
            release_owner(txn);
        }).get();
    }

    [[nodiscard]] std::size_t expire_due(TimePoint now) {
        return submit([this, now] {
                   std::size_t released = 0;
                   std::vector<HoldId> expired;
                   for (auto it = holds_.begin(); it != holds_.end();) {
                       if (it->second.expires_at > now) {
                           ++it;
                           continue;
                       }
                       const Hold hold = it->second;
                       if (!apply_hold_release(wallets_, hold).has_value()) {
                           ++it;
                           continue;
                       }
                       expired.push_back(hold.id);
                       it = holds_.erase(it);
                       ++released;
                   }
                   if (!expired.empty()) {
                       wal_->with([&](Wal& wal) { wal.append_expire(now, expired); });
                   }
                   return released;
               })
            .get();
    }

    // Runs on the writer thread. The returned future is ready once the writer has finished the work,
    // so the caller observes the partition's state exactly as the writer left it.
    template <class F>
    [[nodiscard]] auto submit(F&& work) -> std::future<std::invoke_result_t<F>> {
        return enqueue(false, std::nullopt, std::forward<F>(work));
    }

    // A prepare, commit, or rollback of the transaction that owns this writer. It runs while ordinary
    // commands wait, and it is the only kind of message that can release that wait.
    template <class F>
    [[nodiscard]] auto submit_control(CaptureTxnId txn, F&& work)
        -> std::future<std::invoke_result_t<F>> {
        return enqueue(true, txn, std::forward<F>(work));
    }

private:
    using Task = std::function<void()>;

    struct Envelope {
        Task task;
        bool control{false};
        std::optional<CaptureTxnId> txn;
    };

    template <class F>
    [[nodiscard]] auto enqueue(bool control, std::optional<CaptureTxnId> txn, F&& work)
        -> std::future<std::invoke_result_t<F>> {
        using Result = std::invoke_result_t<F>;
        // A writer that waits on its own queue can never drain it.
        assert(std::this_thread::get_id() != writer_.get_id());

        auto promise = std::make_shared<std::promise<Result>>();
        std::future<Result> ready = promise->get_future();
        Envelope envelope;
        envelope.control = control;
        envelope.txn = txn;
        envelope.task = [promise, work = std::forward<F>(work)]() mutable {
            try {
                if constexpr (std::is_void_v<Result>) {
                    work();
                    promise->set_value();
                } else {
                    promise->set_value(work());
                }
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        };
        {
            std::lock_guard lock(inbox_mutex_);
            inbox_.push_back(std::move(envelope));
        }
        inbox_cv_.notify_one();
        return ready;
    }

    void note_owner(CaptureTxnId txn) {
        if (!owner_.has_value()) {
            owner_ = txn;
        }
    }

    void release_owner(CaptureTxnId txn) {
        if (owner_.has_value() && owner_.value() == txn) {
            owner_.reset();
        }
    }

    [[nodiscard]] Result<Hold, LedgerError> reserve_local(const AccountId& cardholder,
                                                           Money amount,
                                                           MerchantId merchant,
                                                           TimePoint now,
                                                           std::chrono::seconds ttl) {
        const auto wallet_it = wallets_.find(cardholder);
        if (wallet_it == wallets_.end()) {
            return Result<Hold, LedgerError>::err(LedgerError::UnknownWallet);
        }
        if (amount.currency() != wallet_it->second.currency()) {
            return Result<Hold, LedgerError>::err(LedgerError::CurrencyMismatch);
        }
        if (amount.minor_units() <= 0) {
            return Result<Hold, LedgerError>::err(LedgerError::InvalidAmount);
        }
        if (wallet_it->second.balance(Bucket::Available).minor_units() < amount.minor_units()) {
            return Result<Hold, LedgerError>::err(LedgerError::InsufficientFunds);
        }

        const PostingBatch batch{
            PostingLine{cardholder, Bucket::Available, amount, false},
            PostingLine{cardholder, Bucket::Holds, amount, true},
        };
        assert_balanced(batch);
        if (!apply_batch(wallets_, batch).has_value()) {
            return Result<Hold, LedgerError>::err(LedgerError::UnknownWallet);
        }

        Hold created{
            make_hold_id(index_, next_local_hold_++),
            cardholder,
            std::move(merchant),
            amount,
            now + ttl,
        };
        holds_.emplace(created.id, created);
        wal_->with([&](Wal& wal) { wal.append_reserve(created); });
        return Result<Hold, LedgerError>::ok(created);
    }

    [[nodiscard]] Result<Money, LedgerError> balance_local(const AccountId& id, Bucket bucket) const {
        const auto it = wallets_.find(id);
        if (it == wallets_.end()) {
            return Result<Money, LedgerError>::err(LedgerError::UnknownWallet);
        }
        return Result<Money, LedgerError>::ok(it->second.balance(bucket));
    }

    [[nodiscard]] bool defers(const Envelope& envelope) const {
        if (!owner_.has_value()) {
            return false;
        }
        return !(envelope.control && envelope.txn.has_value() &&
                 envelope.txn.value() == owner_.value());
    }

    void promote_deferred() {
        if (owner_.has_value() || deferred_.empty()) {
            return;
        }
        std::lock_guard lock(inbox_mutex_);
        for (auto it = deferred_.rbegin(); it != deferred_.rend(); ++it) {
            inbox_.push_front(std::move(*it));
        }
        deferred_.clear();
        inbox_cv_.notify_all();
    }

    void loop(std::stop_token stop) {
        while (true) {
            Envelope envelope;
            {
                std::unique_lock lock(inbox_mutex_);
                inbox_cv_.wait(lock, [this, &stop] {
                    return !inbox_.empty() || !deferred_.empty() || closed_ ||
                           stop.stop_requested();
                });
                if (inbox_.empty()) {
                    if (!deferred_.empty() && !owner_.has_value()) {
                        lock.unlock();
                        promote_deferred();
                        continue;
                    }
                    if (closed_ || stop.stop_requested()) {
                        return;
                    }
                    continue;
                }
                envelope = std::move(inbox_.front());
                inbox_.pop_front();
                // Anything queued behind a capture stays queued, including work that arrived before
                // the writer noticed the fence. Only the owning transaction's control runs now.
                if (owner_.has_value()) {
                    std::deque<Envelope> waiting;
                    while (!inbox_.empty()) {
                        Envelope next = std::move(inbox_.front());
                        inbox_.pop_front();
                        const bool owns = next.control && next.txn.has_value() &&
                                          next.txn.value() == owner_.value();
                        if (owns) {
                            waiting.push_back(std::move(next));
                        } else {
                            deferred_.push_back(std::move(next));
                        }
                    }
                    for (auto it = waiting.rbegin(); it != waiting.rend(); ++it) {
                        inbox_.push_front(std::move(*it));
                    }
                }
            }
            if (defers(envelope)) {
                deferred_.push_back(std::move(envelope));
                continue;
            }
            envelope.task();
            promote_deferred();
        }
    }

    void close() {
        {
            std::lock_guard lock(inbox_mutex_);
            closed_ = true;
        }
        inbox_cv_.notify_all();
        if (writer_.joinable()) {
            writer_.request_stop();
            writer_.join();
        }
    }

    std::size_t index_;
    std::unordered_map<AccountId, Wallet> wallets_;
    std::unordered_map<HoldId, Hold> holds_;
    std::unordered_map<CaptureTxnId, PreparedCapture> staged_;
    std::uint64_t next_local_hold_{1};
    std::chrono::seconds hold_ttl_{std::chrono::hours{24}};
    WalSink* wal_;

    std::optional<CaptureTxnId> owner_;
    std::deque<Envelope> deferred_;

    std::mutex inbox_mutex_;
    std::condition_variable inbox_cv_;
    std::deque<Envelope> inbox_;
    bool closed_{false};
    std::jthread writer_;
};

} // namespace aegis::ledger
