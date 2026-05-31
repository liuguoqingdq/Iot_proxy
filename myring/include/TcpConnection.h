#ifndef COMPUT_MYRING_TCP_CONNECTION_H
#define COMPUT_MYRING_TCP_CONNECTION_H

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>

#include "nocopyable.h"

namespace myring {

class TcpServer;

class TcpConnection : private nocopyable,
                      public std::enable_shared_from_this<TcpConnection> {
public:
    ~TcpConnection();

    int fd() const noexcept;
    bool connected() const noexcept;

    bool send(const std::string& data);
    bool send(const char* data, std::size_t len);
    void shutdown_write();
    void shutdown();

private:
    friend class TcpServer;

    TcpConnection(const std::shared_ptr<TcpServer>& server, int fd);
    void set_connected(bool connected) noexcept;

private:
    std::weak_ptr<TcpServer> server_;
    int fd_;
    std::atomic<bool> connected_;
};

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

}  // namespace myring

#endif
