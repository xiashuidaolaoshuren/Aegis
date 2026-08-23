#include <gtest/gtest.h>

#include <aegis/iso8583/codec.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>

namespace aegis::iso8583 {
namespace {

TEST(Iso8583CodecTest, ParseEmptySpanIsTruncated) {
    const auto result = parse(std::span<const std::byte>{});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecError::Truncated);
}

TEST(Iso8583CodecTest, ParseOneByteSpanIsTruncated) {
    const std::array<std::byte, 1> bytes{std::byte{0x01}};
    const auto result = parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecError::Truncated);
}

TEST(Iso8583CodecTest, EmptyAuthorizationRequestRoundTrips) {
    const Message original{Mti::AuthorizationRequest};
    const auto encoded = serialise(original);
    ASSERT_TRUE(encoded.has_value());
    const auto parsed = parse(encoded.value());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().mti(), Mti::AuthorizationRequest);
    for (const FieldSpec& spec : kFieldTable) {
        EXPECT_FALSE(parsed.value().has(spec.id));
    }
}

TEST(Iso8583CodecTest, AsciiResponseCodeRoundTrips) {
    Message original{Mti::AuthorizationRequest};
    ASSERT_TRUE(original.set(FieldId::ResponseCode, "00").has_value());
    const auto encoded = serialise(original);
    ASSERT_TRUE(encoded.has_value());
    const auto parsed = parse(encoded.value());
    ASSERT_TRUE(parsed.has_value());
    auto value = parsed.value().get(FieldId::ResponseCode);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), "00");
}

TEST(Iso8583CodecTest, PackedBcdEvenAndOddRoundTrips) {
    Message original{Mti::AuthorizationRequest};
    ASSERT_TRUE(original.set(FieldId::Stan, "000042").has_value());
    ASSERT_TRUE(original.set(FieldId::Currency, "840").has_value());
    const auto encoded = serialise(original);
    ASSERT_TRUE(encoded.has_value());
    const auto parsed = parse(encoded.value());
    ASSERT_TRUE(parsed.has_value());
    auto stan = parsed.value().get(FieldId::Stan);
    ASSERT_TRUE(stan.has_value());
    EXPECT_EQ(stan.value(), "000042");
    auto currency = parsed.value().get(FieldId::Currency);
    ASSERT_TRUE(currency.has_value());
    EXPECT_EQ(currency.value(), "840");
}

TEST(Iso8583CodecTest, LlvarPanRoundTrips) {
    Message original{Mti::AuthorizationRequest};
    ASSERT_TRUE(original.set(FieldId::Pan, "4242424242424242").has_value());
    const auto encoded = serialise(original);
    ASSERT_TRUE(encoded.has_value());
    const auto parsed = parse(encoded.value());
    ASSERT_TRUE(parsed.has_value());
    auto pan = parsed.value().get(FieldId::Pan);
    ASSERT_TRUE(pan.has_value());
    EXPECT_EQ(pan.value(), "4242424242424242");
}

TEST(Iso8583CodecTest, BinaryPinBlockRoundTrips) {
    const std::string pin_block{"\x01\x23\x45\x67\x89\xAB\xCD\xEF", 8};
    Message original{Mti::AuthorizationRequest};
    ASSERT_TRUE(original.set(FieldId::PinBlock, pin_block).has_value());
    const auto encoded = serialise(original);
    ASSERT_TRUE(encoded.has_value());
    const auto parsed = parse(encoded.value());
    ASSERT_TRUE(parsed.has_value());
    auto value = parsed.value().get(FieldId::PinBlock);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), pin_block);
}

TEST(Iso8583CodecTest, SecondaryBitmapField90RoundTrips) {
    const std::string original_data(42, '1');
    Message original{Mti::AuthorizationRequest};
    ASSERT_TRUE(original.set(FieldId::OriginalDataElements, original_data).has_value());
    const auto encoded = serialise(original);
    ASSERT_TRUE(encoded.has_value());
    ASSERT_GE(encoded.value().size(), 3U);
    EXPECT_EQ(encoded.value()[2] & std::byte{0x80}, std::byte{0x80});
    const auto parsed = parse(encoded.value());
    ASSERT_TRUE(parsed.has_value());
    auto value = parsed.value().get(FieldId::OriginalDataElements);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value.value(), original_data);
}

TEST(Iso8583CodecTest, AllEightMtisRoundTrip) {
    const std::array mtis{
        Mti::AuthorizationRequest,
        Mti::AuthorizationResponse,
        Mti::CaptureRequest,
        Mti::CaptureResponse,
        Mti::ReversalRequest,
        Mti::ReversalResponse,
        Mti::NetworkManagementRequest,
        Mti::NetworkManagementResponse,
    };
    for (const Mti mti : mtis) {
        const Message original{mti};
        const auto encoded = serialise(original);
        ASSERT_TRUE(encoded.has_value());
        const auto parsed = parse(encoded.value());
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(parsed.value().mti(), mti);
    }
}

