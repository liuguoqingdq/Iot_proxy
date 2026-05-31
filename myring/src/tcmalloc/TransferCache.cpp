#include "tcmalloc/TransferCache.hpp"

#include <cassert>
#include <utility>

#include "tcmalloc/CentralCache.hpp"


namespace mini_tcmalloc
{
TransferCache& TransferCache::instance() {
    static TransferCache cache;
    return cache;
}
FreeList::Range TransferCache::fetch_range(std::size_t class_index,std::size_t n)
{
    assert(class_index < SizeClass::kNumClasses);
    assert(n > 0);

    if(class_index >= SizeClass::kNumClasses || n==0){
        return {};
    }
    TransferClass& cls = classes_[class_index];

    {
        std::lock_guard<std::mutex> lock(cls.mutex);
        if(!cls.ranges.empty()) {
            FreeList::Range range = cls.ranges.back();
            cls.ranges.pop_back();

            if(cls.object_count >= range.count){
                cls.object_count -= range.count;
            }else{
                cls.object_count = 0;
            }
            return range;
        }
    }
    return CentralCache::instance().fetch_range(class_index, n);
}
    /*
        释放一批对象到 TransferCache。

        如果 TransferCache 对应 class 已满，就转交给 CentralCache。
    */
void TransferCache::release_range(
    std::size_t class_index,
    void* first,
    void* last,
    std::size_t count
) noexcept {
    assert(class_index < SizeClass::kNumClasses);
    assert(first!=nullptr);
    assert(last!=nullptr);
    assert(count > 0);

    if(class_index >= SizeClass::kNumClasses ||
         first == nullptr ||
        last == nullptr ||
        count == 0){
            return;
        }
    TransferClass& cls = classes_[class_index];

    bool should_release_to_central = false;
    {
        std::lock_guard<std::mutex> lock(cls.mutex);

        std::size_t max_ranges =max_ranges_for_class(class_index);

        if(cls.ranges.size() < max_ranges) {
            try{
                cls.ranges.push_back(FreeList::Range{first,last,count});
                cls.object_count += count;
            }catch(...){
                should_release_to_central = true;
            }
        }else {
            should_release_to_central = true;
        }
    }
    if(should_release_to_central) {
        CentralCache::instance().release_range(
            class_index,
            first,
            last,
            count
        );
    }
}


void TransferCache::drain_class(std::size_t class_index) noexcept {
    assert(class_index < SizeClass::kNumClasses);

    if (class_index >= SizeClass::kNumClasses) {
        return;
    }

    TransferClass& cls = classes_[class_index];

    std::vector<FreeList::Range> ranges;

    {
        std::lock_guard<std::mutex> lock(cls.mutex);

        ranges.swap(cls.ranges);
        cls.object_count = 0;
    }

    for (FreeList::Range& range : ranges) {
        if (!range.empty()) {
            CentralCache::instance().release_range(
                class_index,
                range.first,
                range.last,
                range.count
            );
        }
    }
}

void TransferCache::drain_all() noexcept {
    for (std::size_t i = 0; i < SizeClass::kNumClasses; ++i) {
        drain_class(i);
    }
}

std::size_t TransferCache::cached_range_count(
    std::size_t class_index
) const {
    assert(class_index < SizeClass::kNumClasses);

    if (class_index >= SizeClass::kNumClasses) {
        return 0;
    }

    const TransferClass& cls = classes_[class_index];

    std::lock_guard<std::mutex> lock(cls.mutex);

    return cls.ranges.size();
}


std::size_t TransferCache::cached_object_count(
    std::size_t class_index
) const {
    assert(class_index < SizeClass::kNumClasses);

    if (class_index >= SizeClass::kNumClasses) {
        return 0;
    }

    const TransferClass& cls = classes_[class_index];

    std::lock_guard<std::mutex> lock(cls.mutex);

    return cls.object_count;
}

std::size_t TransferCache::max_ranges_for_class(
    std::size_t class_index
) noexcept {
    assert(class_index < SizeClass::kNumClasses);

    std::size_t size = SizeClass::class_size(class_index);

    /*
        小对象允许缓存更多批次。
        大对象少缓存一些，避免TransferCache占太多内存。
    */
    if (size <= 64) {
        return 64;
    }

    if (size <= 256) {
        return 32;
    }

    if (size <= 1024) {
        return 16;
    }

    return 8;
}
}
