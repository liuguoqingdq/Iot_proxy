#include "Proactor/LoopThreadPool.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <utility>

LoopThreadPool::LoopThreadPool(int listen_fd,
                               unsigned entries,
                               std::size_t max_events,
                               std::size_t threads,
                               std::size_t recv_buffer_size)
    : running_(false),
      acceptor_(new Proactor(entries, max_events)),
      accept_req_(make_accept_request(
          listen_fd,
          [this](RequestContext* ctx, int res) {
              this->handle_accept(ctx, res);
          }
      )),
      pool_(),
      next_thread_index_(0),
      recv_buffer_size_(recv_buffer_size),
      recv_callback_(),
      recv_request_factory_(),
      accept_result_callback_() {
    pool_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        pool_.push_back(std::unique_ptr<ProactorThread>(
            new ProactorThread(entries, max_events)
        ));
    }
}

LoopThreadPool::~LoopThreadPool() {
    stop();
}

void LoopThreadPool::set_recv_callback(CompleteCallback cb) {
    recv_callback_ = std::move(cb);
}

void LoopThreadPool::set_recv_request_factory(RecvRequestFactory factory) {
    recv_request_factory_ = std::move(factory);
}

void LoopThreadPool::set_accept_result_callback(AcceptResultCallback cb) {
    accept_result_callback_ = std::move(cb);
}

void LoopThreadPool::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }

    for (std::size_t i = 0; i < pool_.size(); ++i) {
        pool_[i]->start_loop();
    }

    if (!rearm_accept()) {
        running_.store(false, std::memory_order_release);
        for (std::size_t i = 0; i < pool_.size(); ++i) {
            pool_[i]->stop();
        }
        return;
    }

    accept_thread_ = std::thread(&LoopThreadPool::run_in_loop, this);
}

void LoopThreadPool::stop() {
    running_.store(false, std::memory_order_release);

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    for (std::size_t i = 0; i < pool_.size(); ++i) {
        pool_[i]->stop();
    }
}

void LoopThreadPool::run_in_loop() {
    while (running_.load(std::memory_order_acquire)) {
        unsigned ready = acceptor_->peek_batch();
        if (ready == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            continue;
        }

        const std::size_t count = acceptor_->ready_count();
        for (std::size_t i = 0; i < count; ++i) {
            RequestContext* reqctx = acceptor_->request_at(i);
            if (reqctx == nullptr) {
                continue;
            }

            invoke_request_callback(reqctx, reqctx->io_result);
        }
    }
}

std::size_t LoopThreadPool::thread_count() const noexcept {
    return pool_.size();
}

void LoopThreadPool::handle_accept(RequestContext* ctx, int res) {
    AcceptRequest* accept_req = static_cast<AcceptRequest*>(ctx);
    if (accept_req == nullptr) {
        return;
    }

    if (running_.load(std::memory_order_acquire)) {
        rearm_accept();
    }

    if (res < 0) {
        return;
    }

    ProactorThread* worker = pick_thread();
    if (worker == nullptr) {
        ::close(res);
        return;
    }

    RecvRequestPtr recv_req;
    if (recv_request_factory_) {
        recv_req = recv_request_factory_(res, worker);
    } else {
        recv_req = make_recv_request(
            res,
            recv_buffer_size_,
            recv_callback_
        );
    }

    const bool pushed = recv_req && worker->push(std::move(recv_req));
    if (accept_result_callback_) {
        accept_result_callback_(res, worker, pushed);
    }

    if (!pushed) {
        ::close(res);
    }
}

ProactorThread* LoopThreadPool::pick_thread() noexcept {
    if (pool_.empty()) {
        return nullptr;
    }

    const std::size_t index = next_thread_index_ % pool_.size();
    next_thread_index_ = (index + 1) % pool_.size();
    return pool_[index].get();
}

bool LoopThreadPool::rearm_accept() {
    if (!accept_req_) {
        return false;
    }

    accept_req_->client_len = sizeof(accept_req_->client_addr);
    std::memset(&accept_req_->client_addr, 0, sizeof(accept_req_->client_addr));

    if (!acceptor_->prep_accept(accept_req_.get())) {
        return false;
    }

    return acceptor_->submit();
}