TEST(Iso8583CodecTest, UnknownMtiIsRejected) {
    const std::array<std::byte, 10> bytes{
        std::byte{0x03},
        std::byte{0x00},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
    };
    const auto result = parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecError::UnknownMti);
}

TEST(Iso8583CodecTest, TrailingBytesAreRejected) {
    const Message original{Mti::AuthorizationRequest};
    const auto encoded = serialise(original);
    ASSERT_TRUE(encoded.has_value());
    auto bytes = encoded.value();
    bytes.push_back(std::byte{0xFF});
    const auto result = parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecError::TrailingBytes);
}

TEST(Iso8583CodecTest, UnsupportedBitmapBitIsRejected) {
    const std::array<std::byte, 10> bytes{
        std::byte{0x01},
        std::byte{0x00},
        std::byte{0x08},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
    };
    const auto result = parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecError::UnsupportedField);
}

std::string sample_field_value(const FieldSpec& spec) {
    if (spec.encoding == Encoding::Binary) {
        return std::string(spec.max_length, '\xA5');
    }
    if (spec.encoding == Encoding::Ascii) {
        return std::string(spec.max_length, 'A');
    }
    if (spec.length_kind == LengthKind::Llvar) {
        return std::string(spec.min_length, '4');
    }
    return std::string(spec.max_length, '1');
}

TEST(Iso8583CodecTest, AllInScopeFieldsRoundTrip) {
    Message original{Mti::AuthorizationRequest};
    for (const FieldSpec& spec : kFieldTable) {
        ASSERT_TRUE(original.set(spec.id, sample_field_value(spec)).has_value());
    }
    const auto encoded = serialise(original);
    ASSERT_TRUE(encoded.has_value());
    const auto parsed = parse(encoded.value());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().mti(), Mti::AuthorizationRequest);
    for (const FieldSpec& spec : kFieldTable) {
        auto value = parsed.value().get(spec.id);
        ASSERT_TRUE(value.has_value()) << "missing field " << static_cast<int>(spec.id);
        EXPECT_EQ(value.value(), sample_field_value(spec)) << "field " << static_cast<int>(spec.id);
    }
}

TEST(Iso8583CodecTest, ShortFixedBcdValueIsLeftPadded) {
    Message original{Mti::AuthorizationRequest};
    ASSERT_TRUE(original.set(FieldId::Amount, "100").has_value());
    const auto encoded = serialise(original);
    ASSERT_TRUE(encoded.has_value());
    const auto parsed = parse(encoded.value());
    ASSERT_TRUE(parsed.has_value());
    auto amount = parsed.value().get(FieldId::Amount);
    ASSERT_TRUE(amount.has_value());
    EXPECT_EQ(amount.value(), "000000000100");
}

TEST(Iso8583CodecTest, OverlongAsciiIsRejected) {
    Message original{Mti::AuthorizationRequest};
    ASSERT_TRUE(original.set(FieldId::ResponseCode, "000").has_value());
    const auto encoded = serialise(original);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error(), CodecError::InvalidLength);
}

TEST(Iso8583CodecTest, OverlongBinaryIsRejected) {
    const std::string pin_block(9, '\x01');
    Message original{Mti::AuthorizationRequest};
    ASSERT_TRUE(original.set(FieldId::PinBlock, pin_block).has_value());
    const auto encoded = serialise(original);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error(), CodecError::InvalidLength);
}

TEST(Iso8583CodecTest, LlvarPanTooShortIsRejected) {
    const std::array<std::byte, 17> bytes{
        std::byte{0x01},
        std::byte{0x00},
        std::byte{0x40},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0x12},
        std::byte{0x12},
        std::byte{0x34},
        std::byte{0x56},
        std::byte{0x78},
        std::byte{0x90},
        std::byte{0x12},
    };
    const auto result = parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecError::InvalidLength);
}

TEST(Iso8583CodecTest, LlvarPanTooLongIsRejected) {
    const std::array<std::byte, 21> bytes{
        std::byte{0x01},
        std::byte{0x00},
        std::byte{0x40},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0},
        std::byte{0x20},
        std::byte{0x12},
        std::byte{0x34},
        std::byte{0x56},
        std::byte{0x78},
        std::byte{0x90},
        std::byte{0x12},
        std::byte{0x34},
        std::byte{0x56},
        std::byte{0x78},
        std::byte{0x90},
    };
    const auto result = parse(bytes);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), CodecError::InvalidLength);
}

TEST(Iso8583CodecTest, NonDigitBcdIsRejected) {
    Message original{Mti::AuthorizationRequest};
    ASSERT_TRUE(original.set(FieldId::Amount, "12AB45").has_value());
    const auto encoded = serialise(original);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error(), CodecError::Malformed);
}

} // namespace
} // namespace aegis::iso8583
