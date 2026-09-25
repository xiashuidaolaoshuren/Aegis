#include <gtest/gtest.h>

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/genesis.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/ledger/wal.hpp>
#include <aegis/money.hpp>
#include <shadow_model.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace aegis::ledger {
namespace {

std::array<GenesisRecord, 3> tiny_genesis_records() {
    return {
        GenesisRecord{
            WalletKind::Cardholder,
            AccountId{"4242424242424242"},
            Currency::Usd,
            10000,
        },
        GenesisRecord{
            WalletKind::Merchant,
            AccountId{"m-1"},
            Currency::Usd,
            0,
        },
        GenesisRecord{
            WalletKind::System,
            AccountId{"system"},
            Currency::Usd,
            0,
        },
    };
}

std::unordered_map<AccountId, Wallet> load_tiny_genesis_wallets() {
    const auto result = load_genesis(tiny_genesis_records());
    EXPECT_TRUE(result.has_value());
    return result.value();
}

void expect_bucket(
    const Ledger& ledger,
    const ShadowLedger& shadow,
    AccountId id,
    Bucket bucket,
    Money expected) {
    const auto ledger_balance = ledger.balance(id, bucket);
    const auto shadow_balance = shadow.balance(id, bucket);
    ASSERT_TRUE(ledger_balance.has_value());
    ASSERT_TRUE(shadow_balance.has_value());
    EXPECT_EQ(ledger_balance.value(), expected);
    EXPECT_EQ(shadow_balance.value(), expected);
}

void expect_all_buckets_match(const Ledger& ledger, const ShadowLedger& shadow) {
    const AccountId cardholder_id{"4242424242424242"};
    const AccountId merchant_id{"m-1"};
    const AccountId system_id{"system"};

    for (Bucket bucket : {
             Bucket::Available,
             Bucket::Holds,
             Bucket::Payable,
             Bucket::Interchange,
         }) {
        const auto ledger_card = ledger.balance(cardholder_id, bucket);
        const auto shadow_card = shadow.balance(cardholder_id, bucket);
        ASSERT_TRUE(ledger_card.has_value());
        ASSERT_TRUE(shadow_card.has_value());
        EXPECT_EQ(ledger_card.value(), shadow_card.value());

        const auto ledger_merchant = ledger.balance(merchant_id, bucket);
        const auto shadow_merchant = shadow.balance(merchant_id, bucket);
        ASSERT_TRUE(ledger_merchant.has_value());
        ASSERT_TRUE(shadow_merchant.has_value());
        EXPECT_EQ(ledger_merchant.value(), shadow_merchant.value());

        const auto ledger_system = ledger.balance(system_id, bucket);
        const auto shadow_system = shadow.balance(system_id, bucket);
        ASSERT_TRUE(ledger_system.has_value());
        ASSERT_TRUE(shadow_system.has_value());
        EXPECT_EQ(ledger_system.value(), shadow_system.value());
    }
}

TEST(CrashRecoveryTest, ShadowMatchesLedgerThroughLifecycle) {
    Ledger ledger{load_tiny_genesis_wallets()};
    ShadowLedger shadow{load_tiny_genesis_wallets()};

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};
    const TimePoint now{};

    ledger.set_hold_ttl(std::chrono::seconds{5});
    shadow.set_hold_ttl(std::chrono::seconds{5});

    const auto reserve_ledger = ledger.reserve(cardholder_id, amount, merchant_id, now);
    const auto reserve_shadow = shadow.reserve(cardholder_id, amount, merchant_id, now);
    ASSERT_TRUE(reserve_ledger.has_value());
    ASSERT_TRUE(reserve_shadow.has_value());
    expect_all_buckets_match(ledger, shadow);

    const HoldId hold_id = reserve_ledger.value().id;
    ASSERT_TRUE(ledger.capture(hold_id, amount).has_value());
    ASSERT_TRUE(shadow.capture(hold_id, amount).has_value());
    expect_all_buckets_match(ledger, shadow);

    const auto reserve2_ledger = ledger.reserve(cardholder_id, amount, merchant_id, now);
    const auto reserve2_shadow = shadow.reserve(cardholder_id, amount, merchant_id, now);
    ASSERT_TRUE(reserve2_ledger.has_value());
    ASSERT_TRUE(reserve2_shadow.has_value());
    const HoldId hold2 = reserve2_ledger.value().id;

    ASSERT_TRUE(ledger.reverse(hold2).has_value());
    ASSERT_TRUE(shadow.reverse(hold2).has_value());
    expect_all_buckets_match(ledger, shadow);

    const auto reserve3_ledger = ledger.reserve(cardholder_id, amount, merchant_id, now);
    const auto reserve3_shadow = shadow.reserve(cardholder_id, amount, merchant_id, now);
    ASSERT_TRUE(reserve3_ledger.has_value());
    ASSERT_TRUE(reserve3_shadow.has_value());

    const TimePoint at_expiry = now + std::chrono::seconds{5};
    EXPECT_EQ(ledger.expire_due(at_expiry), 1U);
    EXPECT_EQ(shadow.expire_due(at_expiry), 1U);
    expect_all_buckets_match(ledger, shadow);
}

std::filesystem::path make_temp_wal_path(const char* suffix) {
    return std::filesystem::temp_directory_path() /
           ("aegis_crash_recovery_" + std::string{suffix} + ".wal");
}

