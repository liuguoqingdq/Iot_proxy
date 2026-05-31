#ifndef IOTA_PROXY_FRAME_EDGE_DATA_FRAME_HPP
#define IOTA_PROXY_FRAME_EDGE_DATA_FRAME_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "iota_proxy/common/ByteView.hpp"

namespace iota_proxy {

constexpr std::size_t kEdgeMacSize = 6;
constexpr std::size_t kEdgeTimestampSize = 8;
constexpr std::size_t kEdgePayloadLengthSize = 4;
constexpr std::size_t kEdgeDataFrameHeaderSize =
    kEdgeMacSize + kEdgeTimestampSize + kEdgePayloadLengthSize;

struct EdgeDataFrameView {
    std::array<std::uint8_t, kEdgeMacSize> mac{};
    std::uint64_t timestamp_ms = 0;
    ByteView payload;
};

bool decode_edge_data_frame(ByteView frame, EdgeDataFrameView* out) noexcept;
bool edge_data_frame_payload_size(ByteView header,
                                  std::uint32_t* out) noexcept;
std::string edge_mac_to_hex(
    const std::array<std::uint8_t, kEdgeMacSize>& mac);

}  // namespace iota_proxy

#endif
