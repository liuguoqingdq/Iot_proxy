#ifndef SPAN_HPP
#define SPAN_HPP
#include <cstddef>
#include <cstdint>

#include "Common.hpp"

namespace mini_tcmalloc {

    struct Span 
    {
        void* start = nullptr;
        std::size_t page_count = 0;

        std::size_t bytes = 0;

        void* allocation_base = nullptr;

        std::size_t allocation_bytes = 0;

        std::size_t class_index = kInvalidSizeClass;

        std::size_t object_size = 0;

        std::size_t object_count = 0;

        std::size_t central_free_count = 0;

        Span* next = nullptr;
        Span* prev = nullptr;

        void* end() noexcept {
            return static_cast<char*> (start)+bytes;
        }
        const void* end() const noexcept {
            return static_cast<const char*>(start)+bytes;
        }

        bool contains(void* ptr) const noexcept {
            std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ptr);
            std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(start);
            std::uintptr_t finish = reinterpret_cast<std::uintptr_t>(end());

            return addr >= begin && addr < finish;
        }

        bool is_whole_allocation() const noexcept {
            return start == allocation_base && bytes == allocation_bytes;
        }

void bind_size_class(std::size_t idx, std::size_t obj_size) noexcept {
    class_index = idx;
    object_size = obj_size;

    if (object_size == 0) {
        object_count = 0;
        central_free_count = 0;
        return;
    }

    object_count = bytes / object_size;

    /*
        刚刚切完Span时，所有对象都在CentralCache里，
        所以central_free_count初始等于object_count。
    */
    central_free_count = object_count;
}

            
void reset_object_info() noexcept {
    class_index = kInvalidSizeClass;
    object_size = 0;
    object_count = 0;
    central_free_count = 0;
    next = nullptr;
    prev = nullptr;
}
    };




}




#endif