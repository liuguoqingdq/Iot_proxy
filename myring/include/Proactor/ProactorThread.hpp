#ifndef MYRING_PROACTOR_PROACTOR_THREAD_HPP
#define MYRING_PROACTOR_PROACTOR_THREAD_HPP

#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>

#include "NoMtxQueue/NoMtxQueue.hpp"
#include "Proactor/Proactor.hpp"
#include "Proactor/Request.hpp"
#include "nocopyable.h"

class ProactorThread : private nocopyable {
public:
    static constexpr std::size_t kQueueCapacity = 4096;

public:
    explicit ProactorThread(unsigned entries, std::size_t max_events = 64);
    ~ProactorThread();

    void start_loop();
    void stop();
    void loop();

    bool push(RequestContextPtr req);

    std::size_t ready_count() const noexcept;
    RequestContext* request_at(std::size_t index) const noexcept;

private:
    bool arm_wakeup();
    void notify_wakeup() noexcept;
    void handle_ready_requests();
    void handle_wakeup(RequestContext* reqctx);
    bool pop(RequestContextPtr& req);
    bool submit_request(RequestContextPtr req);
    void drain_submissions();
    void drain_completions();
    void process(RequestContext* reqctx);
    void resubmit_owner(RequestContextPtr req);

private:
    std::atomic<bool> running_;
    std::thread thread_;
    bool wakeup_armed_;
    int wake_fd_;
    WakeRequestPtr wake_request_;
    std::unordered_map<RequestContext*, RequestContextPtr> inflight_;
    MpmcBoundedQueue<RequestContextPtr, kQueueCapacity> queue_;
    std::unique_ptr<Proactor> proactor_;
};

#endif
