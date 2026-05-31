#include "tcmalloc/ThreadCache.hpp"

#include <cassert>

#include "tcmalloc/CentralCache.hpp"
#include "tcmalloc/TransferCache.hpp"

namespace mini_tcmalloc {

ThreadCache::ThreadCache()
    : cached_bytes_(0),
      cache_limit_bytes_(kDefaultThreadCacheLimit) {}

ThreadCache& ThreadCache::current() {
    thread_local ThreadCache cache;
    return cache;
}

ThreadCache::~ThreadCache() {
    release_all();
}

void* ThreadCache::allocate(std::size_t size) {
    if (!SizeClass::is_small(size)) {
        return nullptr;
    }

    std::size_t class_index = SizeClass::index(size);
    assert(class_index < SizeClass::kNumClasses);

    std::size_t class_size = SizeClass::class_size(class_index);

    FreeList& list = local_free_lists_[class_index];

    /*
        快路径：
            本地FreeList有对象，直接弹出。
            这个路径不加锁。
    */
    void* obj = list.pop();
    if (obj != nullptr) {
        assert(cached_bytes_ >= class_size);
        cached_bytes_ -= class_size;
        return obj;
    }

    /*
        慢路径：
            本地没有对象，向CentralCache批量拿。
    */
    return fetch_from_central(class_index);
}

void ThreadCache::deallocate(void* ptr, std::size_t size) noexcept {
    if (ptr == nullptr) {
        return;
    }

    if (!SizeClass::is_small(size)) {
        return;
    }

    std::size_t class_index = SizeClass::index(size);
    assert(class_index < SizeClass::kNumClasses);

    std::size_t class_size = SizeClass::class_size(class_index);

    FreeList& list = local_free_lists_[class_index];

    /*
        快路径：
            释放到当前线程本地FreeList。
            跨线程释放时，对象也会先进当前线程的ThreadCache。
    */
    list.push(ptr);
    cached_bytes_ += class_size;

    /*
        局部限制：
            某个size class缓存太多，先归还一批。
    */
    if (list.size() >= max_local_count(class_index)) {
        release_to_central(class_index);
    }

    /*
        全局限制：
            当前线程缓存总字节数太大，触发scavenge。
    */
    if (cached_bytes_ > cache_limit_bytes_) {
        scavenge();
    }
}

void* ThreadCache::fetch_from_central(std::size_t class_index) {
    assert(class_index < SizeClass::kNumClasses);

    std::size_t batch = SizeClass::batch_count(class_index);
    std::size_t class_size = SizeClass::class_size(class_index);

    FreeList::Range range =
        TransferCache::instance().fetch_range(class_index, batch);

    if (range.empty()) {
        return nullptr;
    }

    /*
        把CentralCache返回的一批对象先放到当前线程本地链表。
    */
    FreeList& list = local_free_lists_[class_index];

    list.push_range(range.first, range.last, range.count);
    cached_bytes_ += range.count * class_size;

    /*
        再从本地链表弹出一个返回给用户。
    */
    void* obj = list.pop();
    if (obj != nullptr) {
        assert(cached_bytes_ >= class_size);
        cached_bytes_ -= class_size;
    }

    return obj;
}

void ThreadCache::release_to_central(std::size_t class_index) noexcept {
    assert(class_index < SizeClass::kNumClasses);

    FreeList& list = local_free_lists_[class_index];

    if (list.empty()) {
        return;
    }

    std::size_t batch = SizeClass::batch_count(class_index);
    std::size_t class_size = SizeClass::class_size(class_index);

    FreeList::Range range = list.pop_range(batch);

    if (range.empty()) {
        return;
    }

    assert(cached_bytes_ >= range.count * class_size);
    cached_bytes_ -= range.count * class_size;

    TransferCache::instance().release_range(
        class_index,
        range.first,
        range.last,
        range.count
    );
}

void ThreadCache::scavenge() noexcept {
    /*
        目标不是一下子清空ThreadCache。

        清空会导致后面又频繁向CentralCache拿对象。
        所以我们只把缓存压到上限的75%左右。
    */
    std::size_t target = cache_limit_bytes_ * 3 / 4;

    if (target == 0) {
        target = 1;
    }

    /*
        优先从大size class开始归还。

        原因：
            大对象单个占用字节多，归还一批更容易降低cached_bytes_。
    */
    for (std::size_t round = 0;
         cached_bytes_ > target && round < SizeClass::kNumClasses;
         ++round) {

        bool released_any = false;

        for (std::size_t i = SizeClass::kNumClasses; i > 0; --i) {
            std::size_t class_index = i - 1;

            if (local_free_lists_[class_index].empty()) {
                continue;
            }

            std::size_t before = cached_bytes_;
            release_to_central(class_index);

            if (cached_bytes_ < before) {
                released_any = true;
            }

            if (cached_bytes_ <= target) {
                break;
            }
        }

        if (!released_any) {
            break;
        }
    }
}

void ThreadCache::release_all() noexcept {
    for (std::size_t i = 0; i < SizeClass::kNumClasses; ++i) {
        FreeList& list = local_free_lists_[i];

        while (!list.empty()) {
            std::size_t class_size = SizeClass::class_size(i);

            FreeList::Range range = list.pop_range(list.size());

            if (range.empty()) {
                break;
            }

            assert(cached_bytes_ >= range.count * class_size);
            cached_bytes_ -= range.count * class_size;

            TransferCache::instance().release_range(
                i,
                range.first,
                range.last,
                range.count
            );
        }
    }

    assert(cached_bytes_ == 0);
    cached_bytes_ = 0;
}

std::size_t ThreadCache::local_free_count(
    std::size_t class_index
) const noexcept {
    assert(class_index < SizeClass::kNumClasses);

    if (class_index >= SizeClass::kNumClasses) {
        return 0;
    }

    return local_free_lists_[class_index].size();
}

std::size_t ThreadCache::total_local_free_count() const noexcept {
    std::size_t total = 0;

    for (const FreeList& list : local_free_lists_) {
        total += list.size();
    }

    return total;
}

std::size_t ThreadCache::cached_bytes() const noexcept {
    return cached_bytes_;
}

std::size_t ThreadCache::cache_limit_bytes() const noexcept {
    return cache_limit_bytes_;
}

void ThreadCache::set_cache_limit_bytes(std::size_t bytes) noexcept {
    /*
        至少保留一个最小值，避免设置成0后频繁抖动。
    */
    if (bytes < 8 * 1024) {
        bytes = 8 * 1024;
    }

    cache_limit_bytes_ = bytes;

    if (cached_bytes_ > cache_limit_bytes_) {
        scavenge();
    }
}

std::size_t ThreadCache::max_local_count(
    std::size_t class_index
) noexcept {
    assert(class_index < SizeClass::kNumClasses);

    return SizeClass::batch_count(class_index) * 2;
}

} // namespace mini_tcmalloc
