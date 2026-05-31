#include "TcpServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Proactor/LoopThreadPool.hpp"
#include "Proactor/Request.hpp"

namespace myring {

namespace {

int create_listen_fd(const std::string& host,
                     std::uint16_t port,
                     int backlog) {
    int fd = ::socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        IPPROTO_TCP
    );
    if (fd < 0) {
        throw std::runtime_error(
            std::string("socket failed: ") + std::strerror(errno)
        );
    }

    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        throw std::runtime_error("invalid listen host: " + host);
    }

    if (::bind(fd,
               reinterpret_cast<const sockaddr*>(&addr),
               sizeof(addr)) != 0) {
        const std::string message =
            std::string("bind failed: ") + std::strerror(errno);
        ::close(fd);
        throw std::runtime_error(message);
    }

    if (::listen(fd, backlog) != 0) {
        const std::string message =
            std::string("listen failed: ") + std::strerror(errno);
        ::close(fd);
        throw std::runtime_error(message);
    }

    return fd;
}

std::vector<std::string> extract_messages(std::string* buffer,
                                          bool flush_remaining) {
    std::vector<std::string> messages;
    if (buffer == nullptr) {
        return messages;
    }

    std::size_t start = 0;
    while (true) {
        const std::size_t end = buffer->find('\n', start);
        if (end == std::string::npos) {
            break;
        }

        std::string message = buffer->substr(start, end - start);
        if (!message.empty() && message.back() == '\r') {
            message.pop_back();
        }
        if (!message.empty()) {
            messages.push_back(std::move(message));
        }
        start = end + 1;
    }

    if (start > 0) {
        buffer->erase(0, start);
    }

    if (flush_remaining && !buffer->empty()) {
        messages.push_back(*buffer);
        buffer->clear();
    }

    return messages;
}

void close_socket_fd(int fd) noexcept {
    if (fd < 0) {
        return;
    }

    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
}

}  // namespace

struct TcpServer::Impl {
    struct ConnectionState {
        TcpConnectionPtr connection;
        ProactorThread* worker;
        std::string input_buffer;
        bool read_closed = false;
        bool write_shutdown_requested = false;
        bool write_closed = false;
        std::size_t pending_sends = 0;
    };

    Impl(TcpServer* owner_in,
         unsigned entries_in,
         std::string host_in,
         std::uint16_t port_in,
         std::size_t worker_threads_in,
         int backlog_in,
         std::size_t recv_buffer_size_in)
        : owner(owner_in),
          entries(entries_in),
          host(std::move(host_in)),
          port(port_in),
          worker_threads(worker_threads_in == 0 ? 1 : worker_threads_in),
          backlog(backlog_in),
          recv_buffer_size(recv_buffer_size_in == 0 ? 4096
                                                   : recv_buffer_size_in),
          listen_fd(create_listen_fd(host, port, backlog)),
          started(false),
          connections_mutex(),
          connections(),
          connection_callback(),
          message_callback(),
          raw_message_callback(),
          pool() {}

    ~Impl() {
        if (listen_fd >= 0) {
            ::close(listen_fd);
            listen_fd = -1;
        }
    }

    RecvRequestPtr make_initial_recv_request(int fd, ProactorThread* worker) {
        std::shared_ptr<TcpServer> server = owner->shared_from_this();
        TcpConnectionPtr conn(new TcpConnection(server, fd));

        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            connections[fd] = ConnectionState{
                conn,
                worker,
                std::string(),
                false,
                false,
                false,
                0
            };
        }

