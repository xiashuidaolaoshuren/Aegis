#include <aegis/engine/engine.hpp>
#include <aegis/metrics/histogram.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
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
    aegis::engine::LedgerStage ledger_stage{aegis::engine::LedgerStage::GlobalMutex};
    bool bench{false};
    std::filesystem::path bench_out;
    bool bench_out_set{false};
    std::size_t bench_duration_s{0};
    bool bench_duration_set{false};
    std::size_t bench_rate_per_s{0};
    std::size_t bench_duration_load_s{0};
    std::size_t bench_terminals{0};
    std::size_t bench_pans{0};
};

void print_usage() {
    std::cout << "usage: aegisd [--port <0-65535>] [--genesis <path>]\n"
                 "              [--io-threads <n>] [--workers <n>]\n"
                 "              [--inbound-capacity <n>] [--ledger-stage <1|2>]\n"
                 "              [--bench] [--bench-out <path>] [--bench-duration <seconds>]\n"
                 "              [--bench-rate <n>] [--bench-load-duration <s>] [--bench-terminals <n>]\n"
                 "              [--bench-pans <n>]\n";
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

struct BenchReport {
    std::size_t requests{0};
    double elapsed_ms{0.0};
    double throughput_per_s{0.0};
    std::int64_t p50_ns{0};
    std::int64_t p99_ns{0};
    std::int64_t p99_9_ns{0};
};

[[nodiscard]] std::optional<BenchReport> make_bench_report(
    const aegis::engine::Engine& engine,
    std::chrono::steady_clock::duration elapsed) {
    const auto& histogram = engine.latency_histogram();
    if (histogram.count() == 0U) {
        return std::nullopt;
    }

    const auto p50 = histogram.percentile(aegis::metrics::Percentile::P50);
    const auto p99 = histogram.percentile(aegis::metrics::Percentile::P99);
    const auto p999 = histogram.percentile(aegis::metrics::Percentile::P999);
    if (!p50.has_value() || !p99.has_value() || !p999.has_value()) {
        return std::nullopt;
    }

    BenchReport report{};
    report.requests = histogram.count();
    report.elapsed_ms =
        std::chrono::duration<double, std::milli>(elapsed).count();
    if (report.elapsed_ms > 0.0) {
        report.throughput_per_s =
            static_cast<double>(report.requests) * 1000.0 / report.elapsed_ms;
    }
    report.p50_ns = p50.value().count();
    report.p99_ns = p99.value().count();
    report.p99_9_ns = p999.value().count();
    return report;
}

void print_bench_report(const BenchReport& report) {
    std::cout << "bench requests=" << report.requests << '\n'
              << "bench elapsed_ms=" << std::fixed << std::setprecision(3) << report.elapsed_ms
              << '\n'
              << "bench throughput_per_s=" << std::fixed << std::setprecision(3)
              << report.throughput_per_s << '\n'
              << "bench p50_ns=" << report.p50_ns << '\n'
              << "bench p99_ns=" << report.p99_ns << '\n'
              << "bench p99_9_ns=" << report.p99_9_ns << '\n';
}

[[nodiscard]] bool write_bench_file(const std::filesystem::path& path,
                                    const Options& options,
                                    const BenchReport& report) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream out{path};
    if (!out) {
        return false;
    }

    out << "stage=" << static_cast<int>(options.ledger_stage) << '\n'
        << "genesis=" << options.genesis.string() << '\n'
        << "rate_per_s=" << options.bench_rate_per_s << '\n'
        << "duration_s=" << options.bench_duration_load_s << '\n'
        << "terminals=" << options.bench_terminals << '\n'
        << "pans=" << options.bench_pans << '\n'
        << "workers=" << options.worker_count << '\n'
        << "requests=" << report.requests << '\n'
        << "elapsed_ms=" << std::fixed << std::setprecision(3) << report.elapsed_ms << '\n'
        << "throughput_per_s=" << std::fixed << std::setprecision(3) << report.throughput_per_s
        << '\n'
        << "p50_ns=" << report.p50_ns << '\n'
        << "p99_ns=" << report.p99_ns << '\n'
        << "p99_9_ns=" << report.p99_9_ns << '\n';
    return static_cast<bool>(out);
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
        if (arg == "--ledger-stage") {
            const std::string_view value = next(arg);
            if (value == "1") {
                options.ledger_stage = aegis::engine::LedgerStage::GlobalMutex;
            } else if (value == "2") {
                options.ledger_stage = aegis::engine::LedgerStage::Striped;
            } else {
                std::cerr << "aegisd: invalid ledger stage (expected 1 or 2)\n";
                return 2;
            }
            continue;
        }
        if (arg == "--bench") {
            options.bench = true;
            continue;
        }
        if (arg == "--bench-out") {
            options.bench_out = std::filesystem::path{std::string{next(arg)}};
            options.bench_out_set = true;
            continue;
        }
        if (arg == "--bench-duration") {
            if (!parse_size(next(arg), options.bench_duration_s)) {
                std::cerr << "aegisd: invalid bench duration\n";
                return 2;
            }
            options.bench_duration_set = true;
            continue;
        }
        if (arg == "--bench-rate") {
            if (!parse_size(next(arg), options.bench_rate_per_s)) {
                std::cerr << "aegisd: invalid bench rate\n";
                return 2;
            }
            continue;
        }
        if (arg == "--bench-load-duration") {
            if (!parse_size(next(arg), options.bench_duration_load_s)) {
                std::cerr << "aegisd: invalid bench load duration\n";
                return 2;
            }
            continue;
        }
        if (arg == "--bench-terminals") {
            if (!parse_size(next(arg), options.bench_terminals)) {
                std::cerr << "aegisd: invalid bench terminal count\n";
                return 2;
            }
            continue;
        }
        if (arg == "--bench-pans") {
            if (!parse_size(next(arg), options.bench_pans)) {
                std::cerr << "aegisd: invalid bench pan count\n";
                return 2;
            }
            continue;
        }

        std::cerr << "aegisd: unknown argument '" << arg << "'\n";
        print_usage();
        return 2;
    }

    if (options.bench && !options.bench_out_set) {
        std::cerr << "aegisd: --bench requires --bench-out\n";
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
    config.ledger_stage = options.ledger_stage;

    aegis::engine::Engine engine;
    const auto bench_window_start = std::chrono::steady_clock::now();
    if (!engine.start(config).has_value()) {
        std::cerr << "aegisd: engine failed to start with genesis '" << options.genesis.string()
                  << "'\n";
        return 1;
    }

    std::cout << "aegisd listening on 127.0.0.1:" << engine.port() << std::endl;

    const auto run_until = options.bench_duration_set
                               ? bench_window_start + std::chrono::seconds{options.bench_duration_s}
                               : std::chrono::steady_clock::time_point::max();

    while (!g_shutdown_requested.load()) {
        if (std::chrono::steady_clock::now() >= run_until) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }

    const auto bench_window_end = std::chrono::steady_clock::now();
    engine.stop();

    if (options.bench) {
        const auto report = make_bench_report(engine, bench_window_end - bench_window_start);
        if (!report.has_value()) {
            std::cerr << "aegisd: bench histogram is empty\n";
            return 1;
        }
        print_bench_report(report.value());
        if (!write_bench_file(options.bench_out, options, report.value())) {
            std::cerr << "aegisd: failed to write bench output '" << options.bench_out.string()
                      << "'\n";
            return 1;
        }
    } else {
        std::cout << "aegisd stopped" << std::endl;
    }

    return 0;
}
