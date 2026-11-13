#include <gtest/gtest.h>

#include <aegis/engine/engine.hpp>
#include <aegis/ids.hpp>
#include <aegis/iso8583/codec.hpp>
#include <aegis/iso8583/fields.hpp>
#include <aegis/iso8583/message.hpp>
#include <aegis/ledger/genesis.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/ledger/partition.hpp>
#include <aegis/ledger/wal.hpp>
#include <aegis/ledger/wallet.hpp>
#include <aegis/money.hpp>
#include <aegis/net/framing.hpp>
#include <aegis/net/socket.hpp>
#include <shadow_model.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <latch>
#include <optional>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace aegis::ledger {
namespace {

static_assert(!std::is_convertible_v<std::uint64_t, CaptureTxnId>);
static_assert(!std::is_convertible_v<HoldId, CaptureTxnId>);
static_assert(!std::is_convertible_v<CaptureTxnId, HoldId>);
static_assert(!std::is_constructible_v<CaptureTxnId, HoldId>);
static_assert(!std::is_constructible_v<HoldId, CaptureTxnId>);

AccountId account(std::string id) {
    return AccountId{std::move(id)};
}

TEST(Stage3Test, PartitionMappingIsStableAndInRange) {
    const WalletPartitioner partitioner{8};

    EXPECT_EQ(partitioner.size(), 8U);

    const AccountId a = account("4242424200000000");
    const AccountId b = account("m-1");
    const AccountId c = account("system");

    EXPECT_EQ(partitioner.partition_of(a), partitioner.partition_of(a));
    for (const AccountId& id : {a, b, c}) {
        EXPECT_LT(partitioner.partition_of(id), partitioner.size());
    }

    // The same accounts, listed in a different order, hash to the same partitions.
    std::vector<std::size_t> first{partitioner.partition_of(a), partitioner.partition_of(b),
                                   partitioner.partition_of(c)};
    std::vector<std::size_t> second{partitioner.partition_of(c), partitioner.partition_of(a),
                                    partitioner.partition_of(b)};
    std::sort(first.begin(), first.end());
    std::sort(second.begin(), second.end());
    EXPECT_EQ(first, second);
}

TEST(Stage3Test, HoldIdEncodesOwningPartition) {
    std::set<std::uint64_t> seen;
    for (std::size_t partition = 0; partition < 8; ++partition) {
        for (std::uint64_t local = 1; local <= 3; ++local) {
            const HoldId id = make_hold_id(partition, local);
            EXPECT_EQ(hold_partition(id), partition);
            EXPECT_EQ(hold_local(id), local);
            EXPECT_TRUE(seen.insert(id.value()).second) << "hold ids must be unique";
        }
    }

    // Two partitions minting their first hold must not collide.
    EXPECT_NE(make_hold_id(0, 1), make_hold_id(1, 1));
}

TEST(Stage3Test, PartitionWritersAreIndependentAndFifo) {
    WalSink wal;
    Partition first{0, {}, wal};
    Partition second{1, {}, wal};

    std::atomic<bool> first_started{false};
    auto first_done = first.submit([&first_started] {
        first_started.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds{1500});
        return 1;
    });

    while (!first_started.load()) {
        std::this_thread::yield();
    }

    // A task queued behind the sleeper on the same writer must wait for it.
    auto queued_behind = first.submit([] { return 2; });

    // A task on the other partition must not wait for the sleeper at all.
    const auto started_other = std::chrono::steady_clock::now();
    EXPECT_EQ(second.submit([] { return 7; }).get(), 7);
    const auto other_elapsed = std::chrono::steady_clock::now() - started_other;
    EXPECT_LT(other_elapsed, std::chrono::milliseconds{1000});

    EXPECT_EQ(first_done.get(), 1);
    EXPECT_EQ(queued_behind.get(), 2);
    const auto stalled_elapsed = std::chrono::steady_clock::now() - started_other;
    EXPECT_GE(stalled_elapsed, std::chrono::milliseconds{1000});
}

TEST(Stage3Test, PartitionReserveAndBalanceMoveFunds) {
    const AccountId cardholder = account("4242424200000000");
    Wallet wallet{cardholder, WalletKind::Cardholder, Currency::Usd};
    ASSERT_TRUE(wallet.adjust(Bucket::Available, Money{Currency::Usd, 10000}).has_value());

    std::unordered_map<AccountId, Wallet> wallets;
    wallets.emplace(cardholder, std::move(wallet));

    WalSink wal;
    Partition partition{3, std::move(wallets), wal};

    const Money amount{Currency::Usd, 5000};
    const auto reserved =
        partition.reserve(cardholder, amount, MerchantId{"m-1"}, std::chrono::system_clock::now());
    ASSERT_TRUE(reserved.has_value());
    EXPECT_EQ(hold_partition(reserved.value().id), 3U);
    EXPECT_EQ(hold_local(reserved.value().id), 1U);
    EXPECT_EQ(reserved.value().cardholder, cardholder);
    EXPECT_EQ(reserved.value().amount, amount);

    const auto available = partition.balance(cardholder, Bucket::Available);
    const auto holds = partition.balance(cardholder, Bucket::Holds);
    ASSERT_TRUE(available.has_value());
    ASSERT_TRUE(holds.has_value());
    EXPECT_EQ(available.value(), (Money{Currency::Usd, 5000}));
    EXPECT_EQ(holds.value(), amount);
    EXPECT_EQ(partition.live_hold_count(), 1U);

    // The checks that decline before any posting.
    EXPECT_EQ(partition
                  .reserve(account("missing"), amount, MerchantId{"m-1"},
                           std::chrono::system_clock::now())
                  .error(),
              LedgerError::UnknownWallet);
    EXPECT_EQ(partition
                  .reserve(cardholder, Money{Currency::Eur, 1}, MerchantId{"m-1"},
                           std::chrono::system_clock::now())
                  .error(),
              LedgerError::CurrencyMismatch);
    EXPECT_EQ(partition
                  .reserve(cardholder, Money{Currency::Usd, 0}, MerchantId{"m-1"},
                           std::chrono::system_clock::now())
                  .error(),
              LedgerError::InvalidAmount);
    EXPECT_EQ(partition
                  .reserve(cardholder, Money{Currency::Usd, 6000}, MerchantId{"m-1"},
                           std::chrono::system_clock::now())
                  .error(),
              LedgerError::InsufficientFunds);

    // A declined reserve posts nothing and mints no hold.
    EXPECT_EQ(partition.balance(cardholder, Bucket::Available).value(),
              (Money{Currency::Usd, 5000}));
    EXPECT_EQ(partition.live_hold_count(), 1U);
    EXPECT_EQ(partition.balance(account("missing"), Bucket::Available).error(),
              LedgerError::UnknownWallet);
}

