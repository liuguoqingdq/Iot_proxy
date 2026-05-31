#ifndef MYRING_KCP_KCP_CONV_HPP
#define MYRING_KCP_KCP_CONV_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace myring {
namespace kcp {

struct KcpConv {
    static constexpr std::size_t kSize = 32;

    std::array<std::uint8_t, kSize> bytes;

    KcpConv() noexcept
        : bytes{} {}

    static KcpConv from_uint32(std::uint32_t value) noexcept {
        KcpConv conv;
        conv.bytes[0] = static_cast<std::uint8_t>(value & 0xffu);
        conv.bytes[1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
        conv.bytes[2] = static_cast<std::uint8_t>((value >> 16) & 0xffu);
        conv.bytes[3] = static_cast<std::uint8_t>((value >> 24) & 0xffu);
        return conv;
    }

    bool empty() const noexcept {
        return std::all_of(
            bytes.begin(),
            bytes.end(),
            [](std::uint8_t value) {
                return value == 0;
            }
        );
    }

    const std::uint8_t* data() const noexcept {
        return bytes.data();
    }

    std::uint8_t* data() noexcept {
        return bytes.data();
    }

    std::size_t size() const noexcept {
        return bytes.size();
    }

    bool operator==(const KcpConv& other) const noexcept {
        return bytes == other.bytes;
    }

    bool operator!=(const KcpConv& other) const noexcept {
        return !(*this == other);
    }
};

struct KcpConvHash {
    std::size_t operator()(const KcpConv& conv) const noexcept {
        std::size_t seed = 1469598103934665603ULL;
        for (std::uint8_t byte : conv.bytes) {
            seed ^= static_cast<std::size_t>(byte);
            seed *= 1099511628211ULL;
        }
        return seed;
    }
};

}  // namespace kcp
}  // namespace myring

#endif
