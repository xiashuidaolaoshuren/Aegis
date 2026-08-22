#pragma once

#include <aegis/iso8583/message.hpp>
#include <aegis/result.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace aegis::iso8583 {

enum class CodecError {
    Truncated,
    TrailingBytes,
    UnknownMti,
    UnsupportedField,
    InvalidLength,
    Malformed,
};

namespace detail {

constexpr std::size_t kMtiSize = 2;
constexpr std::size_t kPrimaryBitmapSize = 8;
constexpr std::size_t kSecondaryBitmapSize = 8;
constexpr std::size_t kHeaderSize = kMtiSize + kPrimaryBitmapSize;
constexpr std::uint8_t kPrimaryFieldMax = 64;
constexpr std::uint8_t kFieldMax = 128;

[[nodiscard]] inline bool bitmap_bit(std::span<const std::byte> bitmap, std::uint8_t field) {
    const unsigned index = static_cast<unsigned>(field) - 1U;
    const auto mask = std::byte{static_cast<std::uint8_t>(0x80U >> (index % 8U))};
    return (bitmap[index / 8U] & mask) != std::byte{0};
}

inline void set_bitmap_bit(std::span<std::byte> bitmap, std::uint8_t field) {
    const unsigned index = static_cast<unsigned>(field) - 1U;
    const auto mask = std::byte{static_cast<std::uint8_t>(0x80U >> (index % 8U))};
    bitmap[index / 8U] |= mask;
}

[[nodiscard]] inline std::size_t packed_bcd_size(std::uint8_t digit_count) {
    return (static_cast<std::size_t>(digit_count) + 1U) / 2U;
}

[[nodiscard]] inline bool is_decimal_string(const std::string& digits) {
    for (const char c : digits) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool packed_bcd_nibbles_valid(std::span<const std::byte> packed) {
    for (const std::byte b : packed) {
        const auto value = static_cast<std::uint8_t>(b);
        if ((value >> 4) > 9U || (value & 0x0FU) > 9U) {
            return false;
        }
    }
    return true;
}

inline void append_packed_bcd(std::vector<std::byte>& out, std::string digits, std::uint8_t digit_count) {
    if (digits.size() < digit_count) {
        digits.insert(digits.begin(), digit_count - digits.size(), '0');
    }
    if (digit_count % 2U != 0U) {
        digits.insert(digits.begin(), '0');
    }
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        const auto hi = static_cast<std::uint8_t>(digits[i] - '0');
        const auto lo = static_cast<std::uint8_t>(digits[i + 1] - '0');
        out.push_back(std::byte{static_cast<std::uint8_t>((hi << 4) | lo)});
    }
}

[[nodiscard]] inline std::string unpack_packed_bcd(std::span<const std::byte> packed, std::uint8_t digit_count) {
    std::string digits;
    digits.reserve(packed.size() * 2);
    for (const std::byte b : packed) {
        const auto value = static_cast<std::uint8_t>(b);
        digits.push_back(static_cast<char>('0' + ((value >> 4) & 0x0F)));
        digits.push_back(static_cast<char>('0' + (value & 0x0F)));
    }
    if (digit_count % 2U != 0U && !digits.empty()) {
        digits.erase(digits.begin());
    }
    digits.resize(digit_count, '0');
    return digits;
}

[[nodiscard]] inline Result<Mti, CodecError> mti_from_packed(std::uint16_t value) {
    switch (static_cast<Mti>(value)) {
    case Mti::AuthorizationRequest:
    case Mti::AuthorizationResponse:
    case Mti::CaptureRequest:
    case Mti::CaptureResponse:
    case Mti::ReversalRequest:
    case Mti::ReversalResponse:
    case Mti::NetworkManagementRequest:
    case Mti::NetworkManagementResponse:
        return Result<Mti, CodecError>::ok(static_cast<Mti>(value));
    default:
        return Result<Mti, CodecError>::err(CodecError::UnknownMti);
    }
}

} // namespace detail

