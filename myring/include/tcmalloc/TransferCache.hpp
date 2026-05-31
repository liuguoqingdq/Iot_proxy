#ifndef MINI_TCMALLOC_TRANSFER_CACHE_HPP
#define MINI_TCMALLOC_TRANSFER_CACHE_HPP

#include <array>
#include <cstddef>
#include <mutex>
#include <vector>

#include "FreeList.hpp"
#include "SizeClass.hpp"

namespace mini_tcmalloc {

/*
    TransferCache 位于 ThreadCache 和 CentralCache 之间。

    它缓存的是“一批对象”，也就是 FreeList::Range。

    ThreadCache 本地为空时：
        先找 TransferCache 拿一批对象
        TransferCache 也没有，才找 CentralCache

    ThreadCache 本地缓存太多时：
        先把一批对象交给 TransferCache
        TransferCache 满了，再交给 CentralCache

    这样可以减少 CentralCache 的锁竞争。
*/
class TransferCache {
public:
    static TransferCache& instance();

    /*
        从 TransferCache 获取一批对象。

        如果 TransferCache 里没有，就从 CentralCache 获取。
    */
    FreeList::Range fetch_range(std::size_t class_index, std::size_t n);

    /*
        释放一批对象到 TransferCache。

        如果 TransferCache 对应 class 已满，就转交给 CentralCache。
    */
    void release_range(
        std::size_t class_index,
        void* first,
        void* last,
        std::size_t count
    ) noexcept;

    /*
        主动把某个 class 的 TransferCache 全部刷回 CentralCache。

        后面做测试、退出、内存回收时会用。
    */
    void drain_class(std::size_t class_index) noexcept;

    /*
        把所有 size class 的 TransferCache 全部刷回 CentralCache。
    */
    void drain_all() noexcept;

    std::size_t cached_range_count(std::size_t class_index) const;
    std::size_t cached_object_count(std::size_t class_index) const;

    TransferCache(const TransferCache&) = delete;
    TransferCache& operator=(const TransferCache&) = delete;

private:
    TransferCache() = default;

private:
    struct TransferClass {
        mutable std::mutex mutex;

        /*
            每个 Range 是一段对象链表。
            例如：
                obj0 -> obj1 -> ... -> obj63
        */
        std::vector<FreeList::Range> ranges;

        /*
            当前这个 class 在 TransferCache 中缓存了多少个对象。
        */
        std::size_t object_count = 0;
    };

private:
    std::array<TransferClass, SizeClass::kNumClasses> classes_;

private:
    static std::size_t max_ranges_for_class(
        std::size_t class_index
    ) noexcept;
};

} // namespace mini_tcmalloc

#endif