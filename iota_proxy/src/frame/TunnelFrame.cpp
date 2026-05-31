#include "iota_proxy/frame/TunnelFrame.hpp"

#include <limits>

namespace iota_proxy {
namespace {

constexpr std::uint32_t kMagic = 0x49505450u;
constexpr std::uint8_t kVersion = 1;

void store_u16(char* dst, std::uint16_t value) noexcept {
    dst[0] = static_cast<char>((value >> 8) & 0xffu);
    dst[1] = static_cast<char>(value & 0xffu);
}

void store_u32(char* dst, std::uint32_t value) noexcept {
    dst[0] = static_cast<char>((value >> 24) & 0xffu);
    dst[1] = static_cast<char>((value >> 16) & 0xffu);
    dst[2] = static_cast<char>((value >> 8) & 0xffu);
    dst[3] = static_cast<char>(value & 0xffu);
}

void store_u64(char* dst, std::uint64_t value) noexcept {
    for (int i = 7; i >= 0; --i) {
        dst[7 - i] = static_cast<char>((value >> (i * 8)) & 0xffu);
    }
}

std::uint16_t load_u16(const char* src) noexcept {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(src);
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) |
        static_cast<std::uint16_t>(p[1])
    );
}

std::uint32_t load_u32(const char* src) noexcept {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(src);
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint64_t load_u64(const char* src) noexcept {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(src);
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(p[i]);
    }
    return value;
}

bool valid_type(std::uint8_t value) noexcept {
    return value == static_cast<std::uint8_t>(TunnelFrameType::Open) ||
           value == static_cast<std::uint8_t>(TunnelFrameType::Data) ||
           value == static_cast<std::uint8_t>(TunnelFrameType::Close) ||
           value == static_cast<std::uint8_t>(TunnelFrameType::Control) ||
           value == static_cast<std::uint8_t>(TunnelFrameType::Finish) ||
           value == static_cast<std::uint8_t>(TunnelFrameType::Replicate);
}

}  // namespace

std::string encode_tunnel_frame(TunnelFrameType type,
                                std::uint64_t stream_id,
                                ByteView payload) {
    if (payload.size() >
        static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::string();
    }

    std::string packet(kTunnelFrameHeaderSize + payload.size(), '\0');
    char* out = &packet[0];

    store_u32(out, kMagic);
    out[4] = static_cast<char>(kVersion);
    out[5] = static_cast<char>(type);
    store_u16(out + 6, 0);
    store_u64(out + 8, stream_id);
    store_u32(out + 16, static_cast<std::uint32_t>(payload.size()));

    if (!payload.empty()) {
        packet.replace(
            kTunnelFrameHeaderSize,
            payload.size(),
            payload.data(),
            payload.size()
        );
    }

    return packet;
}

bool decode_tunnel_frame(ByteView packet, TunnelFrameView* out) noexcept {
    if (out == nullptr || packet.data() == nullptr ||
        packet.size() < kTunnelFrameHeaderSize) {
        return false;
    }

    const char* data = packet.data();
    if (load_u32(data) != kMagic ||
        static_cast<std::uint8_t>(data[4]) != kVersion ||
        !valid_type(static_cast<std::uint8_t>(data[5]))) {
        return false;
    }

    const std::uint16_t flags = load_u16(data + 6);
    if (flags != 0) {
        return false;
    }

    const std::uint32_t payload_len = load_u32(data + 16);
    if (packet.size() - kTunnelFrameHeaderSize != payload_len) {
        return false;
    }

    out->type = static_cast<TunnelFrameType>(
        static_cast<std::uint8_t>(data[5])
    );
    out->stream_id = load_u64(data + 8);
    out->payload = ByteView(data + kTunnelFrameHeaderSize, payload_len);
    return out->type == TunnelFrameType::Control || out->stream_id != 0;
}

const char* tunnel_frame_type_name(TunnelFrameType type) noexcept {
    switch (type) {
        case TunnelFrameType::Open:
            return "open";
        case TunnelFrameType::Data:
            return "data";
        case TunnelFrameType::Close:
            return "close";
        case TunnelFrameType::Control:
            return "control";
        case TunnelFrameType::Finish:
            return "finish";
        case TunnelFrameType::Replicate:
            return "replicate";
        default:
            return "unknown";
    }
}

}  // namespace iota_proxy
