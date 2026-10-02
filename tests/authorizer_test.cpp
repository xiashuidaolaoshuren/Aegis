#include <gtest/gtest.h>

#include <aegis/authorizer/authorizer.hpp>
#include <aegis/iso8583/message.hpp>
#include <aegis/ledger/genesis.hpp>
#include <aegis/ledger/idempotency.hpp>
#include <aegis/ledger/ledger.hpp>

#include <array>
#include <chrono>

namespace aegis::authorizer {
namespace {

using iso8583::FieldId;
using iso8583::Message;
using iso8583::Mti;
using ledger::IdempotencyStore;
using ledger::Ledger;
using ledger::load_genesis;

Ledger make_tiny_ledger() {
    const std::array<ledger::GenesisRecord, 3> records{{
        {ledger::WalletKind::Cardholder, AccountId{"4242424242424242"}, Currency::Usd, 10000},
        {ledger::WalletKind::Merchant, AccountId{"m-1"}, Currency::Usd, 0},
        {ledger::WalletKind::System, AccountId{"system"}, Currency::Usd, 0},
    }};
    const auto loaded = load_genesis(records);
    EXPECT_TRUE(loaded.has_value());
    return Ledger{std::move(loaded.value())};
}

Message make_valid_auth_request() {
    Message request{Mti::AuthorizationRequest};
    (void)request.set(FieldId::Pan, "4242424242424242");
    (void)request.set(FieldId::Amount, "000000005000");
    (void)request.set(FieldId::TransmissionDateTime, "1002153045");
    (void)request.set(FieldId::Stan, "000001");
    (void)request.set(FieldId::TerminalId, "TERM0001");
    (void)request.set(FieldId::MerchantId, "m-1          ");
    (void)request.set(FieldId::Currency, "840");
    return request;
}

[[nodiscard]] std::string response_code(const Message& response) {
    const auto code = response.get(FieldId::ResponseCode);
    EXPECT_TRUE(code.has_value());
    return code.value();
}

struct AuthorizerFixture {
    Ledger ledger{make_tiny_ledger()};
    IdempotencyStore store;
    issuersim::IssuerSim issuer{};
    ScreenConfig screen{};
    Authorizer authorizer{ledger, store, issuer, screen};
};

TEST(AuthorizerTest, MissingRequiredFieldReturns30WithoutIssuerCall) {
    AuthorizerFixture fixture;
    Message request{Mti::AuthorizationRequest};
    (void)request.set(FieldId::Amount, "000000005000");
    (void)request.set(FieldId::TransmissionDateTime, "1002153045");
    (void)request.set(FieldId::Stan, "000001");
    (void)request.set(FieldId::TerminalId, "TERM0001");
    (void)request.set(FieldId::MerchantId, "m-1          ");
    (void)request.set(FieldId::Currency, "840");

    const Message response = fixture.authorizer.handle(request);

    EXPECT_EQ(response.mti(), Mti::AuthorizationResponse);
    EXPECT_EQ(response_code(response), "30");
    EXPECT_EQ(fixture.issuer.calls, 0);
    EXPECT_EQ(fixture.ledger.live_hold_count(), 0U);
}

TEST(AuthorizerTest, ZeroAmountReturns05WithoutIssuerCall) {
    AuthorizerFixture fixture;
    Message request = make_valid_auth_request();
    (void)request.set(FieldId::Amount, "000000000000");

    const Message response = fixture.authorizer.handle(request);

    EXPECT_EQ(response_code(response), "05");
    EXPECT_EQ(fixture.issuer.calls, 0);
    EXPECT_EQ(fixture.ledger.live_hold_count(), 0U);
}

TEST(AuthorizerTest, CurrencyMismatchReturns05WithoutIssuerCall) {
    AuthorizerFixture fixture;
    Message request = make_valid_auth_request();
    (void)request.set(FieldId::Currency, "978");

    const Message response = fixture.authorizer.handle(request);

    EXPECT_EQ(response_code(response), "05");
    EXPECT_EQ(fixture.issuer.calls, 0);
    EXPECT_EQ(fixture.ledger.live_hold_count(), 0U);
}

TEST(AuthorizerTest, AmountOverMaxReturns05WithoutIssuerCall) {
    AuthorizerFixture fixture;
    fixture.screen.max_amount = Money{Currency::Usd, 1000};
    Message request = make_valid_auth_request();
    (void)request.set(FieldId::Amount, "000000005000");

    const Message response = fixture.authorizer.handle(request);

    EXPECT_EQ(response_code(response), "05");
    EXPECT_EQ(fixture.issuer.calls, 0);
    EXPECT_EQ(fixture.ledger.live_hold_count(), 0U);
}

TEST(AuthorizerTest, InsufficientFundsReturns51WithoutIssuerCall) {
    AuthorizerFixture fixture;
    Message request = make_valid_auth_request();
    (void)request.set(FieldId::Amount, "000000010001");

    const Message response = fixture.authorizer.handle(request);

    EXPECT_EQ(response_code(response), "51");
    EXPECT_EQ(fixture.issuer.calls, 0);
    EXPECT_EQ(fixture.ledger.live_hold_count(), 0U);
}

TEST(AuthorizerTest, ValidRequestReservesWaitsAndReturns00) {
    AuthorizerFixture fixture;
    const Message request = make_valid_auth_request();

    const Message response = fixture.authorizer.handle(request);

    EXPECT_EQ(response_code(response), "00");
    EXPECT_EQ(fixture.issuer.calls, 1);
    EXPECT_EQ(fixture.ledger.live_hold_count(), 1U);
}

TEST(AuthorizerTest, IssuerTimeoutReversesHoldAndReturns05) {
    AuthorizerFixture fixture;
    fixture.issuer.latency = std::chrono::seconds{5};
    fixture.issuer.timeout = std::chrono::seconds{1};
    const Message request = make_valid_auth_request();

    const Message response = fixture.authorizer.handle(request);

    EXPECT_EQ(response_code(response), "05");
    EXPECT_EQ(fixture.issuer.calls, 1);
    EXPECT_EQ(fixture.ledger.live_hold_count(), 0U);
}

TEST(AuthorizerTest, IssuerDeclineReversesHoldAndReturns05) {
    AuthorizerFixture fixture;
    fixture.issuer.inject_decline = true;
    const Message request = make_valid_auth_request();

    const Message response = fixture.authorizer.handle(request);

    EXPECT_EQ(response_code(response), "05");
    EXPECT_EQ(fixture.issuer.calls, 1);
    EXPECT_EQ(fixture.ledger.live_hold_count(), 0U);
}

TEST(AuthorizerTest, DuplicateKeyReturnsStoredResponseWithoutSecondReserveOrIssuerCall) {
    AuthorizerFixture fixture;
    const Message request = make_valid_auth_request();

    const Message first = fixture.authorizer.handle(request);
    const Message second = fixture.authorizer.handle(request);

    EXPECT_EQ(response_code(first), "00");
    EXPECT_EQ(response_code(second), "00");
    EXPECT_EQ(fixture.issuer.calls, 1);
    EXPECT_EQ(fixture.ledger.live_hold_count(), 1U);
}

} // namespace
} // namespace aegis::authorizer
