#include "tcmalloc/CentralCache.hpp"

#include <algorithm>
#include <cassert>

#include "tcmalloc/PageHeap.hpp"

namespace mini_tcmalloc {

CentralCache& CentralCache::instance() {
    static CentralCache cache;
    return cache;
}

FreeList::Range CentralCache::fetch_range(
    std::size_t class_index,
    std::size_t n
) {
    assert(class_index < SizeClass::kNumClasses);
    assert(n > 0);

    if (class_index >= SizeClass::kNumClasses || n == 0) {
        return {};
    }

    CentralClass& cls = classes_[class_index];

    std::lock_guard<std::mutex> lock(cls.mutex);

    if (cls.free_list.empty()) {
        bool ok = populate_unlocked(class_index, cls);
        if (!ok) {
            return {};
        }
    }

    FreeList::Range range = cls.free_list.pop_range(n);

    /*
        这些对象被发给ThreadCache，
        不再留在CentralCache里。
    */
    if (!range.empty()) {
        on_fetch_range_unlocked(range);
    }

    return range;
}

void CentralCache::release_range(
    std::size_t class_index,
    void* first,
    void* last,
    std::size_t count
) {
    assert(class_index < SizeClass::kNumClasses);
    assert(first != nullptr);
    assert(last != nullptr);
    assert(count > 0);

    if (class_index >= SizeClass::kNumClasses ||
        first == nullptr ||
        last == nullptr ||
        count == 0) {
        return;
    }

    CentralClass& cls = classes_[class_index];

    std::lock_guard<std::mutex> lock(cls.mutex);

    /*
        先更新Span计数。

        这些对象从ThreadCache回到CentralCache，
        所以central_free_count要增加。
    */
    on_release_range_unlocked(first, count);

    /*
        再把对象挂回CentralCache的free_list。
    */
    cls.free_list.push_range(first, last, count);

    /*
        如果某些Span已经完全空闲，就整块回收给PageHeap。
    */
    release_empty_spans_unlocked(cls);
}

bool CentralCache::populate_unlocked(
    std::size_t class_index,
    CentralClass& cls
) {
    assert(class_index < SizeClass::kNumClasses);

    std::size_t object_size = SizeClass::class_size(class_index);
    std::size_t pages = SizeClass::pages_for_class(class_index);

    Span* span = PageHeap::instance().new_span(pages);
    if (span == nullptr) {
        return false;
    }

    span->bind_size_class(class_index, object_size);

    std::size_t object_count = span->object_count;
    if (object_count == 0) {
        PageHeap::instance().release_span(span);
        return false;
    }

    try {
        cls.spans.push_back(span);
    } catch (...) {
        PageHeap::instance().release_span(span);
        return false;
    }

    char* begin = static_cast<char*>(span->start);

    void* first = begin;
    void* last = nullptr;

    for (std::size_t i = 0; i < object_count; ++i) {
        void* current = begin + i * object_size;

        if (i + 1 < object_count) {
            void* next_obj = begin + (i + 1) * object_size;
            next(current) = next_obj;
        } else {
            next(current) = nullptr;
            last = current;
        }
    }

    /*
        刚切好的Span，所有对象都在CentralCache里。
        bind_size_class()里已经设置：
            central_free_count = object_count
    */
    cls.free_list.push_range(first, last, object_count);
    cls.total_objects += object_count;

    return true;
}
/*
    CentralCache把对象发给ThreadCache后，
    这些对象就不再处于CentralCache中。
    所以对应Span的central_free_count要减少。
*/
void CentralCache::on_fetch_range_unlocked(
    const FreeList::Range& range
) {
    void* cur = range.first;

    for (std::size_t i = 0; i < range.count; ++i) {
        assert(cur != nullptr);

        Span* span = PageHeap::instance().map_object_to_span(cur);
        if (span == nullptr) {
            cur = next(cur);
            continue;
        }

        assert(span->central_free_count > 0);
        if (span->central_free_count > 0) {
            --span->central_free_count;
        }

        cur = next(cur);
    }
}
/*
    ThreadCache把对象还给CentralCache后，
    对应Span的central_free_count要增加。
*/
void CentralCache::on_release_range_unlocked(
    void* first,
    std::size_t count
) {
    void* cur = first;

    for (std::size_t i = 0; i < count; ++i) {
        assert(cur != nullptr);

        Span* span = PageHeap::instance().map_object_to_span(cur);
        if (span == nullptr) {
            cur = next(cur);
            continue;
        }

        assert(span->central_free_count < span->object_count);
        ++span->central_free_count;

        cur = next(cur);
    }
}
/*
    尝试回收已经完全空闲的Span。

    条件：
        span->central_free_count == span->object_count

    回收动作：
        1. 从CentralCache free_list中移除该Span的所有对象
        2. 从cls.spans中移除该Span
        3. 还给PageHeap
*/
void CentralCache::release_empty_spans_unlocked(
    CentralClass& cls
) {
    auto it = cls.spans.begin();

    while (it != cls.spans.end()) {
        Span* span = *it;

        if (span == nullptr) {
            it = cls.spans.erase(it);
            continue;
        }

        /*
            只有当这个Span切出来的所有对象都已经回到CentralCache，
            才能回收整块Span。
        */
        if (span->object_count == 0 ||
            span->central_free_count != span->object_count) {
            ++it;
            continue;
        }

        /*
            回收前，要先确认CentralCache的free_list里确实有
            这个Span的所有对象。
        */
        std::size_t count_in_list =
            cls.free_list.count_if([span](void* obj) {
                return span->contains(obj);
            });

        assert(count_in_list == span->object_count);

        if (count_in_list != span->object_count) {
            /*
                理论上不应该发生。
                如果发生，说明计数和链表状态不一致。
                为了安全，跳过回收。
            */
            ++it;
            continue;
        }

        /*
            从CentralCache free_list里摘掉这个Span的所有对象。
        */
        std::size_t removed =
            cls.free_list.remove_if([span](void* obj) {
                return span->contains(obj);
            });

        assert(removed == span->object_count);

        if (removed != span->object_count) {
            ++it;
            continue;
        }

        if (cls.total_objects >= removed) {
            cls.total_objects -= removed;
        } else {
            cls.total_objects = 0;
        }

        /*
            从CentralCache记录的Span列表中移除。
        */
        it = cls.spans.erase(it);

        /*
            整块Span还给PageHeap。
            PageHeap会删除page map映射，并释放内存。
        */
        PageHeap::instance().release_span(span);
    }
}

std::size_t CentralCache::free_count(std::size_t class_index) const {
    assert(class_index < SizeClass::kNumClasses);

    if (class_index >= SizeClass::kNumClasses) {
        return 0;
    }

    const CentralClass& cls = classes_[class_index];

    std::lock_guard<std::mutex> lock(cls.mutex);

    return cls.free_list.size();
}

std::size_t CentralCache::span_count(std::size_t class_index) const {
    assert(class_index < SizeClass::kNumClasses);

    if (class_index >= SizeClass::kNumClasses) {
        return 0;
    }

    const CentralClass& cls = classes_[class_index];

    std::lock_guard<std::mutex> lock(cls.mutex);

    return cls.spans.size();
}

std::size_t CentralCache::total_objects(std::size_t class_index) const {
    assert(class_index < SizeClass::kNumClasses);

    if (class_index >= SizeClass::kNumClasses) {
        return 0;
    }

    const CentralClass& cls = classes_[class_index];

    std::lock_guard<std::mutex> lock(cls.mutex);

    return cls.total_objects;
}

} // namespace mini_tcmalloc
