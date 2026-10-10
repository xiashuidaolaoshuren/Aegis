#include <aegis/net/socket.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using native_socket_t = SOCKET;
constexpr native_socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using native_socket_t = int;
constexpr native_socket_t kInvalidSocket = -1;
#endif

namespace aegis::net {
namespace {

struct NetworkRuntime {
    NetworkRuntime() {
#ifdef _WIN32
        WSADATA wsa_data{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            std::abort();
        }
#endif
    }

    ~NetworkRuntime() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

[[nodiscard]] NetworkRuntime& network_runtime() {
    static NetworkRuntime runtime;
    return runtime;
}

[[nodiscard]] std::atomic<int>& network_runtime_users() {
    static std::atomic<int> users{0};
    return users;
}

class NetworkGuard {
public:
    NetworkGuard() {
        if (network_runtime_users().fetch_add(1) == 0) {
            (void)network_runtime();
        }
    }

    ~NetworkGuard() {
        network_runtime_users().fetch_sub(1);
    }
};

void close_socket(native_socket_t socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

[[nodiscard]] bool set_receive_timeout_ms(native_socket_t socket, int timeout_ms) {
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(timeout_ms);
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value),
                      sizeof(value)) == 0;
#else
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

[[nodiscard]] bool is_timeout_error() {
#ifdef _WIN32
    return WSAGetLastError() == WSAETIMEDOUT;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

[[nodiscard]] Result<void, SocketError> fill_sockaddr_loopback(std::uint16_t port, sockaddr_in& addr) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#ifdef _WIN32
    if (InetPtonA(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        return Result<void, SocketError>::err(SocketError::Io);
    }
#else
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
        return Result<void, SocketError>::err(SocketError::Io);
    }
#endif
    return Result<void, SocketError>::ok();
}

} // namespace

struct Connection::Impl {
    NetworkGuard guard;
    native_socket_t socket{kInvalidSocket};

    explicit Impl(native_socket_t socket_fd) : socket(socket_fd) {}

    ~Impl() {
        close_socket(socket);
    }
};

Connection::Connection() : impl_(std::make_unique<Impl>(kInvalidSocket)) {}

Connection::~Connection() = default;

Connection::Connection(Connection&& other) noexcept = default;
Connection& Connection::operator=(Connection&& other) noexcept = default;

Connection::Connection(int socket_fd)
    : impl_(std::make_unique<Impl>(static_cast<native_socket_t>(socket_fd))) {}

bool Connection::is_open() const {
    return impl_ && impl_->socket != kInvalidSocket;
}

Result<void, SocketError> Connection::connect_loopback(std::uint16_t port) {
    if (is_open()) {
        close();
    }

    const native_socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket) {
        return Result<void, SocketError>::err(SocketError::Io);
    }

    sockaddr_in addr{};
    if (const auto bound = fill_sockaddr_loopback(port, addr); !bound.has_value()) {
        close_socket(socket);
        return bound;
    }

    if (::connect(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(socket);
        return Result<void, SocketError>::err(SocketError::Io);
    }

    impl_ = std::make_unique<Impl>(socket);
    return Result<void, SocketError>::ok();
}

Result<std::size_t, SocketError> Connection::read_some(std::span<std::byte> buffer) {
    if (!is_open()) {
        return Result<std::size_t, SocketError>::err(SocketError::Closed);
    }
    if (buffer.empty()) {
        return Result<std::size_t, SocketError>::ok(0);
    }

#ifdef _WIN32
    const int received = ::recv(impl_->socket, reinterpret_cast<char*>(buffer.data()),
                                static_cast<int>(buffer.size()), 0);
#else
    const ssize_t received =
        ::recv(impl_->socket, buffer.data(), buffer.size(), 0);
#endif

    if (received == 0) {
        return Result<std::size_t, SocketError>::ok(0);
    }
    if (received < 0) {
        if (is_timeout_error()) {
            return Result<std::size_t, SocketError>::err(SocketError::Timeout);
        }
        return Result<std::size_t, SocketError>::err(SocketError::Io);
    }
    return Result<std::size_t, SocketError>::ok(static_cast<std::size_t>(received));
}

Result<void, SocketError> Connection::write_all(std::span<const std::byte> bytes) {
    if (!is_open()) {
        return Result<void, SocketError>::err(SocketError::Closed);
    }

    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = bytes.subspan(offset);
#ifdef _WIN32
        const int sent = ::send(impl_->socket, reinterpret_cast<const char*>(chunk.data()),
                                static_cast<int>(chunk.size()), 0);
#else
        const ssize_t sent = ::send(impl_->socket, chunk.data(), chunk.size(), 0);
#endif
        if (sent <= 0) {
            return Result<void, SocketError>::err(SocketError::Io);
        }
        offset += static_cast<std::size_t>(sent);
    }
    return Result<void, SocketError>::ok();
}

void Connection::set_receive_timeout(std::chrono::milliseconds timeout) {
    if (!is_open()) {
        return;
    }
    (void)set_receive_timeout_ms(impl_->socket, static_cast<int>(timeout.count()));
}

void Connection::close() {
    if (impl_) {
        close_socket(impl_->socket);
        impl_->socket = kInvalidSocket;
    }
}

struct Listener::Impl {
    NetworkGuard guard;
    native_socket_t socket{kInvalidSocket};
    std::uint16_t port{0};

    ~Impl() {
        close_socket(socket);
    }
};

Listener::Listener() : impl_(std::make_unique<Impl>()) {}
Listener::~Listener() = default;
Listener::Listener(Listener&& other) noexcept = default;
Listener& Listener::operator=(Listener&& other) noexcept = default;

Result<void, SocketError> Listener::bind_loopback(std::uint16_t port) {
    if (impl_->socket != kInvalidSocket) {
        close_socket(impl_->socket);
        impl_->socket = kInvalidSocket;
        impl_->port = 0;
    }

    const native_socket_t socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket) {
        return Result<void, SocketError>::err(SocketError::Io);
    }

    const int reuse = 1;
    (void)setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     sizeof(reuse));

    sockaddr_in addr{};
    if (const auto bound = fill_sockaddr_loopback(port, addr); !bound.has_value()) {
        close_socket(socket);
        return bound;
    }

    if (::bind(socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(socket);
        return Result<void, SocketError>::err(SocketError::Io);
    }

    if (::listen(socket, SOMAXCONN) != 0) {
        close_socket(socket);
        return Result<void, SocketError>::err(SocketError::Io);
    }

    if (port == 0) {
        sockaddr_in actual{};
        socklen_t length = sizeof(actual);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&actual), &length) != 0) {
            close_socket(socket);
            return Result<void, SocketError>::err(SocketError::Io);
        }
        impl_->port = ntohs(actual.sin_port);
    } else {
        impl_->port = port;
    }

    impl_->socket = socket;
    return Result<void, SocketError>::ok();
}

std::uint16_t Listener::port() const {
    return impl_->port;
}

Connection Listener::accept() {
    if (impl_->socket == kInvalidSocket) {
        return Connection{};
    }

#ifdef _WIN32
    const native_socket_t client = ::accept(impl_->socket, nullptr, nullptr);
#else
    const native_socket_t client = ::accept(impl_->socket, nullptr, nullptr);
#endif

    if (client == kInvalidSocket) {
        return Connection{};
    }

    return Connection(static_cast<int>(client));
}

} // namespace aegis::net