TEST(CrashRecoveryTest, FlushedReserveReplayRestoresState) {
    const auto genesis = load_tiny_genesis_wallets();
    const auto wal_path = make_temp_wal_path("reserve");
    std::filesystem::remove(wal_path);

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};

    HoldId expected_hold_id{0};

    {
        Ledger ledger{genesis};
        Wal wal{wal_path};
        ledger.set_wal(&wal);

        const auto result = ledger.reserve(cardholder_id, amount, merchant_id);
        ASSERT_TRUE(result.has_value());
        expected_hold_id = result.value().id;
        wal.flush();
    }

    const auto replayed = Wal::replay(wal_path, genesis);
    ASSERT_TRUE(replayed.has_value());
    const Ledger& ledger = replayed.value();

    const Money expected_available{Currency::Usd, 5000};
    const Money expected_holds{Currency::Usd, 5000};
    const auto available = ledger.balance(cardholder_id, Bucket::Available);
    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);
    ASSERT_TRUE(available.has_value());
    ASSERT_TRUE(holds.has_value());
    EXPECT_EQ(available.value(), expected_available);
    EXPECT_EQ(holds.value(), expected_holds);
    EXPECT_EQ(ledger.live_hold_count(), 1U);
    EXPECT_EQ(expected_hold_id, HoldId{1});

    std::filesystem::remove(wal_path);
}

TEST(CrashRecoveryTest, FullLifecycleReplayMatchesShadow) {
    const auto genesis = load_tiny_genesis_wallets();
    const auto wal_path = make_temp_wal_path("lifecycle");
    std::filesystem::remove(wal_path);

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};
    const TimePoint now{};

    {
        Ledger ledger{genesis};
        Wal wal{wal_path};
        ledger.set_wal(&wal);
        ledger.set_hold_ttl(std::chrono::seconds{5});

        const auto reserve = ledger.reserve(cardholder_id, amount, merchant_id, now);
        ASSERT_TRUE(reserve.has_value());
        wal.flush();

        const HoldId hold_id = reserve.value().id;
        ASSERT_TRUE(ledger.capture(hold_id, amount).has_value());
        wal.flush();

        const auto reserve2 = ledger.reserve(cardholder_id, amount, merchant_id, now);
        ASSERT_TRUE(reserve2.has_value());
        wal.flush();

        const HoldId hold2 = reserve2.value().id;
        ASSERT_TRUE(ledger.reverse(hold2).has_value());
        wal.flush();

        ASSERT_TRUE(ledger.reserve(cardholder_id, amount, merchant_id, now).has_value());
        wal.flush();

        const TimePoint at_expiry = now + std::chrono::seconds{5};
        EXPECT_EQ(ledger.expire_due(at_expiry), 1U);
        wal.flush();
    }

    ShadowLedger shadow{genesis};
    shadow.set_hold_ttl(std::chrono::seconds{5});

    const auto shadow_reserve1 = shadow.reserve(cardholder_id, amount, merchant_id, now);
    ASSERT_TRUE(shadow_reserve1.has_value());
    ASSERT_TRUE(shadow.capture(shadow_reserve1.value().id, amount).has_value());

    const auto shadow_reserve2 = shadow.reserve(cardholder_id, amount, merchant_id, now);
    ASSERT_TRUE(shadow_reserve2.has_value());
    ASSERT_TRUE(shadow.reverse(shadow_reserve2.value().id).has_value());

    ASSERT_TRUE(shadow.reserve(cardholder_id, amount, merchant_id, now).has_value());
    const TimePoint at_expiry = now + std::chrono::seconds{5};
    EXPECT_EQ(shadow.expire_due(at_expiry), 1U);

    const auto replayed = Wal::replay(wal_path, genesis);
    ASSERT_TRUE(replayed.has_value());
    expect_all_buckets_match(replayed.value(), shadow);

    std::filesystem::remove(wal_path);
}

TEST(CrashRecoveryTest, UnflushedReserveAbsentAfterReplay) {
    const auto genesis = load_tiny_genesis_wallets();
    const auto wal_path = make_temp_wal_path("unflushed");
    std::filesystem::remove(wal_path);

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};

    {
        Ledger ledger{genesis};
        Wal wal{wal_path};
        ledger.set_wal(&wal);

        ASSERT_TRUE(ledger.reserve(cardholder_id, amount, merchant_id).has_value());
        wal.flush();

        ASSERT_TRUE(ledger.reserve(cardholder_id, amount, merchant_id).has_value());
    }

    ShadowLedger shadow{genesis};
    ASSERT_TRUE(shadow.reserve(cardholder_id, amount, merchant_id).has_value());

    const auto replayed = Wal::replay(wal_path, genesis);
    ASSERT_TRUE(replayed.has_value());
    expect_all_buckets_match(replayed.value(), shadow);

    std::filesystem::remove(wal_path);
}

TEST(CrashRecoveryTest, TornLastLineDropped) {
    const auto genesis = load_tiny_genesis_wallets();
    const auto wal_path = make_temp_wal_path("torn");
    std::filesystem::remove(wal_path);

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};

    {
        Ledger ledger{genesis};
        Wal wal{wal_path};
        ledger.set_wal(&wal);

        ASSERT_TRUE(ledger.reserve(cardholder_id, amount, merchant_id).has_value());
        wal.flush();

        ASSERT_TRUE(ledger.reserve(cardholder_id, amount, merchant_id).has_value());
        wal.flush();
    }

    {
        std::ofstream out(wal_path, std::ios::app | std::ios::binary);
        ASSERT_TRUE(out);
        out << "R|999|4242424242424242|m-1|1000|Usd|0";
    }

    ShadowLedger shadow{genesis};
    ASSERT_TRUE(shadow.reserve(cardholder_id, amount, merchant_id).has_value());
    ASSERT_TRUE(shadow.reserve(cardholder_id, amount, merchant_id).has_value());

    const auto replayed = Wal::replay(wal_path, genesis);
    ASSERT_TRUE(replayed.has_value());
    expect_all_buckets_match(replayed.value(), shadow);

    std::filesystem::remove(wal_path);
}

} // namespace
} // namespace aegis::ledger
