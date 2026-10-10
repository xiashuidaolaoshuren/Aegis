#pragma once

#include <aegis/result.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace aegis::net {

enum class SocketError {
    Io,
    Closed,
    Timeout,
};

class Connection {
public:
    Connection();
    ~Connection();

    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    [[nodiscard]] bool is_open() const;

    [[nodiscard]] Result<void, SocketError> connect_loopback(std::uint16_t port);
    [[nodiscard]] Result<std::size_t, SocketError> read_some(std::span<std::byte> buffer);
    [[nodiscard]] Result<void, SocketError> write_all(std::span<const std::byte> bytes);
    void set_receive_timeout(std::chrono::milliseconds timeout);
    void set_send_timeout(std::chrono::milliseconds timeout);
    void close();

private:
    friend class Listener;

    explicit Connection(int socket_fd);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Listener {
public:
    Listener();
    ~Listener();

    Listener(Listener&& other) noexcept;
    Listener& operator=(Listener&& other) noexcept;

    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    [[nodiscard]] Result<void, SocketError> bind_loopback(std::uint16_t port);
    [[nodiscard]] std::uint16_t port() const;
    [[nodiscard]] Connection accept();
    // Waits up to `timeout` for a pending connection. Returns an engaged optional holding the
    // accepted connection, or an empty optional when the wait timed out without a peer.
    [[nodiscard]] Result<std::optional<Connection>, SocketError> accept(
        std::chrono::milliseconds timeout);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegis::net
