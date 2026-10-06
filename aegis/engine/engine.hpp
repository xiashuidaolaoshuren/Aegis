#pragma once

#include <aegis/authorizer/authorizer.hpp>
#include <aegis/concurrent/bounded_queue.hpp>
#include <aegis/concurrent/thread_pool.hpp>
#include <aegis/iso8583/codec.hpp>
#include <aegis/iso8583/message.hpp>
#include <aegis/issuersim/issuersim.hpp>
#include <aegis/ledger/genesis.hpp>
#include <aegis/ledger/idempotency.hpp>
#include <aegis/ledger/ledger.hpp>
#include <aegis/metrics/counter.hpp>
#include <aegis/net/framing.hpp>
#include <aegis/net/socket.hpp>
#include <aegis/result.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aegis::engine {
namespace detail {

[[nodiscard]] inline iso8583::Message make_system_response(const iso8583::Message& request,
                                                           std::string_view response_code) {
    iso8583::Message response{iso8583::Mti::AuthorizationResponse};
    (void)response.set(iso8583::FieldId::ResponseCode, std::string{response_code});

    if (const auto pan = request.get(iso8583::FieldId::Pan); pan.has_value()) {
        (void)response.set(iso8583::FieldId::Pan, pan.value());
    }
    if (const auto amount = request.get(iso8583::FieldId::Amount); amount.has_value()) {
        (void)response.set(iso8583::FieldId::Amount, amount.value());
    }
    if (const auto stan = request.get(iso8583::FieldId::Stan); stan.has_value()) {
        (void)response.set(iso8583::FieldId::Stan, stan.value());
    }
    if (const auto terminal = request.get(iso8583::FieldId::TerminalId); terminal.has_value()) {
        (void)response.set(iso8583::FieldId::TerminalId, terminal.value());
    }
    if (const auto merchant = request.get(iso8583::FieldId::MerchantId); merchant.has_value()) {
        (void)response.set(iso8583::FieldId::MerchantId, merchant.value());
    }
    if (const auto currency = request.get(iso8583::FieldId::Currency); currency.has_value()) {
        (void)response.set(iso8583::FieldId::Currency, currency.value());
    }
    if (const auto transmission = request.get(iso8583::FieldId::TransmissionDateTime);
        transmission.has_value()) {
        (void)response.set(iso8583::FieldId::TransmissionDateTime, transmission.value());
    }

    return response;
}

// A frame the codec rejected carries no fields to echo, so the format-error response is bare.
[[nodiscard]] inline iso8583::Message make_format_error_response() {
    iso8583::Message response{iso8583::Mti::AuthorizationResponse};
    (void)response.set(iso8583::FieldId::ResponseCode, std::string{"30"});
    return response;
}

[[nodiscard]] inline bool write_response(net::Connection& connection,
                                         const iso8583::Message& response,
                                         metrics::AuthCounter& counter) {
    const auto payload = iso8583::serialise(response);
    if (!payload.has_value()) {
        return false;
    }
    if (!net::write_frame(connection, payload.value()).has_value()) {
        return false;
    }
    counter.increment();
    return true;
}

} // namespace detail

enum class EngineError {
    BindFailed,
    GenesisLoadFailed,
};

struct EngineConfig {
    std::filesystem::path genesis_path;
    std::uint16_t listen_port = 0;
    std::size_t io_threads = 1;
    std::size_t worker_count = 4;
    std::size_t inbound_capacity = 64;
    std::size_t connection_queue_capacity = 64;
    std::chrono::milliseconds frame_read_timeout{5000};
    std::chrono::milliseconds response_write_timeout{5000};
};

class Engine {
public:
    Engine() = default;

