#ifndef MINI_TCMALLOC_PAGE_HEAP_HPP
#define MINI_TCMALLOC_PAGE_HEAP_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "Span.hpp"

namespace mini_tcmalloc {

class PageHeap {
public:
    static PageHeap& instance();

    /*
        申请page_count页Span。

        顺序：
            1. 找完全相同页数的cached span
            2. 找更大的cached span，拆分
            3. 向系统申请新内存
    */
    Span* new_span(std::size_t page_count);

    /*
        释放Span到PageHeap缓存。

        这一版release_span会尝试和左右相邻的空闲Span合并。
    */
    bool release_span(Span* span) noexcept;

    Span* map_object_to_span(void* ptr) const noexcept;

    /*
        主动释放PageHeap缓存中的完整Span。
    */
    std::size_t purge_cached_spans() noexcept;

    std::size_t span_count() const;
    std::size_t cached_span_count() const;

    std::size_t active_bytes() const;
    std::size_t cached_bytes() const;
    std::size_t system_bytes() const;

    PageHeap(const PageHeap&) = delete;
    PageHeap& operator=(const PageHeap&) = delete;

    ~PageHeap();

private:
    PageHeap();

private:
    static std::uintptr_t page_id_of(void* ptr) noexcept;

    static std::uintptr_t span_start_page(Span* span) noexcept;
    static std::uintptr_t span_end_page(Span* span) noexcept;

    void register_span_unlocked(Span* span);
    void unregister_span_unlocked(Span* span) noexcept;

    /*
        cached span索引维护。
    */
    void insert_cached_span_unlocked(Span* span);
    void erase_cached_span_unlocked(Span* span) noexcept;

    /*
        找缓存Span。
    */
    Span* take_cached_span_unlocked(std::size_t page_count);
    Span* take_larger_cached_span_unlocked(std::size_t page_count);

    /*
        拆分Span。
    */
    Span* split_span_unlocked(Span* span, std::size_t needed_pages);

    /*
        合并Span。

        输入span还没有进入cached结构。
        函数会尝试和cached中的左右邻居合并。
    */
    Span* coalesce_span_unlocked(Span* span) noexcept;

    bool can_merge_unlocked(Span* left, Span* right) const noexcept;

    /*
        把Span放入缓存。
        内部会先尝试合并。
    */
    void cache_span_unlocked(Span* span);

    /*
        真正释放完整Span到底层系统分配器。
    */
    void free_whole_span_to_system_unlocked(Span* span) noexcept;

private:
    mutable std::mutex mutex_;

    /*
        active spans。
    */
    std::vector<Span*> spans_;

    /*
        active page id -> active Span*
    */
    std::unordered_map<std::uintptr_t, Span*> page_to_span_;

    /*
        cached spans，按页数分类。
    */
    std::unordered_map<std::size_t, std::vector<Span*>> free_spans_by_pages_;

    /*
        cached span起始页 -> Span*
        用于快速找右邻居。
    */
    std::unordered_map<std::uintptr_t, Span*> cached_start_page_to_span_;

    /*
        cached span结束页 -> Span*
        用于快速找左邻居。

        注意：
            end_page是右开边界。
            例如start=100，page_count=8，则end=108。
    */
    std::unordered_map<std::uintptr_t, Span*> cached_end_page_to_span_;

    std::size_t active_bytes_;
    std::size_t cached_bytes_;
    std::size_t system_bytes_;
};

} // namespace mini_tcmalloc

#endif