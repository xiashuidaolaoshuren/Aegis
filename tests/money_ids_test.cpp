#include <gtest/gtest.h>

#include <aegis/ids.hpp>
#include <aegis/money.hpp>
#include <aegis/result.hpp>
#include <aegis/tagged.hpp>

#include <string>
#include <type_traits>

namespace aegis {
namespace {

TEST(ResultTest, HoldsOkValue) {
    Result<int, int> result = Result<int, int>::ok(5);
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 5);
}

TEST(ResultTest, HoldsErrValue) {
    Result<int, int> result = Result<int, int>::err(7);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), 7);
}

struct TagATag {};
struct TagBTag {};

TEST(TaggedTest, EqualWhenSameTagAndValue) {
    Tagged<int, TagATag> left{1};
    Tagged<int, TagATag> right{1};
    EXPECT_EQ(left, right);
}

static_assert(!std::is_convertible_v<Tagged<int, TagATag>, Tagged<int, TagBTag>>);

TEST(MoneyTest, AddsSameCurrency) {
    auto result = Money{Currency::Usd, 100} + Money{Currency::Usd, 50};
    ASSERT_TRUE(result.has_value());
    const Money expected{Currency::Usd, 150};
    EXPECT_EQ(result.value(), expected);
}

TEST(MoneyTest, RejectsMismatchedCurrency) {
    auto result = Money{Currency::Usd, 100} + Money{Currency::Eur, 50};
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), MoneyError::MismatchedCurrency);
}

static_assert(!std::is_convertible_v<Money, int>);
static_assert(!std::is_constructible_v<Money, int>);

TEST(IdsTest, StrongTypesAreEqualWhenValuesMatch) {
    EXPECT_EQ(AccountId{"wallet-1"}, AccountId{"wallet-1"});
    EXPECT_EQ(MerchantId{"merchant-1"}, MerchantId{"merchant-1"});
    EXPECT_EQ(TerminalId{"terminal-1"}, TerminalId{"terminal-1"});
    EXPECT_EQ(Stan{42}, Stan{42});
    EXPECT_EQ(Rrn{"rrn-1"}, Rrn{"rrn-1"});
}

TEST(PanTest, MasksCardNumberForDisplay) {
    EXPECT_EQ(Pan{"4242424242424242"}.masked(), "424242******4242");
}

TEST(PanTest, ExposesFullValueExplicitly) {
    Pan pan{"4242424242424242"};
    EXPECT_EQ(pan.full(), "4242424242424242");
}

static_assert(!std::is_convertible_v<Pan, std::string>);

} // namespace
} // namespace aegis
