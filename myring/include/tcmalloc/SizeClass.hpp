#ifndef MINI_TCMALLOC_SIZE_CLASS_HPP
#define MINI_TCMALLOC_SIZE_CLASS_HPP

#include <array>
#include <cassert>
#include <cstddef>

#include "Common.hpp"

namespace mini_tcmalloc {

namespace size_class_detail {

constexpr std::size_t kAlignment = 8;

constexpr std::array<std::size_t, 36> kClassSizes = {
    8, 16, 24, 32, 40, 48, 56, 64,

    80, 96, 112, 128,

    160, 192, 224, 256,

    320, 384, 448, 512,

    640, 768, 896, 1024,

    1280, 1536, 1792, 2048,

    2560, 3072, 3584, 4096,

    5120, 6144, 7168, 8192
};

constexpr std::size_t kNumClasses = kClassSizes.size();
constexpr std::size_t kLookupCount = kMaxSmallSize / kAlignment;

constexpr std::size_t round_up_to_alignment(std::size_t size) noexcept {
    return (size + kAlignment - 1) / kAlignment * kAlignment;
}

constexpr std::size_t find_class_index_slow(std::size_t rounded_size) noexcept {
    for (std::size_t i = 0; i < kNumClasses; ++i) {
        if (rounded_size <= kClassSizes[i]) {
            return i;
        }
    }

    return kNumClasses;
}

inline std::array<std::size_t, kLookupCount> build_class_index_by_size() noexcept {
    std::array<std::size_t, kLookupCount> table = {};

    for (std::size_t slot = 0; slot < kLookupCount; ++slot) {
        const std::size_t rounded_size = (slot + 1) * kAlignment;
        table[slot] = find_class_index_slow(rounded_size);
    }

    return table;
}

inline const std::array<std::size_t, kLookupCount>& class_index_by_size() noexcept {
    static const std::array<std::size_t, kLookupCount> table =
        build_class_index_by_size();
    return table;
}

} // namespace size_class_detail

/*
    SizeClass负责把用户申请的任意小对象大小，映射到固定规格。

    例如：
        1   -> 8
        9   -> 16
        17  -> 24
        65  -> 80
        129 -> 160

    这样做的目的：
        1. ThreadCache不需要为每种任意大小都维护链表
        2. CentralCache可以按固定规格管理对象
        3. 减少内部碎片
*/
class SizeClass {
public:
    /*
        小对象最小对齐粒度。

        我们让所有小对象至少按8字节对齐。
    */
    static constexpr std::size_t kAlignment = size_class_detail::kAlignment;

    /*
        更细的size class表。

        第一段：8字节递增，覆盖8~64
        第二段：16字节递增，覆盖80~128
        第三段：32字节递增，覆盖160~256
        第四段：64字节递增，覆盖320~512
        第五段：128字节递增，覆盖640~1024
        第六段：256字节递增，覆盖1280~2048
        第七段：512字节递增，覆盖2560~4096
        第八段：1024字节递增，覆盖5120~8192

        这不是完整TCMalloc表，但比简单2倍增长更接近工业分配器思路。
    */
    static constexpr std::size_t kNumClasses = size_class_detail::kNumClasses;
    static constexpr std::size_t kLookupCount = size_class_detail::kLookupCount;

    static constexpr std::size_t round_up_to_alignment(
        std::size_t size
    ) noexcept {
        return size_class_detail::round_up_to_alignment(size);
    }

    static constexpr std::size_t find_class_index_slow(
        std::size_t rounded_size
    ) noexcept {
        return size_class_detail::find_class_index_slow(rounded_size);
    }
public:
    static constexpr bool is_small(std::size_t size) noexcept {
        return size > 0 && size <= kMaxSmallSize;
    }

    /*
        根据用户请求大小，返回class index。

        小对象：
            返回合法class index

        大对象：
            返回kNumClasses
    */
    static std::size_t index(std::size_t size) noexcept {
        if (!is_small(size)) {
            return kNumClasses;
        }

        std::size_t rounded = round_up_to_alignment(size);
        std::size_t slot = rounded / kAlignment - 1;
        return size_class_detail::class_index_by_size()[slot];
    }

    /*
        根据class index返回对应块大小。
    */
    static constexpr std::size_t class_size(std::size_t class_index) noexcept {
        assert(class_index < kNumClasses);
        return size_class_detail::kClassSizes[class_index];
    }

    /*
        把用户请求大小向上取整到size class规格。
    */
    static std::size_t round_up(std::size_t size) noexcept {
        std::size_t idx = index(size);

        if (idx == kNumClasses) {
            return size;
        }

        return class_size(idx);
    }

    /*
        ThreadCache本地链表为空时，一次从CentralCache拿多少个对象。

        思路：
            小对象可以多拿一些，减少访问CentralCache次数。
            大对象少拿一些，避免单线程缓存过多内存。

        这里用目标批量大小64KB，然后限制最多64个对象、最少2个对象。
    */
    static constexpr std::size_t batch_count(std::size_t class_index) noexcept {
        assert(class_index < kNumClasses);

        std::size_t size = class_size(class_index);

        constexpr std::size_t target_bytes = 64 * 1024;
        constexpr std::size_t max_batch = 64;
        constexpr std::size_t min_batch = 2;

        std::size_t count = target_bytes / size;

        if (count < min_batch) {
            count = min_batch;
        }

        if (count > max_batch) {
            count = max_batch;
        }

        return count;
    }

    /*
        CentralCache缺对象时，向PageHeap申请多少页。

        目标：
            一个Span大概64KB左右。
            同时保证这个Span至少能容纳一次batch_count需要的对象。
    */
    static constexpr std::size_t pages_for_class(std::size_t class_index) noexcept {
        assert(class_index < kNumClasses);

        std::size_t size = class_size(class_index);
        std::size_t batch = batch_count(class_index);

        constexpr std::size_t target_bytes = 64 * 1024;

        std::size_t need_bytes = size * batch;
        if (need_bytes < target_bytes) {
            need_bytes = target_bytes;
        }

        std::size_t pages = (need_bytes + kPageSize - 1) / kPageSize;

        if (pages == 0) {
            pages = 1;
        }

        return pages;
    }
};

} // namespace mini_tcmalloc

#endif
