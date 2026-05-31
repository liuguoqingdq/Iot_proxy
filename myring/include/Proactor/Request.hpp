#ifndef MYRING_PROACTOR_REQUEST_HPP
#define MYRING_PROACTOR_REQUEST_HPP

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/uio.h>
#include <liburing.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "objectpool/Buffer.hpp"
#include "objectpool/ObjectPool.hpp"

enum class RequestType {
    ACCEPT,
    RECV,
    SEND,
    UDP_RECV,
    UDP_SEND,
    TIMEOUT,
    CLOSE,
    UNKNOWN,
    WAKEUP
};


struct RequestContext;

using CompleteCallback = std::function<void(RequestContext*, int)>;

struct RequestContext {
    RequestType type;
    int fd;
    void* user_data;
    CompleteCallback callback;
    int io_result;
    unsigned cqe_flags;

    RequestContext(RequestType request_type, int request_fd)
        : type(request_type),
          fd(request_fd),
          user_data(nullptr),
          callback(nullptr),
          io_result(0),
          cqe_flags(0) {}

    virtual ~RequestContext() noexcept = default;
};

inline void bind_request_context(RequestContext* ctx,
                                 CompleteCallback cb = {},
                                 void* user_data = nullptr) {
    if (ctx == nullptr) {
        return;
    }

    ctx->callback = std::move(cb);
    ctx->user_data = user_data;
}

inline void invoke_request_callback(RequestContext* ctx, int result) {
    if (ctx != nullptr && ctx->callback) {
        ctx->callback(ctx, result);
    }
}

struct AcceptRequest : public RequestContext {
    sockaddr_storage client_addr;
    socklen_t client_len;

    explicit AcceptRequest(int listen_fd)
        : RequestContext(RequestType::ACCEPT, listen_fd),
          client_len(sizeof(client_addr)) {
        std::memset(&client_addr, 0, sizeof(client_addr));
    }

    sockaddr* addr_ptr() noexcept {
        return reinterpret_cast<sockaddr*>(&client_addr);
    }

    socklen_t* len_ptr() noexcept {
        return &client_len;
    }
};

struct RecvRequest : public RequestContext {
    Buffer buffer;
    int flags;

    explicit RecvRequest(int conn_fd, std::size_t buffer_size = 4096)
        : RequestContext(RequestType::RECV, conn_fd),
          buffer(buffer_size),
          flags(0) {}

    char* data() noexcept {
        return buffer.data();
    }

    const char* data() const noexcept {
        return buffer.data();
    }

    std::size_t size() const noexcept {
        return buffer.capacity();
    }
};

struct SendRequest : public RequestContext {
    Buffer buffer;
    std::size_t offset;
    int flags;

    SendRequest(int conn_fd, const char* data, std::size_t len)
        : RequestContext(RequestType::SEND, conn_fd),
          buffer(len),
          offset(0),
          flags(MSG_NOSIGNAL) {
        buffer.assign(data, len);
    }

    SendRequest(int conn_fd, const std::string& data)
        : RequestContext(RequestType::SEND, conn_fd),
          buffer(data.size()),
          offset(0),
          flags(MSG_NOSIGNAL) {
        buffer.assign(data.data(), data.size());
    }

    const char* data() const noexcept {
        return buffer.data() + offset;
    }

    std::size_t length() const noexcept {
        return buffer.length();
    }

    std::size_t remaining() const noexcept {
        return buffer.length() - offset;
    }

    void advance(std::size_t n) noexcept {
        offset += n;
    }

    bool finished() const noexcept {
        return offset >= buffer.length();
    }
};

struct CloseRequest : public RequestContext {
    explicit CloseRequest(int conn_fd)
        : RequestContext(RequestType::CLOSE, conn_fd) {}
};


struct WakeRequest : public RequestContext {
    std::uint64_t value;

    explicit WakeRequest(int wake_fd)
        : RequestContext(RequestType::WAKEUP, wake_fd),
          value(0) {}
};




