#ifndef CENTRAL_CACHE_HPP
#define CENTRAL_CACHE_HPP

#include <array>
#include <cstddef>
#include <mutex>
#include <vector>

#include "nocopyable.h"
#include "FreeList.hpp"
#include "SizeClass.hpp"
#include "Span.hpp"


namespace mini_tcmalloc {

    class CentralCache : nocopyable{
        public:
        static CentralCache& instance();

        FreeList::Range fetch_range(std::size_t class_index,std::size_t n);

        void release_range(
        std::size_t class_index,
        void* first,
        void* last,
        std::size_t count);
            /*
        统计接口。
    */
    std::size_t free_count(std::size_t class_index) const;
    std::size_t span_count(std::size_t class_index) const;
    std::size_t total_objects(std::size_t class_index) const;

    private:
    CentralCache() = default;

    private:
    struct CentralClass {
        mutable std::mutex mutex;
        FreeList free_list;

        std::vector<Span*> spans;

        std::size_t total_objects = 0;
    };
    
    std::array<CentralClass,SizeClass::kNumClasses> classes_;

    bool populate_unlocked(std::size_t class_index,CentralClass& cls);

    static void*& next(void* obj) noexcept {
        return *reinterpret_cast<void**>(obj);
    }

    void on_fetch_range_unlocked(const FreeList::Range& range);
    void on_release_range_unlocked(void* first,std::size_t count);
    void release_empty_spans_unlocked(CentralClass& cls);

    };
}




#endif
