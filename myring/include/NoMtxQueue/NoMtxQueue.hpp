#ifndef NO_MTX_QUEUE_HPP
#define NO_MTX_QUEUE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

#include "BackOff.h"

template <typename T, std::size_t Capacity>
class MpmcBoundedQueue {
private:
    using Counter = std::uint64_t;

    using Storage = typename std::aligned_storage<
        sizeof(T),
        alignof(T)
    >::type;

    static constexpr std::size_t CACHE_LINE_SIZE = 64;

    static_assert(Capacity > 1, "Capacity must be greater than 1");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be power of two");
    static_assert(Capacity <= (std::numeric_limits<Counter>::max() / 2),
                  "Capacity is too large");
    static_assert(std::is_nothrow_destructible<T>::value,
                  "T must be nothrow destructible");
    static_assert(std::is_nothrow_move_assignable<T>::value,
                  "T must be nothrow move assignable");

private:
    struct Cell {
        std::atomic<Counter> sequence;
        Storage storage;
    };

public:
    MpmcBoundedQueue();
    ~MpmcBoundedQueue();

    MpmcBoundedQueue(const MpmcBoundedQueue&) = delete;
    MpmcBoundedQueue& operator=(const MpmcBoundedQueue&) = delete;

    MpmcBoundedQueue(MpmcBoundedQueue&&) = delete;
    MpmcBoundedQueue& operator=(MpmcBoundedQueue&&) = delete;

public:
    bool try_push(const T& value);
    bool try_push(T&& value);

    template <typename... Args>
    bool try_emplace(Args&&... args);

    bool try_pop(T& value);

public:
    void push_wait(const T& value);
    void push_wait(T&& value);

    template <typename... Args>
    void emplace_wait(Args&&... args);

    void pop_wait(T& value);

    template <typename StopPredicate>
    bool pop_wait(T& value, StopPredicate should_stop);

public:
    bool empty() const;
    bool full() const;

private:
    template <typename... Args>
    bool try_emplace_impl(Args&&... args);

    static constexpr std::size_t index_of(Counter sequence);
    static constexpr bool before(Counter a, Counter b);

    T* ptr_at(Cell* cell);
    const T* ptr_at(const Cell* cell) const;

private:
    Cell buffer_[Capacity];

    alignas(CACHE_LINE_SIZE) std::atomic<Counter> dequeue_pos_;
    alignas(CACHE_LINE_SIZE) std::atomic<Counter> enqueue_pos_;
};

#include "NoMtxQueue.tpp"

#endif
