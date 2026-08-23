#pragma once

#include <aegis/result.hpp>

#include <array>
#include <cstdint>

namespace aegis::iso8583 {

enum class FieldId : std::uint8_t {
    Pan = 2,
    ProcessingCode = 3,
    Amount = 4,
    TransmissionDateTime = 7,
    Stan = 11,
    LocalTime = 12,
    LocalDate = 13,
    Rrn = 37,
    AuthorizationId = 38,
    ResponseCode = 39,
    TerminalId = 41,
    MerchantId = 42,
    Currency = 49,
    PinBlock = 52,
    OriginalDataElements = 90,
};

enum class Encoding { Bcd, Ascii, Binary };

enum class LengthKind { Fixed, Llvar };

struct FieldSpec {
    FieldId id;
    Encoding encoding;
    LengthKind length_kind;
    std::uint8_t min_length;
    std::uint8_t max_length;
};

enum class Mti : std::uint16_t {
    AuthorizationRequest = 0x0100,
    AuthorizationResponse = 0x0110,
    CaptureRequest = 0x0200,
    CaptureResponse = 0x0210,
    ReversalRequest = 0x0400,
    ReversalResponse = 0x0410,
    NetworkManagementRequest = 0x0800,
    NetworkManagementResponse = 0x0810,
};

enum class MessageError { FieldAbsent, UnknownField, InvalidMti };

inline constexpr std::array<FieldSpec, 15> kFieldTable{{
    {FieldId::Pan, Encoding::Bcd, LengthKind::Llvar, 13, 19},
    {FieldId::ProcessingCode, Encoding::Bcd, LengthKind::Fixed, 6, 6},
    {FieldId::Amount, Encoding::Bcd, LengthKind::Fixed, 12, 12},
    {FieldId::TransmissionDateTime, Encoding::Bcd, LengthKind::Fixed, 10, 10},
    {FieldId::Stan, Encoding::Bcd, LengthKind::Fixed, 6, 6},
    {FieldId::LocalTime, Encoding::Bcd, LengthKind::Fixed, 6, 6},
    {FieldId::LocalDate, Encoding::Bcd, LengthKind::Fixed, 4, 4},
    {FieldId::Rrn, Encoding::Ascii, LengthKind::Fixed, 12, 12},
    {FieldId::AuthorizationId, Encoding::Ascii, LengthKind::Fixed, 6, 6},
    {FieldId::ResponseCode, Encoding::Ascii, LengthKind::Fixed, 2, 2},
    {FieldId::TerminalId, Encoding::Ascii, LengthKind::Fixed, 8, 8},
    {FieldId::MerchantId, Encoding::Ascii, LengthKind::Fixed, 15, 15},
    {FieldId::Currency, Encoding::Bcd, LengthKind::Fixed, 3, 3},
    {FieldId::PinBlock, Encoding::Binary, LengthKind::Fixed, 8, 8},
    {FieldId::OriginalDataElements, Encoding::Bcd, LengthKind::Fixed, 42, 42},
}};

[[nodiscard]] inline constexpr const FieldSpec* find_field(FieldId id) {
    for (const FieldSpec& spec : kFieldTable) {
        if (spec.id == id) {
            return &spec;
        }
    }
    return nullptr;
}

[[nodiscard]] inline constexpr bool is_request(Mti mti) {
    switch (mti) {
    case Mti::AuthorizationRequest:
    case Mti::CaptureRequest:
    case Mti::ReversalRequest:
    case Mti::NetworkManagementRequest:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline constexpr bool is_response(Mti mti) {
    switch (mti) {
    case Mti::AuthorizationResponse:
    case Mti::CaptureResponse:
    case Mti::ReversalResponse:
    case Mti::NetworkManagementResponse:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] inline Result<Mti, MessageError> response_mti(Mti mti) {
    switch (mti) {
    case Mti::AuthorizationRequest:
        return Result<Mti, MessageError>::ok(Mti::AuthorizationResponse);
    case Mti::CaptureRequest:
        return Result<Mti, MessageError>::ok(Mti::CaptureResponse);
    case Mti::ReversalRequest:
        return Result<Mti, MessageError>::ok(Mti::ReversalResponse);
    case Mti::NetworkManagementRequest:
        return Result<Mti, MessageError>::ok(Mti::NetworkManagementResponse);
    default:
        return Result<Mti, MessageError>::err(MessageError::InvalidMti);
    }
}

} // namespace aegis::iso8583