        return make_recv_request(
            fd,
            recv_buffer_size,
            [this](RequestContext* ctx, int res) {
                this->handle_recv(ctx, res);
            }
        );
    }

    void handle_accept_result(int fd,
                              ProactorThread* worker,
                              bool accepted) {
        (void)worker;

        TcpConnectionPtr conn;
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            auto it = connections.find(fd);
            if (it == connections.end()) {
                return;
            }

            conn = it->second.connection;
            if (!accepted) {
                conn->set_connected(false);
                connections.erase(it);
            }
        }

        if (!accepted) {
            return;
        }

        if (connection_callback && conn) {
            connection_callback(conn);
        }
    }

    bool rearm_recv(int fd) {
        ProactorThread* worker = nullptr;
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            auto it = connections.find(fd);
            if (it == connections.end()) {
                return false;
            }
            worker = it->second.worker;
        }

        if (worker == nullptr) {
            return false;
        }

        RecvRequestPtr req = make_recv_request(
            fd,
            recv_buffer_size,
            [this](RequestContext* ctx, int res) {
                this->handle_recv(ctx, res);
            }
        );
        if (!req || !worker->push(std::move(req))) {
            shutdown_connection(fd);
            return false;
        }

        return true;
    }

    void handle_recv(RequestContext* ctx, int res) {
        if (ctx == nullptr) {
            return;
        }

        const int fd = ctx->fd;
        TcpConnectionPtr conn;
        std::vector<std::string> messages;
        bool should_disconnect = false;
        bool should_notify_read_closed = false;
        const char* raw_data = nullptr;
        std::size_t raw_len = 0;

        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            auto it = connections.find(fd);
            if (it == connections.end()) {
                return;
            }

            conn = it->second.connection;

            if (res > 0) {
                RecvRequest* recv_req = static_cast<RecvRequest*>(ctx);
                raw_data = recv_req->data();
                raw_len = recv_req->buffer.length();

                if (message_callback) {
                    it->second.input_buffer.append(
                        recv_req->data(),
                        recv_req->buffer.length()
                    );
                    messages = extract_messages(
                        &it->second.input_buffer,
                        false
                    );
                }
            } else if (res == 0) {
                if (message_callback) {
                    messages = extract_messages(
                        &it->second.input_buffer,
                        true
                    );
                }
                if (!it->second.read_closed) {
                    it->second.read_closed = true;
                    should_notify_read_closed = true;
                }
            } else {
                conn->set_connected(false);
                connections.erase(it);
                should_disconnect = true;
            }
        }

        if (raw_message_callback && conn && raw_data != nullptr &&
            raw_len != 0) {
            raw_message_callback(conn, raw_data, raw_len);
        }

        if (message_callback && conn) {
            for (std::size_t i = 0; i < messages.size(); ++i) {
                message_callback(conn, messages[i]);
            }
        }

        if (should_disconnect) {
            close_socket_fd(fd);
            if (connection_callback && conn) {
                connection_callback(conn);
            }
            return;
        }

        if (should_notify_read_closed) {
            if (read_closed_callback && conn) {
                read_closed_callback(conn);
            } else {
                shutdown_connection(fd);
            }
            return;
        }

        if (conn && conn->connected()) {
            rearm_recv(fd);
        }
    }

    void handle_send(RequestContext* ctx, int res) {
        if (ctx == nullptr) {
            return;
        }

        const int fd = ctx->fd;
        TcpConnectionPtr conn;
        bool should_shutdown_write = false;
        bool should_disconnect = false;
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            auto it = connections.find(fd);
            if (it == connections.end()) {
                return;
            }

            ConnectionState& state = it->second;
            conn = state.connection;
            if (state.pending_sends > 0) {
                --state.pending_sends;
            }

            if (res < 0) {
                if (conn) {
                    conn->set_connected(false);
                }
                connections.erase(it);
                should_disconnect = true;
            } else if (state.write_shutdown_requested &&
                       state.pending_sends == 0 &&
                       !state.write_closed) {
                state.write_closed = true;
                should_shutdown_write = true;
                if (state.read_closed) {
                    if (conn) {
                        conn->set_connected(false);
                    }
                    connections.erase(it);
                    should_disconnect = true;
                }
            }
        }

        if (should_shutdown_write) {
            ::shutdown(fd, SHUT_WR);
        }
        if (should_disconnect) {
            close_socket_fd(fd);
            if (connection_callback && conn) {
                connection_callback(conn);
            }
        }
    }

    bool send_on_connection(int fd, const char* data, std::size_t len) {
        ProactorThread* worker = nullptr;
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            auto it = connections.find(fd);
            if (it == connections.end() ||
                !it->second.connection ||
                !it->second.connection->connected()) {
                return false;
            }
            ConnectionState& state = it->second;
            worker = state.worker;
            ++state.pending_sends;
        }

        if (worker == nullptr || data == nullptr || len == 0) {
            return false;
        }

        SendRequestPtr req = make_send_request(
            fd,
            data,
            len,
            [this](RequestContext* ctx, int res) {
                this->handle_send(ctx, res);
            }
        );
        if (!req || !worker->push(std::move(req))) {
            {
                std::lock_guard<std::mutex> lock(connections_mutex);
                auto it = connections.find(fd);
                if (it != connections.end() &&
                    it->second.pending_sends > 0) {
                    --it->second.pending_sends;
                }
            }
            shutdown_connection(fd);
            return false;
        }

        return true;
    }

    void shutdown_write_connection(int fd) {
        TcpConnectionPtr conn;
        bool should_shutdown = false;
        bool should_disconnect = false;
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            auto it = connections.find(fd);
            if (it == connections.end() ||
                !it->second.connection ||
                !it->second.connection->connected()) {
                return;
            }
            ConnectionState& state = it->second;
            if (state.write_closed) {
                return;
            }

            conn = state.connection;
            state.write_shutdown_requested = true;
            if (state.pending_sends == 0) {
                state.write_closed = true;
                should_shutdown = true;
                if (state.read_closed) {
                    if (conn) {
                        conn->set_connected(false);
                    }
                    connections.erase(it);
                    should_disconnect = true;
                }
            }
        }

        if (should_shutdown) {
            ::shutdown(fd, SHUT_WR);
        }
        if (should_disconnect) {
            close_socket_fd(fd);
            if (connection_callback && conn) {
                connection_callback(conn);
            }
        }
    }

    void shutdown_connection(int fd) {
        TcpConnectionPtr conn;
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            auto it = connections.find(fd);
            if (it == connections.end()) {
                return;
            }

            conn = it->second.connection;
            if (conn) {
                conn->set_connected(false);
            }
            connections.erase(it);
        }

        close_socket_fd(fd);

        if (connection_callback && conn) {
            connection_callback(conn);
        }
    }

    void close_all_connections() {
        std::vector<std::pair<int, TcpConnectionPtr> > active;
        {
            std::lock_guard<std::mutex> lock(connections_mutex);
            active.reserve(connections.size());
            for (auto it = connections.begin(); it != connections.end(); ++it) {
                if (it->second.connection) {
                    it->second.connection->set_connected(false);
                }
                active.push_back(
                    std::make_pair(it->first, it->second.connection)
                );
            }
            connections.clear();
        }

        for (std::size_t i = 0; i < active.size(); ++i) {
            close_socket_fd(active[i].first);
            if (connection_callback && active[i].second) {
                connection_callback(active[i].second);
            }
        }
    }

    TcpServer* owner;
    unsigned entries;
    std::string host;
    std::uint16_t port;
    std::size_t worker_threads;
    int backlog;
    std::size_t recv_buffer_size;
    int listen_fd;
    std::atomic<bool> started;
    std::mutex connections_mutex;
    std::unordered_map<int, ConnectionState> connections;
    ConnectionCallback connection_callback;
    MessageCallback message_callback;
    RawMessageCallback raw_message_callback;
    ReadClosedCallback read_closed_callback;
    std::unique_ptr<LoopThreadPool> pool;
};

