#include <gtest/gtest.h>

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/posting.hpp>
#include <aegis/ledger/wallet.hpp>
#include <aegis/money.hpp>

#include <unordered_map>

namespace aegis::ledger {
namespace {

TEST(WalletTest, ConstructAndReadZeroBalances) {
    const Wallet wallet{AccountId{"ch-1"}, WalletKind::Cardholder, Currency::Usd};
    const Money zero{Currency::Usd, 0};

    EXPECT_EQ(wallet.balance(Bucket::Available), zero);
    EXPECT_EQ(wallet.balance(Bucket::Holds), zero);
    EXPECT_EQ(wallet.balance(Bucket::Payable), zero);
    EXPECT_EQ(wallet.balance(Bucket::Interchange), zero);
}

TEST(PostingBatchTest, HoldsLinesAndExposesContents) {
    const Money amount{Currency::Usd, 5000};
    const PostingLine line{
        AccountId{"ch-1"},
        Bucket::Available,
        amount,
        false,
    };
    const PostingBatch batch{line};

    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch[0].account, AccountId{"ch-1"});
    EXPECT_EQ(batch[0].bucket, Bucket::Available);
    EXPECT_EQ(batch[0].amount, amount);
    EXPECT_FALSE(batch[0].is_credit);
}

TEST(PostingBatchTest, BalancedBatchPassesValidation) {
    const Money amount{Currency::Usd, 5000};
    const PostingBatch batch{
        PostingLine{AccountId{"ch-1"}, Bucket::Available, amount, false},
        PostingLine{AccountId{"ch-1"}, Bucket::Holds, amount, true},
    };

    EXPECT_TRUE(is_balanced(batch));
    EXPECT_TRUE(validate(batch).has_value());
    assert_balanced(batch);
}

TEST(PostingBatchTest, UnbalancedBatchFailsValidation) {
    const Money debit{Currency::Usd, 5000};
    const Money credit{Currency::Usd, 4000};
    const PostingBatch batch{
        PostingLine{AccountId{"ch-1"}, Bucket::Available, debit, false},
        PostingLine{AccountId{"ch-1"}, Bucket::Holds, credit, true},
    };

    EXPECT_FALSE(is_balanced(batch));
    const auto result = validate(batch);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PostingError::Unbalanced);
}

TEST(ApplyBatchTest, AppliesBalancedIntraWalletBatch) {
    const AccountId cardholder_id{"ch-1"};
    std::unordered_map<AccountId, Wallet> wallets;
    wallets.emplace(cardholder_id, Wallet{cardholder_id, WalletKind::Cardholder, Currency::Usd});

    const Money amount{Currency::Usd, 5000};
    const PostingBatch batch{
        PostingLine{cardholder_id, Bucket::Available, amount, false},
        PostingLine{cardholder_id, Bucket::Holds, amount, true},
    };

    const auto result = apply_batch(wallets, batch);
    ASSERT_TRUE(result.has_value());
    const Money expected_available{Currency::Usd, -5000};
    const Money expected_holds{Currency::Usd, 5000};
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Available), expected_available);
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Holds), expected_holds);
}

TEST(ApplyBatchTest, RejectsUnbalancedBatchWithoutMutating) {
    const AccountId cardholder_id{"ch-1"};
    std::unordered_map<AccountId, Wallet> wallets;
    wallets.emplace(cardholder_id, Wallet{cardholder_id, WalletKind::Cardholder, Currency::Usd});

    const Money debit{Currency::Usd, 5000};
    const Money credit{Currency::Usd, 4000};
    const PostingBatch batch{
        PostingLine{cardholder_id, Bucket::Available, debit, false},
        PostingLine{cardholder_id, Bucket::Holds, credit, true},
    };

    const auto result = apply_batch(wallets, batch);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PostingError::Unbalanced);

    const Money zero{Currency::Usd, 0};
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Available), zero);
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Holds), zero);
}

TEST(ApplyBatchTest, AppliesBalancedCrossWalletBatch) {
    const AccountId cardholder_id{"ch-1"};
    const AccountId merchant_id{"m-1"};
    const AccountId system_id{"sys-1"};
    std::unordered_map<AccountId, Wallet> wallets;
    wallets.emplace(cardholder_id, Wallet{cardholder_id, WalletKind::Cardholder, Currency::Usd});
    wallets.emplace(merchant_id, Wallet{merchant_id, WalletKind::Merchant, Currency::Usd});
    wallets.emplace(system_id, Wallet{system_id, WalletKind::System, Currency::Usd});

    const Money hold_amount{Currency::Usd, 5000};
    const Money payable_amount{Currency::Usd, 4855};
    const Money interchange_amount{Currency::Usd, 145};
    const PostingBatch batch{
        PostingLine{cardholder_id, Bucket::Holds, hold_amount, false},
        PostingLine{merchant_id, Bucket::Payable, payable_amount, true},
        PostingLine{system_id, Bucket::Interchange, interchange_amount, true},
    };

    const auto result = apply_batch(wallets, batch);
    ASSERT_TRUE(result.has_value());
    const Money expected_holds_debit{Currency::Usd, -5000};
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Holds), expected_holds_debit);
    EXPECT_EQ(wallets.at(merchant_id).balance(Bucket::Payable), payable_amount);
    EXPECT_EQ(wallets.at(system_id).balance(Bucket::Interchange), interchange_amount);
}

TEST(ApplyBatchTest, RejectsUnknownWalletWithoutMutating) {
    const AccountId cardholder_id{"ch-1"};
    const AccountId unknown_id{"missing"};
    std::unordered_map<AccountId, Wallet> wallets;
    wallets.emplace(cardholder_id, Wallet{cardholder_id, WalletKind::Cardholder, Currency::Usd});

    const Money amount{Currency::Usd, 5000};
    const PostingBatch batch{
        PostingLine{cardholder_id, Bucket::Available, amount, false},
        PostingLine{unknown_id, Bucket::Holds, amount, true},
    };

    const auto result = apply_batch(wallets, batch);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), PostingError::UnknownWallet);

    const Money zero{Currency::Usd, 0};
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Available), zero);
    EXPECT_EQ(wallets.at(cardholder_id).balance(Bucket::Holds), zero);
}

} // namespace
} // namespace aegis::ledger