[[nodiscard]] inline Result<Message, CodecError> parse(std::span<const std::byte> bytes) {
    if (bytes.size() < detail::kHeaderSize) {
        return Result<Message, CodecError>::err(CodecError::Truncated);
    }
    const auto mti_value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[0]) << 8) | static_cast<std::uint16_t>(bytes[1]));
    const auto mti = detail::mti_from_packed(mti_value);
    if (!mti.has_value()) {
        return Result<Message, CodecError>::err(mti.error());
    }
    Message message{mti.value()};
    std::array<std::byte, detail::kPrimaryBitmapSize + detail::kSecondaryBitmapSize> bitmap{};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(detail::kMtiSize), detail::kPrimaryBitmapSize, bitmap.begin());
    std::size_t offset = detail::kHeaderSize;
    if (detail::bitmap_bit(bitmap, 1)) {
        if (bytes.size() - offset < detail::kSecondaryBitmapSize) {
            return Result<Message, CodecError>::err(CodecError::Truncated);
        }
        std::copy_n(
            bytes.begin() + static_cast<std::ptrdiff_t>(offset),
            detail::kSecondaryBitmapSize,
            bitmap.begin() + static_cast<std::ptrdiff_t>(detail::kPrimaryBitmapSize));
        offset += detail::kSecondaryBitmapSize;
    }
    for (int field = 2; field <= static_cast<int>(detail::kFieldMax); ++field) {
        if (!detail::bitmap_bit(bitmap, static_cast<std::uint8_t>(field))) {
            continue;
        }
        const FieldSpec* spec = find_field(static_cast<FieldId>(field));
        if (spec == nullptr) {
            return Result<Message, CodecError>::err(CodecError::UnsupportedField);
        }
        if (spec->encoding == Encoding::Ascii) {
            if (bytes.size() - offset < spec->max_length) {
                return Result<Message, CodecError>::err(CodecError::Truncated);
            }
            std::string value(spec->max_length, '\0');
            for (std::uint8_t i = 0; i < spec->max_length; ++i) {
                value[i] = static_cast<char>(bytes[offset + i]);
            }
            (void)message.set(spec->id, std::move(value));
            offset += spec->max_length;
            continue;
        }
        if (spec->encoding == Encoding::Bcd && spec->length_kind == LengthKind::Fixed) {
            const std::size_t packed_size = detail::packed_bcd_size(spec->max_length);
            if (bytes.size() - offset < packed_size) {
                return Result<Message, CodecError>::err(CodecError::Truncated);
            }
            const auto packed = bytes.subspan(offset, packed_size);
            if (!detail::packed_bcd_nibbles_valid(packed)) {
                return Result<Message, CodecError>::err(CodecError::Malformed);
            }
            (void)message.set(
                spec->id, detail::unpack_packed_bcd(packed, spec->max_length));
            offset += packed_size;
            continue;
        }
        if (spec->encoding == Encoding::Bcd && spec->length_kind == LengthKind::Llvar) {
            if (bytes.size() - offset < 1) {
                return Result<Message, CodecError>::err(CodecError::Truncated);
            }
            const auto length_byte = static_cast<std::uint8_t>(bytes[offset]);
            const auto hi = static_cast<std::uint8_t>(length_byte >> 4);
            const auto lo = static_cast<std::uint8_t>(length_byte & 0x0F);
            if (hi > 9U || lo > 9U) {
                return Result<Message, CodecError>::err(CodecError::Malformed);
            }
            const auto digit_count = static_cast<std::uint8_t>(hi * 10U + lo);
            if (digit_count < spec->min_length || digit_count > spec->max_length) {
                return Result<Message, CodecError>::err(CodecError::InvalidLength);
            }
            ++offset;
            const std::size_t packed_size = detail::packed_bcd_size(digit_count);
            if (bytes.size() - offset < packed_size) {
                return Result<Message, CodecError>::err(CodecError::Truncated);
            }
            const auto packed = bytes.subspan(offset, packed_size);
            if (!detail::packed_bcd_nibbles_valid(packed)) {
                return Result<Message, CodecError>::err(CodecError::Malformed);
            }
            (void)message.set(spec->id, detail::unpack_packed_bcd(packed, digit_count));
            offset += packed_size;
        }
        if (spec->encoding == Encoding::Binary) {
            if (bytes.size() - offset < spec->max_length) {
                return Result<Message, CodecError>::err(CodecError::Truncated);
            }
            std::string value(spec->max_length, '\0');
            for (std::uint8_t i = 0; i < spec->max_length; ++i) {
                value[i] = static_cast<char>(bytes[offset + i]);
            }
            (void)message.set(spec->id, std::move(value));
            offset += spec->max_length;
        }
    }
    if (offset != bytes.size()) {
        return Result<Message, CodecError>::err(CodecError::TrailingBytes);
    }
    return Result<Message, CodecError>::ok(std::move(message));
}