void expect_matches_shadow(const Ledger& ledger, const ShadowLedger& shadow) {
    const Bucket buckets[] = {Bucket::Available, Bucket::Holds, Bucket::Payable, Bucket::Interchange};
    std::vector<AccountId> accounts;
    for (std::size_t i = 0; i < 8; ++i) {
        char pan[17] = {};
        std::snprintf(pan, sizeof(pan), "42424242%08zu", i);
        accounts.push_back(account(pan));
    }
    accounts.push_back(account("m-1"));
    accounts.push_back(account("system"));

    for (const AccountId& id : accounts) {
        for (const Bucket bucket : buckets) {
            const auto actual = ledger.balance(id, bucket);
            const auto expected = shadow.balance(id, bucket);
            ASSERT_TRUE(actual.has_value());
            ASSERT_TRUE(expected.has_value());
            EXPECT_EQ(actual.value(), expected.value()) << id.value();
        }
    }
    EXPECT_EQ(ledger.live_hold_count(), shadow.live_hold_count());
}

std::vector<GenesisRecord> stage3_records() {
    std::vector<GenesisRecord> records;
    for (std::size_t i = 0; i < 8; ++i) {
        char pan[17] = {};
        std::snprintf(pan, sizeof(pan), "42424242%08zu", i);
        records.push_back(
            GenesisRecord{WalletKind::Cardholder, AccountId{pan}, Currency::Usd, 1'000'000});
    }
    records.push_back(GenesisRecord{WalletKind::Merchant, AccountId{"m-1"}, Currency::Usd, 0});
    records.push_back(GenesisRecord{WalletKind::System, AccountId{"system"}, Currency::Usd, 0});
    return records;
}

TEST(Stage3Test, PartitionedLedgerRoutesReserveAndBalance) {
    const std::vector<GenesisRecord> records = stage3_records();
    auto ledger_wallets = load_genesis(records);
    auto shadow_wallets = load_genesis(records);
    ASSERT_TRUE(ledger_wallets.has_value());
    ASSERT_TRUE(shadow_wallets.has_value());

    constexpr std::size_t kPartitions = 8;
    Ledger ledger{std::move(ledger_wallets).take(), LedgerMode::Partitioned, kPartitions};
    ShadowLedger shadow{std::move(shadow_wallets).take()};

    EXPECT_EQ(ledger.partition_count(), kPartitions);

    const AccountId cardholder = account("4242424200000000");
    const AccountId other = account("4242424200000001");
    EXPECT_LT(ledger.partition_of(cardholder), kPartitions);
    EXPECT_EQ(ledger.partition_of(cardholder), ledger.partition_of(cardholder));
    // With eight partitions these two pans land on different writers, which is what makes the
    // routing observable: each reserve is posted by the writer that owns that pan.
    EXPECT_NE(ledger.partition_of(cardholder), ledger.partition_of(other));

    const Money amount{Currency::Usd, 1000};
    const MerchantId merchant{"m-1"};
    const auto hold = ledger.reserve(cardholder, amount, merchant);
    const auto replayed = shadow.reserve(cardholder, amount, merchant);
    ASSERT_TRUE(hold.has_value());
    ASSERT_TRUE(replayed.has_value());
    EXPECT_EQ(hold_partition(hold.value().id), ledger.partition_of(cardholder));

    for (const AccountId& id : {cardholder, other, account("m-1"), account("system")}) {
        for (const Bucket bucket :
             {Bucket::Available, Bucket::Holds, Bucket::Payable, Bucket::Interchange}) {
            const auto actual = ledger.balance(id, bucket);
            const auto expected = shadow.balance(id, bucket);
            ASSERT_TRUE(actual.has_value());
            ASSERT_TRUE(expected.has_value());
            EXPECT_EQ(actual.value(), expected.value());
        }
    }

    EXPECT_EQ(ledger.reserve(account("missing"), amount, merchant).error(),
              LedgerError::UnknownWallet);
}

TEST(Stage3Test, PartitionedLiveHoldCountAndTtl) {
    const std::vector<GenesisRecord> records = stage3_records();
    auto ledger_wallets = load_genesis(records);
    auto shadow_wallets = load_genesis(records);
    ASSERT_TRUE(ledger_wallets.has_value());
    ASSERT_TRUE(shadow_wallets.has_value());

    Ledger ledger{std::move(ledger_wallets).take(), LedgerMode::Partitioned, 8};
    ShadowLedger shadow{std::move(shadow_wallets).take()};

    // Zero TTL makes a hold due the instant it exists, so a sweep right after reserving releases it.
    ledger.set_hold_ttl(std::chrono::seconds{0});
    shadow.set_hold_ttl(std::chrono::seconds{0});

    const Money amount{Currency::Usd, 1000};
    const MerchantId merchant{"m-1"};
    for (std::size_t i = 0; i < 8; ++i) {
        char pan[17] = {};
        std::snprintf(pan, sizeof(pan), "42424242%08zu", i);
        const AccountId cardholder = account(pan);
        ASSERT_TRUE(ledger.reserve(cardholder, amount, merchant).has_value());
        ASSERT_TRUE(shadow.reserve(cardholder, amount, merchant).has_value());
    }

    // The eight pans span all eight partitions, so the count only adds up if every shard reports.
    EXPECT_EQ(ledger.live_hold_count(), shadow.live_hold_count());
    EXPECT_EQ(ledger.live_hold_count(), 8U);

    const auto swept = ledger.expire_due(std::chrono::system_clock::now());
    EXPECT_EQ(swept, shadow.expire_due(std::chrono::system_clock::now()));
    EXPECT_EQ(ledger.live_hold_count(), 0U);

    // An expired hold gives the money back to Available, on every partition.
    for (std::size_t i = 0; i < 8; ++i) {
        char pan[17] = {};
        std::snprintf(pan, sizeof(pan), "42424242%08zu", i);
        const auto available = ledger.balance(account(pan), Bucket::Available);
        ASSERT_TRUE(available.has_value());
        EXPECT_EQ(available.value(), (Money{Currency::Usd, 1'000'000}));
    }
}

