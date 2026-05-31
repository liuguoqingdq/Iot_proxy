#ifndef MINI_TCMALLOC_THREAD_CACHE_HPP
#define MINI_TCMALLOC_THREAD_CACHE_HPP

#include <array>
#include <cstddef>

#include "Common.hpp"
#include "FreeList.hpp"
#include "SizeClass.hpp"

namespace mini_tcmalloc {

/*
    ThreadCache 是 MiniTCMalloc 的前端缓存。

    每个线程拥有一个 ThreadCache。
    每个 ThreadCache 为每个 size class 维护一条本地 FreeList。

    这版新增：
        cached_bytes_:
            当前线程本地缓存总字节数。

        cache_limit_bytes_:
            当前线程允许缓存的最大字节数。

        scavenge():
            本地缓存超限时，批量归还对象给 CentralCache。
*/
class ThreadCache {
public:
    static ThreadCache& current();

    void* allocate(std::size_t size);

    void deallocate(void* ptr, std::size_t size) noexcept;

    void release_all() noexcept;

    std::size_t local_free_count(std::size_t class_index) const noexcept;

    std::size_t total_local_free_count() const noexcept;

    /*
        当前ThreadCache本地缓存了多少字节。
    */
    std::size_t cached_bytes() const noexcept;

    /*
        当前ThreadCache缓存上限。
    */
    std::size_t cache_limit_bytes() const noexcept;

    /*
        调整当前ThreadCache缓存上限。

        例如：
            ThreadCache::current().set_cache_limit_bytes(1024 * 1024);
    */
    void set_cache_limit_bytes(std::size_t bytes) noexcept;

    ThreadCache(const ThreadCache&) = delete;
    ThreadCache& operator=(const ThreadCache&) = delete;

    ~ThreadCache();

private:
    ThreadCache();

private:
    std::array<FreeList, SizeClass::kNumClasses> local_free_lists_;

    /*
        当前线程本地缓存的总字节数。

        注意：
            只统计已经在local_free_lists_里的空闲对象。
            已经分配给用户的对象不算。
    */
    std::size_t cached_bytes_;

    /*
        当前线程缓存上限。
    */
    std::size_t cache_limit_bytes_;

private:
    void* fetch_from_central(std::size_t class_index);

    void release_to_central(std::size_t class_index) noexcept;

    /*
        本地缓存超限时触发。

        它会从本地FreeList中批量弹出对象，
        归还给CentralCache。
    */
    void scavenge() noexcept;

    /*
        某个size class自己的本地链表上限。

        这是“局部限制”。
        cached_bytes_是“整体限制”。
    */
    static std::size_t max_local_count(std::size_t class_index) noexcept;
};

} // namespace mini_tcmalloc

#endif