    ~Engine() {
        stop();
    }

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] Result<void, EngineError> start(EngineConfig config) {
        std::lock_guard lock(lifecycle_mutex_);
        if (started_) {
            return Result<void, EngineError>::ok();
        }

        config_ = std::move(config);

        const auto wallets = ledger::load_genesis(config_.genesis_path);
        if (!wallets.has_value()) {
            return Result<void, EngineError>::err(EngineError::GenesisLoadFailed);
        }
        ledger_ = std::make_unique<ledger::Ledger>(std::move(wallets.value()));
        authorizer_.emplace(*ledger_, idempotency_store_, issuer_, screen_);

        connection_queue_ = std::make_unique<concurrent::BoundedQueue<net::Connection>>(
            config_.connection_queue_capacity);

        if (!listener_.bind_loopback(config_.listen_port).has_value()) {
            return Result<void, EngineError>::err(EngineError::BindFailed);
        }
        port_ = listener_.port();

        pool_ = std::make_unique<concurrent::ThreadPool>(config_.worker_count,
                                                         config_.inbound_capacity);

        acceptor_ = std::jthread([this](std::stop_token stop) { acceptor_loop(stop); });

        io_threads_.reserve(config_.io_threads);
        for (std::size_t i = 0; i < config_.io_threads; ++i) {
            io_threads_.emplace_back([this](std::stop_token stop) { io_loop(stop); });
        }

        started_ = true;
        return Result<void, EngineError>::ok();
    }

    void stop() {
        std::lock_guard lock(lifecycle_mutex_);
        if (!started_) {
            return;
        }

        if (acceptor_.joinable()) {
            acceptor_.request_stop();
        }
        for (std::jthread& thread : io_threads_) {
            if (thread.joinable()) {
                thread.request_stop();
            }
        }

        if (pool_) {
            pool_->shutdown();
        }

        if (acceptor_.joinable()) {
            acceptor_.join();
        }
        for (std::jthread& thread : io_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        io_threads_.clear();

        // Every thread that could touch the listener has been joined, so replacing it here is
        // ordered after any in-flight accept() and cannot race.
        listener_ = net::Listener{};

        pool_.reset();
        connection_queue_.reset();
        authorizer_.reset();
        ledger_.reset();
        started_ = false;
        port_ = 0;
    }

    [[nodiscard]] std::uint16_t port() const {
        return port_;
    }

    [[nodiscard]] metrics::AuthCounter& auth_counter() {
        return auth_counter_;
    }

    [[nodiscard]] const metrics::AuthCounter& auth_counter() const {
        return auth_counter_;
    }

private:
    // The acceptor polls with a bounded wait so it notices a stop request without relying on the
    // listening socket being closed from another thread.
    static constexpr auto kAcceptPollInterval = std::chrono::milliseconds{100};
    static constexpr auto kAcceptErrorBackoff = std::chrono::milliseconds{10};

    void acceptor_loop(std::stop_token stop) {
        while (!stop.stop_requested()) {
            auto polled = listener_.accept(kAcceptPollInterval);
            if (!polled.has_value()) {
                // The listening socket is unusable; back off so a persistent error does not spin.
                std::this_thread::sleep_for(kAcceptErrorBackoff);
                continue;
            }
            if (!polled.value().has_value()) {
                continue; // the poll timed out; re-check the stop token
            }

            std::optional<net::Connection> pending = std::move(polled).take();
            net::Connection connection = std::move(pending.value());
            if (!connection_queue_->push(std::move(connection))) {
                connection.close();
            }
        }
    }

    void io_loop(std::stop_token stop) {
        while (true) {
            auto connection = connection_queue_->pop(stop);
            if (!connection.has_value()) {
                if (stop.stop_requested()) {
                    return;
                }
                continue;
            }

            auto conn = std::make_shared<net::Connection>(std::move(connection.value()));

            if (stop.stop_requested()) {
                // Shutdown is in progress: do not start a read that would only have to time out.
                conn->close();
                continue;
            }

            // The whole-frame deadline bounds this thread even if the peer trickles bytes, and
            // the send timeout bounds the response write on the worker that picks it up.
            conn->set_send_timeout(config_.response_write_timeout);

            const auto frame = net::read_frame(*conn, config_.frame_read_timeout);
            if (!frame.has_value()) {
                conn->close();
                continue;
            }

            const auto parsed = iso8583::parse(frame.value());
            if (!parsed.has_value()) {
                // A frame we could not parse still gets a format-error response (field 39 = 30).
                const iso8583::Message response = detail::make_format_error_response();
                (void)detail::write_response(*conn, response, auth_counter_);
                conn->close();
                continue;
            }

            iso8583::Message request = parsed.value();
            const iso8583::Message request_for_backpressure = request;

            const bool submitted = pool_->submit([this, conn, request = std::move(request)]() mutable {
                iso8583::Message response = [&]() {
                    std::lock_guard lock(ledger_mutex_);
                    return authorizer_->handle(request);
                }();
                (void)detail::write_response(*conn, response, auth_counter_);
                conn->close();
            });

            if (!submitted) {
                const iso8583::Message response =
                    detail::make_system_response(request_for_backpressure, "96");
                (void)detail::write_response(*conn, response, auth_counter_);
                conn->close();
            }
        }
    }

    EngineConfig config_{};
    net::Listener listener_{};
    std::uint16_t port_{0};
    std::unique_ptr<concurrent::BoundedQueue<net::Connection>> connection_queue_;
    std::unique_ptr<concurrent::ThreadPool> pool_;
    std::jthread acceptor_;
    std::vector<std::jthread> io_threads_;
    bool started_{false};
    std::mutex lifecycle_mutex_;

    std::unique_ptr<ledger::Ledger> ledger_;
    ledger::IdempotencyStore idempotency_store_;
    issuersim::IssuerSim issuer_{};
    authorizer::ScreenConfig screen_{};
    std::optional<authorizer::Authorizer> authorizer_;
    std::mutex ledger_mutex_;
    metrics::AuthCounter auth_counter_;
};

} // namespace aegis::engine
