#ifndef IOTA_PROXY_KCP_KCP_TUNNEL_HPP
#define IOTA_PROXY_KCP_KCP_TUNNEL_HPP

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Kcp/KcpManager.hpp"
#include "iota_proxy/business/NodeIdentity.hpp"
#include "iota_proxy/common/ByteView.hpp"
#include "iota_proxy/common/Endpoint.hpp"
#include "iota_proxy/frame/TunnelFrame.hpp"

namespace iota_proxy {

struct KcpTunnelConfig {
    Ipv4Endpoint local = {"0.0.0.0", 9000};
    NodeIdentity identity;
    NodeAddress peer = {
        Ipv4Endpoint(),
        myring::kcp::KcpConv()
    };
    myring::kcp::KcpManagerConfig manager;
    std::size_t max_peer_links = 512;
    std::uint8_t broadcast_ttl = 4;
    std::size_t max_broadcast_fanout = 32;
    std::size_t max_seen_broadcasts = 8192;
    std::uint32_t seen_broadcast_seconds = 10 * 60;
};

using KcpFrameCallback = std::function<void(const TunnelFrameView&)>;
using KcpControlCallback =
    std::function<void(const myring::kcp::KcpSessionKey&, ByteView)>;

class KcpTunnel {
public:
    explicit KcpTunnel(const KcpTunnelConfig& config);
    ~KcpTunnel();

    void set_frame_callback(KcpFrameCallback callback);
    void set_control_callback(KcpControlCallback callback);

    bool start();
    void stop();

    bool add_peer_link(const NodeAddress& peer,
                       const myring::kcp::KcpConv& conv);
    bool send_frame(TunnelFrameType type,
                    std::uint64_t stream_id,
                    ByteView payload);
    bool send_frame_to(const myring::kcp::KcpConv& next_hop_id,
                       TunnelFrameType type,
                       std::uint64_t stream_id,
                       ByteView payload);
    bool send_control_to(const NodeAddress& peer,
                         const myring::kcp::KcpConv& conv,
                         ByteView payload);
    std::size_t broadcast_control(ByteView payload);
    std::size_t broadcast_frame(TunnelFrameType type,
                                std::uint64_t stream_id,
                                ByteView payload,
                                const BroadcastMetadata* metadata = nullptr);
    std::size_t peer_link_count() const;

private:
    void handle_kcp_message(const myring::kcp::KcpSessionKey& key,
                            const char* data,
                            std::size_t len);
    bool prepare_inbound_broadcast(TunnelFrameType type,
                                   std::uint64_t stream_id,
                                   ByteView payload,
                                   std::string* decoded_payload,
                                   bool* decoded,
                                   BroadcastMetadata* metadata);
    std::size_t broadcast_packet(const std::string& packet,
                                 const myring::kcp::KcpConv* message_id);
    bool mark_broadcast_seen(const std::string& key);

private:
    struct PeerLink {
        NodeAddress peer;
        myring::kcp::KcpConv conv;
        sockaddr_in addr;
    };

    KcpTunnelConfig config_;
    sockaddr_in peer_addr_;
    myring::kcp::KcpManager manager_;
    KcpFrameCallback frame_callback_;
    KcpControlCallback control_callback_;
    mutable std::mutex peers_mutex_;
    std::unordered_map<myring::kcp::KcpConv,
                       PeerLink,
                       myring::kcp::KcpConvHash> peer_links_;
    mutable std::mutex seen_mutex_;
    std::unordered_map<std::string, std::int64_t> seen_broadcasts_;
    std::atomic<bool> running_;
};

}  // namespace iota_proxy

#endif
