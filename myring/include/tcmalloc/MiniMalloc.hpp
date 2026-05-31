#ifndef MINI_MALLOC_HPP
#define MINI_MALLOC_HPP

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace mini_tcmalloc {

void* mini_malloc(std::size_t size);
void mini_free(void* ptr) noexcept;

template <class T, class... Args>
T* mini_new(Args&&... args) {
    static_assert(!std::is_array<T>::value,
                  "mini_new does not support array type");

    void* mem = mini_malloc(sizeof(T));
    if (mem == nullptr) {
        throw std::bad_alloc();
    }

    try {
        return new (mem) T(std::forward<Args>(args)...);
    } catch (...) {
        mini_free(mem);
        throw;
    }
}

template <typename T>
void mini_delete(T* ptr) noexcept {
    if (ptr == nullptr) {
        return;
    }

    ptr->~T();
    mini_free(ptr);
}

template <class T>
struct MiniDelete {
    void operator()(T* ptr) const noexcept {
        mini_delete(ptr);
    }
};

template <class T>
using MiniUniquePtr = std::unique_ptr<T, MiniDelete<T>>;

template <class T, class... Args>
MiniUniquePtr<T> mini_make_unique(Args&&... args) {
    return MiniUniquePtr<T>(mini_new<T>(std::forward<Args>(args)...));
}

}  // namespace mini_tcmalloc

#endif
