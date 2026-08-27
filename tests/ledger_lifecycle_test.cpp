#include <gtest/gtest.h>

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/genesis.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/money.hpp>

#include <array>

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

Ledger make_ledger_from_tiny_genesis() {
    const auto records = tiny_genesis_records();
    const auto result = load_genesis(records);
    EXPECT_TRUE(result.has_value());
    return Ledger{std::move(result.value())};
}

TEST(LifecycleTest, ConstructsFromGenesisAndReadsAvailable) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const Money expected_available{Currency::Usd, 10000};
    const auto balance = ledger.balance(cardholder_id, Bucket::Available);

    ASSERT_TRUE(balance.has_value());
    EXPECT_EQ(balance.value(), expected_available);
}

TEST(LifecycleTest, ReserveMovesAvailableToHolds) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};

    const auto result = ledger.reserve(cardholder_id, amount, merchant_id);
    ASSERT_TRUE(result.has_value());

    const Hold& hold = result.value();
    EXPECT_EQ(hold.cardholder, cardholder_id);
    EXPECT_EQ(hold.merchant, merchant_id);
    EXPECT_EQ(hold.amount, amount);

    const Money expected_available{Currency::Usd, 5000};
    const Money expected_holds{Currency::Usd, 5000};
    const auto available = ledger.balance(cardholder_id, Bucket::Available);
    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);

    ASSERT_TRUE(available.has_value());
    ASSERT_TRUE(holds.has_value());
    EXPECT_EQ(available.value(), expected_available);
    EXPECT_EQ(holds.value(), expected_holds);
    EXPECT_EQ(ledger.live_hold_count(), 1U);
}

TEST(LifecycleTest, InsufficientFundsPostsNoHold) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 10001};
    const MerchantId merchant_id{"m-1"};

    const auto result = ledger.reserve(cardholder_id, amount, merchant_id);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LedgerError::InsufficientFunds);

    const Money expected_available{Currency::Usd, 10000};
    const Money zero{Currency::Usd, 0};
    const auto available = ledger.balance(cardholder_id, Bucket::Available);
    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);

    ASSERT_TRUE(available.has_value());
    ASSERT_TRUE(holds.has_value());
    EXPECT_EQ(available.value(), expected_available);
    EXPECT_EQ(holds.value(), zero);
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

TEST(LifecycleTest, UnknownPanIsNotReserved) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const Pan unknown_pan{"4000000000000002"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};

    const auto result = ledger.reserve(AccountId{unknown_pan.full()}, amount, merchant_id);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), LedgerError::UnknownWallet);
    EXPECT_EQ(ledger.live_hold_count(), 0U);

    const Money expected_available{Currency::Usd, 10000};
    const auto available = ledger.balance(cardholder_id, Bucket::Available);
    ASSERT_TRUE(available.has_value());
    EXPECT_EQ(available.value(), expected_available);
}

} // namespace
} // namespace aegis::ledger
