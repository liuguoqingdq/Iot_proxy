#include "iota_proxy/kcp/KcpTunnel.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace iota_proxy {
namespace {

constexpr std::uint32_t kBroadcastMagic = 0x49504742u;  // IPGB
constexpr std::uint8_t kBroadcastVersion = 1;
constexpr std::size_t kBroadcastHeaderSize = 76;
constexpr std::size_t kBroadcastOriginOffset = 8;
constexpr std::size_t kBroadcastIdOffset = 40;
constexpr std::size_t kBroadcastPayloadLenOffset = 72;

struct BroadcastEnvelopeView {
    myring::kcp::KcpConv origin;
    myring::kcp::KcpConv message_id;
    std::uint8_t ttl = 0;
    ByteView payload;
};

std::int64_t now_ms() noexcept {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
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

std::uint32_t load_u32(const char* src) noexcept {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(src);
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

bool is_relayed_type(TunnelFrameType type) noexcept {
    return type == TunnelFrameType::Control ||
           type == TunnelFrameType::Replicate;
}

std::string make_seen_key(TunnelFrameType type,
                          std::uint64_t stream_id,
                          const myring::kcp::KcpConv& origin,
                          const myring::kcp::KcpConv& message_id) {
    std::string key;
    key.reserve(1 + sizeof(stream_id) + origin.size() + message_id.size());
    key.push_back(static_cast<char>(type));
    char stream_bytes[sizeof(stream_id)] = {};
    store_u64(stream_bytes, stream_id);
    key.append(stream_bytes, sizeof(stream_bytes));
    key.append(reinterpret_cast<const char*>(origin.data()), origin.size());
    key.append(reinterpret_cast<const char*>(message_id.data()),
               message_id.size());
    return key;
}

bool make_random_message_id(myring::kcp::KcpConv* out) {
    if (out == nullptr) {
        return false;
    }
    if (RAND_bytes(out->bytes.data(), static_cast<int>(out->bytes.size())) !=
        1) {
        return false;
    }
    if (out->empty()) {
        out->bytes.back() = 1;
    }
    return true;
}

std::string encode_broadcast_envelope(
    const myring::kcp::KcpConv& origin,
    const myring::kcp::KcpConv& message_id,
    std::uint8_t ttl,
    ByteView payload) {
    if (origin.empty() || message_id.empty() ||
        payload.size() >
            static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return std::string();
    }

    std::string out(kBroadcastHeaderSize + payload.size(), '\0');
    char* data = &out[0];
    store_u32(data, kBroadcastMagic);
    data[4] = static_cast<char>(kBroadcastVersion);
    data[5] = static_cast<char>(ttl);
    data[6] = 0;
    data[7] = 0;
    std::memcpy(data + kBroadcastOriginOffset,
                origin.data(),
                origin.size());
    std::memcpy(data + kBroadcastIdOffset,
                message_id.data(),
                message_id.size());
    store_u32(data + kBroadcastPayloadLenOffset,
              static_cast<std::uint32_t>(payload.size()));
    if (!payload.empty()) {
        out.replace(kBroadcastHeaderSize,
                    payload.size(),
                    payload.data(),
                    payload.size());
    }
    return out;
}

enum class BroadcastDecodeStatus {
    Plain,
    Invalid,
    Envelope
};

BroadcastDecodeStatus decode_broadcast_envelope(ByteView payload,
                                                 BroadcastEnvelopeView* out) {
    if (payload.size() < 4) {
        return BroadcastDecodeStatus::Plain;
    }
    if (load_u32(payload.data()) != kBroadcastMagic) {
        return BroadcastDecodeStatus::Plain;
    }
    if (payload.size() < kBroadcastHeaderSize || out == nullptr ||
        static_cast<std::uint8_t>(payload.data()[4]) != kBroadcastVersion ||
        payload.data()[6] != 0 ||
        payload.data()[7] != 0) {
        return BroadcastDecodeStatus::Invalid;
    }

    const std::uint32_t payload_len =
        load_u32(payload.data() + kBroadcastPayloadLenOffset);
    if (payload.size() - kBroadcastHeaderSize != payload_len) {
        return BroadcastDecodeStatus::Invalid;
    }

    BroadcastEnvelopeView view;
    std::memcpy(view.origin.data(),
                payload.data() + kBroadcastOriginOffset,
                view.origin.size());
    std::memcpy(view.message_id.data(),
                payload.data() + kBroadcastIdOffset,
                view.message_id.size());
    if (view.origin.empty() || view.message_id.empty()) {
        return BroadcastDecodeStatus::Invalid;
    }
    view.ttl = static_cast<std::uint8_t>(payload.data()[5]);
    view.payload = ByteView(
        payload.data() + kBroadcastHeaderSize,
        payload_len
    );
    *out = view;
    return BroadcastDecodeStatus::Envelope;
}

}  // namespace

KcpTunnel::KcpTunnel(const KcpTunnelConfig& config)
    : config_(config),
      peer_addr_(),
      manager_(config.manager),
      frame_callback_(),
      control_callback_(),
      peers_mutex_(),
      peer_links_(),
      seen_mutex_(),
      seen_broadcasts_(),
      running_(false) {
    std::memset(&peer_addr_, 0, sizeof(peer_addr_));
}

KcpTunnel::~KcpTunnel() {
    stop();
}

void KcpTunnel::set_frame_callback(KcpFrameCallback callback) {
    frame_callback_ = std::move(callback);
}

void KcpTunnel::set_control_callback(KcpControlCallback callback) {
    control_callback_ = std::move(callback);
}

bool KcpTunnel::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return true;
    }

