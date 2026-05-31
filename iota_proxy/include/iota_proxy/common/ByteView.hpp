#ifndef IOTA_PROXY_COMMON_BYTE_VIEW_HPP
#define IOTA_PROXY_COMMON_BYTE_VIEW_HPP

#include <cstddef>
#include <string>

namespace iota_proxy {

class ByteView {
public:
    ByteView() noexcept
        : data_(nullptr),
          size_(0) {}

    ByteView(const char* data, std::size_t size) noexcept
        : data_(data),
          size_(size) {}

    const char* data() const noexcept {
        return data_;
    }

    std::size_t size() const noexcept {
        return size_;
    }

    bool empty() const noexcept {
        return size_ == 0;
    }

private:
    const char* data_;
    std::size_t size_;
};

inline ByteView make_byte_view(const std::string& data) noexcept {
    return ByteView(data.data(), data.size());
}

}  // namespace iota_proxy

#endif
