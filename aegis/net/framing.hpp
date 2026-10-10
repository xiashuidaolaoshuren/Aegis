#pragma once

#include <aegis/net/socket.hpp>
#include <aegis/result.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aegis::net {

enum class FrameError {
    Truncated,
    Io,
    TooLarge,
    InvalidLength,
};

namespace detail {

[[nodiscard]] inline Result<void, FrameError> map_socket_error(SocketError error) {
    if (error == SocketError::Closed) {
        return Result<void, FrameError>::err(FrameError::Truncated);
    }
    return Result<void, FrameError>::err(FrameError::Io);
}

[[nodiscard]] inline std::uint16_t decode_length_prefix(std::span<const std::byte, 2> prefix) {
    const auto hi = static_cast<std::uint16_t>(prefix[0]);
    const auto lo = static_cast<std::uint16_t>(prefix[1]);
    return static_cast<std::uint16_t>((hi << 8) | lo);
}

[[nodiscard]] inline std::array<std::byte, 2> encode_length_prefix(std::size_t length) {
    const auto value = static_cast<std::uint16_t>(length);
    return {std::byte{static_cast<std::uint8_t>((value >> 8) & 0xFFU)},
            std::byte{static_cast<std::uint8_t>(value & 0xFFU)}};
}

[[nodiscard]] inline Result<void, FrameError> read_exact(Connection& connection, std::span<std::byte> buffer) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        const auto read = connection.read_some(buffer.subspan(offset));
        if (!read.has_value()) {
            if (read.error() == SocketError::Closed) {
                return Result<void, FrameError>::err(FrameError::Truncated);
            }
            return Result<void, FrameError>::err(FrameError::Io);
        }
        if (read.value() == 0) {
            return Result<void, FrameError>::err(FrameError::Truncated);
        }
        offset += read.value();
    }
    return Result<void, FrameError>::ok();
}

} // namespace detail

[[nodiscard]] inline Result<void, FrameError> write_frame(Connection& connection,
                                                          std::span<const std::byte> payload) {
    if (payload.size() > 65535U) {
        return Result<void, FrameError>::err(FrameError::TooLarge);
    }

    const auto prefix = detail::encode_length_prefix(payload.size());
    std::array<std::byte, 2> header = prefix;
    if (const auto wrote = connection.write_all(header); !wrote.has_value()) {
        return detail::map_socket_error(wrote.error());
    }
    if (const auto wrote = connection.write_all(payload); !wrote.has_value()) {
        return detail::map_socket_error(wrote.error());
    }
    return Result<void, FrameError>::ok();
}

[[nodiscard]] inline Result<std::vector<std::byte>, FrameError> read_frame(Connection& connection) {
    std::array<std::byte, 2> prefix{};
    if (const auto header = detail::read_exact(connection, prefix);
        !header.has_value()) {
        return Result<std::vector<std::byte>, FrameError>::err(header.error());
    }

    const std::uint16_t length = detail::decode_length_prefix(prefix);
    if (length == 0) {
        return Result<std::vector<std::byte>, FrameError>::ok(std::vector<std::byte>{});
    }

    std::vector<std::byte> body(length);
    if (const auto payload = detail::read_exact(connection, body);
        !payload.has_value()) {
        return Result<std::vector<std::byte>, FrameError>::err(payload.error());
    }
    return Result<std::vector<std::byte>, FrameError>::ok(std::move(body));
}

} // namespace aegis::net