    if (!config_.peer.endpoint.host.empty() &&
        config_.peer.endpoint.port != 0) {
        if (!make_sockaddr(config_.peer.endpoint, &peer_addr_)) {
            running_.store(false, std::memory_order_release);
            return false;
        }
        add_peer_link(config_.peer, config_.peer.id);
    }

    if (!manager_.bind(config_.local.host, config_.local.port)) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    manager_.set_message_callback(
        [this](const myring::kcp::KcpSessionKey& key,
               const char* data,
               std::size_t len) {
            handle_kcp_message(key, data, len);
        }
    );
    manager_.start();
    return true;
}

void KcpTunnel::stop() {
    const bool was_running =
        running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running) {
        return;
    }

    manager_.stop();
}

bool KcpTunnel::add_peer_link(const NodeAddress& peer,
                              const myring::kcp::KcpConv& conv) {
    if (peer.endpoint.host.empty() || peer.endpoint.port == 0 ||
        peer.id.empty() || conv.empty()) {
        return false;
    }

    PeerLink link;
    link.peer = peer;
    link.conv = conv;
    if (!make_sockaddr(peer.endpoint, &link.addr)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(peers_mutex_);
    if (peer_links_.find(peer.id) == peer_links_.end() &&
        peer_links_.size() >= config_.max_peer_links) {
        return false;
    }
    peer_links_[peer.id] = link;
    return true;
}

bool KcpTunnel::send_frame(TunnelFrameType type,
                           std::uint64_t stream_id,
                           ByteView payload) {
    if (!running_.load(std::memory_order_acquire) || stream_id == 0) {
        return false;
    }

    std::string packet = encode_tunnel_frame(type, stream_id, payload);
    if (packet.empty()) {
        return false;
    }

    return manager_.send(
        reinterpret_cast<const sockaddr*>(&peer_addr_),
        sizeof(peer_addr_),
        config_.peer.id,
        std::move(packet)
    );
}

bool KcpTunnel::send_frame_to(const myring::kcp::KcpConv& next_hop_id,
                              TunnelFrameType type,
                              std::uint64_t stream_id,
                              ByteView payload) {
    if (!running_.load(std::memory_order_acquire) ||
        next_hop_id.empty() ||
        stream_id == 0) {
        return false;
    }

    PeerLink link;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        const auto it = peer_links_.find(next_hop_id);
        if (it == peer_links_.end()) {
            return false;
        }
        link = it->second;
    }

    std::string packet = encode_tunnel_frame(type, stream_id, payload);
    if (packet.empty()) {
        return false;
    }

    return manager_.send(
        reinterpret_cast<const sockaddr*>(&link.addr),
        sizeof(link.addr),
        link.conv,
        std::move(packet)
    );
}

bool KcpTunnel::send_control_to(const NodeAddress& peer,
                                const myring::kcp::KcpConv& conv,
                                ByteView payload) {
    if (!running_.load(std::memory_order_acquire) || payload.empty()) {
        return false;
    }

    sockaddr_in addr;
    if (!make_sockaddr(peer.endpoint, &addr)) {
        return false;
    }

    std::string packet =
        encode_tunnel_frame(TunnelFrameType::Control, 0, payload);
    if (packet.empty()) {
        return false;
    }

    return manager_.send(
        reinterpret_cast<const sockaddr*>(&addr),
        sizeof(addr),
        conv,
        std::move(packet)
    );
}

