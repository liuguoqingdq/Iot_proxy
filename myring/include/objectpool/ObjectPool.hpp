#ifndef OBJECT_POOL_HPP
#define OBJECT_POOL_HPP

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "nocopyable.h"
#include "objectpool/ObjectPoolOptions.hpp"
#include "objectpool/ObjectPoolStats.hpp"
#include "tcmalloc/MiniMalloc.hpp"

namespace pool {

template <typename T>
class ObjectPool : public nocopyable {
public:
    static_assert(!std::is_array<T>::value,
                  "ObjectPool does not support array type");
    static_assert(!std::is_void<T>::value,
                  "ObjectPool does not support void type");

    struct Deleter {
        ObjectPool* pool = nullptr;

        void operator()(T* obj) const noexcept {
            if (pool != nullptr && obj != nullptr) {
                pool->destroy(obj);
            }
        }
    };

    using UniquePtr = std::unique_ptr<T, Deleter>;

public:
    explicit ObjectPool(ObjectPoolOptions options = {})
        : options_(std::move(options)) {
        if (options_.prewarm_on_construct && options_.prewarm_count > 0) {
            if (!prewarm(options_.prewarm_count)) {
                prewarm_failed_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    explicit ObjectPool(std::string name, std::size_t max_alive = 0)
        : ObjectPool(ObjectPoolOptions{std::move(name), max_alive, false, 0}) {}

    ~ObjectPool() {
        assert(alive_count_.load(std::memory_order_relaxed) == 0 &&
               "ObjectPool destroyed while objects are still alive");
    }

    template <typename... Args>
    T* try_make(Args&&... args) {
        if (!try_acquire_alive_slot()) {
            limit_rejected_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        void* mem = mini_tcmalloc::mini_malloc(sizeof(T));
        if (mem == nullptr) {
            release_alive_slot();
            allocation_failed_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        T* obj = nullptr;
        try {
            obj = new (mem) T(std::forward<Args>(args)...);
        } catch (...) {
            mini_tcmalloc::mini_free(mem);
            release_alive_slot();
            allocation_failed_.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }

        created_count_.fetch_add(1, std::memory_order_relaxed);
        update_peak_alive();
        return obj;
    }

    template <typename... Args>
    T* make(Args&&... args) {
        T* obj = try_make(std::forward<Args>(args)...);
        if (obj == nullptr) {
            throw std::bad_alloc();
        }
        return obj;
    }

    void destroy(T* obj) noexcept {
        if (obj == nullptr) {
            return;
        }

        try {
            obj->~T();
        } catch (...) {
            std::terminate();
        }

        mini_tcmalloc::mini_free(obj);
        destroyed_count_.fetch_add(1, std::memory_order_relaxed);
        release_alive_slot();
    }

    template <typename... Args>
    UniquePtr try_make_unique(Args&&... args) {
        T* obj = try_make(std::forward<Args>(args)...);
        return UniquePtr(obj, Deleter{this});
    }

    template <typename... Args>
    UniquePtr make_unique(Args&&... args) {
        return UniquePtr(make(std::forward<Args>(args)...), Deleter{this});
    }

    bool prewarm(std::size_t count) {
        std::vector<void*> blocks;

        try {
            blocks.reserve(count);
        } catch (...) {
            prewarm_failed_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        for (std::size_t i = 0; i < count; ++i) {
            void* block = mini_tcmalloc::mini_malloc(sizeof(T));
            if (block == nullptr) {
                for (void* ptr : blocks) {
                    mini_tcmalloc::mini_free(ptr);
                }
                prewarm_failed_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            blocks.push_back(block);
        }

        for (void* ptr : blocks) {
            mini_tcmalloc::mini_free(ptr);
        }

        return true;
    }

    ObjectPoolStats stats() const {
        ObjectPoolStats s;
        s.name = options_.name;
        s.created = created_count_.load(std::memory_order_relaxed);
        s.destroyed = destroyed_count_.load(std::memory_order_relaxed);
        s.alive = alive_count_.load(std::memory_order_relaxed);
        s.peak_alive = peak_alive_count_.load(std::memory_order_relaxed);
        s.allocation_failed = allocation_failed_.load(std::memory_order_relaxed);
        s.limit_rejected = limit_rejected_.load(std::memory_order_relaxed);
        s.prewarm_failed = prewarm_failed_.load(std::memory_order_relaxed);
        return s;
    }

    const std::string& name() const noexcept {
        return options_.name;
    }

    std::size_t max_alive() const noexcept {
        return options_.max_alive;
    }

    std::size_t created_count() const noexcept {
        return created_count_.load(std::memory_order_relaxed);
    }

    std::size_t destroyed_count() const noexcept {
        return destroyed_count_.load(std::memory_order_relaxed);
    }

    std::size_t alive_count() const noexcept {
        return alive_count_.load(std::memory_order_relaxed);
    }

    std::size_t peak_alive_count() const noexcept {
        return peak_alive_count_.load(std::memory_order_relaxed);
    }

    std::size_t allocation_failed_count() const noexcept {
        return allocation_failed_.load(std::memory_order_relaxed);
    }

    std::size_t limit_rejected_count() const noexcept {
        return limit_rejected_.load(std::memory_order_relaxed);
    }

    std::size_t prewarm_failed_count() const noexcept {
        return prewarm_failed_.load(std::memory_order_relaxed);
    }

private:
    bool try_acquire_alive_slot() noexcept {
        if (options_.max_alive == 0) {
            alive_count_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        std::size_t cur = alive_count_.load(std::memory_order_relaxed);
        while (true) {
            if (cur >= options_.max_alive) {
                return false;
            }

            if (alive_count_.compare_exchange_weak(
                    cur,
                    cur + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    void release_alive_slot() noexcept {
        const std::size_t old =
            alive_count_.fetch_sub(1, std::memory_order_relaxed);
        assert(old > 0);
        (void)old;
    }

    void update_peak_alive() noexcept {
        std::size_t alive = alive_count_.load(std::memory_order_relaxed);
        std::size_t peak = peak_alive_count_.load(std::memory_order_relaxed);

        while (alive > peak) {
            if (peak_alive_count_.compare_exchange_weak(
                    peak,
                    alive,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                break;
            }
        }
    }

private:
    ObjectPoolOptions options_;

    std::atomic<std::size_t> created_count_{0};
    std::atomic<std::size_t> destroyed_count_{0};
    std::atomic<std::size_t> alive_count_{0};
    std::atomic<std::size_t> peak_alive_count_{0};
    std::atomic<std::size_t> allocation_failed_{0};
    std::atomic<std::size_t> limit_rejected_{0};
    std::atomic<std::size_t> prewarm_failed_{0};
};

}  // namespace pool

#endif
