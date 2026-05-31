template <typename T, std::size_t Capacity>
MpmcBoundedQueue<T, Capacity>::MpmcBoundedQueue()
    : dequeue_pos_(0),
      enqueue_pos_(0) {
    for (std::size_t i = 0; i < Capacity; ++i) {
        buffer_[i].sequence.store(static_cast<Counter>(i),
                                  std::memory_order_relaxed);
    }
}

template <typename T, std::size_t Capacity>
MpmcBoundedQueue<T, Capacity>::~MpmcBoundedQueue() {
    Counter head = dequeue_pos_.load(std::memory_order_relaxed);
    Counter tail = enqueue_pos_.load(std::memory_order_relaxed);

    while (head != tail) {
        Cell* cell = &buffer_[index_of(head)];
        ptr_at(cell)->~T();
        ++head;
    }
}

template <typename T, std::size_t Capacity>
bool MpmcBoundedQueue<T, Capacity>::try_push(const T& value) {
    return try_emplace_impl(value);
}

template <typename T, std::size_t Capacity>
bool MpmcBoundedQueue<T, Capacity>::try_push(T&& value) {
    return try_emplace_impl(std::move(value));
}

template <typename T, std::size_t Capacity>
template <typename... Args>
bool MpmcBoundedQueue<T, Capacity>::try_emplace(Args&&... args) {
    return try_emplace_impl(std::forward<Args>(args)...);
}

template <typename T, std::size_t Capacity>
bool MpmcBoundedQueue<T, Capacity>::try_pop(T& value) {
    Cell* cell = nullptr;
    Counter pos = dequeue_pos_.load(std::memory_order_relaxed);

    while (true) {
        cell = &buffer_[index_of(pos)];

        Counter seq = cell->sequence.load(std::memory_order_acquire);
        Counter expected_seq = pos + 1;

        if (seq == expected_seq) {
            if (dequeue_pos_.compare_exchange_weak(
                    pos,
                    pos + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                break;
            }
        } else if (before(seq, expected_seq)) {
            return false;
        } else {
            pos = dequeue_pos_.load(std::memory_order_relaxed);
        }
    }

    T* item = ptr_at(cell);
    value = std::move(*item);
    item->~T();

    cell->sequence.store(pos + static_cast<Counter>(Capacity),
                         std::memory_order_release);

    return true;
}

template <typename T, std::size_t Capacity>
void MpmcBoundedQueue<T, Capacity>::push_wait(const T& value) {
    Backoff backoff;

    while (!try_push(value)) {
        backoff.pause();
    }
}

template <typename T, std::size_t Capacity>
void MpmcBoundedQueue<T, Capacity>::push_wait(T&& value) {
    Backoff backoff;

    while (!try_push(std::move(value))) {
        backoff.pause();
    }
}

template <typename T, std::size_t Capacity>
template <typename... Args>
void MpmcBoundedQueue<T, Capacity>::emplace_wait(Args&&... args) {
    Backoff backoff;

    while (!try_emplace(std::forward<Args>(args)...)) {
        backoff.pause();
    }
}

template <typename T, std::size_t Capacity>
void MpmcBoundedQueue<T, Capacity>::pop_wait(T& value) {
    Backoff backoff;

    while (!try_pop(value)) {
        backoff.pause();
    }
}

template <typename T, std::size_t Capacity>
template <typename StopPredicate>
bool MpmcBoundedQueue<T, Capacity>::pop_wait(T& value,
                                             StopPredicate should_stop) {
    Backoff backoff;

    while (true) {
        if (try_pop(value)) {
            return true;
        }

        if (should_stop()) {
            return false;
        }

        backoff.pause();
    }
}

template <typename T, std::size_t Capacity>
bool MpmcBoundedQueue<T, Capacity>::empty() const {
    Counter head = dequeue_pos_.load(std::memory_order_acquire);
    Counter tail = enqueue_pos_.load(std::memory_order_acquire);

    return head == tail;
}

template <typename T, std::size_t Capacity>
bool MpmcBoundedQueue<T, Capacity>::full() const {
    Counter head = dequeue_pos_.load(std::memory_order_acquire);
    Counter tail = enqueue_pos_.load(std::memory_order_acquire);

    return tail - head >= static_cast<Counter>(Capacity);
}

template <typename T, std::size_t Capacity>
template <typename... Args>
bool MpmcBoundedQueue<T, Capacity>::try_emplace_impl(Args&&... args) {
    static_assert(std::is_nothrow_constructible<T, Args&&...>::value,
                  "T must be nothrow constructible with these arguments");

    Cell* cell = nullptr;
    Counter pos = enqueue_pos_.load(std::memory_order_relaxed);

    while (true) {
        cell = &buffer_[index_of(pos)];

        Counter seq = cell->sequence.load(std::memory_order_acquire);

        if (seq == pos) {
            if (enqueue_pos_.compare_exchange_weak(
                    pos,
                    pos + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                break;
            }
        } else if (before(seq, pos)) {
            return false;
        } else {
            pos = enqueue_pos_.load(std::memory_order_relaxed);
        }
    }

    new (static_cast<void*>(&cell->storage)) T(std::forward<Args>(args)...);

    cell->sequence.store(pos + 1, std::memory_order_release);

    return true;
}

template <typename T, std::size_t Capacity>
constexpr std::size_t
MpmcBoundedQueue<T, Capacity>::index_of(Counter sequence) {
    return static_cast<std::size_t>(
        sequence & static_cast<Counter>(Capacity - 1)
    );
}

template <typename T, std::size_t Capacity>
constexpr bool
MpmcBoundedQueue<T, Capacity>::before(Counter a, Counter b) {
    return (a - b) >= ((std::numeric_limits<Counter>::max() / 2) + 1);
}

template <typename T, std::size_t Capacity>
T* MpmcBoundedQueue<T, Capacity>::ptr_at(Cell* cell) {
    return reinterpret_cast<T*>(&cell->storage);
}

template <typename T, std::size_t Capacity>
const T* MpmcBoundedQueue<T, Capacity>::ptr_at(const Cell* cell) const {
    return reinterpret_cast<const T*>(&cell->storage);
}
