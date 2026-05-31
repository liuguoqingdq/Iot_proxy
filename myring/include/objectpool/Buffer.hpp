#ifndef BUFFER_HPP
#define BUFFER_HPP

#include <cstddef>
#include <cstring>
#include <new>

#include "tcmalloc/MiniMalloc.hpp"

class Buffer {
public:
    explicit Buffer(std::size_t capacity)
        : data_(nullptr),
          capacity_(capacity),
          length_(0) {
        data_ = static_cast<char*>(
            mini_tcmalloc::mini_malloc(capacity_)
        );

        if (data_ == nullptr) {
            throw std::bad_alloc();
        }
    }

    ~Buffer() {
        if (data_ != nullptr) {
            mini_tcmalloc::mini_free(data_);
            data_ = nullptr;
        }
    }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    char* data() noexcept {
        return data_;
    }

    const char* data() const noexcept {
        return data_;
    }

    std::size_t capacity() const noexcept {
        return capacity_;
    }

    std::size_t length() const noexcept {
        return length_;
    }

    void set_length(std::size_t n) noexcept {
        length_ = n <= capacity_ ? n : capacity_;
    }

    void clear() noexcept {
        length_ = 0;
    }

    void assign(const char* src, std::size_t n) noexcept {
        if (src == nullptr || capacity_ == 0) {
            length_ = 0;
            return;
        }

        const std::size_t copy_len = n <= capacity_ ? n : capacity_;
        std::memcpy(data_, src, copy_len);
        length_ = copy_len;
    }

private:
    char* data_;
    std::size_t capacity_;
    std::size_t length_;
};

#endif
