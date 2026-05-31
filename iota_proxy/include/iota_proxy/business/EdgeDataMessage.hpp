#ifndef IOTA_PROXY_BUSINESS_EDGE_DATA_MESSAGE_HPP
#define IOTA_PROXY_BUSINESS_EDGE_DATA_MESSAGE_HPP

#include <cstdint>
#include <string>

#include "iota_proxy/common/ByteView.hpp"
#include "iota_proxy/frame/TunnelFrame.hpp"

namespace iota_proxy {

struct EdgeDataMessage {
    std::string mac_hex;
    std::string mac_addr;
    std::uint64_t timestamp_ms = 0;
    std::uint64_t stream_id = 0;
    std::string payload_base64;
    std::size_t payload_len = 0;
    std::string origin;
    std::string message_id;
    std::int64_t received_at_ms = 0;
};

bool make_edge_data_message(std::uint64_t stream_id,
                            ByteView frame,
                            const BroadcastMetadata& metadata,
                            EdgeDataMessage* out);
std::string serialize_edge_data_message_json(const EdgeDataMessage& message);

}  // namespace iota_proxy

#endif
