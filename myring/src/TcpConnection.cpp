#include "TcpConnection.h"

#include <utility>

#include "TcpServer.h"

namespace myring {

TcpConnection::TcpConnection(const std::shared_ptr<TcpServer>& server, int fd)
    : server_(server),
      fd_(fd),
      connected_(true) {}

TcpConnection::~TcpConnection() = default;

int TcpConnection::fd() const noexcept {
    return fd_;
}

bool TcpConnection::connected() const noexcept {
    return connected_.load(std::memory_order_acquire);
}

bool TcpConnection::send(const std::string& data) {
    return send(data.data(), data.size());
}

bool TcpConnection::send(const char* data, std::size_t len) {
    std::shared_ptr<TcpServer> server = server_.lock();
    if (!server || !connected() || data == nullptr || len == 0) {
        return false;
    }

    return server->send_on_connection(fd_, data, len);
}

void TcpConnection::shutdown_write() {
    std::shared_ptr<TcpServer> server = server_.lock();
    if (!server) {
        return;
    }

    server->shutdown_write_connection(fd_);
}

void TcpConnection::shutdown() {
    std::shared_ptr<TcpServer> server = server_.lock();
    if (!server) {
        return;
    }

    server->shutdown_connection(fd_);
}

void TcpConnection::set_connected(bool connected) noexcept {
    connected_.store(connected, std::memory_order_release);
}

}  // namespace myring
