#ifndef MYRING_PROACTOR_PROACTOR_HPP
#define MYRING_PROACTOR_PROACTOR_HPP

#include <liburing.h>

#include <cstddef>
#include <memory>
#include <vector>

#include "Proactor/Request.hpp"
#include "nocopyable.h"

class Proactor : private nocopyable {
public:
    explicit Proactor(unsigned entries, std::size_t max_events = 64);
    ~Proactor();

    io_uring_sqe* get_sqe() noexcept;

    // prep_* 只负责填 SQE，不负责自动 submit。
    // 调用方可以先批量 prep，再显式调用 submit()。
    bool prep_accept(AcceptRequest* req);
    bool prep_recv(RecvRequest* req);
    bool prep_send(SendRequest* req);
    bool prep_udp_recv(UdpRecvRequest* req);
    bool prep_udp_send(UdpSendRequest* req);
    bool prep_timeout(TimeoutRequest* req);

    bool prep_wakeup(WakeRequest* req);
    bool submit() noexcept;

    // peek_batch / wait_one 会把 cqe->res 和 cqe->flags
    // 写回对应 RequestContext 的 io_result / cqe_flags，
    // 然后在 Proactor 内部完成 advance/seen。
    unsigned peek_batch() noexcept;
    bool wait_one() noexcept;

    RequestContext* request_at(std::size_t index) const noexcept;
    const std::vector<RequestContext*>& ready_requests() const noexcept;
    std::size_t ready_count() const noexcept;

    std::size_t max_events() const noexcept;

    
private:
    std::unique_ptr<io_uring> ring_;
    std::size_t max_events_;
    std::vector<io_uring_cqe*> cqes_;
    std::vector<RequestContext*> ready_requests_;
    std::size_t ready_count_;
};

#endif
