#include <gtest/gtest.h>

#include <aegis/engine/engine.hpp>
#include <aegis/iso8583/codec.hpp>
#include <aegis/iso8583/message.hpp>
#include <aegis/net/framing.hpp>
#include <aegis/net/socket.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

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

} // namespace
} // namespace aegis::engine