std::shared_ptr<TcpServer> TcpServer::create(unsigned entries,
                                             const char* host,
                                             std::uint16_t port,
                                             std::size_t worker_threads,
                                             int backlog,
                                             std::size_t recv_buffer_size) {
    return std::shared_ptr<TcpServer>(
        new TcpServer(
            entries,
            host == nullptr ? std::string() : std::string(host),
            port,
            worker_threads,
            backlog,
            recv_buffer_size
        )
    );
}

TcpServer::TcpServer(unsigned entries,
                     std::string host,
                     std::uint16_t port,
                     std::size_t worker_threads,
                     int backlog,
                     std::size_t recv_buffer_size)
    : impl_(new Impl(
          this,
          entries,
          std::move(host),
          port,
          worker_threads,
          backlog,
          recv_buffer_size
      )) {}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::set_connection_callback(ConnectionCallback cb) {
    impl_->connection_callback = std::move(cb);
}

void TcpServer::set_message_callback(MessageCallback cb) {
    impl_->message_callback = std::move(cb);
}

void TcpServer::set_raw_message_callback(RawMessageCallback cb) {
    impl_->raw_message_callback = std::move(cb);
}

void TcpServer::set_read_closed_callback(ReadClosedCallback cb) {
    impl_->read_closed_callback = std::move(cb);
}

void TcpServer::start(std::size_t max_events) {
    bool expected = false;
    if (!impl_->started.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        return;
    }

    impl_->pool.reset(new LoopThreadPool(
        impl_->listen_fd,
        impl_->entries,
        max_events,
        impl_->worker_threads,
        impl_->recv_buffer_size
    ));

    impl_->pool->set_recv_request_factory(
        [this](int fd, ProactorThread* worker) {
            return impl_->make_initial_recv_request(fd, worker);
        }
    );
    impl_->pool->set_accept_result_callback(
        [this](int fd, ProactorThread* worker, bool accepted) {
            impl_->handle_accept_result(fd, worker, accepted);
        }
    );
    impl_->pool->start();
}

void TcpServer::stop() {
    const bool was_started = impl_->started.exchange(
        false,
        std::memory_order_acq_rel
    );
    if (!was_started) {
        return;
    }

    if (impl_->pool) {
        impl_->pool->stop();
        impl_->pool.reset();
    }

    impl_->close_all_connections();
}

bool TcpServer::started() const noexcept {
    return impl_->started.load(std::memory_order_acquire);
}

bool TcpServer::send_on_connection(int fd,
                                   const char* data,
                                   std::size_t len) {
    return impl_->send_on_connection(fd, data, len);
}

void TcpServer::shutdown_write_connection(int fd) {
    impl_->shutdown_write_connection(fd);
}

void TcpServer::shutdown_connection(int fd) {
    impl_->shutdown_connection(fd);
}

}  // namespace myring
