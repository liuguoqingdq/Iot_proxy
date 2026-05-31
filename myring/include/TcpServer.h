#ifndef COMPUT_MYRING_TCP_SERVER_H
#define COMPUT_MYRING_TCP_SERVER_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "TcpConnection.h"
#include "nocopyable.h"

namespace myring {

using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&,
                                           const std::string&)>;
using RawMessageCallback = std::function<void(const TcpConnectionPtr&,
                                              const char*,
                                              std::size_t)>;
using ReadClosedCallback = std::function<void(const TcpConnectionPtr&)>;

class TcpServer : private nocopyable,
                  public std::enable_shared_from_this<TcpServer> {
public:
    static std::shared_ptr<TcpServer> create(
        unsigned entries,
        const char* host,
        std::uint16_t port,
        std::size_t worker_threads = 1,
        int backlog = 128,
        std::size_t recv_buffer_size = 4096
    );

    ~TcpServer();

    void set_connection_callback(ConnectionCallback cb);
    void set_message_callback(MessageCallback cb);
    void set_raw_message_callback(RawMessageCallback cb);
    void set_read_closed_callback(ReadClosedCallback cb);

    void start(std::size_t max_events = 64);
    void stop();

    bool started() const noexcept;

private:
    friend class TcpConnection;

    TcpServer(unsigned entries,
              std::string host,
              std::uint16_t port,
              std::size_t worker_threads,
              int backlog,
              std::size_t recv_buffer_size);

    bool send_on_connection(int fd, const char* data, std::size_t len);
    void shutdown_write_connection(int fd);
    void shutdown_connection(int fd);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace myring

#endif
