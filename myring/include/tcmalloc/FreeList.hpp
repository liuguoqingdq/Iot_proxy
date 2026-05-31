#ifndef FREE_LIST_HPP
#define FREE_LIST_HPP

#include <cassert>
#include <cstddef>

#include "nocopyable.h"

namespace mini_tcmalloc {

class FreeList : public nocopyable {
public:
    struct Range {
        void* first = nullptr;
        void* last = nullptr;
        std::size_t count = 0;

        bool empty() const noexcept {
            return first == nullptr;
        }
    };

public:
    FreeList() noexcept
        : head_(nullptr),
          size_(0) {}

    bool empty() const noexcept {
        return head_ == nullptr;
    }

    std::size_t size() const noexcept {
        return size_;
    }

    void push(void* obj) noexcept {
        assert(obj != nullptr);
        next(obj) = head_;
        head_ = obj;
        ++size_;
    }

    void* pop() noexcept {
        if (head_ == nullptr) {
            return nullptr;
        }

        void* obj = head_;
        head_ = next(obj);
        next(obj) = nullptr;
        --size_;
        return obj;
    }

    void push_range(void* first, void* last, std::size_t count) noexcept {
        assert(first != nullptr);
        assert(last != nullptr);
        assert(count > 0);

        next(last) = head_;
        head_ = first;
        size_ += count;
    }

    Range pop_range(std::size_t n) noexcept {
        assert(n > 0);
        Range range;

        if (head_ == nullptr) {
            return range;
        }

        range.first = head_;
        range.last = head_;
        range.count = 1;

        while (range.count < n && next(range.last) != nullptr) {
            range.last = next(range.last);
            ++range.count;
        }

        head_ = next(range.last);
        next(range.last) = nullptr;
        size_ -= range.count;
        return range;
    }

    void clear() noexcept {
        head_ = nullptr;
        size_ = 0;
    }

    template <typename Predicate>
    std::size_t count_if(Predicate pred) const noexcept {
        std::size_t count = 0;
        void* cur = head_;

        while (cur != nullptr) {
            if (pred(cur)) {
                ++count;
            }
            cur = next(cur);
        }

        return count;
    }

    template <typename Predicate>
    std::size_t remove_if(Predicate pred) noexcept {
        std::size_t removed = 0;
        void* cur = head_;
        void* prev = nullptr;

        while (cur != nullptr) {
            void* nxt = next(cur);
            if (pred(cur)) {
                if (prev == nullptr) {
                    head_ = nxt;
                } else {
                    next(prev) = nxt;
                }
                next(cur) = nullptr;
                --size_;
                ++removed;
            } else {
                prev = cur;
            }
            cur = nxt;
        }

        return removed;
    }

private:
    static void*& next(void* obj) noexcept {
        return *reinterpret_cast<void**>(obj);
    }

private:
    void* head_;
    std::size_t size_;
};

}  // namespace mini_tcmalloc

#endif
