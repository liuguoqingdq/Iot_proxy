#include "iota_proxy/business/EdgeDataMessage.hpp"

#include "iota_proxy/business/NodeIdentity.hpp"
#include "iota_proxy/frame/EdgeDataFrame.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cctype>
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

std::string json_string(const nlohmann::json& doc, const char* key) {
    const auto it = doc.find(key);
    if (it == doc.end() || !it->is_string()) {
        return std::string();
    }
    return it->get<std::string>();
}

std::uint64_t json_u64(const nlohmann::json& doc, const char* key) {
    const auto it = doc.find(key);
    if (it == doc.end()) {
        return 0;
    }
    if (it->is_number_unsigned()) {
        return it->get<std::uint64_t>();
    }
    if (it->is_number_integer()) {
        const auto value = it->get<long long>();
        return value < 0 ? 0 : static_cast<std::uint64_t>(value);
    }
    return 0;
}

std::string compact_mac_hex(std::string value) {
    std::string compact;
    compact.reserve(value.size());
    for (char ch : value) {
        if (ch == ':' || ch == '-' || std::isspace(
                static_cast<unsigned char>(ch))) {
            continue;
        }
        compact.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch))));
    }
    if (compact.size() != 12) {
        return std::string();
    }
    for (char ch : compact) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return std::string();
        }
    }
    return compact;
}

std::string mac_from_topic(const std::string& topic) {
    const std::string marker = "/devices/";
    const std::size_t marker_pos = topic.find(marker);
    if (marker_pos == std::string::npos) {
        return std::string();
    }
    const std::size_t mac_begin = marker_pos + marker.size();
    const std::size_t mac_end = topic.find('/', mac_begin);
    if (mac_end == std::string::npos ||
        topic.compare(mac_end, 10, "/telemetry") != 0) {
        return std::string();
    }
    return topic.substr(mac_begin, mac_end - mac_begin);
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

bool parse_edge_kafka_message_json(ByteView payload,
                                   const BroadcastMetadata& metadata,
                                   EdgeDataMessage* out) {
    if (out == nullptr || payload.empty()) {
        return false;
    }

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(payload.data(),
                                    payload.data() + payload.size());
    } catch (...) {
        return false;
    }
    if (!doc.is_object()) {
        return false;
    }

    const std::string schema = json_string(doc, "schema");
    if (!schema.empty() && schema != "iota.edge.raw.v1") {
        return false;
    }

    EdgeDataMessage message;
    message.mac_hex = compact_mac_hex(json_string(doc, "mac_hex"));
    if (message.mac_hex.empty()) {
        message.mac_hex = compact_mac_hex(json_string(doc, "mac_addr"));
    }
    if (message.mac_hex.empty()) {
        message.mac_hex =
            compact_mac_hex(mac_from_topic(json_string(doc, "topic")));
    }
    if (message.mac_hex.empty()) {
        return false;
    }
    message.mac_addr = mac_hex_to_addr(message.mac_hex);
    message.timestamp_ms = json_u64(doc, "timestamp_ms");
    if (message.timestamp_ms == 0) {
        message.timestamp_ms = json_u64(doc, "timestamp");
    }
    message.stream_id = json_u64(doc, "stream_id");
    message.payload_base64 = json_string(doc, "payload_base64");
    message.payload_len = json_u64(doc, "payload_len");
    message.origin = json_string(doc, "origin");
    message.message_id = json_string(doc, "message_id");
    if (message.message_id.empty()) {
        message.message_id = json_string(doc, "mqtt_message_id");
    }
    message.received_at_ms = now_ms();

    if (metadata.valid) {
        if (message.origin.empty()) {
            message.origin = convid_to_hex(metadata.origin);
        }
        if (message.message_id.empty()) {
            message.message_id = convid_to_hex(metadata.message_id);
        }
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
