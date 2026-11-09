#pragma once

#include <aegis/ledger/fee.hpp>
#include <aegis/ledger/partition.hpp>
#include <aegis/result.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace aegis::ledger {

// Capture always moves one hold onto a merchant and the system wallet, and at stage 3 those three
// live on their own writer threads. Every participant prepares before any of them commits, and a
// single refusal rolls back everyone who already prepared, so a capture pays all three or none.
[[nodiscard]] inline Result<void, LedgerError> run_capture_2pc(
    std::span<const std::unique_ptr<Partition>> partitions,
    const WalletPartitioner& partitioner,
    const std::optional<AccountId>& system_account,
    WalSink& wal,
    std::uint64_t txn,
    HoldId hold_id,
    Money amount) {
    const std::size_t holder_index = hold_partition(hold_id);
    if (holder_index >= partitions.size()) {
        return Result<void, LedgerError>::err(LedgerError::UnknownHold);
    }

    std::vector<std::size_t> prepared;
    const auto rollback_all = [&] {
        for (auto it = prepared.rbegin(); it != prepared.rend(); ++it) {
            partitions[*it]->rollback(txn);
        }
    };

    // The hold names its merchant, so the cardholder partition has to prepare first and hand it back.
    // Nothing is shared between writers, so waiting for one prepare before starting the next cannot
    // deadlock: a writer only ever touches its own wallets.
    const auto claimed = partitions[holder_index]->prepare_capture(txn, hold_id, amount);
    if (!claimed.has_value()) {
        return Result<void, LedgerError>::err(claimed.error());
    }
    prepared.push_back(holder_index);
    const Hold hold = claimed.value();

    if (!system_account.has_value()) {
        rollback_all();
        return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
    }

    const AccountId merchant_account{hold.merchant.value()};
    const AccountId system = system_account.value();
    const CaptureSplit split = split_capture(amount);

    const auto prepared_already = [&](std::size_t index) {
        return std::find(prepared.begin(), prepared.end(), index) != prepared.end();
    };

    const std::size_t merchant_index = partitioner.partition_of(merchant_account);
    const auto merchant_vote = partitions[merchant_index]->prepare_credit(
        txn, merchant_account, Bucket::Payable, split.payable);
    if (!merchant_vote.has_value()) {
        rollback_all();
        return Result<void, LedgerError>::err(merchant_vote.error());
    }
    if (!prepared_already(merchant_index)) {
        prepared.push_back(merchant_index);
    }

    const std::size_t system_index = partitioner.partition_of(system);
    const auto system_vote = partitions[system_index]->prepare_credit(
        txn, system, Bucket::Interchange, split.interchange);
    if (!system_vote.has_value()) {
        rollback_all();
        return Result<void, LedgerError>::err(system_vote.error());
    }
    if (!prepared_already(system_index)) {
        prepared.push_back(system_index);
    }

    // One record, written before anything is committed, so a crash either replays the whole capture
    // or none of it. A capture that was refused never reaches this point.
    wal.with([&](Wal& log) { log.append_capture(hold_id, amount); });

    for (const std::size_t index : prepared) {
        partitions[index]->commit(txn);
    }
    return Result<void, LedgerError>::ok();
}

} // namespace aegis::ledger