[[nodiscard]] inline Result<std::vector<std::byte>, CodecError> serialise(const Message& message) {
    std::vector<std::byte> out(detail::kHeaderSize);
    const auto mti = static_cast<std::uint16_t>(message.mti());
    out[0] = static_cast<std::byte>(static_cast<std::uint8_t>((mti >> 8) & 0xFF));
    out[1] = static_cast<std::byte>(static_cast<std::uint8_t>(mti & 0xFF));
    std::array<std::byte, detail::kPrimaryBitmapSize + detail::kSecondaryBitmapSize> bitmap{};
    std::vector<std::byte> field_bytes;
    bool needs_secondary = false;
    for (const FieldSpec& spec : kFieldTable) {
        if (message.has(spec.id) && static_cast<std::uint8_t>(spec.id) > detail::kPrimaryFieldMax) {
            needs_secondary = true;
            break;
        }
    }
    if (needs_secondary) {
        detail::set_bitmap_bit(bitmap, 1);
    }
    for (const FieldSpec& spec : kFieldTable) {
        const auto field_id = static_cast<std::uint8_t>(spec.id);
        if (!message.has(spec.id)) {
            continue;
        }
        auto field = message.get(spec.id);
        std::string value = field.value();
        if (spec.encoding == Encoding::Ascii) {
            if (value.size() > spec.max_length) {
                return Result<std::vector<std::byte>, CodecError>::err(CodecError::InvalidLength);
            }
            detail::set_bitmap_bit(bitmap, field_id);
            value.resize(spec.max_length, ' ');
            for (std::uint8_t i = 0; i < spec.max_length; ++i) {
                field_bytes.push_back(static_cast<std::byte>(value[i]));
            }
            continue;
        }
        if (spec.encoding == Encoding::Bcd && spec.length_kind == LengthKind::Fixed) {
            if (value.size() > spec.max_length) {
                return Result<std::vector<std::byte>, CodecError>::err(CodecError::InvalidLength);
            }
            if (!detail::is_decimal_string(value)) {
                return Result<std::vector<std::byte>, CodecError>::err(CodecError::Malformed);
            }
            detail::set_bitmap_bit(bitmap, field_id);
            detail::append_packed_bcd(field_bytes, std::move(value), spec.max_length);
            continue;
        }
        if (spec.encoding == Encoding::Bcd && spec.length_kind == LengthKind::Llvar) {
            if (value.size() < spec.min_length || value.size() > spec.max_length) {
                return Result<std::vector<std::byte>, CodecError>::err(CodecError::InvalidLength);
            }
            if (!detail::is_decimal_string(value)) {
                return Result<std::vector<std::byte>, CodecError>::err(CodecError::Malformed);
            }
            detail::set_bitmap_bit(bitmap, field_id);
            const auto digit_count = static_cast<std::uint8_t>(value.size());
            const auto hi = static_cast<std::uint8_t>(digit_count / 10);
            const auto lo = static_cast<std::uint8_t>(digit_count % 10);
            field_bytes.push_back(std::byte{static_cast<std::uint8_t>((hi << 4) | lo)});
            detail::append_packed_bcd(field_bytes, std::move(value), digit_count);
            continue;
        }
        if (spec.encoding == Encoding::Binary) {
            if (value.size() != spec.max_length) {
                return Result<std::vector<std::byte>, CodecError>::err(CodecError::InvalidLength);
            }
            detail::set_bitmap_bit(bitmap, field_id);
            for (std::uint8_t i = 0; i < spec.max_length; ++i) {
                field_bytes.push_back(static_cast<std::byte>(value[i]));
            }
        }
    }
    const std::size_t bitmap_size =
        needs_secondary ? (detail::kPrimaryBitmapSize + detail::kSecondaryBitmapSize) : detail::kPrimaryBitmapSize;
    out.resize(detail::kMtiSize + bitmap_size);
    std::copy(
        bitmap.begin(),
        bitmap.begin() + static_cast<std::ptrdiff_t>(bitmap_size),
        out.begin() + static_cast<std::ptrdiff_t>(detail::kMtiSize));
    out.insert(out.end(), field_bytes.begin(), field_bytes.end());
    return Result<std::vector<std::byte>, CodecError>::ok(std::move(out));
}

} // namespace aegis::iso8583
