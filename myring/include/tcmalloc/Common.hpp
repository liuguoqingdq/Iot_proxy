#ifndef COMMON_HPP
#define COMMON_HPP

#include <cstddef>

namespace mini_tcmalloc {
    static constexpr std::size_t kPageSize = 8*1024;
    static constexpr std::size_t kMaxSmallSize = 8*1024;

    static constexpr std::size_t kInvalidSizeClass = static_cast<std::size_t> (-1);



    static constexpr std::size_t kDefaultThreadCacheLimit = 512 * 1024;
    //向上取到alignment的整倍数
    inline std::size_t align_up(std::size_t size,std::size_t alignment) noexcept {
        return (size+alignment -1) & ~(alignment -1);
    }
}






#endif
