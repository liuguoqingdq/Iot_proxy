#ifndef OBJECT_POOL_OPTIONS_HPP
#define OBJECT_POOL_OPTIONS_HPP

#include <cstddef>
#include <string>

namespace pool {

//配置
    struct ObjectPoolOptions {
        std::string name;
        std::size_t max_alive = 0;

        //预热
        bool prewarm_on_construct = false;
        std::size_t prewarm_count = 0;
    };
}

#endif