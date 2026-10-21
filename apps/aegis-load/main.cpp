#include <aegis/iso8583/codec.hpp>
#include <aegis/iso8583/message.hpp>
#include <aegis/net/framing.hpp>
#include <aegis/net/socket.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8583};
    std::size_t count{1};
    bool count_explicit{false};
    std::size_t terminals{1};
    std::size_t rate_per_s{0};
    std::size_t duration_s{0};
    bool duration_set{false};
};

void print_usage() {
    std::cout << "usage: aegis-load [--host <127.0.0.1|localhost>] [--port <0-65535>]\n"
                 "                  [--count <n>]\n"
                 "                  [--terminals <n>] [--rate <req/s>] [--duration <seconds>]\n";
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

[[nodiscard]] bool parse_size(std::string_view text, std::size_t& out) {
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

[[nodiscard]] std::string terminal_id(std::size_t terminal_index) {
    char buffer[9] = {};
    std::snprintf(buffer, sizeof(buffer), "TERM%04zu", terminal_index);
    return std::string{buffer};
}

[[nodiscard]] aegis::iso8583::Message make_request(std::size_t stan,
                                                     std::string_view terminal_id) {
    using aegis::iso8583::FieldId;
    using aegis::iso8583::Message;
    using aegis::iso8583::Mti;

    Message request{Mti::AuthorizationRequest};
    (void)request.set(FieldId::Pan, "4242424242424242");
    (void)request.set(FieldId::Amount, "000000001000");
    (void)request.set(FieldId::TransmissionDateTime, "1002153045");

    char stan_text[7] = {};
    std::snprintf(stan_text, sizeof(stan_text), "%06zu", stan % 1000000U);
    (void)request.set(FieldId::Stan, std::string{stan_text});

    (void)request.set(FieldId::TerminalId, std::string{terminal_id});
    (void)request.set(FieldId::MerchantId, std::string{"m-1"} + std::string(12, ' '));
    (void)request.set(FieldId::Currency, "840");
    return request;
}

[[nodiscard]] bool send_one(std::uint16_t port, std::size_t stan, std::string_view terminal_id) {
    aegis::net::Connection connection;
    if (!connection.connect_loopback(port).has_value()) {
        return false;
    }

    const auto payload = aegis::iso8583::serialise(make_request(stan, terminal_id));
    if (!payload.has_value() ||
        !aegis::net::write_frame(connection, payload.value()).has_value()) {
        return false;
    }

    connection.set_receive_timeout(std::chrono::seconds{5});
    const auto frame = aegis::net::read_frame(connection);
    if (!frame.has_value()) {
        return false;
    }

    const auto parsed = aegis::iso8583::parse(frame.value());
    if (!parsed.has_value()) {
        return false;
    }

    const auto code = parsed.value().get(aegis::iso8583::FieldId::ResponseCode);
    if (!code.has_value()) {
        return false;
    }

    std::cout << "0110 39=" << code.value() << "\n";
    return true;
}

void paced_run(const Options& options) {
    const auto interval =
        std::chrono::duration<double>{1.0 / static_cast<double>(options.rate_per_s)};
    const auto end_at = std::chrono::steady_clock::now() +
                        std::chrono::seconds{options.duration_s};

    std::atomic<std::size_t> next_sequence{0};
    std::atomic<std::size_t> successes{0};
    std::atomic<std::size_t> attempts{0};
    const auto start = std::chrono::steady_clock::now();

    std::vector<std::jthread> workers;
    workers.reserve(options.terminals);
    for (std::size_t terminal = 1; terminal <= options.terminals; ++terminal) {
        workers.emplace_back([&, terminal_id = terminal_id(terminal)](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (std::chrono::steady_clock::now() >= end_at) {
                    return;
                }

                const std::size_t sequence = next_sequence.fetch_add(1);
                const auto scheduled = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                   interval * static_cast<double>(sequence));
                if (scheduled >= end_at) {
                    return;
                }
                std::this_thread::sleep_until(scheduled);

                ++attempts;
                if (send_one(options.port, sequence + 1U, terminal_id)) {
                    successes.fetch_add(1);
                }
            }
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    std::cout << "aegis-load: " << successes.load() << "/" << attempts.load()
              << " response(s) received" << std::endl;
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
            options.count_explicit = true;
            continue;
        }
        if (arg == "--terminals") {
            if (!parse_size(next(arg), options.terminals)) {
                std::cerr << "aegis-load: invalid terminal count\n";
                return 2;
            }
            continue;
        }
        if (arg == "--rate") {
            if (!parse_size(next(arg), options.rate_per_s)) {
                std::cerr << "aegis-load: invalid rate\n";
                return 2;
            }
            continue;
        }
        if (arg == "--duration") {
            if (!parse_size(next(arg), options.duration_s)) {
                std::cerr << "aegis-load: invalid duration\n";
                return 2;
            }
            options.duration_set = true;
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

    if (options.duration_set && options.rate_per_s == 0) {
        std::cerr << "aegis-load: --duration requires --rate\n";
        return 2;
    }
    if (options.duration_set && options.count_explicit) {
        std::cerr << "aegis-load: --duration cannot be used with --count\n";
        return 2;
    }
    if (options.rate_per_s != 0 && !options.duration_set) {
        std::cerr << "aegis-load: --rate requires --duration\n";
        return 2;
    }

    if (options.duration_set) {
        paced_run(options);
        return 0; // paced mode reports successes in stdout
    }

    std::size_t failures = 0;
    for (std::size_t i = 0; i < options.count; ++i) {
        if (!send_one(options.port, i + 1U, terminal_id(1))) {
            std::cerr << "aegis-load: connection " << (i + 1U) << " failed\n";
            ++failures;
        }
    }

    std::cout << "aegis-load: " << (options.count - failures) << "/" << options.count
              << " response(s) received" << std::endl;

    return failures == 0 ? 0 : 1;
}