std::size_t KcpTunnel::broadcast_control(ByteView payload) {
    if (!running_.load(std::memory_order_acquire) || payload.empty()) {
        return 0;
    }

    ByteView wire_payload = payload;
    std::string envelope;
    myring::kcp::KcpConv message_id;
    if (!config_.identity.id.empty()) {
        if (!make_random_message_id(&message_id)) {
            return 0;
        }
        envelope = encode_broadcast_envelope(
            config_.identity.id,
            message_id,
            config_.broadcast_ttl,
            payload
        );
        if (envelope.empty()) {
            return 0;
        }
        mark_broadcast_seen(make_seen_key(
            TunnelFrameType::Control,
            0,
            config_.identity.id,
            message_id
        ));
        wire_payload = ByteView(envelope.data(), envelope.size());
    }

    std::string packet =
        encode_tunnel_frame(TunnelFrameType::Control, 0, wire_payload);
    if (packet.empty()) {
        return 0;
    }
    return broadcast_packet(packet,
                            message_id.empty() ? nullptr : &message_id);
}

std::size_t KcpTunnel::broadcast_frame(TunnelFrameType type,
                                       std::uint64_t stream_id,
                                       ByteView payload,
                                       const BroadcastMetadata* metadata) {
    if (!running_.load(std::memory_order_acquire) || stream_id == 0) {
        return 0;
    }

    ByteView wire_payload = payload;
    std::string envelope;
    myring::kcp::KcpConv message_id;
    if (is_relayed_type(type) && !config_.identity.id.empty()) {
        myring::kcp::KcpConv origin = config_.identity.id;
        if (metadata != nullptr && metadata->valid &&
            !metadata->origin.empty() && !metadata->message_id.empty()) {
            origin = metadata->origin;
            message_id = metadata->message_id;
        } else if (!make_random_message_id(&message_id)) {
            return 0;
        }
        envelope = encode_broadcast_envelope(
            origin,
            message_id,
            config_.broadcast_ttl,
            payload
        );
        if (envelope.empty()) {
            return 0;
        }
        mark_broadcast_seen(make_seen_key(
            type,
            stream_id,
            origin,
            message_id
        ));
        wire_payload = ByteView(envelope.data(), envelope.size());
    }

    std::string packet = encode_tunnel_frame(type, stream_id, wire_payload);
    if (packet.empty()) {
        return 0;
    }

    return broadcast_packet(packet,
                            message_id.empty() ? nullptr : &message_id);
}

std::size_t KcpTunnel::peer_link_count() const {
    std::lock_guard<std::mutex> lock(peers_mutex_);
    return peer_links_.size();
}

void KcpTunnel::handle_kcp_message(
    const myring::kcp::KcpSessionKey& key,
    const char* data,
    std::size_t len) {
    TunnelFrameView frame;
    if (!decode_tunnel_frame(ByteView(data, len), &frame)) {
        return;
    }

    std::string decoded_payload;
    bool decoded_broadcast = false;
    if (!prepare_inbound_broadcast(
            frame.type,
            frame.stream_id,
            frame.payload,
            &decoded_payload,
            &decoded_broadcast,
            &frame.broadcast)) {
        return;
    }
    if (decoded_broadcast) {
        frame.payload = ByteView(decoded_payload.data(), decoded_payload.size());
    }

    if (frame.type == TunnelFrameType::Control) {
        if (control_callback_) {
            control_callback_(key, frame.payload);
        }
        return;
    }

    if (frame_callback_) {
        frame_callback_(frame);
    }
}