struct UdpRecvRequest : public RequestContext {
    Buffer buffer;
    int flags;
    sockaddr_storage peer_addr;
    iovec iov;
    msghdr msg;
    explicit UdpRecvRequest(int socket_fd,std::size_t size = 2048)
    :RequestContext(RequestType::UDP_RECV,socket_fd),
    buffer(size),
    flags(0),
    peer_addr(),
    iov(),
    msg()
    {
        reset_message();
    }
    void reset_message() noexcept {
        buffer.clear();
        std::memset(&peer_addr,0,sizeof(peer_addr));
        std::memset(&iov,0,sizeof(iov));
        std::memset(&msg,0,sizeof(msg));
        iov.iov_base = buffer.data();
        iov.iov_len = buffer.capacity();

        msg.msg_name = &peer_addr;
        msg.msg_namelen = sizeof(peer_addr);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
    }
    msghdr* msg_ptr() noexcept {
        return &msg;
    }
    char* data() noexcept {
        return buffer.data();
    }
    std::size_t size() const noexcept {
        return buffer.capacity();
    }
    const sockaddr* addr() const noexcept{
        return reinterpret_cast<const sockaddr*>(&peer_addr);
    }
    socklen_t addr_len() const noexcept {
        return static_cast<socklen_t>(msg.msg_namelen);
    }
};

struct UdpSendRequest : public RequestContext {
    Buffer buffer;
    int flags;
    sockaddr_storage peer_addr;
    socklen_t peer_len;
    iovec iov;
    msghdr msg;

    UdpSendRequest(int socket_fd,
                   const sockaddr* peer,
                   socklen_t peer_len_in,
                   const char* data,
                   std::size_t len)
        : RequestContext(RequestType::UDP_SEND, socket_fd),
          buffer(len),
          flags(0),
          peer_addr(),
          peer_len(0),
          iov(),
          msg() {
        assign_peer(peer, peer_len_in);
        buffer.assign(data, len);
        reset_message();
    }
    void assign_peer(const sockaddr* peer,socklen_t peer_len_in) noexcept{
        std::memset(&peer_addr,0,sizeof(peer_addr));
        peer_len = 0;
        if(peer == nullptr || peer_len_in == 0){
            return;
        }
        const socklen_t copy_len = 
            peer_len_in <= sizeof(peer_addr) 
            ? peer_len_in :static_cast<socklen_t>(sizeof(peer_addr));
        std::memcpy(&peer_addr,peer,copy_len);
        peer_len = copy_len;
    }
    void reset_message() noexcept {
        std::memset(&iov, 0, sizeof(iov));
        std::memset(&msg, 0, sizeof(msg));

        iov.iov_base = const_cast<char*>(buffer.data());
        iov.iov_len = buffer.length();

        msg.msg_name = &peer_addr;
        msg.msg_namelen = peer_len;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
    }

    msghdr* msg_ptr() noexcept {
        return &msg;
    }

    std::size_t length() const noexcept {
        return buffer.length();
    }
};
struct TimeoutRequest : public RequestContext {
    std::uint64_t timeout_ms;
    __kernel_timespec kernel_timeout;

    explicit TimeoutRequest(std::uint64_t ms)
        : RequestContext(RequestType::TIMEOUT, -1),
          timeout_ms(ms),
          kernel_timeout{} {
        sync_timeout();
    }

    void set_timeout_ms(std::uint64_t ms) noexcept {
        timeout_ms = ms;
        sync_timeout();
    }

    __kernel_timespec* timeout_ptr() noexcept {
        return &kernel_timeout;
    }

private:
    void sync_timeout() noexcept {
        kernel_timeout.tv_sec =
            static_cast<__kernel_time64_t>(timeout_ms / 1000);
        kernel_timeout.tv_nsec =
            static_cast<long>((timeout_ms % 1000) * 1000000ULL);
    }
};


inline pool::ObjectPool<AcceptRequest>& accept_request_pool() {
    static pool::ObjectPool<AcceptRequest> pool("accept_request");
    return pool;
}
inline pool::ObjectPool<UdpRecvRequest>& udp_recv_request_pool() {
    static pool::ObjectPool<UdpRecvRequest> pool("udp_recv_request");
    return pool;
}

inline pool::ObjectPool<UdpSendRequest>& udp_send_request_pool() {
    static pool::ObjectPool<UdpSendRequest> pool("udp_send_request");
    return pool;
}

inline pool::ObjectPool<TimeoutRequest>& timeout_request_pool() {
    static pool::ObjectPool<TimeoutRequest> pool("timeout_request");
    return pool;
}


inline pool::ObjectPool<RecvRequest>& recv_request_pool() {
    static pool::ObjectPool<RecvRequest> pool("recv_request");
    return pool;
}

inline pool::ObjectPool<SendRequest>& send_request_pool() {
    static pool::ObjectPool<SendRequest> pool("send_request");
    return pool;
}

