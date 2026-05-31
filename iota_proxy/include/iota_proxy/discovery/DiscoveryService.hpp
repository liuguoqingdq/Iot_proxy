#ifndef IOTA_PROXY_DISCOVERY_DISCOVERY_SERVICE_HPP
#define IOTA_PROXY_DISCOVERY_DISCOVERY_SERVICE_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "iota_proxy/business/NodeIdentity.hpp"
#include "iota_proxy/common/ByteView.hpp"
#include "iota_proxy/discovery/RouteManager.hpp"

namespace iota_proxy {

struct DiscoveryConfig {
    NodeIdentity identity;
    Ipv4Endpoint local_endpoint = {"0.0.0.0", 9000};
    std::vector<NodeAddress> bootstrap_nodes;
    std::size_t max_kcp_links = 512;
    int listen_backlog = 128;
    std::uint32_t route_feeler_interval_seconds = 30;
    std::string route_table_file;
    RouteManagerConfig route_manager;
};

using DiscoveryKcpLinkCallback =
    std::function<bool(const NodeAddress&, const myring::kcp::KcpConv&)>;
using DiscoveryControlBroadcast =
    std::function<std::size_t(ByteView)>;
using DiscoveryCapabilityProvider = std::function<NodeCapability()>;
using DiscoveryKcpAdmissionCallback =
    std::function<bool(const NodeAddress&)>;

class DiscoveryService {
public:
    explicit DiscoveryService(const DiscoveryConfig& config);
    ~DiscoveryService();

    void set_kcp_link_callback(DiscoveryKcpLinkCallback callback);
    void set_control_broadcast(DiscoveryControlBroadcast callback);
    void set_capability_provider(DiscoveryCapabilityProvider callback);
    void set_kcp_admission_callback(DiscoveryKcpAdmissionCallback callback);

    bool start();
    void stop();

    void on_kcp_control(ByteView payload);

    std::vector<RouteEntry> routes() const;
    std::size_t kcp_link_count() const;
    std::optional<RouteNextHop> select_next_hop(
        const myring::kcp::KcpConv& target_id) const;
    std::optional<RouteNextHop> select_default_next_hop() const;

private:
    bool start_listener();
    void accept_loop();
    void bootstrap_loop();
    void feeler_loop();
    void handle_tcp_session(int fd, const std::string& remote_host);
    void connect_bootstrap_node(const NodeAddress& node);

    bool add_route(const NodeAddress& node,
                   const NodeAddress* source,
                   bool kcp_connected,
                   std::uint8_t advertised_hop_count = 0,
                   const NodeCapability* capability = nullptr);
    void announce_route(const RouteEntry& route);
    void send_routes(int fd);
    bool negotiate_kcp_link(const NodeAddress& peer, int fd);
    void handle_control_json(const std::string& payload);
    bool load_route_table();
    bool save_route_table() const;

private:
    DiscoveryConfig config_;
    std::atomic<bool> running_;
    int listen_fd_;
    std::thread accept_thread_;
    std::thread bootstrap_thread_;
    std::thread feeler_thread_;
    RouteManager routes_;
    DiscoveryKcpLinkCallback kcp_link_callback_;
    DiscoveryControlBroadcast control_broadcast_;
    DiscoveryCapabilityProvider capability_provider_;
    DiscoveryKcpAdmissionCallback kcp_admission_callback_;
};

myring::kcp::KcpConv derive_kcp_convid(const Ipv4Endpoint& peer,
                                       const Ipv4Endpoint& local) noexcept;

}  // namespace iota_proxy

#endif
