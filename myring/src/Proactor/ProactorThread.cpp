#include "Proactor/ProactorThread.hpp"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

int create_wake_fd() {
    int fd = ::eventfd(0, EFD_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error(
            std::string("eventfd failed: ") + std::strerror(errno)
        );
    }

    return fd;
}

}  // namespace

ProactorThread::ProactorThread(unsigned entries, std::size_t max_events)
    : running_(false),
      thread_(),
      wakeup_armed_(false),
      wake_fd_(-1),
      wake_request_(),
      inflight_(),
      queue_(),
      proactor_(new Proactor(entries, max_events)) {
    try {
        wake_fd_ = create_wake_fd();
        wake_request_ = make_wake_request(wake_fd_);
    } catch (...) {
        if (wake_fd_ >= 0) {
            ::close(wake_fd_);
        }
        throw;
    }
}

ProactorThread::~ProactorThread() {
    stop();

    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
}

void ProactorThread::start_loop() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    thread_ = std::thread(&ProactorThread::loop, this);
}

void ProactorThread::stop() {
    const bool was_running =
        running_.exchange(false, std::memory_order_acq_rel);

    if (was_running) {
        notify_wakeup();
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void ProactorThread::loop() {
    while (running_.load(std::memory_order_acquire)) {
        if (!wakeup_armed_ && !arm_wakeup()) {
            running_.store(false, std::memory_order_release);
            break;
        }

        if (!proactor_->wait_one()) {
            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }

        handle_ready_requests();

        while (proactor_->peek_batch() > 0) {
            handle_ready_requests();
        }
    }
}

bool ProactorThread::push(RequestContextPtr req) {
    if (!req) {
        return false;
    }

    queue_.push_wait(std::move(req));
    notify_wakeup();
    return true;
}

std::size_t ProactorThread::ready_count() const noexcept {
    return proactor_->ready_count();
}

RequestContext* ProactorThread::request_at(std::size_t index) const noexcept {
    return proactor_->request_at(index);
}

bool ProactorThread::arm_wakeup() {
    if (!wake_request_ || wakeup_armed_) {
        return false;
    }

    if (!proactor_->prep_wakeup(wake_request_.get())) {
        return false;
    }

    if (!proactor_->submit()) {
        return false;
    }

    wakeup_armed_ = true;
    return true;
}

void ProactorThread::notify_wakeup() noexcept {
    if (wake_fd_ < 0) {
        return;
    }

    const std::uint64_t one = 1;
    const ssize_t written = ::write(wake_fd_, &one, sizeof(one));
    (void)written;
}

void ProactorThread::handle_ready_requests() {
    const std::size_t count = proactor_->ready_count();
    for (std::size_t i = 0; i < count; ++i) {
        RequestContext* reqctx = proactor_->request_at(i);
        if (reqctx == nullptr) {
            continue;
        }

        if (reqctx->type == RequestType::WAKEUP) {
            handle_wakeup(reqctx);
            continue;
        }

        process(reqctx);
    }
}

void ProactorThread::handle_wakeup(RequestContext* reqctx) {
    if (reqctx != wake_request_.get()) {
        return;
    }

    wakeup_armed_ = false;

    if (running_.load(std::memory_order_acquire)) {
        drain_submissions();
    }
}

bool ProactorThread::pop(RequestContextPtr& req) {
    return queue_.try_pop(req);
}

bool ProactorThread::submit_request(RequestContextPtr req) {
    if (!req) {
        return false;
    }

    RequestContext* raw = req.get();
    bool prepared = false;

    switch (raw->type) {
        case RequestType::ACCEPT:
            prepared = proactor_->prep_accept(static_cast<AcceptRequest*>(raw));
            break;
        case RequestType::RECV:
            prepared = proactor_->prep_recv(static_cast<RecvRequest*>(raw));
            break;
        case RequestType::SEND:
            prepared = proactor_->prep_send(static_cast<SendRequest*>(raw));
            break;
        case RequestType::CLOSE:
            invoke_request_callback(raw, raw->io_result);
            return true;
        case RequestType::WAKEUP:
            return false;
        case RequestType::UDP_RECV:
            prepared = proactor_->prep_udp_recv(static_cast<UdpRecvRequest*>(raw));
            break;
        case RequestType::UDP_SEND:
            prepared = proactor_->prep_udp_send(static_cast<UdpSendRequest*>(raw));
            break;
        case RequestType::TIMEOUT:
            prepared = proactor_->prep_timeout(static_cast<TimeoutRequest*>(raw));
            break;
        default:
            return false;
    }

    if (!prepared || !proactor_->submit()) {
        return false;
    }

    inflight_.emplace(raw, std::move(req));
    return true;
}

void ProactorThread::drain_submissions() {
    RequestContextPtr req;

    while (pop(req)) {
        submit_request(std::move(req));
    }
}

void ProactorThread::drain_completions() {
    handle_ready_requests();
}

void ProactorThread::process(RequestContext* reqctx) {
    if (reqctx == nullptr) {
        return;
    }

    auto it = inflight_.find(reqctx);
    if (it == inflight_.end()) {
        return;
    }

    RequestContextPtr owner = std::move(it->second);
    inflight_.erase(it);

    switch (reqctx->type) {
        case RequestType::ACCEPT: {
            invoke_request_callback(reqctx, reqctx->io_result);

            if (running_.load(std::memory_order_acquire)) {
                resubmit_owner(std::move(owner));
            }
            return;
        }

        case RequestType::RECV: {
            if (reqctx->io_result > 0) {
                static_cast<RecvRequest*>(reqctx)->buffer.set_length(
                    static_cast<std::size_t>(reqctx->io_result)
                );
            }

            invoke_request_callback(reqctx, reqctx->io_result);
            return;
        }

        case RequestType::SEND: {
            SendRequest* send_req = static_cast<SendRequest*>(reqctx);

            if (reqctx->io_result > 0) {
                send_req->advance(static_cast<std::size_t>(reqctx->io_result));
            }

            if (reqctx->io_result > 0 && !send_req->finished()) {
                resubmit_owner(std::move(owner));
                return;
            }

            invoke_request_callback(reqctx, reqctx->io_result);
            return;
        }

        case RequestType::CLOSE:
            invoke_request_callback(reqctx, reqctx->io_result);
            return;
        case RequestType::UDP_RECV: {
            if (reqctx->io_result > 0) {
                static_cast<UdpRecvRequest*>(reqctx)->buffer.set_length(
                    static_cast<std::size_t>(reqctx->io_result)
                );
            }

            invoke_request_callback(reqctx, reqctx->io_result);
            return;
        }

        case RequestType::UDP_SEND:
            invoke_request_callback(reqctx, reqctx->io_result);
            return;

        case RequestType::TIMEOUT:
            invoke_request_callback(reqctx, reqctx->io_result);
            return;
        default:
            return;
    }
}

void ProactorThread::resubmit_owner(RequestContextPtr req) {
    submit_request(std::move(req));
}
