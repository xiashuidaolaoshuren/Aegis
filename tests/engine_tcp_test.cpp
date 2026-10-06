#include <gtest/gtest.h>

#include <aegis/engine/engine.hpp>
#include <aegis/iso8583/codec.hpp>
#include <aegis/iso8583/message.hpp>
#include <aegis/net/framing.hpp>
#include <aegis/net/socket.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aegis::engine {
namespace {

using iso8583::FieldId;
using iso8583::Message;
using iso8583::Mti;

[[nodiscard]] Message make_auth_request_for_stan(std::string_view stan) {
    Message request{Mti::AuthorizationRequest};
    (void)request.set(FieldId::Pan, "4242424242424242");
    (void)request.set(FieldId::Amount, "000000005000");
    (void)request.set(FieldId::TransmissionDateTime, "1002153045");
    (void)request.set(FieldId::Stan, std::string{stan});
    (void)request.set(FieldId::TerminalId, "TERM0001");
    (void)request.set(FieldId::MerchantId, "m-1          ");
    (void)request.set(FieldId::Currency, "840");
    return request;
}

[[nodiscard]] Message make_valid_auth_request() {
    return make_auth_request_for_stan("000001");
}

[[nodiscard]] EngineConfig make_default_config() {
    EngineConfig config{};
    config.genesis_path = std::filesystem::path{FIXTURES_DIR} / "genesis_tiny.csv";
    config.io_threads = 1;
    config.worker_count = 1;
    config.inbound_capacity = 4;
    return config;
}

TEST(EngineTcpTest, StartBindsLoopbackAndStopReturns) {
    EngineConfig config{};
    config.genesis_path = std::filesystem::path{FIXTURES_DIR} / "genesis_tiny.csv";
    config.io_threads = 1;
    config.worker_count = 1;
    config.inbound_capacity = 4;

    Engine engine;
    ASSERT_TRUE(engine.start(config).has_value());
    EXPECT_NE(engine.port(), 0U);
    engine.stop();
}

TEST(EngineTcpTest, LoopbackAuthorizationReturnsApprovedResponse) {
    Engine engine;
    ASSERT_TRUE(engine.start(make_default_config()).has_value());

    net::Connection client;
    ASSERT_TRUE(client.connect_loopback(engine.port()).has_value());

    const Message request = make_valid_auth_request();
    const auto payload = iso8583::serialise(request);
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(net::write_frame(client, payload.value()).has_value());

    client.set_receive_timeout(std::chrono::seconds{2});
    const auto frame = net::read_frame(client);
    ASSERT_TRUE(frame.has_value());

    const auto parsed = iso8583::parse(frame.value());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().mti(), Mti::AuthorizationResponse);
    const auto code = parsed.value().get(FieldId::ResponseCode);
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(code.value(), "00");

    engine.stop();
}

[[nodiscard]] std::string send_auth_and_read_code(net::Connection& client,
                                                  const Message& request) {
    const auto payload = iso8583::serialise(request);
    EXPECT_TRUE(payload.has_value());
    EXPECT_TRUE(net::write_frame(client, payload.value()).has_value());

    client.set_receive_timeout(std::chrono::seconds{2});
    const auto frame = net::read_frame(client);
    EXPECT_TRUE(frame.has_value());

    const auto parsed = iso8583::parse(frame.value());
    EXPECT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().mti(), Mti::AuthorizationResponse);
    const auto code = parsed.value().get(FieldId::ResponseCode);
    EXPECT_TRUE(code.has_value());
    return code.value();
}

TEST(EngineTcpTest, FullInboundQueueReturnsSystemMalfunction) {
    EngineConfig config = make_default_config();
    config.worker_count = 0;
    config.inbound_capacity = 1;

    Engine engine;
    ASSERT_TRUE(engine.start(config).has_value());

    const Message request = make_valid_auth_request();

    net::Connection filler;
    ASSERT_TRUE(filler.connect_loopback(engine.port()).has_value());
    const auto filler_payload = iso8583::serialise(request);
    ASSERT_TRUE(filler_payload.has_value());
    ASSERT_TRUE(net::write_frame(filler, filler_payload.value()).has_value());

    net::Connection second;
    ASSERT_TRUE(second.connect_loopback(engine.port()).has_value());
    EXPECT_EQ(send_auth_and_read_code(second, request), "96");

    net::Connection third;
    ASSERT_TRUE(third.connect_loopback(engine.port()).has_value());
    EXPECT_EQ(send_auth_and_read_code(third, request), "96");

    engine.stop();
}

[[nodiscard]] std::uint16_t free_loopback_port() {
    net::Listener probe;
    EXPECT_TRUE(probe.bind_loopback(0).has_value());
    return probe.port();
}

TEST(EngineTcpTest, ConfiguredListenPortBindsThatPort) {
    const std::uint16_t port = free_loopback_port();

    EngineConfig config = make_default_config();
    config.listen_port = port;

    Engine engine;
    ASSERT_TRUE(engine.start(config).has_value());
    EXPECT_EQ(engine.port(), port);

    net::Connection client;
    ASSERT_TRUE(client.connect_loopback(port).has_value());
    EXPECT_EQ(send_auth_and_read_code(client, make_auth_request_for_stan("000002")), "00");

    engine.stop();
}

