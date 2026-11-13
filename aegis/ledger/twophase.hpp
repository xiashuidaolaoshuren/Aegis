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
// live on their own writer threads. Participants are prepared in ascending partition order so two
// captures cannot deadlock, and each writer then hides its staged postings until commit or rollback.
[[nodiscard]] inline Result<void, LedgerError> run_capture_2pc(
    std::span<const std::unique_ptr<Partition>> partitions,
    const WalletPartitioner& partitioner,
    const std::optional<AccountId>& system_account,
    WalSink& wal,
    CaptureTxnId txn,
    HoldId hold_id,
    Money amount) {
    const std::size_t holder_index = hold_partition(hold_id);
    if (holder_index >= partitions.size()) {
        return Result<void, LedgerError>::err(LedgerError::UnknownHold);
    }

    // The hold names its merchant. Reading it claims nothing, so a refusal below fences nobody.
    const auto peeked = partitions[holder_index]->find_hold(hold_id);
    if (!peeked.has_value()) {
        return Result<void, LedgerError>::err(peeked.error());
    }
    if (peeked.value().amount != amount) {
        return Result<void, LedgerError>::err(LedgerError::AmountMismatch);
    }
    if (!system_account.has_value()) {
        return Result<void, LedgerError>::err(LedgerError::UnknownWallet);
    }

    const AccountId merchant_account{peeked.value().merchant.value()};
    const AccountId system = system_account.value();
    const CaptureSplit split = split_capture(amount);
    const std::size_t merchant_index = partitioner.partition_of(merchant_account);
    const std::size_t system_index = partitioner.partition_of(system);

    struct Participant {
        std::size_t index{};
        bool capture{false};
        bool payable{false};
        bool interchange{false};
    };
    std::vector<Participant> participants;
    const auto role = [&](std::size_t index) -> Participant& {
        for (Participant& participant : participants) {
            if (participant.index == index) {
                return participant;
            }
        }
        participants.push_back(Participant{index});
        return participants.back();
    };
    role(holder_index).capture = true;
    role(merchant_index).payable = true;
    role(system_index).interchange = true;
    std::sort(participants.begin(), participants.end(),
              [](const Participant& lhs, const Participant& rhs) {
                  return lhs.index < rhs.index;
              });

    std::vector<std::size_t> enlisted;
    const auto rollback_all = [&] {
        for (auto it = enlisted.rbegin(); it != enlisted.rend(); ++it) {
            partitions[*it]->rollback(txn);
        }
    };

    for (const Participant& participant : participants) {
        bool staged = false;
        if (participant.capture) {
            const auto claimed =
                partitions[participant.index]->prepare_capture(txn, hold_id, amount);
            if (!claimed.has_value()) {
                rollback_all();
                return Result<void, LedgerError>::err(claimed.error());
            }
            staged = true;
        }
        if (participant.payable) {
            const auto vote = partitions[participant.index]->prepare_credit(
                txn, merchant_account, Bucket::Payable, split.payable);
            if (!vote.has_value()) {
                if (staged) {
                    enlisted.push_back(participant.index);
                }
                rollback_all();
                return Result<void, LedgerError>::err(vote.error());
            }
            staged = true;
        }
        if (participant.interchange) {
            const auto vote = partitions[participant.index]->prepare_credit(
                txn, system, Bucket::Interchange, split.interchange);
            if (!vote.has_value()) {
                if (staged) {
                    enlisted.push_back(participant.index);
                }
                rollback_all();
                return Result<void, LedgerError>::err(vote.error());
            }
            staged = true;
        }
        if (staged) {
            enlisted.push_back(participant.index);
        }
    }

    // One record, written before anything is committed, so a crash either replays the whole capture
    // or none of it. A capture that was refused never reaches this point.
    wal.with([&](Wal& log) { log.append_capture(hold_id, amount); });

    for (const std::size_t index : enlisted) {
        partitions[index]->commit(txn);
    }
    return Result<void, LedgerError>::ok();
}

} // namespace aegis::ledger
