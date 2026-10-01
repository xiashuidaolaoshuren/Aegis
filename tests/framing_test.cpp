#include <gtest/gtest.h>

#include <aegis/net/framing.hpp>
#include <aegis/net/socket.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace aegis::net {
namespace {

TEST(FramingTest, LoopbackAcceptExchangesRawPayloadAndDroppedPeerReadsAsEof) {
    Listener listener;
    ASSERT_TRUE(listener.bind_loopback(0).has_value());
    const std::uint16_t port = listener.port();

    Connection client;
    ASSERT_TRUE(client.connect_loopback(port).has_value());

    Connection server = listener.accept();
    ASSERT_TRUE(server.is_open());

    constexpr std::string_view payload = "ping";
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(payload.data()), payload.size());
    ASSERT_TRUE(server.write_all(bytes).has_value());

    std::array<std::byte, 4> buffer{};
    const auto read = client.read_some(std::span<std::byte>(buffer));
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value(), 4U);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(buffer.data()), 4), "ping");

    server.close();
    client.set_receive_timeout(std::chrono::milliseconds{500});

    std::array<std::byte, 8> drain{};
    const auto eof_read = client.read_some(std::span<std::byte>(drain));
    ASSERT_TRUE(eof_read.has_value());
    EXPECT_EQ(eof_read.value(), 0U);
}

TEST(FramingTest, NonEmptyFrameRoundTripsWithBigEndianBodyLength) {
    Listener listener;
    ASSERT_TRUE(listener.bind_loopback(0).has_value());
    const std::uint16_t port = listener.port();

    Connection client;
    ASSERT_TRUE(client.connect_loopback(port).has_value());
    Connection server = listener.accept();
    ASSERT_TRUE(server.is_open());

    const std::vector<std::byte> payload{
        std::byte{0x01}, std::byte{0x00}, std::byte{0x11}, std::byte{0x22}, std::byte{0x33}};

    ASSERT_TRUE(write_frame(server, payload).has_value());

    const auto read = read_frame(client);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read.value(), payload);
}

TEST(FramingTest, WriteFramePrefixIsBigEndianBodyLength) {
    Listener listener;
    ASSERT_TRUE(listener.bind_loopback(0).has_value());

    Connection client;
    ASSERT_TRUE(client.connect_loopback(listener.port()).has_value());
    Connection server = listener.accept();

    const std::vector<std::byte> payload{std::byte{0xAB}, std::byte{0xCD}, std::byte{0xEF}};
    ASSERT_TRUE(write_frame(server, payload).has_value());

    std::array<std::byte, 2> prefix{};
    const auto prefix_chunk = client.read_some(prefix);
    ASSERT_TRUE(prefix_chunk.has_value());
    EXPECT_EQ(prefix_chunk.value(), prefix.size());
    EXPECT_EQ(prefix[0], std::byte{0x00});
    EXPECT_EQ(prefix[1], std::byte{static_cast<unsigned char>(payload.size())});

    std::vector<std::byte> body(payload.size());
    const auto body_chunk = client.read_some(body);
    ASSERT_TRUE(body_chunk.has_value());
    EXPECT_EQ(body_chunk.value(), body.size());
    EXPECT_EQ(body, payload);
}

TEST(FramingTest, EmptyPayloadRoundTrips) {
    Listener listener;
    ASSERT_TRUE(listener.bind_loopback(0).has_value());

    Connection client;
    ASSERT_TRUE(client.connect_loopback(listener.port()).has_value());
    Connection server = listener.accept();

    const std::vector<std::byte> empty_payload;
    ASSERT_TRUE(write_frame(server, empty_payload).has_value());

    const auto read = read_frame(client);
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(read.value().empty());
}

TEST(FramingTest, PayloadOver65535ReturnsTooLargeAndWritesNothing) {
    Listener listener;
    ASSERT_TRUE(listener.bind_loopback(0).has_value());

    Connection client;
    ASSERT_TRUE(client.connect_loopback(listener.port()).has_value());
    Connection server = listener.accept();

    client.set_receive_timeout(std::chrono::milliseconds{100});

    std::vector<std::byte> oversized(65536U);
    const auto write = write_frame(server, oversized);
    ASSERT_FALSE(write.has_value());
    EXPECT_EQ(write.error(), FrameError::TooLarge);

    std::array<std::byte, 8> drain{};
    const auto peek = client.read_some(drain);
    ASSERT_FALSE(peek.has_value());
    EXPECT_EQ(peek.error(), SocketError::Timeout);
}

TEST(FramingTest, ShortBodyBeforePeerCloseReturnsTruncated) {
    Listener listener;
    ASSERT_TRUE(listener.bind_loopback(0).has_value());

    Connection client;
    ASSERT_TRUE(client.connect_loopback(listener.port()).has_value());
    Connection server = listener.accept();

    const std::array<std::byte, 2> header{std::byte{0x00}, std::byte{0x05}};
    const std::array<std::byte, 2> partial_body{std::byte{0xAA}, std::byte{0xBB}};
    ASSERT_TRUE(server.write_all(header).has_value());
    ASSERT_TRUE(server.write_all(partial_body).has_value());
    server.close();

    client.set_receive_timeout(std::chrono::milliseconds{500});

    const auto read = read_frame(client);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error(), FrameError::Truncated);
}

TEST(FramingTest, ShortHeaderBeforePeerCloseReturnsTruncated) {
    Listener listener;
    ASSERT_TRUE(listener.bind_loopback(0).has_value());

    Connection client;
    ASSERT_TRUE(client.connect_loopback(listener.port()).has_value());
    Connection server = listener.accept();

    const std::array<std::byte, 1> partial_header{std::byte{0x00}};
    ASSERT_TRUE(server.write_all(partial_header).has_value());
    server.close();

    client.set_receive_timeout(std::chrono::milliseconds{500});

    const auto read = read_frame(client);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error(), FrameError::Truncated);
}

} // namespace
} // namespace aegis::net
