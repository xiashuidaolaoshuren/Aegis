#include <gtest/gtest.h>

#include <aegis/ids.hpp>
#include <aegis/ledger/bucket.hpp>
#include <aegis/ledger/fee.hpp>
#include <aegis/ledger/genesis.hpp>
#include <aegis/ledger/hold.hpp>
#include <aegis/ledger/idempotency.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/money.hpp>

#include <array>
#include <chrono>

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

TEST(LifecycleTest, SplitCaptureAppliesFee) {
    const Money amount{Currency::Usd, 5000};
    const CaptureSplit split = split_capture(amount);

    const Money expected_payable{Currency::Usd, 4855};
    const Money expected_interchange{Currency::Usd, 145};
    EXPECT_EQ(split.payable, expected_payable);
    EXPECT_EQ(split.interchange, expected_interchange);
}

TEST(LifecycleTest, CaptureConsumesHoldAndSplitsFee) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const AccountId merchant_id{"m-1"};
    const AccountId system_id{"system"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant{merchant_id.value()};

    const auto reserve_result = ledger.reserve(cardholder_id, amount, merchant);
    ASSERT_TRUE(reserve_result.has_value());
    const HoldId hold_id = reserve_result.value().id;

    const auto capture_result = ledger.capture(hold_id, amount);
    ASSERT_TRUE(capture_result.has_value());

    const Money expected_holds{Currency::Usd, 0};
    const Money expected_payable{Currency::Usd, 4855};
    const Money expected_interchange{Currency::Usd, 145};

    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);
    const auto payable = ledger.balance(merchant_id, Bucket::Payable);
    const auto interchange = ledger.balance(system_id, Bucket::Interchange);

    ASSERT_TRUE(holds.has_value());
    ASSERT_TRUE(payable.has_value());
    ASSERT_TRUE(interchange.has_value());
    EXPECT_EQ(holds.value(), expected_holds);
    EXPECT_EQ(payable.value(), expected_payable);
    EXPECT_EQ(interchange.value(), expected_interchange);
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

TEST(LifecycleTest, CaptureRejectsAmountMismatch) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const AccountId merchant_id{"m-1"};
    const Money hold_amount{Currency::Usd, 5000};
    const Money wrong_amount{Currency::Usd, 4000};
    const MerchantId merchant{merchant_id.value()};

    const auto reserve_result = ledger.reserve(cardholder_id, hold_amount, merchant);
    ASSERT_TRUE(reserve_result.has_value());
    const HoldId hold_id = reserve_result.value().id;

    const auto capture_result = ledger.capture(hold_id, wrong_amount);
    ASSERT_FALSE(capture_result.has_value());
    EXPECT_EQ(capture_result.error(), LedgerError::AmountMismatch);

    const Money expected_holds{Currency::Usd, 5000};
    const Money expected_payable{Currency::Usd, 0};
    const Money expected_interchange{Currency::Usd, 0};

    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);
    const auto payable = ledger.balance(merchant_id, Bucket::Payable);
    const auto interchange = ledger.balance(AccountId{"system"}, Bucket::Interchange);

    ASSERT_TRUE(holds.has_value());
    ASSERT_TRUE(payable.has_value());
    ASSERT_TRUE(interchange.has_value());
    EXPECT_EQ(holds.value(), expected_holds);
    EXPECT_EQ(payable.value(), expected_payable);
    EXPECT_EQ(interchange.value(), expected_interchange);
    EXPECT_EQ(ledger.live_hold_count(), 1U);
}

TEST(LifecycleTest, CaptureRejectsUnknownHold) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const AccountId merchant_id{"m-1"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant{merchant_id.value()};

    const auto reserve_result = ledger.reserve(cardholder_id, amount, merchant);
    ASSERT_TRUE(reserve_result.has_value());
    const HoldId hold_id = reserve_result.value().id;

    const HoldId missing_hold_id{999};
    const auto missing_result = ledger.capture(missing_hold_id, amount);
    ASSERT_FALSE(missing_result.has_value());
    EXPECT_EQ(missing_result.error(), LedgerError::UnknownHold);

    const auto first_capture = ledger.capture(hold_id, amount);
    ASSERT_TRUE(first_capture.has_value());

    const auto second_capture = ledger.capture(hold_id, amount);
    ASSERT_FALSE(second_capture.has_value());
    EXPECT_EQ(second_capture.error(), LedgerError::UnknownHold);

    const Money expected_holds{Currency::Usd, 0};
    const Money expected_payable{Currency::Usd, 4855};
    const Money expected_interchange{Currency::Usd, 145};

    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);
    const auto payable = ledger.balance(merchant_id, Bucket::Payable);
    const auto interchange = ledger.balance(AccountId{"system"}, Bucket::Interchange);

    ASSERT_TRUE(holds.has_value());
    ASSERT_TRUE(payable.has_value());
    ASSERT_TRUE(interchange.has_value());
    EXPECT_EQ(holds.value(), expected_holds);
    EXPECT_EQ(payable.value(), expected_payable);
    EXPECT_EQ(interchange.value(), expected_interchange);
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