inline pool::ObjectPool<CloseRequest>& close_request_pool() {
    static pool::ObjectPool<CloseRequest> pool("close_request");
    return pool;
}

template <typename RequestT>
inline RequestT* configure_request(RequestT* req,
                                   CompleteCallback cb = {},
                                   void* user_data = nullptr) {
    bind_request_context(req, std::move(cb), user_data);
    return req;
}

inline AcceptRequest* try_make_accept_request(int listen_fd,
                                              CompleteCallback cb = {},
                                              void* user_data = nullptr) {
    return configure_request(
        accept_request_pool().try_make(listen_fd),
        std::move(cb),
        user_data
    );
}

inline RecvRequest* try_make_recv_request(int conn_fd,
                                          std::size_t buffer_size = 4096,
                                          CompleteCallback cb = {},
                                          void* user_data = nullptr) {
    return configure_request(
        recv_request_pool().try_make(conn_fd, buffer_size),
        std::move(cb),
        user_data
    );
}

inline SendRequest* try_make_send_request(int conn_fd,
                                          const char* data,
                                          std::size_t len,
                                          CompleteCallback cb = {},
                                          void* user_data = nullptr) {
    return configure_request(
        send_request_pool().try_make(conn_fd, data, len),
        std::move(cb),
        user_data
    );
}

inline SendRequest* try_make_send_request(int conn_fd,
                                          const std::string& data,
                                          CompleteCallback cb = {},
                                          void* user_data = nullptr) {
    return configure_request(
        send_request_pool().try_make(conn_fd, data),
        std::move(cb),
        user_data
    );
}

inline CloseRequest* try_make_close_request(int conn_fd,
                                            CompleteCallback cb = {},
                                            void* user_data = nullptr) {
    return configure_request(
        close_request_pool().try_make(conn_fd),
        std::move(cb),
        user_data
    );
}

inline WakeRequest* try_make_wake_request(int wake_fd) {
    return new WakeRequest(wake_fd);
}


inline UdpRecvRequest* try_make_udp_recv_request(
    int socket_fd,
    std::size_t buffer_size = 2048,
    CompleteCallback cb = {},
    void* user_data = nullptr) {
    return configure_request(
        udp_recv_request_pool().try_make(socket_fd, buffer_size),
        std::move(cb),
        user_data
    );
}

inline UdpSendRequest* try_make_udp_send_request(
    int socket_fd,
    const sockaddr* peer,
    socklen_t peer_len,
    const char* data,
    std::size_t len,
    CompleteCallback cb = {},
    void* user_data = nullptr) {
    return configure_request(
        udp_send_request_pool().try_make(
            socket_fd,
            peer,
            peer_len,
            data,
            len
        ),
        std::move(cb),
        user_data
    );
}

inline UdpSendRequest* try_make_udp_send_request(
    int socket_fd,
    const sockaddr* peer,
    socklen_t peer_len,
    const std::string& data,
    CompleteCallback cb = {},
    void* user_data = nullptr) {
    return configure_request(
        udp_send_request_pool().try_make(
            socket_fd,
            peer,
            peer_len,
            data.data(),
            data.size()
        ),
        std::move(cb),
        user_data
    );
}

inline TimeoutRequest* try_make_timeout_request(
    std::uint64_t timeout_ms,
    CompleteCallback cb = {},
    void* user_data = nullptr) {
    return configure_request(
        timeout_request_pool().try_make(timeout_ms),
        std::move(cb),
        user_data
    );
}


inline void destroy_request_context(RequestContext* ctx) noexcept {
    if (ctx == nullptr) {
        return;
    }

    switch (ctx->type) {
        case RequestType::ACCEPT:
            accept_request_pool().destroy(static_cast<AcceptRequest*>(ctx));
            return;
        case RequestType::RECV:
            recv_request_pool().destroy(static_cast<RecvRequest*>(ctx));
            return;
        case RequestType::SEND:
            send_request_pool().destroy(static_cast<SendRequest*>(ctx));
            return;
        case RequestType::CLOSE:
            close_request_pool().destroy(static_cast<CloseRequest*>(ctx));
            return;
        case RequestType::WAKEUP:
            delete static_cast<WakeRequest*>(ctx);
            return;
        case RequestType::UDP_RECV:
            udp_recv_request_pool().destroy(static_cast<UdpRecvRequest*>(ctx));
            return;
        case RequestType::UDP_SEND:
            udp_send_request_pool().destroy(static_cast<UdpSendRequest*>(ctx));
            return;
        case RequestType::TIMEOUT:
            timeout_request_pool().destroy(static_cast<TimeoutRequest*>(ctx));
            return;
        default:
            delete ctx;
            return;
    }
}