TEST(Stage3Test, PartitionedCapturePaysSplitAndConsumesHold) {
    const std::vector<GenesisRecord> records = stage3_records();
    auto wallets = load_genesis(records);
    ASSERT_TRUE(wallets.has_value());

    // m-1 and system hash apart from each other and from this pan, so the capture crosses three
    // partitions and only pays out if all three commit.
    Ledger ledger{std::move(wallets).take(), LedgerMode::Partitioned, 8};
    const AccountId cardholder = account("4242424200000000");
    ASSERT_NE(ledger.partition_of(cardholder), ledger.partition_of(account("m-1")));
    ASSERT_NE(ledger.partition_of(account("m-1")), ledger.partition_of(account("system")));

    const Money amount{Currency::Usd, 5000};
    const auto hold = ledger.reserve(cardholder, amount, MerchantId{"m-1"});
    ASSERT_TRUE(hold.has_value());

    ASSERT_TRUE(ledger.capture(hold.value().id, amount).has_value());

    const auto holds = ledger.balance(cardholder, Bucket::Holds);
    const auto payable = ledger.balance(account("m-1"), Bucket::Payable);
    const auto interchange = ledger.balance(account("system"), Bucket::Interchange);
    ASSERT_TRUE(holds.has_value());
    ASSERT_TRUE(payable.has_value());
    ASSERT_TRUE(interchange.has_value());
    EXPECT_EQ(holds.value(), (Money{Currency::Usd, 0}));
    EXPECT_EQ(payable.value(), (Money{Currency::Usd, 4855}));
    EXPECT_EQ(interchange.value(), (Money{Currency::Usd, 145}));
    EXPECT_EQ(ledger.live_hold_count(), 0U);

    // The hold is gone, so neither a second capture nor a reverse can move the money again.
    EXPECT_EQ(ledger.capture(hold.value().id, amount).error(), LedgerError::UnknownHold);
    EXPECT_EQ(ledger.reverse(hold.value().id).error(), LedgerError::UnknownHold);
    EXPECT_EQ(ledger.balance(account("m-1"), Bucket::Payable).value(),
              (Money{Currency::Usd, 4855}));
}

TEST(Stage3Test, PartitionedCaptureRollsBackOnParticipantFailure) {
    const std::array<GenesisRecord, 3> records{{
        GenesisRecord{WalletKind::Cardholder, AccountId{"4242424242424242"}, Currency::Usd, 10000},
        GenesisRecord{WalletKind::Merchant, AccountId{"m-eur"}, Currency::Eur, 0},
        GenesisRecord{WalletKind::System, AccountId{"system"}, Currency::Usd, 0},
    }};
    auto ledger_wallets = load_genesis(records);
    auto shadow_wallets = load_genesis(records);
    ASSERT_TRUE(ledger_wallets.has_value());
    ASSERT_TRUE(shadow_wallets.has_value());

    Ledger ledger{std::move(ledger_wallets).take(), LedgerMode::Partitioned, 4};
    ShadowLedger shadow{std::move(shadow_wallets).take()};

    const AccountId cardholder = account("4242424242424242");
    const Money amount{Currency::Usd, 5000};
    const auto hold = ledger.reserve(cardholder, amount, MerchantId{"m-eur"});
    const auto replayed = shadow.reserve(cardholder, amount, MerchantId{"m-eur"});
    ASSERT_TRUE(hold.has_value());
    ASSERT_TRUE(replayed.has_value());

    // The merchant takes a different currency than the hold, so its partition refuses. Every balance
    // must still match the shadow, which never applied the capture.
    EXPECT_EQ(ledger.capture(hold.value().id, amount).error(), LedgerError::CurrencyMismatch);
    EXPECT_EQ(shadow.capture(replayed.value().id, amount).error(), LedgerError::CurrencyMismatch);

    for (const AccountId& id : {cardholder, account("m-eur"), account("system")}) {
        for (const Bucket bucket :
             {Bucket::Available, Bucket::Holds, Bucket::Payable, Bucket::Interchange}) {
            const auto actual = ledger.balance(id, bucket);
            const auto expected = shadow.balance(id, bucket);
            ASSERT_TRUE(actual.has_value());
            ASSERT_TRUE(expected.has_value());
            EXPECT_EQ(actual.value(), expected.value()) << id.value();
        }
    }
    EXPECT_EQ(ledger.live_hold_count(), 1U);

    // The hold was given back, so a reversal still releases it.
    ASSERT_TRUE(ledger.reverse(hold.value().id).has_value());
    ASSERT_TRUE(shadow.reverse(replayed.value().id).has_value());
    EXPECT_EQ(ledger.balance(cardholder, Bucket::Available).value(),
              shadow.balance(cardholder, Bucket::Available).value());
    EXPECT_EQ(ledger.live_hold_count(), 0U);

    // The failures that happen before any partition prepares leave the hold untouched as well.
    const auto second = ledger.reserve(cardholder, amount, MerchantId{"m-eur"});
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(ledger.capture(second.value().id, Money{Currency::Usd, 1}).error(),
              LedgerError::AmountMismatch);
    EXPECT_EQ(ledger.capture(HoldId{99}, amount).error(), LedgerError::UnknownHold);
    EXPECT_EQ(ledger.live_hold_count(), 1U);
    EXPECT_EQ(ledger.balance(cardholder, Bucket::Holds).value(), amount);
}

