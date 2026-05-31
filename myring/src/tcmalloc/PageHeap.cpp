#include "tcmalloc/PageHeap.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <unordered_set>

namespace mini_tcmalloc {

PageHeap::PageHeap()
    : active_bytes_(0),
      cached_bytes_(0),
      system_bytes_(0) {}

PageHeap::~PageHeap() {
    /*
        析构时要避免同一个allocation_base被释放多次。
        因为拆分后的多个Span可能共享同一个底层系统分配块。
    */
    std::unordered_set<void*> freed_allocations;

    auto release_once = [&freed_allocations](Span* span) {
        if (span == nullptr) {
            return;
        }

        void* base = span->allocation_base;

        if (base != nullptr && freed_allocations.insert(base).second) {
            ::operator delete(
                base,
                std::align_val_t(kPageSize)
            );
        }

        delete span;
    };

    for (Span* span : spans_) {
        release_once(span);
    }

    spans_.clear();

    for (auto& kv : free_spans_by_pages_) {
        for (Span* span : kv.second) {
            release_once(span);
        }

        kv.second.clear();
    }

    free_spans_by_pages_.clear();
    cached_start_page_to_span_.clear();
    cached_end_page_to_span_.clear();
    page_to_span_.clear();

    active_bytes_ = 0;
    cached_bytes_ = 0;
    system_bytes_ = 0;
}

PageHeap& PageHeap::instance() {
    static PageHeap heap;
    return heap;
}

std::uintptr_t PageHeap::page_id_of(void* ptr) noexcept {
    return reinterpret_cast<std::uintptr_t>(ptr) / kPageSize;
}

std::uintptr_t PageHeap::span_start_page(Span* span) noexcept {
    return page_id_of(span->start);
}

std::uintptr_t PageHeap::span_end_page(Span* span) noexcept {
    return span_start_page(span) + span->page_count;
}

void PageHeap::register_span_unlocked(Span* span) {
    if (span == nullptr || span->start == nullptr || span->page_count == 0) {
        return;
    }

    std::uintptr_t first_page = page_id_of(span->start);

    for (std::size_t i = 0; i < span->page_count; ++i) {
        page_to_span_[first_page + i] = span;
    }
}

void PageHeap::unregister_span_unlocked(Span* span) noexcept {
    if (span == nullptr || span->start == nullptr || span->page_count == 0) {
        return;
    }

    std::uintptr_t first_page = page_id_of(span->start);

    for (std::size_t i = 0; i < span->page_count; ++i) {
        page_to_span_.erase(first_page + i);
    }
}

void PageHeap::insert_cached_span_unlocked(Span* span) {
    if (span == nullptr) {
        return;
    }

    span->reset_object_info();

    free_spans_by_pages_[span->page_count].push_back(span);

    cached_start_page_to_span_[span_start_page(span)] = span;
    cached_end_page_to_span_[span_end_page(span)] = span;

    cached_bytes_ += span->bytes;
}

void PageHeap::erase_cached_span_unlocked(Span* span) noexcept {
    if (span == nullptr) {
        return;
    }

    auto it = free_spans_by_pages_.find(span->page_count);
    if (it != free_spans_by_pages_.end()) {
        std::vector<Span*>& list = it->second;

        auto vit = std::find(list.begin(), list.end(), span);
        if (vit != list.end()) {
            list.erase(vit);
        }

        if (list.empty()) {
            free_spans_by_pages_.erase(it);
        }
    }

    cached_start_page_to_span_.erase(span_start_page(span));
    cached_end_page_to_span_.erase(span_end_page(span));

    if (cached_bytes_ >= span->bytes) {
        cached_bytes_ -= span->bytes;
    } else {
        cached_bytes_ = 0;
    }
}

Span* PageHeap::take_cached_span_unlocked(std::size_t page_count) {
    auto it = free_spans_by_pages_.find(page_count);
    if (it == free_spans_by_pages_.end()) {
        return nullptr;
    }

    std::vector<Span*>& list = it->second;

    if (list.empty()) {
        free_spans_by_pages_.erase(it);
        return nullptr;
    }

    Span* span = list.back();

    /*
        注意：
            这里不要自己pop_back。
            统一交给erase_cached_span_unlocked，
            它会同步维护：
                free_spans_by_pages_
                cached_start_page_to_span_
                cached_end_page_to_span_
                cached_bytes_
    */
    erase_cached_span_unlocked(span);

    return span;
}

Span* PageHeap::take_larger_cached_span_unlocked(std::size_t page_count) {
    auto best_it = free_spans_by_pages_.end();
    std::size_t best_pages = std::numeric_limits<std::size_t>::max();

    for (auto it = free_spans_by_pages_.begin();
         it != free_spans_by_pages_.end();
         ++it) {

        std::size_t pages = it->first;
        std::vector<Span*>& list = it->second;

        if (list.empty()) {
            continue;
        }

        if (pages > page_count && pages < best_pages) {
            best_pages = pages;
            best_it = it;
        }
    }

    if (best_it == free_spans_by_pages_.end()) {
        return nullptr;
    }

    std::vector<Span*>& list = best_it->second;
    Span* span = list.back();

    erase_cached_span_unlocked(span);

    return span;
}

Span* PageHeap::split_span_unlocked(Span* span, std::size_t needed_pages) {
    if (span == nullptr) {
        return nullptr;
    }

    if (needed_pages == 0 || needed_pages >= span->page_count) {
        return nullptr;
    }

    std::size_t old_pages = span->page_count;
    std::size_t remain_pages = old_pages - needed_pages;

    char* old_start = static_cast<char*>(span->start);
    char* remain_start = old_start + needed_pages * kPageSize;

    /*
        remain表示右半部分。
    */
    Span* remain = new Span();

    remain->start = remain_start;
    remain->page_count = remain_pages;
    remain->bytes = remain_pages * kPageSize;

    /*
        右半部分和左半部分共享同一个底层系统分配块。
    */
    remain->allocation_base = span->allocation_base;
    remain->allocation_bytes = span->allocation_bytes;
    remain->reset_object_info();

    /*
        原span作为左半部分返回给调用者。
    */
    span->page_count = needed_pages;
    span->bytes = needed_pages * kPageSize;
    span->reset_object_info();

    return remain;
}

bool PageHeap::can_merge_unlocked(Span* left, Span* right) const noexcept {
    if (left == nullptr || right == nullptr) {
        return false;
    }

    if (left->allocation_base != right->allocation_base ||
        left->allocation_bytes != right->allocation_bytes) {
        return false;
    }

    char* left_end = static_cast<char*>(left->start) + left->bytes;

    return left_end == right->start;
}

Span* PageHeap::coalesce_span_unlocked(Span* span) noexcept {
    if (span == nullptr) {
        return nullptr;
    }

    bool merged = true;

    while (merged) {
        merged = false;

        /*
            尝试找左邻居：
                left.end_page == span.start_page
        */
        auto left_it = cached_end_page_to_span_.find(span_start_page(span));
        if (left_it != cached_end_page_to_span_.end()) {
            Span* left = left_it->second;

            if (can_merge_unlocked(left, span)) {
                erase_cached_span_unlocked(left);

                /*
                    合并到left对象上。
                    left变成新的span。
                */
                left->page_count += span->page_count;
                left->bytes += span->bytes;
                left->reset_object_info();

                delete span;
                span = left;

                merged = true;
                continue;
            }
        }

        /*
            尝试找右邻居：
                span.end_page == right.start_page
        */
        auto right_it = cached_start_page_to_span_.find(span_end_page(span));
        if (right_it != cached_start_page_to_span_.end()) {
            Span* right = right_it->second;

            if (can_merge_unlocked(span, right)) {
                erase_cached_span_unlocked(right);

                /*
                    合并到当前span对象上。
                */
                span->page_count += right->page_count;
                span->bytes += right->bytes;
                span->reset_object_info();

                delete right;

                merged = true;
                continue;
            }
        }
    }

    return span;
}

void PageHeap::cache_span_unlocked(Span* span) {
    if (span == nullptr) {
        return;
    }

    span->reset_object_info();

    /*
        先尝试和缓存中的左右相邻Span合并。
    */
    Span* merged = coalesce_span_unlocked(span);

    /*
        再把合并后的Span放入缓存。
    */
    insert_cached_span_unlocked(merged);
}

void PageHeap::free_whole_span_to_system_unlocked(Span* span) noexcept {
    if (span == nullptr) {
        return;
    }

    if (span->is_whole_allocation() && span->allocation_base != nullptr) {
        ::operator delete(
            span->allocation_base,
            std::align_val_t(kPageSize)
        );
    }

    delete span;
}

Span* PageHeap::new_span(std::size_t page_count) {
    if (page_count == 0) {
        return nullptr;
    }

    if (page_count > std::numeric_limits<std::size_t>::max() / kPageSize) {
        return nullptr;
    }

    const std::size_t bytes = page_count * kPageSize;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        /*
            1. 优先找完全匹配的cached span。
        */
        Span* cached = take_cached_span_unlocked(page_count);
        if (cached != nullptr) {
            cached->reset_object_info();

            try {
                spans_.push_back(cached);
                register_span_unlocked(cached);
            } catch (...) {
                cache_span_unlocked(cached);
                return nullptr;
            }

            active_bytes_ += cached->bytes;
            return cached;
        }

        /*
            2. 找更大的cached span，并拆分。
        */
        Span* larger = take_larger_cached_span_unlocked(page_count);
        if (larger != nullptr) {
            Span* remain = nullptr;

            try {
                remain = split_span_unlocked(larger, page_count);

                if (remain != nullptr) {
                    cache_span_unlocked(remain);
                }

                spans_.push_back(larger);
                register_span_unlocked(larger);

                active_bytes_ += larger->bytes;
                return larger;
            } catch (...) {
                if (remain != nullptr) {
                    cache_span_unlocked(remain);
                }

                cache_span_unlocked(larger);
                return nullptr;
            }
        }
    }

