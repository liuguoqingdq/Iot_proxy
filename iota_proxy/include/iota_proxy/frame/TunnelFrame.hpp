#ifndef IOTA_PROXY_FRAME_TUNNEL_FRAME_HPP
#define IOTA_PROXY_FRAME_TUNNEL_FRAME_HPP

#include <cstdint>
#include <string>

#include "Kcp/KcpConv.hpp"
#include "iota_proxy/common/ByteView.hpp"

namespace iota_proxy {

enum class TunnelFrameType : std::uint8_t {
    Open = 1,
    Data = 2,
    Close = 3,
    Control = 4,
    Finish = 5,
    Replicate = 6,
    KafkaBroadcast = 7
};

struct BroadcastMetadata {
    bool valid = false;
    std::uint8_t ttl = 0;
    myring::kcp::KcpConv origin;
    myring::kcp::KcpConv message_id;
};

struct TunnelFrameView {
    TunnelFrameType type = TunnelFrameType::Data;
    std::uint64_t stream_id = 0;
    ByteView payload;
    BroadcastMetadata broadcast;
};

constexpr std::size_t kTunnelFrameHeaderSize = 20;

std::string encode_tunnel_frame(TunnelFrameType type,
                                std::uint64_t stream_id,
                                ByteView payload);
bool decode_tunnel_frame(ByteView packet, TunnelFrameView* out) noexcept;
const char* tunnel_frame_type_name(TunnelFrameType type) noexcept;

}  // namespace iota_proxy

#endif