TEST(LifecycleTest, ReverseReleasesHoldToAvailable) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};

    const auto reserve_result = ledger.reserve(cardholder_id, amount, merchant_id);
    ASSERT_TRUE(reserve_result.has_value());
    const HoldId hold_id = reserve_result.value().id;

    const auto reverse_result = ledger.reverse(hold_id);
    ASSERT_TRUE(reverse_result.has_value());

    const Money expected_available{Currency::Usd, 10000};
    const Money expected_holds{Currency::Usd, 0};
    const auto available = ledger.balance(cardholder_id, Bucket::Available);
    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);

    ASSERT_TRUE(available.has_value());
    ASSERT_TRUE(holds.has_value());
    EXPECT_EQ(available.value(), expected_available);
    EXPECT_EQ(holds.value(), expected_holds);
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

TEST(LifecycleTest, ReverseAfterCaptureFails) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const AccountId merchant_id{"m-1"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant{merchant_id.value()};

    const auto reserve_result = ledger.reserve(cardholder_id, amount, merchant);
    ASSERT_TRUE(reserve_result.has_value());
    const HoldId hold_id = reserve_result.value().id;

    const auto capture_result = ledger.capture(hold_id, amount);
    ASSERT_TRUE(capture_result.has_value());

    const auto reverse_result = ledger.reverse(hold_id);
    ASSERT_FALSE(reverse_result.has_value());
    EXPECT_EQ(reverse_result.error(), LedgerError::UnknownHold);

    const Money expected_holds{Currency::Usd, 0};
    const Money expected_payable{Currency::Usd, 4855};
    const Money expected_interchange{Currency::Usd, 145};

    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);
    const auto payable = ledger.balance(merchant_id, Bucket::Payable);
    const auto interchange = ledger.balance(AccountId{"system"}, Bucket::Interchange);

    ASSERT_TRUE(holds.has_value());
    ASSERT_TRUE(payable.has_value());
    ASSERT_TRUE(interchange.has_value());
    EXPECT_EQ(holds.value(), expected_holds);
    EXPECT_EQ(payable.value(), expected_payable);
    EXPECT_EQ(interchange.value(), expected_interchange);
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

TEST(LifecycleTest, ReverseRejectsUnknownHold) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};

    const auto reserve_result = ledger.reserve(cardholder_id, amount, merchant_id);
    ASSERT_TRUE(reserve_result.has_value());

    const HoldId missing_hold_id{999};
    const auto reverse_result = ledger.reverse(missing_hold_id);
    ASSERT_FALSE(reverse_result.has_value());
    EXPECT_EQ(reverse_result.error(), LedgerError::UnknownHold);

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

TEST(LifecycleTest, ExpireDueReleasesHold) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};
    const TimePoint now{};

    ledger.set_hold_ttl(std::chrono::seconds{0});

    const auto reserve_result = ledger.reserve(cardholder_id, amount, merchant_id, now);
    ASSERT_TRUE(reserve_result.has_value());

    const std::size_t expired_count = ledger.expire_due(now);
    EXPECT_EQ(expired_count, 1U);

    const Money expected_available{Currency::Usd, 10000};
    const Money expected_holds{Currency::Usd, 0};
    const auto available = ledger.balance(cardholder_id, Bucket::Available);
    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);

    ASSERT_TRUE(available.has_value());
    ASSERT_TRUE(holds.has_value());
    EXPECT_EQ(available.value(), expected_available);
    EXPECT_EQ(holds.value(), expected_holds);
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