TEST(Stage3Test, PartitionedReverseAndExpiryMatchShadow) {
    const std::vector<GenesisRecord> records = stage3_records();
    auto ledger_wallets = load_genesis(records);
    auto shadow_wallets = load_genesis(records);
    ASSERT_TRUE(ledger_wallets.has_value());
    ASSERT_TRUE(shadow_wallets.has_value());

    Ledger ledger{std::move(ledger_wallets).take(), LedgerMode::Partitioned, 8};
    ShadowLedger shadow{std::move(shadow_wallets).take()};

    const Money amount{Currency::Usd, 1000};
    const MerchantId merchant{"m-1"};

    // Due immediately, so the sweep owns them.
    ledger.set_hold_ttl(std::chrono::seconds{0});
    shadow.set_hold_ttl(std::chrono::seconds{0});
    for (std::size_t i = 0; i < 8; ++i) {
        char pan[17] = {};
        std::snprintf(pan, sizeof(pan), "42424242%08zu", i);
        ASSERT_TRUE(ledger.reserve(account(pan), amount, merchant).has_value());
        ASSERT_TRUE(shadow.reserve(account(pan), amount, merchant).has_value());
    }

    // A long TTL keeps these alive so they can be reversed instead of swept. The two ledgers mint
    // different ids, so each reversal uses the id its own ledger returned.
    ledger.set_hold_ttl(std::chrono::hours{24});
    shadow.set_hold_ttl(std::chrono::hours{24});
    std::vector<HoldId> live;
    std::vector<HoldId> shadow_live;
    for (std::size_t n = 0; n < 5; ++n) {
        for (std::size_t i = 0; i < 8; ++i) {
            char pan[17] = {};
            std::snprintf(pan, sizeof(pan), "42424242%08zu", i);
            const auto hold = ledger.reserve(account(pan), amount, merchant);
            const auto replayed = shadow.reserve(account(pan), amount, merchant);
            ASSERT_TRUE(hold.has_value());
            ASSERT_TRUE(replayed.has_value());
            live.push_back(hold.value().id);
            shadow_live.push_back(replayed.value().id);
        }
    }

    for (const HoldId id : live) {
        ASSERT_TRUE(ledger.reverse(id).has_value());
    }
    for (const HoldId id : shadow_live) {
        ASSERT_TRUE(shadow.reverse(id).has_value());
    }

    const auto now = std::chrono::system_clock::now();
    EXPECT_EQ(ledger.expire_due(now), 8U);
    EXPECT_EQ(shadow.expire_due(now), 8U);

    expect_matches_shadow(ledger, shadow);
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

std::size_t count_records(const std::filesystem::path& path, char kind) {
    std::ifstream in{path};
    std::size_t count = 0;
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() >= 2 && line[0] == kind && line[1] == '|') {
            ++count;
        }
    }
    return count;
}

TEST(Stage3Test, PartitionedWalWritesOneCaptureRecordAndSkipsFailedCapture) {
    const std::vector<GenesisRecord> records = stage3_records();
    auto wallets = load_genesis(records);
    ASSERT_TRUE(wallets.has_value());
    const auto genesis = wallets.value();

    const auto wal_path =
        std::filesystem::temp_directory_path() / "aegis_stage3_capture.wal";
    std::filesystem::remove(wal_path);

    const AccountId cardholder = account("4242424200000000");
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant{"m-1"};

    {
        Ledger ledger{genesis, LedgerMode::Partitioned, 8};
        Wal wal{wal_path};
        ledger.set_wal(&wal);

        const auto captured = ledger.reserve(cardholder, amount, merchant);
        ASSERT_TRUE(captured.has_value());
        ASSERT_TRUE(ledger.capture(captured.value().id, amount).has_value());

        // A capture that names an unknown hold must not leave a record behind.
        EXPECT_EQ(ledger.capture(HoldId{99}, amount).error(), LedgerError::UnknownHold);

        const auto reversed = ledger.reserve(cardholder, amount, merchant);
        ASSERT_TRUE(reversed.has_value());
        ASSERT_TRUE(ledger.reverse(reversed.value().id).has_value());

        ASSERT_TRUE(wal.flush().has_value());
    }

    EXPECT_EQ(count_records(wal_path, 'R'), 2U);
    EXPECT_EQ(count_records(wal_path, 'C'), 1U);
    EXPECT_EQ(count_records(wal_path, 'V'), 1U);

    const auto replayed = Wal::replay(wal_path, genesis);
    ASSERT_TRUE(replayed.has_value());
    EXPECT_EQ(replayed.value().balance(cardholder, Bucket::Holds).value(),
              (Money{Currency::Usd, 0}));
    EXPECT_EQ(replayed.value().balance(account("m-1"), Bucket::Payable).value(),
              (Money{Currency::Usd, 4855}));
    EXPECT_EQ(replayed.value().balance(account("system"), Bucket::Interchange).value(),
              (Money{Currency::Usd, 145}));
    EXPECT_EQ(replayed.value().live_hold_count(), 0U);

    std::filesystem::remove(wal_path);
}

TEST(Stage3Test, PartitionedMixedLoadMatchesShadow) {
    const std::vector<GenesisRecord> records = stage3_records();
    auto ledger_wallets = load_genesis(records);
    auto shadow_wallets = load_genesis(records);
    ASSERT_TRUE(ledger_wallets.has_value());
    ASSERT_TRUE(shadow_wallets.has_value());

    Ledger ledger{std::move(ledger_wallets).take(), LedgerMode::Partitioned, 8};
    ShadowLedger shadow{std::move(shadow_wallets).take()};

    const Money amount{Currency::Usd, 1000};
    const MerchantId merchant{"m-1"};

    // Seeded up front, single threaded, so each side has the ids it will use. The concurrent phase
    // only touches the ledger; the shadow replays the same operations afterwards.
    constexpr std::size_t kSeeded = 20;
    std::vector<HoldId> to_capture;
    std::vector<HoldId> shadow_capture;
    std::vector<HoldId> to_reverse;
    std::vector<HoldId> shadow_reverse;
    for (std::size_t n = 0; n < kSeeded; ++n) {
        for (std::size_t i = 0; i < 8; ++i) {
            char pan[17] = {};
            std::snprintf(pan, sizeof(pan), "42424242%08zu", i);
            const AccountId cardholder = account(pan);
            const auto hold = ledger.reserve(cardholder, amount, merchant);
            const auto replayed = shadow.reserve(cardholder, amount, merchant);
            ASSERT_TRUE(hold.has_value());
            ASSERT_TRUE(replayed.has_value());
            if (n < kSeeded / 2) {
                to_capture.push_back(hold.value().id);
                shadow_capture.push_back(replayed.value().id);
            } else {
                to_reverse.push_back(hold.value().id);
                shadow_reverse.push_back(replayed.value().id);
            }
        }
    }

    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kFreshReserves = 10;
    std::latch start{kThreads};
    std::atomic<std::size_t> failures{0};
    std::vector<std::jthread> threads;
    for (std::size_t t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            char pan[17] = {};
            std::snprintf(pan, sizeof(pan), "42424242%08zu", t);
            const AccountId cardholder = account(pan);
            for (std::size_t index = t; index < to_capture.size(); index += kThreads) {
                if (!ledger.capture(to_capture[index], amount).has_value()) {
                    failures.fetch_add(1);
                }
            }
            for (std::size_t index = t; index < to_reverse.size(); index += kThreads) {
                if (!ledger.reverse(to_reverse[index]).has_value()) {
                    failures.fetch_add(1);
                }
            }
            for (std::size_t n = 0; n < kFreshReserves; ++n) {
                if (!ledger.reserve(cardholder, amount, merchant).has_value()) {
                    failures.fetch_add(1);
                }
            }
        });
    }
    threads.clear();
    EXPECT_EQ(failures.load(), 0U);

    for (const HoldId id : shadow_capture) {
        ASSERT_TRUE(shadow.capture(id, amount).has_value());
    }
    for (const HoldId id : shadow_reverse) {
        ASSERT_TRUE(shadow.reverse(id).has_value());
    }
    for (std::size_t t = 0; t < kThreads; ++t) {
        char pan[17] = {};
        std::snprintf(pan, sizeof(pan), "42424242%08zu", t);
        for (std::size_t n = 0; n < kFreshReserves; ++n) {
            ASSERT_TRUE(shadow.reserve(account(pan), amount, merchant).has_value());
        }
    }

    expect_matches_shadow(ledger, shadow);
}

