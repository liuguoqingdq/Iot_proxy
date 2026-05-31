#include "iota_proxy/frame/EdgeDataFrame.hpp"

#include <cstring>

namespace iota_proxy {
namespace {

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

}  // namespace

bool edge_data_frame_payload_size(ByteView header,
                                  std::uint32_t* out) noexcept {
    if (out == nullptr ||
        header.data() == nullptr ||
        header.size() < kEdgeDataFrameHeaderSize) {
        return false;
    }

    *out = load_u32(
        header.data() + kEdgeMacSize + kEdgeTimestampSize
    );
    return true;
}

bool decode_edge_data_frame(ByteView frame, EdgeDataFrameView* out) noexcept {
    if (out == nullptr ||
        frame.data() == nullptr ||
        frame.size() < kEdgeDataFrameHeaderSize) {
        return false;
    }

    std::uint32_t payload_size = 0;
    if (!edge_data_frame_payload_size(frame, &payload_size)) {
        return false;
    }

    if (frame.size() - kEdgeDataFrameHeaderSize != payload_size) {
        return false;
    }

    EdgeDataFrameView view;
    std::memcpy(view.mac.data(), frame.data(), view.mac.size());
    view.timestamp_ms = load_u64(frame.data() + kEdgeMacSize);
    view.payload = ByteView(
        frame.data() + kEdgeDataFrameHeaderSize,
        payload_size
    );
    *out = view;
    return true;
}

std::string edge_mac_to_hex(
    const std::array<std::uint8_t, kEdgeMacSize>& mac) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(kEdgeMacSize * 2);
    for (std::uint8_t byte : mac) {
        out.push_back(kHex[(byte >> 4) & 0x0f]);
        out.push_back(kHex[byte & 0x0f]);
    }
    return out;
}

}  // namespace iota_proxy
