#include "tcmalloc/MiniMalloc.hpp"

#include <cassert>

#include "tcmalloc/Common.hpp"
#include "tcmalloc/PageHeap.hpp"
#include "tcmalloc/SizeClass.hpp"
#include "tcmalloc/ThreadCache.hpp"

namespace mini_tcmalloc {

namespace {

    //a/b向上取整数
std::size_t ceil_div(std::size_t a, std::size_t b) noexcept {
    return (a + b - 1) / b;
}

bool is_valid_small_object_pointer(Span* span, void* ptr) noexcept {
    if (span == nullptr || ptr == nullptr) {
        return false;
    }

    if (span->object_size == 0) {
        return false;
    }

    std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(span->start);
    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ptr);

    if (addr < begin || addr >= begin + span->bytes) {
        return false;
    }

    std::size_t offset = addr - begin;

    return offset % span->object_size == 0;
}

} // namespace

void* mini_malloc(std::size_t size) {
    /*
        malloc(0)在标准里行为比较特殊。
        我们这里简单处理成申请1字节。
    */
    if (size == 0) {
        size = 1;
    }

    /*
        小对象走ThreadCache。
    */
    if (SizeClass::is_small(size)) {
        return ThreadCache::current().allocate(size);
    }

    /*
        大对象直接走PageHeap。

        第一版策略：
            大对象独占一个Span。
    */
    std::size_t pages = ceil_div(size, kPageSize);

    Span* span = PageHeap::instance().new_span(pages);
    if (span == nullptr) {
        return nullptr;
    }

    /*
        大对象不绑定size class。
        class_index保持kInvalidSizeClass。

        object_size这里记录用户请求大小，主要用于调试观察。
    */
    span->class_index = kInvalidSizeClass;
    span->object_size = size;
    span->object_count = 1;
    span->central_free_count = 0;

    return span->start;
}

void mini_free(void* ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }

    /*
        通过page map反查ptr属于哪个Span。
    */
    Span* span = PageHeap::instance().map_object_to_span(ptr);

#ifndef NDEBUG
    assert(span != nullptr && "mini_free: pointer does not belong to MiniTCMalloc");
#endif

    if (span == nullptr) {
        return;
    }

    /*
        小对象：
            Span已经绑定了某个size class。
            释放回当前线程的ThreadCache。
    */
    if (span->class_index != kInvalidSizeClass) {
#ifndef NDEBUG
        assert(is_valid_small_object_pointer(span, ptr) &&
               "mini_free: invalid small object pointer");
#endif

        if (!is_valid_small_object_pointer(span, ptr)) {
            return;
        }

        ThreadCache::current().deallocate(ptr, span->object_size);
        return;
    }

    /*
        大对象：
            要求ptr必须是Span起始地址。
            因为大对象独占Span。
    */
#ifndef NDEBUG
    assert(ptr == span->start &&
           "mini_free: invalid large object pointer");
#endif

    if (ptr != span->start) {
        return;
    }

    PageHeap::instance().release_span(span);
}

} // namespace mini_tcmalloc
