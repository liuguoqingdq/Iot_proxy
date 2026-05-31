#include "iota_proxy/business/EdgeDataMessage.hpp"

#include "iota_proxy/business/NodeIdentity.hpp"
#include "iota_proxy/frame/EdgeDataFrame.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <utility>

namespace iota_proxy {
namespace {

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               system_clock::now().time_since_epoch()
           )
        .count();
}

std::string base64_encode(const char* data, std::size_t len) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const std::uint32_t b0 =
            static_cast<unsigned char>(data[i]);
        const std::uint32_t b1 =
            i + 1 < len ? static_cast<unsigned char>(data[i + 1]) : 0;
        const std::uint32_t b2 =
            i + 2 < len ? static_cast<unsigned char>(data[i + 2]) : 0;
        const std::uint32_t chunk = (b0 << 16) | (b1 << 8) | b2;

        out.push_back(kAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(i + 1 < len ? kAlphabet[(chunk >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < len ? kAlphabet[chunk & 0x3f] : '=');
    }
    return out;
}

std::string mac_hex_to_addr(const std::string& mac_hex) {
    std::string out;
    out.reserve(17);
    for (std::size_t i = 0; i < mac_hex.size(); i += 2) {
        if (i != 0) {
            out.push_back(':');
        }
        out.append(mac_hex, i, 2);
    }
    return out;
}

}  // namespace

bool make_edge_data_message(std::uint64_t stream_id,
                            ByteView frame,
                            const BroadcastMetadata& metadata,
                            EdgeDataMessage* out) {
    if (out == nullptr) {
        return false;
    }

    EdgeDataFrameView edge;
    if (!decode_edge_data_frame(frame, &edge)) {
        return false;
    }

    EdgeDataMessage message;
    message.mac_hex = edge_mac_to_hex(edge.mac);
    message.mac_addr = mac_hex_to_addr(message.mac_hex);
    message.timestamp_ms = edge.timestamp_ms;
    message.stream_id = stream_id;
    message.payload_base64 =
        base64_encode(edge.payload.data(), edge.payload.size());
    message.payload_len = edge.payload.size();
    message.received_at_ms = now_ms();
    if (metadata.valid) {
        message.origin = convid_to_hex(metadata.origin);
        message.message_id = convid_to_hex(metadata.message_id);
    }

    *out = std::move(message);
    return true;
}

std::string serialize_edge_data_message_json(const EdgeDataMessage& message) {
    nlohmann::json doc;
    doc["schema"] = "iota.edge.raw.v1";
    doc["mac_hex"] = message.mac_hex;
    doc["mac_addr"] = message.mac_addr;
    doc["timestamp_ms"] = message.timestamp_ms;
    doc["stream_id"] = message.stream_id;
    doc["payload_base64"] = message.payload_base64;
    doc["payload_len"] = message.payload_len;
    doc["origin"] = message.origin;
    doc["message_id"] = message.message_id;
    doc["received_at_ms"] = message.received_at_ms;
    return doc.dump();
}

}  // namespace iota_proxy
