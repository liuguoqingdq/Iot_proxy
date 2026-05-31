#include "iota_proxy/network/TcpEgress.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>

namespace iota_proxy {
namespace {

int connect_tcp(const Ipv4Endpoint& endpoint, std::uint32_t timeout_ms) {
    sockaddr_in addr;
    if (!make_sockaddr(endpoint, &addr)) {
        return -1;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 ||
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) !=
        0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return -1;
        }

        pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        const int wait_ms =
            timeout_ms == 0 ? -1 : static_cast<int>(timeout_ms);
        int rc = 0;
        do {
            rc = ::poll(&pfd, 1, wait_ms);
        } while (rc < 0 && errno == EINTR);
        if (rc <= 0) {
            ::close(fd);
            errno = rc == 0 ? ETIMEDOUT : errno;
            return -1;
        }

        int error = 0;
        socklen_t error_len = sizeof(error);
        if (::getsockopt(fd,
                         SOL_SOCKET,
                         SO_ERROR,
                         &error,
                         &error_len) != 0 ||
            error != 0) {
            ::close(fd);
            errno = error == 0 ? errno : error;
            return -1;
        }
    }

    if (::fcntl(fd, F_SETFL, flags) != 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

bool write_all(int fd, const char* data, std::size_t len) {
    while (len > 0) {
        const ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        data += static_cast<std::size_t>(n);
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

TcpEgress::Stream::Stream(std::uint64_t stream_id, int socket_fd)
    : id(stream_id),
      fd(socket_fd),
      open(true),
      write_mutex() {}

TcpEgress::TcpEgress(const TcpEgressConfig& config)
    : config_(config),
      handler_(nullptr),
      stopping_(false),
      mutex_(),
      streams_(),
      threads_() {}

TcpEgress::~TcpEgress() {
    stop();
}

void TcpEgress::set_event_handler(TcpEventHandler* handler) noexcept {
    handler_ = handler;
}

bool TcpEgress::open_stream(std::uint64_t stream_id) {
    if (stream_id == 0 || config_.target.host.empty() ||
        config_.target.port == 0 ||
        stopping_.load(std::memory_order_acquire)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (streams_.find(stream_id) != streams_.end() ||
            streams_.size() >= config_.max_streams) {
            return false;
        }
    }

    const int fd = connect_tcp(
        config_.target,
        config_.connect_timeout_ms
    );
    if (fd < 0) {
        std::cerr << "tcp egress: failed to connect target "
                  << endpoint_to_string(config_.target) << "\n";
        return false;
    }

    std::shared_ptr<Stream> stream =
        std::make_shared<Stream>(stream_id, fd);

    try {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_.load(std::memory_order_acquire) ||
            streams_.find(stream_id) != streams_.end() ||
            streams_.size() >= config_.max_streams) {
            ::close(fd);
            return false;
        }
        streams_[stream_id] = stream;
        threads_.emplace_back(&TcpEgress::read_loop, this, stream);
    } catch (...) {
        remove_stream(stream_id);
        ::close(fd);
        throw;
    }

    return true;
}

bool TcpEgress::send_to_stream(std::uint64_t stream_id, ByteView payload) {
    if (stream_id == 0 || payload.empty() ||
        stopping_.load(std::memory_order_acquire)) {
        return false;
    }

    std::shared_ptr<Stream> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            return false;
        }
        stream = it->second;
    }

    if (!stream || !stream->open.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(stream->write_mutex);
    if (!stream->open.load(std::memory_order_acquire)) {
        return false;
    }

    if (!write_all(stream->fd, payload.data(), payload.size())) {
        close_stream(stream_id);
        return false;
    }
    return true;
}

void TcpEgress::close_stream(std::uint64_t stream_id) {
    close_fd(remove_stream(stream_id));
}

void TcpEgress::shutdown_stream_write(std::uint64_t stream_id) {
    if (stream_id == 0) {
        return;
    }

    std::shared_ptr<Stream> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = streams_.find(stream_id);
        if (it == streams_.end()) {
            return;
        }
        stream = it->second;
    }

    if (!stream || !stream->open.load(std::memory_order_acquire)) {
        return;
    }

    std::lock_guard<std::mutex> lock(stream->write_mutex);
    if (stream->open.load(std::memory_order_acquire)) {
        ::shutdown(stream->fd, SHUT_WR);
    }
}

void TcpEgress::stop() {
    const bool was_stopping =
        stopping_.exchange(true, std::memory_order_acq_rel);
    if (was_stopping) {
        return;
    }

    std::vector<std::shared_ptr<Stream>> streams;
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        streams.reserve(streams_.size());
        for (const auto& item : streams_) {
            streams.push_back(item.second);
        }
        streams_.clear();
        threads.swap(threads_);
    }

    for (const std::shared_ptr<Stream>& stream : streams) {
        close_fd(stream);
    }
    for (std::thread& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void TcpEgress::read_loop(std::shared_ptr<Stream> stream) {
    std::vector<char> buffer(
        config_.recv_buffer_size == 0 ? 16 * 1024 : config_.recv_buffer_size
    );

    while (!stopping_.load(std::memory_order_acquire) &&
           stream->open.load(std::memory_order_acquire)) {
        const ssize_t n = ::recv(stream->fd, buffer.data(), buffer.size(), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }

        TcpEvent event;
        event.type = TcpEventType::Data;
        event.source = TcpEventSource::Egress;
        event.stream_id = stream->id;
        event.payload = ByteView(buffer.data(), static_cast<std::size_t>(n));
        if (handler_ != nullptr) {
            handler_->on_tcp_event(event);
        }
    }

    const bool was_open =
        stream->open.exchange(false, std::memory_order_acq_rel);
    const bool should_notify =
        was_open && !stopping_.load(std::memory_order_acquire);
    remove_stream(stream->id);
    if (was_open) {
        ::shutdown(stream->fd, SHUT_RDWR);
        ::close(stream->fd);
    }

    if (should_notify && handler_ != nullptr) {
        TcpEvent event;
        event.type = TcpEventType::Closed;
        event.source = TcpEventSource::Egress;
        event.stream_id = stream->id;
        handler_->on_tcp_event(event);
    }
}

std::shared_ptr<TcpEgress::Stream> TcpEgress::remove_stream(
    std::uint64_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return nullptr;
    }

    std::shared_ptr<Stream> stream = it->second;
    streams_.erase(it);
    return stream;
}

void TcpEgress::close_fd(std::shared_ptr<Stream> stream) {
    if (!stream) {
        return;
    }
    const bool was_open = stream->open.exchange(false, std::memory_order_acq_rel);
    if (!was_open) {
        return;
    }
    ::shutdown(stream->fd, SHUT_RDWR);
    ::close(stream->fd);
}

}  // namespace iota_proxy
