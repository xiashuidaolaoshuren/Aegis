#include <gtest/gtest.h>

#include <aegis/iso8583/fields.hpp>
#include <aegis/iso8583/message.hpp>

namespace aegis::iso8583 {
namespace {

TEST(Iso8583FieldsTest, FindsInScopeField) {
    const FieldSpec* spec = find_field(FieldId::Pan);
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->id, FieldId::Pan);
}

TEST(Iso8583FieldsTest, ReturnsNullForOutOfScopeField) {
    EXPECT_EQ(find_field(static_cast<FieldId>(99)), nullptr);
}

TEST(Iso8583FieldsTest, FreezesPanEncoding) {
    const FieldSpec* spec = find_field(FieldId::Pan);
    ASSERT_NE(spec, nullptr);
    EXPECT_EQ(spec->encoding, Encoding::Bcd);
    EXPECT_EQ(spec->length_kind, LengthKind::Llvar);
    EXPECT_EQ(spec->min_length, 13);
    EXPECT_EQ(spec->max_length, 19);
}

TEST(Iso8583MessageTest, SetsAndGetsPresentField) {
    Message message{Mti::AuthorizationRequest};
    const std::string pan = "4242424242424242";
    ASSERT_TRUE(message.set(FieldId::Pan, pan).has_value());
    EXPECT_TRUE(message.has(FieldId::Pan));
    auto value = message.get(FieldId::Pan);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), pan);
}

TEST(Iso8583MessageTest, ReturnsErrorForMissingField) {
    Message message{Mti::AuthorizationRequest};
    auto value = message.get(FieldId::Stan);
    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error(), MessageError::FieldAbsent);
}

TEST(Iso8583MessageTest, ClassifiesRequestAndResponseMtis) {
    EXPECT_TRUE(is_request(Mti::AuthorizationRequest));
    EXPECT_TRUE(is_response(Mti::AuthorizationResponse));
    EXPECT_FALSE(is_request(Mti::AuthorizationResponse));
    EXPECT_FALSE(is_response(Mti::AuthorizationRequest));

    auto authorization = response_mti(Mti::AuthorizationRequest);
    ASSERT_TRUE(authorization.has_value());
    EXPECT_EQ(authorization.value(), Mti::AuthorizationResponse);

    auto capture = response_mti(Mti::CaptureRequest);
    ASSERT_TRUE(capture.has_value());
    EXPECT_EQ(capture.value(), Mti::CaptureResponse);

    auto reversal = response_mti(Mti::ReversalRequest);
    ASSERT_TRUE(reversal.has_value());
    EXPECT_EQ(reversal.value(), Mti::ReversalResponse);

    auto network = response_mti(Mti::NetworkManagementRequest);
    ASSERT_TRUE(network.has_value());
    EXPECT_EQ(network.value(), Mti::NetworkManagementResponse);
}

TEST(Iso8583MessageTest, RejectsUnknownField) {
    Message message{Mti::AuthorizationRequest};
    auto result = message.set(static_cast<FieldId>(99), "x");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), MessageError::UnknownField);
}

} // namespace
} // namespace aegis::iso8583
