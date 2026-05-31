#ifndef OBJECT_POOL_STATS_HPP
#define OBJECT_POOL_STATS_HPP

#include <cstddef>
#include <string>

namespace pool {
    struct ObjectPoolStats {
    std::string name;

    std::size_t created = 0;
    std::size_t destroyed = 0;
    std::size_t alive = 0;
    std::size_t peak_alive = 0;

    std::size_t allocation_failed = 0;
    std::size_t limit_rejected = 0;

    std::size_t prewarm_failed = 0;
};
}

#endif