struct RequestContextDeleter {
    void operator()(RequestContext* ctx) const noexcept {
        destroy_request_context(ctx);
    }
};

template <typename RequestT>
using RequestPtr = std::unique_ptr<RequestT, RequestContextDeleter>;

using RequestContextPtr = RequestPtr<RequestContext>;
using AcceptRequestPtr = RequestPtr<AcceptRequest>;
using RecvRequestPtr = RequestPtr<RecvRequest>;
using SendRequestPtr = RequestPtr<SendRequest>;
using CloseRequestPtr = RequestPtr<CloseRequest>;
using WakeRequestPtr = RequestPtr<WakeRequest>;
using UdpRecvRequestPtr = RequestPtr<UdpRecvRequest>;
using UdpSendRequestPtr = RequestPtr<UdpSendRequest>;
using TimeoutRequestPtr = RequestPtr<TimeoutRequest>;


template <typename RequestT>
inline RequestPtr<RequestT> adopt_request(RequestT* req) noexcept {
    return RequestPtr<RequestT>(req);
}

inline AcceptRequestPtr make_accept_request(int listen_fd,
                                            CompleteCallback cb = {},
                                            void* user_data = nullptr) {
    return adopt_request(
        try_make_accept_request(listen_fd, std::move(cb), user_data)
    );
}

inline RecvRequestPtr make_recv_request(int conn_fd,
                                        std::size_t buffer_size = 4096,
                                        CompleteCallback cb = {},
                                        void* user_data = nullptr) {
    return adopt_request(
        try_make_recv_request(conn_fd, buffer_size, std::move(cb), user_data)
    );
}

inline SendRequestPtr make_send_request(int conn_fd,
                                        const char* data,
                                        std::size_t len,
                                        CompleteCallback cb = {},
                                        void* user_data = nullptr) {
    return adopt_request(
        try_make_send_request(conn_fd, data, len, std::move(cb), user_data)
    );
}

inline SendRequestPtr make_send_request(int conn_fd,
                                        const std::string& data,
                                        CompleteCallback cb = {},
                                        void* user_data = nullptr) {
    return make_send_request(
        conn_fd,
        data.data(),
        data.size(),
        std::move(cb),
        user_data
    );
}

inline CloseRequestPtr make_close_request(int conn_fd,
                                          CompleteCallback cb = {},
                                          void* user_data = nullptr) {
    return adopt_request(
        try_make_close_request(conn_fd, std::move(cb), user_data)
    );
}

inline WakeRequestPtr make_wake_request(int wake_fd) {
    return adopt_request(try_make_wake_request(wake_fd));
}

inline UdpRecvRequestPtr make_udp_recv_request(
    int socket_fd,
    std::size_t buffer_size = 2048,
    CompleteCallback cb = {},
    void* user_data = nullptr) {
    return adopt_request(
        try_make_udp_recv_request(
            socket_fd,
            buffer_size,
            std::move(cb),
            user_data
        )
    );
}

inline UdpSendRequestPtr make_udp_send_request(
    int socket_fd,
    const sockaddr* peer,
    socklen_t peer_len,
    const char* data,
    std::size_t len,
    CompleteCallback cb = {},
    void* user_data = nullptr) {
    return adopt_request(
        try_make_udp_send_request(
            socket_fd,
            peer,
            peer_len,
            data,
            len,
            std::move(cb),
            user_data
        )
    );
}

inline UdpSendRequestPtr make_udp_send_request(
    int socket_fd,
    const sockaddr* peer,
    socklen_t peer_len,
    const std::string& data,
    CompleteCallback cb = {},
    void* user_data = nullptr) {
    return adopt_request(
        try_make_udp_send_request(
            socket_fd,
            peer,
            peer_len,
            data,
            std::move(cb),
            user_data
        )
    );
}

inline TimeoutRequestPtr make_timeout_request(
    std::uint64_t timeout_ms,
    CompleteCallback cb = {},
    void* user_data = nullptr) {
    return adopt_request(
        try_make_timeout_request(
            timeout_ms,
            std::move(cb),
            user_data
        )
    );
}


#endif