TEST(Stage3Test, ConcurrentDoubleCaptureSucceedsOnce) {
    const std::vector<GenesisRecord> records = stage3_records();
    auto wallets = load_genesis(records);
    ASSERT_TRUE(wallets.has_value());

    Ledger ledger{std::move(wallets).take(), LedgerMode::Partitioned, 8};
    const AccountId cardholder = account("4242424200000000");
    const Money amount{Currency::Usd, 5000};
    const auto hold = ledger.reserve(cardholder, amount, MerchantId{"m-1"});
    ASSERT_TRUE(hold.has_value());

    std::latch start{2};
    std::atomic<int> won{0};
    std::atomic<int> lost{0};
    std::jthread first{[&] {
        start.arrive_and_wait();
        if (ledger.capture(hold.value().id, amount).has_value()) {
            won.fetch_add(1);
        } else {
            lost.fetch_add(1);
        }
    }};
    std::jthread second{[&] {
        start.arrive_and_wait();
        if (ledger.capture(hold.value().id, amount).has_value()) {
            won.fetch_add(1);
        } else {
            lost.fetch_add(1);
        }
    }};
    first.join();
    second.join();

    EXPECT_EQ(won.load(), 1);
    EXPECT_EQ(lost.load(), 1);
    EXPECT_EQ(ledger.balance(account("m-1"), Bucket::Payable).value(),
              (Money{Currency::Usd, 4855}));
    EXPECT_EQ(ledger.balance(account("system"), Bucket::Interchange).value(),
              (Money{Currency::Usd, 145}));
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

TEST(Stage3Test, PartitionCountEdgeCases) {
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant{"m-1"};

    // One partition puts the cardholder, the merchant, and the system on the same writer. The capture
    // still pays exactly once.
    {
        auto wallets = load_genesis(stage3_records());
        ASSERT_TRUE(wallets.has_value());
        Ledger ledger{std::move(wallets).take(), LedgerMode::Partitioned, 1};
        EXPECT_EQ(ledger.partition_count(), 1U);
        EXPECT_EQ(ledger.partition_of(account("4242424200000000")),
                  ledger.partition_of(account("m-1")));

        const auto hold = ledger.reserve(account("4242424200000000"), amount, merchant);
        ASSERT_TRUE(hold.has_value());
        ASSERT_TRUE(ledger.capture(hold.value().id, amount).has_value());
        EXPECT_EQ(ledger.balance(account("m-1"), Bucket::Payable).value(),
                  (Money{Currency::Usd, 4855}));
        EXPECT_EQ(ledger.balance(account("system"), Bucket::Interchange).value(),
                  (Money{Currency::Usd, 145}));
    }

    // Far more partitions than wallets leaves most writers idle, which changes nothing observable.
    {
        auto wallets = load_genesis(stage3_records());
        ASSERT_TRUE(wallets.has_value());
        Ledger ledger{std::move(wallets).take(), LedgerMode::Partitioned, 64};
        const auto hold = ledger.reserve(account("4242424200000007"), amount, merchant);
        ASSERT_TRUE(hold.has_value());
        ASSERT_TRUE(ledger.capture(hold.value().id, amount).has_value());
        EXPECT_EQ(ledger.live_hold_count(), 0U);
        EXPECT_EQ(ledger.balance(account("4242424200000007"), Bucket::Available).value(),
                  (Money{Currency::Usd, 995'000}));
    }

    // A merchant and a system account that hash to the same partition merge onto one writer, so the
    // commit has to post both credits there and post them once.
    {
        WalletPartitioner partitioner{8};
        std::string merchant_id;
        std::string system_id;
        bool found = false;
        for (int i = 0; i < 64 && !found; ++i) {
            for (int j = i + 1; j < 64 && !found; ++j) {
                const std::string left = "w-" + std::to_string(i);
                const std::string right = "w-" + std::to_string(j);
                if (partitioner.partition_of(account(left)) ==
                    partitioner.partition_of(account(right))) {
                    merchant_id = left;
                    system_id = right;
                    found = true;
                }
            }
        }
        ASSERT_TRUE(found);

        const std::array<GenesisRecord, 3> records{{
            GenesisRecord{WalletKind::Cardholder, AccountId{"4242424242424242"}, Currency::Usd,
                          10000},
            GenesisRecord{WalletKind::Merchant, AccountId{merchant_id}, Currency::Usd, 0},
            GenesisRecord{WalletKind::System, AccountId{system_id}, Currency::Usd, 0},
        }};
        auto wallets = load_genesis(records);
        ASSERT_TRUE(wallets.has_value());
        Ledger ledger{std::move(wallets).take(), LedgerMode::Partitioned, 8};
        ASSERT_EQ(ledger.partition_of(account(merchant_id)), ledger.partition_of(account(system_id)));

        const auto hold =
            ledger.reserve(account("4242424242424242"), amount, MerchantId{merchant_id});
        ASSERT_TRUE(hold.has_value());
        ASSERT_TRUE(ledger.capture(hold.value().id, amount).has_value());
        EXPECT_EQ(ledger.balance(account(merchant_id), Bucket::Payable).value(),
                  (Money{Currency::Usd, 4855}));
        EXPECT_EQ(ledger.balance(account(system_id), Bucket::Interchange).value(),
                  (Money{Currency::Usd, 145}));
    }
}

iso8583::Message make_auth_request(std::string_view pan, std::size_t stan) {
    using iso8583::FieldId;
    iso8583::Message request{iso8583::Mti::AuthorizationRequest};
    (void)request.set(FieldId::Pan, std::string{pan});
    (void)request.set(FieldId::Amount, "000000001000");
    (void)request.set(FieldId::TransmissionDateTime, "1002153045");

    char stan_text[7] = {};
    std::snprintf(stan_text, sizeof(stan_text), "%06zu", stan % 1000000U);
    (void)request.set(FieldId::Stan, std::string{stan_text});
    (void)request.set(FieldId::TerminalId, "TERM0001");
    (void)request.set(FieldId::MerchantId, std::string{"m-1"} + std::string(12, ' '));
    (void)request.set(FieldId::Currency, "840");
    return request;
}

TEST(Stage3Test, EngineStageThreeHandlesConcurrentAuth) {
    engine::EngineConfig config{};
    config.genesis_path = std::filesystem::path{FIXTURES_DIR} / "genesis_stage2.csv";
    config.io_threads = 2;
    config.worker_count = 4;
    config.inbound_capacity = 64;
    config.ledger_stage = engine::LedgerStage::Partitioned;
    config.ledger_partitions = 8;

    engine::Engine server;
    ASSERT_TRUE(server.start(config).has_value());

    constexpr std::size_t kClients = 4;
    constexpr std::size_t kRequestsPerClient = 25;
    std::latch start{kClients};
    std::atomic<std::size_t> failures{0};
    std::vector<std::jthread> clients;

    for (std::size_t t = 0; t < kClients; ++t) {
        clients.emplace_back([&, t] {
            start.arrive_and_wait();
            char pan[17] = {};
            std::snprintf(pan, sizeof(pan), "42424242%08zu", t);
            for (std::size_t n = 0; n < kRequestsPerClient; ++n) {
                net::Connection client;
                if (!client.connect_loopback(server.port()).has_value()) {
                    failures.fetch_add(1);
                    continue;
                }
                const auto payload = iso8583::serialise(make_auth_request(pan, (t * 1000) + n));
                if (!payload.has_value() || !net::write_frame(client, payload.value()).has_value()) {
                    failures.fetch_add(1);
                    continue;
                }
                client.set_receive_timeout(std::chrono::seconds{5});
                const auto frame = net::read_frame(client);
                if (!frame.has_value()) {
                    failures.fetch_add(1);
                    continue;
                }
                const auto parsed = iso8583::parse(frame.value());
                if (!parsed.has_value()) {
                    failures.fetch_add(1);
                    continue;
                }
                const auto code = parsed.value().get(iso8583::FieldId::ResponseCode);
                if (!code.has_value() || code.value() != "00") {
                    failures.fetch_add(1);
                }
            }
        });
    }
    clients.clear();

    EXPECT_EQ(failures.load(), 0U);
    server.stop();
}

// Concurrent auth (reserve) mixed with capture, the race the TSan CI job exists to see. Captures
// are not on the ISO path, so this drives the partitioned ledger directly: each thread reserves on
// its own cardholder and captures a disjoint slice of the seeded holds, which cross partitions.
TEST(Stage3Test, ConcurrentAuthAndCaptureSmoke) {
    const std::vector<GenesisRecord> records = stage3_records();
    auto wallets = load_genesis(records);
    ASSERT_TRUE(wallets.has_value());

    Ledger ledger{std::move(wallets).take(), LedgerMode::Partitioned, 4};
    const Money amount{Currency::Usd, 1000};
    const MerchantId merchant{"m-1"};

    constexpr std::size_t kCardholders = 4;
    constexpr std::size_t kSeeded = 8;
    std::vector<HoldId> seeded;
    seeded.reserve(kCardholders * kSeeded);
    for (std::size_t n = 0; n < kSeeded; ++n) {
        for (std::size_t i = 0; i < kCardholders; ++i) {
            char pan[17] = {};
            std::snprintf(pan, sizeof(pan), "42424242%08zu", i);
            const auto hold = ledger.reserve(account(pan), amount, merchant);
            ASSERT_TRUE(hold.has_value());
            seeded.push_back(hold.value().id);
        }
    }

    constexpr std::size_t kFreshReserves = 8;
    std::latch start{kCardholders};
    std::atomic<std::size_t> failures{0};
    std::vector<std::jthread> threads;
    for (std::size_t t = 0; t < kCardholders; ++t) {
        threads.emplace_back([&, t] {
            start.arrive_and_wait();
            char pan[17] = {};
            std::snprintf(pan, sizeof(pan), "42424242%08zu", t);
            const AccountId cardholder = account(pan);
            std::size_t captured = 0;
            std::size_t reserved = 0;
            const std::size_t capture_count = (seeded.size() + kCardholders - 1 - t) / kCardholders;
            while (captured < capture_count || reserved < kFreshReserves) {
                if (captured < capture_count) {
                    const std::size_t index = t + (captured * kCardholders);
                    if (!ledger.capture(seeded[index], amount).has_value()) {
                        failures.fetch_add(1);
                    }
                    ++captured;
                }
                if (reserved < kFreshReserves) {
                    if (!ledger.reserve(cardholder, amount, merchant).has_value()) {
                        failures.fetch_add(1);
                    }
                    ++reserved;
                }
            }
        });
    }
    threads.clear();

    EXPECT_EQ(failures.load(), 0U);
    EXPECT_EQ(ledger.live_hold_count(), kCardholders * kFreshReserves);
    const auto merchant_payable = ledger.balance(account("m-1"), Bucket::Payable);
    ASSERT_TRUE(merchant_payable.has_value());
    EXPECT_EQ(merchant_payable.value().minor_units(),
              static_cast<std::int64_t>(seeded.size()) * 971);
}

Wallet funded_wallet(AccountId id, WalletKind kind, std::int64_t available) {
    Wallet wallet{id, kind, Currency::Usd};
    if (available != 0) {
        EXPECT_TRUE(wallet.adjust(Bucket::Available, Money{Currency::Usd, available}).has_value());
    }
    return wallet;
}

// A reader must not observe a capture that has committed on one participant and not the next. The
// holder commit is applied first, then both balances are read; the merchant read has to wait until
// its own commit, so the pair cannot come back half-applied.
TEST(Stage3Test, CaptureIsolationBlocksUncommittedParticipantRead) {
    const AccountId cardholder = account("4242424200000000");
    const AccountId merchant = account("m-1");
    const AccountId system = account("system");
    const Money amount{Currency::Usd, 5000};
    const CaptureSplit split = split_capture(amount);

    WalSink wal;
    std::unordered_map<AccountId, Wallet> holder_wallets;
    holder_wallets.emplace(cardholder, funded_wallet(cardholder, WalletKind::Cardholder, 10000));
    std::unordered_map<AccountId, Wallet> merchant_wallets;
    merchant_wallets.emplace(merchant, funded_wallet(merchant, WalletKind::Merchant, 0));
    std::unordered_map<AccountId, Wallet> system_wallets;
    system_wallets.emplace(system, funded_wallet(system, WalletKind::System, 0));

    Partition holder{0, std::move(holder_wallets), wal};
    Partition merchant_partition{1, std::move(merchant_wallets), wal};
    Partition system_partition{2, std::move(system_wallets), wal};

    const auto reserved =
        holder.reserve(cardholder, amount, MerchantId{"m-1"}, std::chrono::system_clock::now());
    ASSERT_TRUE(reserved.has_value());

    const CaptureTxnId txn{1};
    const auto claimed = holder.prepare_capture(txn, reserved.value().id, amount);
    ASSERT_TRUE(claimed.has_value());
    ASSERT_TRUE(
        merchant_partition.prepare_credit(txn, merchant, Bucket::Payable, split.payable).has_value());
    ASSERT_TRUE(system_partition
                    .prepare_credit(txn, system, Bucket::Interchange, split.interchange)
                    .has_value());

    holder.commit(txn);

    std::promise<void> reader_started;
    std::future<void> reader_started_ready = reader_started.get_future();
    std::atomic<bool> merchant_read_finished{false};
    std::jthread reader([&] {
        reader_started.set_value();
        const auto holds = holder.balance(cardholder, Bucket::Holds);
        const auto payable = merchant_partition.balance(merchant, Bucket::Payable);
        merchant_read_finished.store(true);
        ASSERT_TRUE(holds.has_value());
        ASSERT_TRUE(payable.has_value());
        EXPECT_EQ(holds.value(), (Money{Currency::Usd, 0}));
        EXPECT_EQ(payable.value(), split.payable);
    });

    ASSERT_EQ(reader_started_ready.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    // The holder has already committed. The merchant has not, so this read must still be waiting.
    EXPECT_FALSE(merchant_read_finished.load());
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    EXPECT_FALSE(merchant_read_finished.load());

    merchant_partition.commit(txn);
    system_partition.commit(txn);
    reader.join();
    EXPECT_TRUE(merchant_read_finished.load());
    EXPECT_EQ(system_partition.balance(system, Bucket::Interchange).value(), split.interchange);
    EXPECT_EQ(holder.live_hold_count(), 0U);
}

// A sweep that overlaps a capture which later fails must still expire the due hold. The merchant
// refuses after the hold is staged, rollback restores it, and the waiting sweep then releases it.
TEST(Stage3Test, CaptureIsolationRollbackPreservesDueExpiry) {
    WalletPartitioner partitioner{4};
    std::string cardholder_id;
    std::string merchant_id;
    bool split = false;
    for (int i = 0; i < 64 && !split; ++i) {
        for (int j = 0; j < 64 && !split; ++j) {
            const std::string left = "c-" + std::to_string(i);
            const std::string right = "m-" + std::to_string(j);
            if (partitioner.partition_of(account(left)) != partitioner.partition_of(account(right))) {
                cardholder_id = left;
                merchant_id = right;
                split = true;
            }
        }
    }
    ASSERT_TRUE(split);
    const AccountId cardholder = account(cardholder_id);
    const AccountId merchant = account(merchant_id);
    const AccountId system = account("system");
    const std::array<GenesisRecord, 3> records{{
        GenesisRecord{WalletKind::Cardholder, cardholder, Currency::Usd, 10000},
        GenesisRecord{WalletKind::Merchant, merchant, Currency::Eur, 0},
        GenesisRecord{WalletKind::System, system, Currency::Usd, 0},
    }};
    auto ledger_wallets = load_genesis(records);
    auto shadow_wallets = load_genesis(records);
    ASSERT_TRUE(ledger_wallets.has_value());
    ASSERT_TRUE(shadow_wallets.has_value());

    Ledger ledger{std::move(ledger_wallets).take(), LedgerMode::Partitioned, 4};
    ShadowLedger shadow{std::move(shadow_wallets).take()};
    ledger.set_hold_ttl(std::chrono::seconds{0});
    shadow.set_hold_ttl(std::chrono::seconds{0});

    const Money amount{Currency::Usd, 5000};
    const auto now = std::chrono::system_clock::now();
    const auto reserved = ledger.reserve(cardholder, amount, MerchantId{merchant_id}, now);
    const auto replayed = shadow.reserve(cardholder, amount, MerchantId{merchant_id}, now);
    ASSERT_TRUE(reserved.has_value());
    ASSERT_TRUE(replayed.has_value());

    std::atomic<bool> capture_started{false};
    std::future<Result<void, LedgerError>> captured = std::async(std::launch::async, [&] {
        capture_started.store(true);
        return ledger.capture(reserved.value().id, amount);
    });
    while (!capture_started.load()) {
        std::this_thread::yield();
    }

    std::future<std::size_t> swept = std::async(std::launch::async, [&] {
        return ledger.expire_due(now);
    });

    ASSERT_EQ(captured.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_EQ(captured.get().error(), LedgerError::CurrencyMismatch);
    EXPECT_EQ(shadow.capture(replayed.value().id, amount).error(), LedgerError::CurrencyMismatch);
    EXPECT_EQ(shadow.expire_due(now), 1U);

    ASSERT_EQ(swept.wait_for(std::chrono::seconds{2}), std::future_status::ready);
    EXPECT_EQ(swept.get(), 1U);
    EXPECT_EQ(ledger.live_hold_count(), shadow.live_hold_count());
    EXPECT_EQ(ledger.balance(cardholder, Bucket::Available).value(),
              shadow.balance(cardholder, Bucket::Available).value());
    EXPECT_EQ(ledger.balance(cardholder, Bucket::Holds).value(),
              shadow.balance(cardholder, Bucket::Holds).value());
    EXPECT_EQ(ledger.balance(merchant, Bucket::Payable).value(), (Money{Currency::Eur, 0}));
    EXPECT_EQ(ledger.expire_due(now), 0U);
}

// Two captures whose holder-first orders oppose each other must both finish, and a wallet on a
// partition neither capture touches must still be able to reserve while they run.
TEST(Stage3Test, CaptureIsolationSharedParticipantsMakeProgress) {
    constexpr std::size_t kPartitions = 8;
    const WalletPartitioner partitioner{kPartitions};
    const std::size_t system_part = partitioner.partition_of(account("system"));

    std::string left_card;
    std::string right_card;
    std::string left_merchant;
    std::string right_merchant;
    std::string idle_card;
    bool found = false;
    for (int i = 0; i < 80 && !found; ++i) {
        const std::string card = "c-" + std::to_string(i);
        const std::size_t card_part = partitioner.partition_of(account(card));
        if (card_part == system_part) {
            continue;
        }
        for (int j = 0; j < 80 && !found; ++j) {
            const std::string other = "d-" + std::to_string(j);
            const std::size_t other_part = partitioner.partition_of(account(other));
            if (other_part == card_part || other_part == system_part) {
                continue;
            }
            for (int k = 0; k < 80 && !found; ++k) {
                const std::string idle = "i-" + std::to_string(k);
                const std::size_t idle_part = partitioner.partition_of(account(idle));
                if (idle_part == card_part || idle_part == other_part || idle_part == system_part) {
                    continue;
                }
                left_card = card;
                right_card = other;
                // Each merchant sits on the other cardholder's partition, so holder-first order
                // for the two captures is the reverse of each other.
                left_merchant = "m-" + other;
                right_merchant = "m-" + card;
                // Merchants are different ids; place them by searching until the partitions match.
                found = false;
            }
        }
    }

    auto place_merchant = [&](std::size_t want, std::string& out) {
        for (int n = 0; n < 120; ++n) {
            const std::string id = "m-" + std::to_string(n);
            if (partitioner.partition_of(account(id)) == want) {
                out = id;
                return true;
            }
        }
        return false;
    };

    found = false;
    for (int i = 0; i < 80 && !found; ++i) {
        const std::string card = "c-" + std::to_string(i);
        const std::size_t card_part = partitioner.partition_of(account(card));
        if (card_part == system_part) {
            continue;
        }
        for (int j = 0; j < 80 && !found; ++j) {
            const std::string other = "d-" + std::to_string(j);
            const std::size_t other_part = partitioner.partition_of(account(other));
            if (other_part == card_part || other_part == system_part || other_part < card_part) {
                continue;
            }
            std::string merchant_on_other;
            std::string merchant_on_card;
            if (!place_merchant(other_part, merchant_on_other) ||
                !place_merchant(card_part, merchant_on_card)) {
                continue;
            }
            for (int k = 0; k < 80 && !found; ++k) {
                const std::string idle = "i-" + std::to_string(k);
                const std::size_t idle_part = partitioner.partition_of(account(idle));
                if (idle_part == card_part || idle_part == other_part || idle_part == system_part) {
                    continue;
                }
                left_card = card;
                right_card = other;
                left_merchant = merchant_on_other;
                right_merchant = merchant_on_card;
                idle_card = idle;
                found = true;
            }
        }
    }
    ASSERT_TRUE(found);

    const AccountId left = account(left_card);
    const AccountId right = account(right_card);
    const AccountId left_payee = account(left_merchant);
    const AccountId right_payee = account(right_merchant);
    const AccountId idle = account(idle_card);
    ASSERT_LT(partitioner.partition_of(left), partitioner.partition_of(left_payee));
    ASSERT_GT(partitioner.partition_of(right), partitioner.partition_of(right_payee));
    ASSERT_EQ(partitioner.partition_of(left), partitioner.partition_of(right_payee));
    ASSERT_EQ(partitioner.partition_of(right), partitioner.partition_of(left_payee));

    const std::array<GenesisRecord, 6> records{{
        GenesisRecord{WalletKind::Cardholder, left, Currency::Usd, 20000},
        GenesisRecord{WalletKind::Cardholder, right, Currency::Usd, 20000},
        GenesisRecord{WalletKind::Cardholder, idle, Currency::Usd, 20000},
        GenesisRecord{WalletKind::Merchant, left_payee, Currency::Usd, 0},
        GenesisRecord{WalletKind::Merchant, right_payee, Currency::Usd, 0},
        GenesisRecord{WalletKind::System, account("system"), Currency::Usd, 0},
    }};
    auto wallets = load_genesis(records);
    ASSERT_TRUE(wallets.has_value());
    Ledger ledger{std::move(wallets).take(), LedgerMode::Partitioned, kPartitions};

    const Money amount{Currency::Usd, 5000};
    const auto left_hold = ledger.reserve(left, amount, MerchantId{left_merchant});
    const auto right_hold = ledger.reserve(right, amount, MerchantId{right_merchant});
    ASSERT_TRUE(left_hold.has_value());
    ASSERT_TRUE(right_hold.has_value());

    std::latch start{3};
    std::atomic<int> failures{0};
    std::jthread first{[&] {
        start.arrive_and_wait();
        if (!ledger.capture(left_hold.value().id, amount).has_value()) {
            failures.fetch_add(1);
        }
    }};
    std::jthread second{[&] {
        start.arrive_and_wait();
        if (!ledger.capture(right_hold.value().id, amount).has_value()) {
            failures.fetch_add(1);
        }
    }};
    std::jthread bystander{[&] {
        start.arrive_and_wait();
        if (!ledger.reserve(idle, amount, MerchantId{left_merchant}).has_value()) {
            failures.fetch_add(1);
        }
    }};
    first.join();
    second.join();
    bystander.join();

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(ledger.balance(left_payee, Bucket::Payable).value(), (Money{Currency::Usd, 4855}));
    EXPECT_EQ(ledger.balance(right_payee, Bucket::Payable).value(), (Money{Currency::Usd, 4855}));
    EXPECT_EQ(ledger.balance(account("system"), Bucket::Interchange).value(),
              (Money{Currency::Usd, 290}));
    EXPECT_EQ(ledger.balance(idle, Bucket::Holds).value(), amount);

    // Both captures have released their fences, so a later reserve on a participant still posts.
    ASSERT_TRUE(ledger.reserve(left, amount, MerchantId{left_merchant}).has_value());
    EXPECT_EQ(ledger.live_hold_count(), 2U);
}

} // namespace
} // namespace aegis::ledger
