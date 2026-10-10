#include <aegis/iso8583/codec.hpp>
#include <aegis/iso8583/message.hpp>
#include <aegis/net/framing.hpp>
#include <aegis/net/socket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct Options {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8583};
    std::size_t count{1};
};

void print_usage() {
    std::cout << "usage: aegis-load [--host <127.0.0.1|localhost>] [--port <0-65535>]\n"
                 "                  [--count <n>]\n";
}

[[nodiscard]] bool parse_port(std::string_view text, std::uint16_t& out) {
    try {
        const unsigned long value = std::stoul(std::string{text});
        if (value > 65535UL) {
            return false;
        }
        out = static_cast<std::uint16_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

[[nodiscard]] bool parse_count(std::string_view text, std::size_t& out) {
    try {
        std::size_t consumed = 0;
        const unsigned long long value = std::stoull(std::string{text}, &consumed);
        if (consumed != text.size() || value == 0ULL) {
            return false;
        }
        out = static_cast<std::size_t>(value);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

[[nodiscard]] aegis::iso8583::Message make_request(std::size_t index) {
    using aegis::iso8583::FieldId;
    using aegis::iso8583::Message;
    using aegis::iso8583::Mti;

    Message request{Mti::AuthorizationRequest};
    (void)request.set(FieldId::Pan, "4242424242424242");
    (void)request.set(FieldId::Amount, "000000001000");
    (void)request.set(FieldId::TransmissionDateTime, "1002153045");

    char stan[7] = {};
    std::snprintf(stan, sizeof(stan), "%06zu", (index + 1U) % 1000000U);
    (void)request.set(FieldId::Stan, std::string{stan});

    (void)request.set(FieldId::TerminalId, "TERM0001");
    (void)request.set(FieldId::MerchantId, std::string{"m-1"} + std::string(12, ' '));
    (void)request.set(FieldId::Currency, "840");
    return request;
}

} // namespace

int main(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto next = [&](std::string_view flag) -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << "aegis-load: missing value for " << flag << "\n";
                std::exit(2);
            }
            return std::string_view{argv[++i]};
        };

        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--host") {
            options.host = std::string{next(arg)};
            continue;
        }
        if (arg == "--port") {
            if (!parse_port(next(arg), options.port)) {
                std::cerr << "aegis-load: invalid port\n";
                return 2;
            }
            continue;
        }
        if (arg == "--count") {
            if (!parse_count(next(arg), options.count)) {
                std::cerr << "aegis-load: invalid connection count\n";
                return 2;
            }
            continue;
        }

        std::cerr << "aegis-load: unknown argument '" << arg << "'\n";
        print_usage();
        return 2;
    }

    if (options.host != "127.0.0.1" && options.host != "localhost") {
        std::cerr << "aegis-load: host must be 127.0.0.1 or localhost\n";
        return 2;
    }

    std::size_t failures = 0;
    for (std::size_t i = 0; i < options.count; ++i) {
        aegis::net::Connection connection;
        if (!connection.connect_loopback(options.port).has_value()) {
            std::cerr << "aegis-load: connection " << (i + 1U) << " failed to connect\n";
            ++failures;
            continue;
        }

        const auto payload = aegis::iso8583::serialise(make_request(i));
        if (!payload.has_value() ||
            !aegis::net::write_frame(connection, payload.value()).has_value()) {
            std::cerr << "aegis-load: connection " << (i + 1U) << " failed to send 0100\n";
            ++failures;
            continue;
        }

        connection.set_receive_timeout(std::chrono::seconds{5});
        const auto frame = aegis::net::read_frame(connection);
        if (!frame.has_value()) {
            std::cerr << "aegis-load: connection " << (i + 1U) << " received no 0110\n";
            ++failures;
            continue;
        }

        const auto parsed = aegis::iso8583::parse(frame.value());
        if (!parsed.has_value()) {
            std::cerr << "aegis-load: connection " << (i + 1U) << " received an unparsable frame\n";
            ++failures;
            continue;
        }

        const auto code = parsed.value().get(aegis::iso8583::FieldId::ResponseCode);
        if (!code.has_value()) {
            std::cerr << "aegis-load: connection " << (i + 1U) << " response missing field 39\n";
            ++failures;
            continue;
        }

        std::cout << "0110 39=" << code.value() << "\n";
    }

    std::cout << "aegis-load: " << (options.count - failures) << "/" << options.count
              << " response(s) received" << std::endl;

    return failures == 0 ? 0 : 1;
}
