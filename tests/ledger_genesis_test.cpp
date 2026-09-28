#include <gtest/gtest.h>

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/genesis.hpp>
#include <aegis/ledger/wallet.hpp>
#include <aegis/money.hpp>

#include <array>
#include <filesystem>

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

TEST(GenesisTest, LoadsWalletsWithOpeningAvailable) {
    const auto records = tiny_genesis_records();
    const auto result = load_genesis(records);
    ASSERT_TRUE(result.has_value());

    const auto& wallets = result.value();
    ASSERT_EQ(wallets.size(), 3U);

    const AccountId cardholder_id{"4242424242424242"};
    const AccountId merchant_id{"m-1"};
    const AccountId system_id{"system"};
    const Money zero{Currency::Usd, 0};
    const Money cardholder_available{Currency::Usd, 10000};

    ASSERT_NE(wallets.find(cardholder_id), wallets.end());
    ASSERT_NE(wallets.find(merchant_id), wallets.end());
    ASSERT_NE(wallets.find(system_id), wallets.end());

    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Available), cardholder_available);
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Holds), zero);
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Payable), zero);
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Interchange), zero);

    EXPECT_EQ(wallets.at(merchant_id).balance(Bucket::Available), zero);
    EXPECT_EQ(wallets.at(system_id).balance(Bucket::Available), zero);
}

TEST(GenesisTest, UnknownPanIsNotAutoFunded) {
    const auto records = tiny_genesis_records();
    const auto result = load_genesis(records);
    ASSERT_TRUE(result.has_value());

    const auto& wallets = result.value();
    const Pan unknown_pan{"4000000000000002"};
    EXPECT_EQ(wallets.find(AccountId{unknown_pan.full()}), wallets.end());
}

TEST(GenesisTest, LoadsTinyFixtureFromFile) {
    const auto result = load_genesis(std::filesystem::path{FIXTURES_DIR} / "genesis_tiny.csv");
    ASSERT_TRUE(result.has_value());

    const auto& wallets = result.value();
    ASSERT_EQ(wallets.size(), 3U);

    const AccountId cardholder_id{"4242424242424242"};
    const Money cardholder_available{Currency::Usd, 10000};
    const Money zero{Currency::Usd, 0};

    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Available), cardholder_available);
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Holds), zero);
    EXPECT_EQ(wallets.at(AccountId{"m-1"}).balance(Bucket::Available), zero);
    EXPECT_EQ(wallets.at(AccountId{"system"}).balance(Bucket::Available), zero);
}

} // namespace
} // namespace aegis::ledger