TEST(LifecycleTest, ExpireDueRespectsHoldTtl) {
    Ledger ledger = make_ledger_from_tiny_genesis();

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};
    const TimePoint now{};

    ledger.set_hold_ttl(std::chrono::seconds{5});

    const auto reserve_result = ledger.reserve(cardholder_id, amount, merchant_id, now);
    ASSERT_TRUE(reserve_result.has_value());
    EXPECT_EQ(reserve_result.value().expires_at, now + std::chrono::seconds{5});

    const TimePoint before_expiry = now + std::chrono::seconds{3};
    const TimePoint at_expiry = now + std::chrono::seconds{5};

    EXPECT_EQ(ledger.expire_due(before_expiry), 0U);
    EXPECT_EQ(ledger.live_hold_count(), 1U);

    const Money expected_available_before{Currency::Usd, 5000};
    const Money expected_holds_before{Currency::Usd, 5000};
    const auto available_before = ledger.balance(cardholder_id, Bucket::Available);
    const auto holds_before = ledger.balance(cardholder_id, Bucket::Holds);
    ASSERT_TRUE(available_before.has_value());
    ASSERT_TRUE(holds_before.has_value());
    EXPECT_EQ(available_before.value(), expected_available_before);
    EXPECT_EQ(holds_before.value(), expected_holds_before);

    EXPECT_EQ(ledger.expire_due(at_expiry), 1U);
    EXPECT_EQ(ledger.live_hold_count(), 0U);

    const Money expected_available_after{Currency::Usd, 10000};
    const Money expected_holds_after{Currency::Usd, 0};
    const auto available_after = ledger.balance(cardholder_id, Bucket::Available);
    const auto holds_after = ledger.balance(cardholder_id, Bucket::Holds);
    ASSERT_TRUE(available_after.has_value());
    ASSERT_TRUE(holds_after.has_value());
    EXPECT_EQ(available_after.value(), expected_available_after);
    EXPECT_EQ(holds_after.value(), expected_holds_after);
}

TEST(LifecycleTest, IdempotencyStoreStoresAndFindsReserveOutcome) {
    IdempotencyStore store;
    const IdempotencyKey key{
        TerminalId{"TERM0001"},
        Stan{123456},
        Mmdd{907},
    };

    const Hold hold{
        HoldId{1},
        AccountId{"4242424242424242"},
        MerchantId{"m-1"},
        Money{Currency::Usd, 5000},
        TimePoint{},
    };
    const Result<Hold, LedgerError> outcome = Result<Hold, LedgerError>::ok(hold);

    store.store_reserve(key, outcome);
    const Result<Hold, LedgerError>* found = store.find_reserve(key);
    ASSERT_NE(found, nullptr);
    ASSERT_TRUE(found->has_value());
    EXPECT_EQ(found->value().id, hold.id);
    EXPECT_EQ(found->value().amount, hold.amount);
}

Result<Hold, LedgerError> idempotent_reserve(
    Ledger& ledger,
    IdempotencyStore& store,
    IdempotencyKey key,
    AccountId cardholder,
    Money amount,
    MerchantId merchant) {
    if (const Result<Hold, LedgerError>* cached = store.find_reserve(key)) {
        return *cached;
    }

    const auto outcome = ledger.reserve(cardholder, amount, merchant);
    store.store_reserve(key, outcome);
    return outcome;
}

Result<void, LedgerError> idempotent_capture(
    Ledger& ledger,
    IdempotencyStore& store,
    IdempotencyKey key,
    HoldId hold_id,
    Money amount) {
    if (const Result<void, LedgerError>* cached = store.find_capture(key)) {
        return *cached;
    }

    const auto outcome = ledger.capture(hold_id, amount);
    store.store_capture(key, outcome);
    return outcome;
}

Result<void, LedgerError> idempotent_reverse(
    Ledger& ledger,
    IdempotencyStore& store,
    IdempotencyKey key,
    HoldId hold_id) {
    if (const Result<void, LedgerError>* cached = store.find_reverse(key)) {
        return *cached;
    }

    const auto outcome = ledger.reverse(hold_id);
    store.store_reverse(key, outcome);
    return outcome;
}

TEST(LifecycleTest, DuplicateReverseReturnsStoredOutcome) {
    Ledger ledger = make_ledger_from_tiny_genesis();
    IdempotencyStore store;

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};

    const IdempotencyKey auth_key{
        TerminalId{"TERM0001"},
        Stan{100001},
        Mmdd{907},
    };
    const auto reserve_result =
        idempotent_reserve(ledger, store, auth_key, cardholder_id, amount, merchant_id);
    ASSERT_TRUE(reserve_result.has_value());
    const HoldId hold_id = reserve_result.value().id;

    const IdempotencyKey reverse_key{
        TerminalId{"TERM0001"},
        Stan{400001},
        Mmdd{907},
    };

    const auto first = idempotent_reverse(ledger, store, reverse_key, hold_id);
    ASSERT_TRUE(first.has_value());

    const auto second = idempotent_reverse(ledger, store, reverse_key, hold_id);
    ASSERT_TRUE(second.has_value());

    const Money expected_available{Currency::Usd, 10000};
    const Money expected_holds{Currency::Usd, 0};
    const auto available = ledger.balance(cardholder_id, Bucket::Available);
    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);

    ASSERT_TRUE(available.has_value());
    ASSERT_TRUE(holds.has_value());
    EXPECT_EQ(available.value(), expected_available);
    EXPECT_EQ(holds.value(), expected_holds);
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

