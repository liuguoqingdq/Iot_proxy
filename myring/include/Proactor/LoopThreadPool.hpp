#ifndef MYRING_PROACTOR_LOOP_THREAD_POOL_HPP
#define MYRING_PROACTOR_LOOP_THREAD_POOL_HPP

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "Proactor/Proactor.hpp"
#include "Proactor/ProactorThread.hpp"
#include "Proactor/Request.hpp"
#include "nocopyable.h"

class LoopThreadPool : private nocopyable {
public:
    using RecvRequestFactory =
        std::function<RecvRequestPtr(int, ProactorThread*)>;
    using AcceptResultCallback =
        std::function<void(int, ProactorThread*, bool)>;

    explicit LoopThreadPool(int listen_fd,
                            unsigned entries = 256,
                            std::size_t max_events = 64,
                            std::size_t threads = 1,
                            std::size_t recv_buffer_size = 4096);
    ~LoopThreadPool();

    void set_recv_callback(CompleteCallback cb);
    void set_recv_request_factory(RecvRequestFactory factory);
    void set_accept_result_callback(AcceptResultCallback cb);

    void start();
    void stop();
    void run_in_loop();

    std::size_t thread_count() const noexcept;

private:
    void handle_accept(RequestContext* ctx, int res);
    ProactorThread* pick_thread() noexcept;
    bool rearm_accept();

private:
    std::atomic<bool> running_;
    std::thread accept_thread_;
    std::unique_ptr<Proactor> acceptor_;
    AcceptRequestPtr accept_req_;
    std::vector<std::unique_ptr<ProactorThread> > pool_;
    std::size_t next_thread_index_;
    std::size_t recv_buffer_size_;
    CompleteCallback recv_callback_;
    RecvRequestFactory recv_request_factory_;
    AcceptResultCallback accept_result_callback_;
};

#endif