    /*
        3. 缓存里没有可用Span，向系统申请新内存。
    */
    void* memory = nullptr;
    Span* span = nullptr;

    try {
        memory = ::operator new(
            bytes,
            std::align_val_t(kPageSize)
        );

        span = new Span();

        span->start = memory;
        span->page_count = page_count;
        span->bytes = bytes;

        span->allocation_base = memory;
        span->allocation_bytes = bytes;

        span->reset_object_info();

        {
            std::lock_guard<std::mutex> lock(mutex_);

            spans_.push_back(span);
            register_span_unlocked(span);

            active_bytes_ += bytes;
            system_bytes_ += bytes;
        }

        return span;
    } catch (...) {
        if (memory != nullptr) {
            ::operator delete(
                memory,
                std::align_val_t(kPageSize)
            );
        }

        delete span;

        return nullptr;
    }
}

bool PageHeap::release_span(Span* span) noexcept {
    if (span == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find(spans_.begin(), spans_.end(), span);
    if (it == spans_.end()) {
        return false;
    }

    spans_.erase(it);
    unregister_span_unlocked(span);

    if (active_bytes_ >= span->bytes) {
        active_bytes_ -= span->bytes;
    } else {
        active_bytes_ = 0;
    }

    try {
        cache_span_unlocked(span);
    } catch (...) {
        /*
            如果缓存失败，教学版保守返回false。
            正式工程可以在这里做更强的错误处理。
        */
        return false;
    }

    return true;
}

Span* PageHeap::map_object_to_span(void* ptr) const noexcept {
    if (ptr == nullptr) {
        return nullptr;
    }

    std::uintptr_t page_id = page_id_of(ptr);

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = page_to_span_.find(page_id);
    if (it == page_to_span_.end()) {
        return nullptr;
    }

    Span* span = it->second;

    if (span == nullptr || !span->contains(ptr)) {
        return nullptr;
    }

    return span;
}

std::size_t PageHeap::purge_cached_spans() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t released = 0;

    for (auto it = free_spans_by_pages_.begin();
         it != free_spans_by_pages_.end(); ) {

        std::vector<Span*>& list = it->second;

        std::vector<Span*> remain;

        for (Span* span : list) {
            if (span == nullptr) {
                continue;
            }

            if (span->is_whole_allocation()) {
                if (cached_bytes_ >= span->bytes) {
                    cached_bytes_ -= span->bytes;
                } else {
                    cached_bytes_ = 0;
                }

                if (system_bytes_ >= span->bytes) {
                    system_bytes_ -= span->bytes;
                } else {
                    system_bytes_ = 0;
                }

                cached_start_page_to_span_.erase(span_start_page(span));
                cached_end_page_to_span_.erase(span_end_page(span));

                free_whole_span_to_system_unlocked(span);
                ++released;
            } else {
                remain.push_back(span);
            }
        }

        if (remain.empty()) {
            it = free_spans_by_pages_.erase(it);
        } else {
            list.swap(remain);
            ++it;
        }
    }

    return released;
}

std::size_t PageHeap::span_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return spans_.size();
}

std::size_t PageHeap::cached_span_count() const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t count = 0;

    for (const auto& kv : free_spans_by_pages_) {
        count += kv.second.size();
    }

    return count;
}

std::size_t PageHeap::active_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_bytes_;
}

std::size_t PageHeap::cached_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cached_bytes_;
}

std::size_t PageHeap::system_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return system_bytes_;
}

} // namespace mini_tcmalloc