TEST(EngineTcpTest, SnapshotCountIncrementsAfterApproval) {
    Engine engine;
    ASSERT_TRUE(engine.start(make_default_config()).has_value());
    EXPECT_EQ(engine.auth_counter().snapshot_count(), 0U);

    net::Connection client;
    ASSERT_TRUE(client.connect_loopback(engine.port()).has_value());
    EXPECT_EQ(send_auth_and_read_code(client, make_valid_auth_request()), "00");
    EXPECT_EQ(engine.auth_counter().snapshot_count(), 1U);

    engine.stop();
}

// Runs stop() on a watchdog thread and terminates the process if it does not return, so a wedged
// shutdown fails the test instead of hanging the whole test binary. It never returns while the
// stopping thread is still running, which keeps the jthread destructor from blocking on join.
void expect_stop_returns(Engine& engine, std::chrono::milliseconds cap) {
    std::atomic<bool> stopped{false};
    std::jthread stopper{[&engine, &stopped] {
        engine.stop();
        stopped.store(true);
    }};

    const auto deadline = std::chrono::steady_clock::now() + cap;
    while (!stopped.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            std::cout.flush();
            std::cerr << "engine.stop() did not return within " << cap.count()
                      << "ms; terminating the test process instead of deadlocking the suite"
                      << std::endl;
            // The engine still owns blocked threads, so ordinary unwinding would hang.
            std::_Exit(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

[[nodiscard]] EngineConfig make_snappy_timeout_config() {
    EngineConfig config = make_default_config();
    config.frame_read_timeout = std::chrono::milliseconds{200};
    config.response_write_timeout = std::chrono::milliseconds{200};
    return config;
}

// Sends one byte of the two-byte length prefix and then stays silent.
void send_partial_prefix(net::Connection& peer) {
    const std::array<std::byte, 1> partial_prefix{std::byte{0x00}};
    ASSERT_TRUE(peer.write_all(partial_prefix).has_value());
}

TEST(EngineTcpTest, StopReturnsWhileIdlePeerHoldsPartialFrame) {
    Engine engine;
    ASSERT_TRUE(engine.start(make_snappy_timeout_config()).has_value());

    net::Connection silent_peer;
    ASSERT_TRUE(silent_peer.connect_loopback(engine.port()).has_value());
    send_partial_prefix(silent_peer);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    expect_stop_returns(engine, std::chrono::seconds{3});
    silent_peer.close();
}

TEST(EngineTcpTest, SilentPeerDoesNotStallOtherClients) {
    Engine engine;
    ASSERT_TRUE(engine.start(make_snappy_timeout_config()).has_value());

    net::Connection silent_peer;
    ASSERT_TRUE(silent_peer.connect_loopback(engine.port()).has_value());
    send_partial_prefix(silent_peer);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    net::Connection client;
    ASSERT_TRUE(client.connect_loopback(engine.port()).has_value());
    EXPECT_EQ(send_auth_and_read_code(client, make_valid_auth_request()), "00");

    expect_stop_returns(engine, std::chrono::seconds{3});
    silent_peer.close();
}

TEST(EngineTcpTest, MalformedFrameReceivesFormatErrorResponse30) {
    Engine engine;
    ASSERT_TRUE(engine.start(make_default_config()).has_value());

    net::Connection client;
    ASSERT_TRUE(client.connect_loopback(engine.port()).has_value());

    // A well-formed frame whose body carries an unknown MTI, so the codec cannot parse it.
    const std::vector<std::byte> body{
        std::byte{0x99},
        std::byte{0x99},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
    };
    ASSERT_TRUE(net::write_frame(client, body).has_value());

    client.set_receive_timeout(std::chrono::seconds{2});
    const auto frame = net::read_frame(client);
    ASSERT_TRUE(frame.has_value());

    const auto parsed = iso8583::parse(frame.value());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed.value().mti(), Mti::AuthorizationResponse);
    const auto code = parsed.value().get(FieldId::ResponseCode);
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(code.value(), "30");

    engine.stop();
}

TEST(EngineTcpTest, SlowDripPeerDoesNotHoldIoThreadBeyondFrameDeadline) {
    EngineConfig config = make_default_config();
    config.frame_read_timeout = std::chrono::milliseconds{300};

    Engine engine;
    ASSERT_TRUE(engine.start(config).has_value());

    net::Connection dripper;
    ASSERT_TRUE(dripper.connect_loopback(engine.port()).has_value());

    std::atomic<bool> keep_dripping{true};
    std::jthread drip_thread{[&dripper, &keep_dripping] {
        // Faster than the receive timeout, so a per-read timer never fires, but sustained far
        // longer than the whole-frame deadline.
        while (keep_dripping.load()) {
            const std::array<std::byte, 1> byte{std::byte{0x30}};
            if (!dripper.write_all(byte).has_value()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    net::Connection client;
    ASSERT_TRUE(client.connect_loopback(engine.port()).has_value());
    const auto payload = iso8583::serialise(make_valid_auth_request());
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(net::write_frame(client, payload.value()).has_value());

    client.set_receive_timeout(std::chrono::seconds{2});
    const auto frame = net::read_frame(client);
    EXPECT_TRUE(frame.has_value()) << "slow-drip peer starved the I/O thread";
    if (frame.has_value()) {
        const auto parsed = iso8583::parse(frame.value());
        ASSERT_TRUE(parsed.has_value());
        const auto code = parsed.value().get(FieldId::ResponseCode);
        ASSERT_TRUE(code.has_value());
        EXPECT_EQ(code.value(), "00");
    }

    keep_dripping.store(false);
    drip_thread.join();
    dripper.close();
    expect_stop_returns(engine, std::chrono::seconds{3});
}

} // namespace
} // namespace aegis::engine