TEST(LifecycleTest, DuplicateCaptureReturnsStoredOutcome) {
    Ledger ledger = make_ledger_from_tiny_genesis();
    IdempotencyStore store;

    const AccountId cardholder_id{"4242424242424242"};
    const AccountId merchant_id{"m-1"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant{merchant_id.value()};

    const IdempotencyKey auth_key{
        TerminalId{"TERM0001"},
        Stan{100001},
        Mmdd{907},
    };
    const auto reserve_result =
        idempotent_reserve(ledger, store, auth_key, cardholder_id, amount, merchant);
    ASSERT_TRUE(reserve_result.has_value());
    const HoldId hold_id = reserve_result.value().id;

    const IdempotencyKey capture_key{
        TerminalId{"TERM0001"},
        Stan{200001},
        Mmdd{907},
    };

    const auto first = idempotent_capture(ledger, store, capture_key, hold_id, amount);
    ASSERT_TRUE(first.has_value());

    const auto second = idempotent_capture(ledger, store, capture_key, hold_id, amount);
    ASSERT_TRUE(second.has_value());

    const Money expected_holds{Currency::Usd, 0};
    const Money expected_payable{Currency::Usd, 4855};
    const Money expected_interchange{Currency::Usd, 145};

    const auto holds = ledger.balance(cardholder_id, Bucket::Holds);
    const auto payable = ledger.balance(merchant_id, Bucket::Payable);
    const auto interchange = ledger.balance(AccountId{"system"}, Bucket::Interchange);

    ASSERT_TRUE(holds.has_value());
    ASSERT_TRUE(payable.has_value());
    ASSERT_TRUE(interchange.has_value());
    EXPECT_EQ(holds.value(), expected_holds);
    EXPECT_EQ(payable.value(), expected_payable);
    EXPECT_EQ(interchange.value(), expected_interchange);
    EXPECT_EQ(ledger.live_hold_count(), 0U);
}

TEST(LifecycleTest, DuplicateReserveReturnsStoredHold) {
    Ledger ledger = make_ledger_from_tiny_genesis();
    IdempotencyStore store;

    const AccountId cardholder_id{"4242424242424242"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant_id{"m-1"};
    const IdempotencyKey key{
        TerminalId{"TERM0001"},
        Stan{100001},
        Mmdd{907},
    };

    const auto first = idempotent_reserve(ledger, store, key, cardholder_id, amount, merchant_id);
    ASSERT_TRUE(first.has_value());

    const auto second = idempotent_reserve(ledger, store, key, cardholder_id, amount, merchant_id);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second.value().id, first.value().id);

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

TEST(LifecycleTest, AuthKeyLocatesHoldForCapture) {
    Ledger ledger = make_ledger_from_tiny_genesis();
    IdempotencyStore store;

    const AccountId cardholder_id{"4242424242424242"};
    const AccountId merchant_id{"m-1"};
    const Money amount{Currency::Usd, 5000};
    const MerchantId merchant{merchant_id.value()};

    const IdempotencyKey auth_key{
        TerminalId{"TERM0001"},
        Stan{100001},
        Mmdd{907},
    };

    const auto reserve_result =
        idempotent_reserve(ledger, store, auth_key, cardholder_id, amount, merchant);
    ASSERT_TRUE(reserve_result.has_value());

    const HoldId* hold_id = store.find_hold_id(auth_key);
    ASSERT_NE(hold_id, nullptr);
    EXPECT_EQ(*hold_id, reserve_result.value().id);

    const IdempotencyKey capture_key{
        TerminalId{"TERM0001"},
        Stan{200002},
        Mmdd{907},
    };
    const auto capture_result =
        idempotent_capture(ledger, store, capture_key, *hold_id, amount);
    ASSERT_TRUE(capture_result.has_value());

    const Money expected_payable{Currency::Usd, 4855};
    const Money expected_interchange{Currency::Usd, 145};
    const auto payable = ledger.balance(merchant_id, Bucket::Payable);
    const auto interchange = ledger.balance(AccountId{"system"}, Bucket::Interchange);

    ASSERT_TRUE(payable.has_value());
    ASSERT_TRUE(interchange.has_value());
    EXPECT_EQ(payable.value(), expected_payable);
    EXPECT_EQ(interchange.value(), expected_interchange);
}

} // namespace
} // namespace aegis::ledger
