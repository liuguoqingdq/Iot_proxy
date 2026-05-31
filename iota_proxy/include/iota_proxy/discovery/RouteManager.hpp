#ifndef IOTA_PROXY_DISCOVERY_ROUTE_MANAGER_HPP
#define IOTA_PROXY_DISCOVERY_ROUTE_MANAGER_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "iota_proxy/business/NodeIdentity.hpp"

namespace iota_proxy {

enum class RouteState {
    New,
    Tried,
    Bad
};

struct NodeCapability {
    bool present = false;
    bool accepting_ingress = true;
    std::size_t max_kcp_links = 0;
    std::size_t current_kcp_links = 0;
    std::size_t max_ingress_streams = 0;
    std::size_t current_ingress_streams = 0;
    std::uint64_t max_ingress_bytes_per_second = 0;
    std::uint64_t current_ingress_bytes_per_second = 0;
    std::int64_t updated_at_ms = 0;
};

struct RouteEntry {
    NodeAddress node;
    NodeAddress source;
    NodeCapability capability;
    RouteState state = RouteState::New;
    bool has_source = false;
    bool kcp_connected = false;
    std::uint32_t attempts = 0;
    std::uint32_t failures = 0;
    std::uint8_t hop_count = 0;
    std::int64_t last_seen_ms = 0;
    std::int64_t last_try_ms = 0;
    std::int64_t last_success_ms = 0;
};

struct RouteNextHop {
    NodeAddress target;
    NodeAddress next_hop;
    bool direct = false;
    std::uint8_t hop_count = 0;
};

struct RouteManagerConfig {
    std::size_t max_routes = 4096;
    std::size_t max_kcp_links = 512;
    std::size_t max_response_routes = 2500;
    std::uint8_t max_response_percent = 23;
    std::uint32_t retry_backoff_seconds = 60;
    std::uint32_t stale_after_seconds = 30U * 24U * 60U * 60U;
    std::uint32_t give_up_attempts = 3;
    std::uint32_t bad_failure_threshold = 10;
    std::uint8_t max_route_hops = 8;
    std::uint32_t capability_stale_after_seconds = 120;
};

class RouteManager {
public:
    explicit RouteManager(const RouteManagerConfig& config = RouteManagerConfig());

    bool add_route(const NodeAddress& node,
                   const NodeAddress* source = nullptr,
                   bool kcp_connected = false,
                   std::uint8_t advertised_hop_count = 0,
                   const NodeCapability* capability = nullptr);
    bool mark_attempt(const myring::kcp::KcpConv& node_id);
    bool mark_success(const myring::kcp::KcpConv& node_id,
                      bool kcp_connected);
    bool mark_failure(const myring::kcp::KcpConv& node_id);
    bool mark_kcp_disconnected(const myring::kcp::KcpConv& node_id);

    bool can_add_kcp_link() const;
    std::size_t route_count() const;
    std::size_t kcp_link_count() const;

    std::vector<RouteEntry> snapshot() const;
    std::vector<RouteEntry> select_routes_for_response() const;
    std::optional<RouteEntry> select_for_feeler() const;
    std::optional<RouteNextHop> select_next_hop(
        const myring::kcp::KcpConv& target_id) const;
    std::optional<RouteNextHop> select_default_next_hop() const;
    std::optional<RouteEntry> find(const myring::kcp::KcpConv& node_id) const;

    bool load_json_file(const std::string& path,
                        const myring::kcp::KcpConv& local_id,
                        std::string* error = nullptr);
    bool save_json_file(const std::string& path,
                        const myring::kcp::KcpConv& local_id,
                        std::string* error = nullptr) const;

private:
    struct ScoredRoute {
        RouteEntry entry;
        double score = 0.0;
        std::uint64_t tie_breaker = 0;
    };

    bool is_terrible(const RouteEntry& entry, std::int64_t now_ms) const;
    bool in_retry_backoff(const RouteEntry& entry,
                          std::int64_t now_ms) const;
    double route_chance(const RouteEntry& entry, std::int64_t now_ms) const;
    double capability_score(const RouteEntry& entry,
                            std::int64_t now_ms) const;
    double route_score(const RouteEntry& entry, std::int64_t now_ms) const;
    void trim_if_needed(std::int64_t now_ms);

private:
    RouteManagerConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<myring::kcp::KcpConv,
                       RouteEntry,
                       myring::kcp::KcpConvHash> routes_;
    std::size_t kcp_link_count_;
    mutable std::mt19937_64 rng_;
};

std::int64_t route_time_now_ms() noexcept;

}  // namespace iota_proxy

#endif
