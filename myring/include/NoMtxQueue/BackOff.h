#ifndef BACK_OFF_H
#define BACK_OFF_H

#include <atomic>
#include <chrono>
#include <thread>

// cpu_relax：短暂自旋时使用，减少CPU压力
#if defined(__x86_64__) || defined(__i386__)
    #include <immintrin.h>

    inline void cpu_relax() {
        _mm_pause();
    }

#elif defined(__aarch64__) || defined(__arm__)
    inline void cpu_relax() {
        __asm__ __volatile__("yield" ::: "memory");
    }

#else
    inline void cpu_relax() {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
#endif

// Backoff：退避策略
//
// 用法：
// Backoff backoff;
// while (!queue.try_push(value)) {
//     backoff.pause();
// }
class Backoff {
public:
    Backoff()
        : count_(0) {}

    void reset() {
        count_ = 0;
    }

    void pause() {
        if (count_ < SPIN_LIMIT) {
            int spin_count = 1 << count_;

            for (int i = 0; i < spin_count; ++i) {
                cpu_relax();
            }

            ++count_;
        } else if (count_ < YIELD_LIMIT) {
            ++count_;
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }

private:
    static const int SPIN_LIMIT = 16;
    static const int YIELD_LIMIT = 32;

    int count_;
};
#endif