#pragma once

#include <aegis/net/socket.hpp>
#include <aegis/result.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace aegis::net {

enum class FrameError {
    Truncated,
    Io,
    TooLarge,
    InvalidLength,
    Timeout,
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

[[nodiscard]] inline Result<void, FrameError> read_exact(
    Connection& connection,
    std::span<std::byte> buffer,
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
        if (deadline.has_value()) {
            const auto left = deadline.value() - std::chrono::steady_clock::now();
            if (left <= std::chrono::steady_clock::duration::zero()) {
                return Result<void, FrameError>::err(FrameError::Timeout);
            }
            // Bound each read by what is left of the whole-frame budget, not a fresh timeout.
            const auto budget = std::max(
                std::chrono::duration_cast<std::chrono::milliseconds>(left),
                std::chrono::milliseconds{1});
            connection.set_receive_timeout(budget);
        }

        const auto read = connection.read_some(buffer.subspan(offset));
        if (!read.has_value()) {
            if (read.error() == SocketError::Closed) {
                return Result<void, FrameError>::err(FrameError::Truncated);
            }
            if (read.error() == SocketError::Timeout && deadline.has_value()) {
                if (std::chrono::steady_clock::now() < deadline.value()) {
                    continue; // a short read timeout; the budget still has room
                }
                return Result<void, FrameError>::err(FrameError::Timeout);
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

[[nodiscard]] inline Result<std::vector<std::byte>, FrameError> read_frame_with_deadline(
    Connection& connection,
    std::optional<std::chrono::steady_clock::time_point> deadline) {
    std::array<std::byte, 2> prefix{};
    if (const auto header = read_exact(connection, prefix, deadline); !header.has_value()) {
        return Result<std::vector<std::byte>, FrameError>::err(header.error());
    }

    const std::uint16_t length = decode_length_prefix(prefix);
    if (length == 0) {
        return Result<std::vector<std::byte>, FrameError>::ok(std::vector<std::byte>{});
    }

    std::vector<std::byte> body(length);
    if (const auto payload = read_exact(connection, body, deadline); !payload.has_value()) {
        return Result<std::vector<std::byte>, FrameError>::err(payload.error());
    }
    return Result<std::vector<std::byte>, FrameError>::ok(std::move(body));
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
    return detail::read_frame_with_deadline(connection, std::nullopt);
}

// Enforces one deadline across the length prefix and the body, so a peer that trickles bytes
// faster than the socket receive timeout still cannot hold the reading thread indefinitely.
[[nodiscard]] inline Result<std::vector<std::byte>, FrameError> read_frame(
    Connection& connection,
    std::chrono::milliseconds total_timeout) {
    return detail::read_frame_with_deadline(
        connection, std::chrono::steady_clock::now() + total_timeout);
}

} // namespace aegis::net