bool KcpTunnel::prepare_inbound_broadcast(
    TunnelFrameType type,
    std::uint64_t stream_id,
    ByteView payload,
    std::string* decoded_payload,
    bool* decoded,
    BroadcastMetadata* metadata) {
    if (!is_relayed_type(type) || decoded_payload == nullptr ||
        decoded == nullptr || metadata == nullptr) {
        return true;
    }
    *decoded = false;
    *metadata = BroadcastMetadata();

    BroadcastEnvelopeView envelope;
    const BroadcastDecodeStatus status =
        decode_broadcast_envelope(payload, &envelope);
    if (status == BroadcastDecodeStatus::Plain) {
        return true;
    }
    if (status == BroadcastDecodeStatus::Invalid) {
        return false;
    }
    if (envelope.origin == config_.identity.id) {
        return false;
    }

    const std::string seen_key =
        make_seen_key(type, stream_id, envelope.origin, envelope.message_id);
    if (!mark_broadcast_seen(seen_key)) {
        return false;
    }

    if (envelope.ttl > 1) {
        const std::uint8_t next_ttl =
            static_cast<std::uint8_t>(envelope.ttl - 1);
        const std::string relay_payload = encode_broadcast_envelope(
            envelope.origin,
            envelope.message_id,
            next_ttl,
            envelope.payload
        );
        if (!relay_payload.empty()) {
            const std::string packet = encode_tunnel_frame(
                type,
                stream_id,
                ByteView(relay_payload.data(), relay_payload.size())
            );
            if (!packet.empty()) {
                broadcast_packet(packet, &envelope.message_id);
            }
        }
    }

    decoded_payload->assign(envelope.payload.data(), envelope.payload.size());
    *decoded = true;
    metadata->valid = true;
    metadata->origin = envelope.origin;
    metadata->message_id = envelope.message_id;
    return true;
}

std::size_t KcpTunnel::broadcast_packet(
    const std::string& packet,
    const myring::kcp::KcpConv* message_id) {
    if (!running_.load(std::memory_order_acquire) || packet.empty()) {
        return 0;
    }

    std::vector<PeerLink> peers;
    {
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peers.reserve(peer_links_.size());
        for (const auto& item : peer_links_) {
            peers.push_back(item.second);
        }
    }

    if (message_id != nullptr && config_.max_broadcast_fanout != 0 &&
        peers.size() > config_.max_broadcast_fanout) {
        const auto score = [message_id](const PeerLink& link) {
            std::uint64_t hash = 1469598103934665603ULL;
            const auto mix_byte = [&hash](std::uint8_t byte) {
                hash ^= static_cast<std::uint64_t>(byte);
                hash *= 1099511628211ULL;
            };
            for (std::uint8_t byte : message_id->bytes) {
                mix_byte(byte);
            }
            for (std::uint8_t byte : link.peer.id.bytes) {
                mix_byte(byte);
            }
            for (std::uint8_t byte : link.conv.bytes) {
                mix_byte(byte);
            }
            const unsigned char* addr =
                reinterpret_cast<const unsigned char*>(
                    &link.addr.sin_addr.s_addr);
            for (std::size_t i = 0; i < sizeof(link.addr.sin_addr.s_addr);
                 ++i) {
                mix_byte(addr[i]);
            }
            const unsigned char* port =
                reinterpret_cast<const unsigned char*>(&link.addr.sin_port);
            for (std::size_t i = 0; i < sizeof(link.addr.sin_port); ++i) {
                mix_byte(port[i]);
            }
            return hash;
        };
        std::sort(peers.begin(),
                  peers.end(),
                  [&score](const PeerLink& lhs, const PeerLink& rhs) {
                      return score(lhs) < score(rhs);
                  });
        peers.resize(config_.max_broadcast_fanout);
    }

    std::size_t sent = 0;
    for (const PeerLink& link : peers) {
        if (manager_.send(
                reinterpret_cast<const sockaddr*>(&link.addr),
                sizeof(link.addr),
                link.conv,
                std::string(packet))) {
            ++sent;
        }
    }
    return sent;
}

bool KcpTunnel::mark_broadcast_seen(const std::string& key) {
    if (key.empty() || config_.max_seen_broadcasts == 0) {
        return true;
    }

    const std::int64_t now = now_ms();
    const std::int64_t ttl_ms =
        static_cast<std::int64_t>(config_.seen_broadcast_seconds) * 1000;
    const std::int64_t expires_at = ttl_ms <= 0
        ? std::numeric_limits<std::int64_t>::max()
        : now + ttl_ms;

    std::lock_guard<std::mutex> lock(seen_mutex_);
    for (auto it = seen_broadcasts_.begin();
         it != seen_broadcasts_.end();) {
        if (it->second <= now) {
            it = seen_broadcasts_.erase(it);
        } else {
            ++it;
        }
    }

    auto it = seen_broadcasts_.find(key);
    if (it != seen_broadcasts_.end()) {
        it->second = expires_at;
        return false;
    }

    while (seen_broadcasts_.size() >= config_.max_seen_broadcasts &&
           !seen_broadcasts_.empty()) {
        seen_broadcasts_.erase(seen_broadcasts_.begin());
    }
    seen_broadcasts_.emplace(key, expires_at);
    return true;
}

}  // namespace iota_proxy
