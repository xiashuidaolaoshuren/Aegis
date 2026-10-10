#include <aegis/engine/engine.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

std::atomic<bool> g_shutdown_requested{false};

void handle_signal(int) {
    g_shutdown_requested.store(true);
}

struct Options {
    std::uint16_t port{8583};
    std::filesystem::path genesis{"genesis.csv"};
    std::size_t io_threads{1};
    std::size_t worker_count{4};
    std::size_t inbound_capacity{1024};
};

void print_usage() {
    std::cout << "usage: aegisd [--port <0-65535>] [--genesis <path>]\n"
                 "              [--io-threads <n>] [--workers <n>]\n"
                 "              [--inbound-capacity <n>]\n";
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

} // namespace

int main(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto next = [&](std::string_view flag) -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << "aegisd: missing value for " << flag << "\n";
                std::exit(2);
            }
            return std::string_view{argv[++i]};
        };

        if (arg == "--help" || arg == "-h") {
            print_usage();
            return 0;
        }
        if (arg == "--port") {
            if (!parse_port(next(arg), options.port)) {
                std::cerr << "aegisd: invalid port\n";
                return 2;
            }
            continue;
        }
        if (arg == "--genesis") {
            options.genesis = std::filesystem::path{std::string{next(arg)}};
            continue;
        }
        if (arg == "--io-threads") {
            if (!parse_size(next(arg), options.io_threads)) {
                std::cerr << "aegisd: invalid io thread count\n";
                return 2;
            }
            continue;
        }
        if (arg == "--workers") {
            if (!parse_size(next(arg), options.worker_count)) {
                std::cerr << "aegisd: invalid worker count\n";
                return 2;
            }
            continue;
        }
        if (arg == "--inbound-capacity") {
            if (!parse_size(next(arg), options.inbound_capacity)) {
                std::cerr << "aegisd: invalid inbound capacity\n";
                return 2;
            }
            continue;
        }

        std::cerr << "aegisd: unknown argument '" << arg << "'\n";
        print_usage();
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    aegis::engine::EngineConfig config{};
    config.genesis_path = options.genesis;
    config.listen_port = options.port;
    config.io_threads = options.io_threads;
    config.worker_count = options.worker_count;
    config.inbound_capacity = options.inbound_capacity;

    aegis::engine::Engine engine;
    if (!engine.start(config).has_value()) {
        std::cerr << "aegisd: engine failed to start with genesis '" << options.genesis.string()
                  << "'\n";
        return 1;
    }

    std::cout << "aegisd listening on 127.0.0.1:" << engine.port() << std::endl;

    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    engine.stop();
    std::cout << "aegisd stopped" << std::endl;
    return 0;
}
