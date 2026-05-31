#include "Proactor/Proactor.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

Proactor::Proactor(unsigned entries, std::size_t max_events)
    : ring_(new io_uring),
      max_events_(max_events == 0 ? 1 : max_events),
      cqes_(max_events_),
      ready_requests_(max_events_, nullptr),
      ready_count_(0) {
    io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags = 0;

    int ret = io_uring_queue_init_params(entries, ring_.get(), &params);
    if (ret < 0) {
        throw std::runtime_error(
            std::string("io_uring_queue_init_params failed: ") +
            std::strerror(-ret)
        );
    }
}

Proactor::~Proactor() {
    if (ring_) {
        io_uring_queue_exit(ring_.get());
    }
}

io_uring_sqe* Proactor::get_sqe() noexcept {
    return io_uring_get_sqe(ring_.get());
}

bool Proactor::prep_accept(AcceptRequest* req) {
    if (req == nullptr) {
        return false;
    }

    io_uring_sqe* sqe = get_sqe();
    if (sqe == nullptr) {
        return false;
    }

    io_uring_prep_accept(
        sqe,
        req->fd,
        req->addr_ptr(),
        req->len_ptr(),
        SOCK_NONBLOCK | SOCK_CLOEXEC
    );
    io_uring_sqe_set_data(sqe, req);

    return true;
}

bool Proactor::prep_recv(RecvRequest* req) {
    if (req == nullptr) {
        return false;
    }

    io_uring_sqe* sqe = get_sqe();
    if (sqe == nullptr) {
        return false;
    }

    io_uring_prep_recv(
        sqe,
        req->fd,
        req->data(),
        req->size(),
        req->flags
    );
    io_uring_sqe_set_data(sqe, req);

    return true;
}

bool Proactor::prep_send(SendRequest* req) {
    if (req == nullptr || req->remaining() == 0) {
        return false;
    }

    io_uring_sqe* sqe = get_sqe();
    if (sqe == nullptr) {
        return false;
    }

    io_uring_prep_send(
        sqe,
        req->fd,
        req->data(),
        req->remaining(),
        req->flags
    );
    io_uring_sqe_set_data(sqe, req);
    
    return true;
}

bool Proactor::prep_udp_recv(UdpRecvRequest* req){
    if(req == nullptr){
        return false;
    }
    io_uring_sqe* sqe = get_sqe();
    if(sqe == nullptr){
        return false;
    }
    req->reset_message();
    io_uring_prep_recvmsg(
        sqe,
        req->fd,
        req->msg_ptr(),
        req->flags
    );
    io_uring_sqe_set_data(sqe,req);
    return true;
}
bool Proactor::prep_udp_send(UdpSendRequest* req){
    if(req == nullptr){
        return false;
    }
    io_uring_sqe* sqe = get_sqe();
    if(sqe == nullptr){
        return false;
    }
    req->reset_message();
    io_uring_prep_sendmsg(
        sqe,
        req->fd,
        req->msg_ptr(),
        req->flags
    );
    io_uring_sqe_set_data(sqe,req);
    return true;
}
bool Proactor::prep_timeout(TimeoutRequest* req) {
    if (req == nullptr) {
        return false;
    }

    io_uring_sqe* sqe = get_sqe();
    if (sqe == nullptr) {
        return false;
    }

    io_uring_prep_timeout(
        sqe,
        req->timeout_ptr(),
        0,
        0
    );
    io_uring_sqe_set_data(sqe, req);
    return true;
}



bool Proactor::prep_wakeup(WakeRequest* req){
    if (req == nullptr) {
        return false;
    }

    io_uring_sqe* sqe = get_sqe();
    if (sqe == nullptr) {
        return false;
    }

    req->value = 0;

    io_uring_prep_read(
        sqe,
        req->fd,
        &req->value,
        sizeof(req->value),
        0
    );
    io_uring_sqe_set_data(sqe, req);

    return true;
}
bool Proactor::submit() noexcept {
    return io_uring_submit(ring_.get()) >= 0;
}

unsigned Proactor::peek_batch() noexcept {
    ready_count_ = 0;

    unsigned count = io_uring_peek_batch_cqe(
        ring_.get(),
        cqes_.data(),
        static_cast<unsigned>(max_events_)
    );

    if (count == 0) {
        return 0;
    }

    for (unsigned i = 0; i < count; ++i) {
        io_uring_cqe* cqe = cqes_[i];
        ready_requests_[i] =
            static_cast<RequestContext*>(io_uring_cqe_get_data(cqe));

        if (ready_requests_[i] != nullptr) {
            ready_requests_[i]->io_result = cqe->res;
            ready_requests_[i]->cqe_flags = cqe->flags;
        }
    }

    ready_count_ = count;
    io_uring_cq_advance(ring_.get(), count);
    return count;
}

bool Proactor::wait_one() noexcept {
    ready_count_ = 0;

    io_uring_cqe* cqe = nullptr;
    int ret = io_uring_wait_cqe(ring_.get(), &cqe);
    if (ret < 0) {
        return false;
    }

    ready_requests_[0] =
        static_cast<RequestContext*>(io_uring_cqe_get_data(cqe));

    if (ready_requests_[0] != nullptr) {
        ready_requests_[0]->io_result = cqe->res;
        ready_requests_[0]->cqe_flags = cqe->flags;
    }

    ready_count_ = 1;
    io_uring_cqe_seen(ring_.get(), cqe);
    return true;
}

RequestContext* Proactor::request_at(std::size_t index) const noexcept {
    if (index >= ready_count_) {
        return nullptr;
    }
    return ready_requests_[index];
}

const std::vector<RequestContext*>& Proactor::ready_requests() const noexcept {
    return ready_requests_;
}

std::size_t Proactor::ready_count() const noexcept {
    return ready_count_;
}

std::size_t Proactor::max_events() const noexcept {
    return max_events_;
}
