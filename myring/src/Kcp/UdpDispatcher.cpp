#include "Kcp/UdpDispatcher.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace myring {
namespace kcp {

namespace {

int create_udp_socket() {
    int fd = ::socket(
        AF_INET,
        SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        IPPROTO_UDP
    );
    if (fd < 0) {
        throw std::runtime_error(
            std::string("udp socket failed: ") + std::strerror(errno)
        );
    }

    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    return fd;
}

bool bind_ipv4_socket(int fd, const std::string& host, std::uint16_t port) {
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return false;
    }

    return ::bind(
        fd,
        reinterpret_cast<const sockaddr*>(&addr),
        sizeof(addr)
    ) == 0;
}

}  // namespace

UdpDispatcher::UdpDispatcher(unsigned entries,
                             std::size_t max_events,
                             std::size_t recv_buffer_size)
    : socket_fd_(-1),
      recv_buffer_size_(recv_buffer_size == 0 ? 2048 : recv_buffer_size),
      running_(false),
      message_callback_(),
      io_thread_(new ProactorThread(entries, max_events)) {}

UdpDispatcher::~UdpDispatcher() {
    stop();

    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

bool UdpDispatcher::bind(const std::string& host, std::uint16_t port) {
    if (socket_fd_ >= 0) {
        return false;
    }

    int fd = create_udp_socket();
    if (!bind_ipv4_socket(fd, host, port)) {
        ::close(fd);
        return false;
    }

    socket_fd_ = fd;
    return true;
}

void UdpDispatcher::set_message_callback(UdpMessageCallback cb) {
    message_callback_ = std::move(cb);
}

void UdpDispatcher::start() {
    if (socket_fd_ < 0) {
        throw std::runtime_error("UdpDispatcher start before bind");
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    io_thread_->start_loop();
    arm_recv();
}

void UdpDispatcher::stop() {
    const bool was_running =
        running_.exchange(false, std::memory_order_acq_rel);

    if (!was_running) {
        return;
    }

    if (io_thread_) {
        io_thread_->stop();
    }
}

bool UdpDispatcher::send_to(const sockaddr* peer,
                            socklen_t peer_len,
                            const char* data,
                            std::size_t len) {
    if (!running_.load(std::memory_order_acquire) || socket_fd_ < 0) {
        return false;
    }

    UdpSendRequestPtr req = make_udp_send_request(
        socket_fd_,
        peer,
        peer_len,
        data,
        len,
        [this](RequestContext* ctx, int res) {
            this->handle_send(ctx, res);
        }
    );

    if (!req) {
        return false;
    }

    return io_thread_->push(std::move(req));
}

bool UdpDispatcher::schedule_timeout(std::uint64_t timeout_ms,
                                     CompleteCallback cb,
                                     void* user_data) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    TimeoutRequestPtr req = make_timeout_request(
        timeout_ms,
        std::move(cb),
        user_data
    );
    if (!req) {
        return false;
    }

    return io_thread_->push(std::move(req));
}

bool UdpDispatcher::post(CompleteCallback cb, void* user_data) {
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    CloseRequestPtr req = make_close_request(-1, std::move(cb), user_data);
    if (!req) {
        return false;
    }

    return io_thread_->push(std::move(req));
}

int UdpDispatcher::fd() const noexcept {
    return socket_fd_;
}

void UdpDispatcher::arm_recv() {
    if (!running_.load(std::memory_order_acquire) || socket_fd_ < 0) {
        return;
    }

    UdpRecvRequestPtr req = make_udp_recv_request(
        socket_fd_,
        recv_buffer_size_,
        [this](RequestContext* ctx, int res) {
            this->handle_recv(ctx, res);
        }
    );

    if (!req) {
        return;
    }

    io_thread_->push(std::move(req));
}

void UdpDispatcher::handle_recv(RequestContext* ctx, int res) {
    UdpRecvRequest* req = static_cast<UdpRecvRequest*>(ctx);

    if (res > 0 && message_callback_) {
        message_callback_(
            req->addr(),
            req->addr_len(),
            req->data(),
            req->buffer.length()
        );
    }

    if (running_.load(std::memory_order_acquire)) {
        arm_recv();
    }
}

void UdpDispatcher::handle_send(RequestContext* ctx, int res) {
    (void)ctx;
    (void)res;
}

}  // namespace kcp
}  // namespace myring
