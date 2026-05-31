#include "iota_proxy/discovery/DiscoveryService.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/rand.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <nlohmann/json.hpp>
#include <thread>
#include <utility>

namespace iota_proxy {
namespace {

constexpr std::int64_t kHelloClockSkewMs = 10 * 60 * 1000;

int create_tcp_listener(const Ipv4Endpoint& endpoint, int backlog) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port);
    if (endpoint.host.empty() || endpoint.host == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, endpoint.host.c_str(), &addr.sin_addr) !=
               1) {
        ::close(fd);
        return -1;
    }

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) !=
            0 ||
        ::listen(fd, backlog) != 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

int connect_tcp(const Ipv4Endpoint& endpoint) {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr;
    if (!make_sockaddr(endpoint, &addr)) {
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) !=
        0) {
        ::close(fd);
        return -1;
    }

    return fd;
}

bool write_all(int fd, const char* data, std::size_t len) {
    while (len > 0) {
        const ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        data += static_cast<std::size_t>(n);
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

bool write_json_line(int fd, const nlohmann::json& doc) {
    std::string line = doc.dump();
    line.push_back('\n');
    return write_all(fd, line.data(), line.size());
}

bool read_line(int fd, std::string* out) {
    if (out == nullptr) {
        return false;
    }

    out->clear();
    char c = 0;
    while (true) {
        const ssize_t n = ::recv(fd, &c, 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return !out->empty();
        }
        if (c == '\n') {
            return true;
        }
        if (out->size() > 1024 * 1024) {
            return false;
        }
        out->push_back(c);
    }
}

nlohmann::json capability_to_json(const NodeCapability& capability) {
    nlohmann::json item;
    item["accepting_ingress"] = capability.accepting_ingress;
    item["max_kcp_links"] = capability.max_kcp_links;
    item["current_kcp_links"] = capability.current_kcp_links;
    item["max_ingress_streams"] = capability.max_ingress_streams;
    item["current_ingress_streams"] = capability.current_ingress_streams;
    item["max_ingress_bytes_per_second"] =
        capability.max_ingress_bytes_per_second;
    item["current_ingress_bytes_per_second"] =
        capability.current_ingress_bytes_per_second;
    item["updated_at_ms"] = capability.updated_at_ms;
    return item;
}

bool capability_from_json(const nlohmann::json& item,
                          NodeCapability* out) {
    if (out == nullptr || !item.is_object()) {
        return false;
    }

    NodeCapability capability;
    capability.present = true;
    capability.accepting_ingress =
        item.value("accepting_ingress", true);
    capability.max_kcp_links =
        static_cast<std::size_t>(item.value("max_kcp_links", 0ULL));
    capability.current_kcp_links =
        static_cast<std::size_t>(item.value("current_kcp_links", 0ULL));
    capability.max_ingress_streams =
        static_cast<std::size_t>(item.value("max_ingress_streams", 0ULL));
    capability.current_ingress_streams =
        static_cast<std::size_t>(
            item.value("current_ingress_streams", 0ULL)
        );
    capability.max_ingress_bytes_per_second =
        item.value("max_ingress_bytes_per_second", 0ULL);
    capability.current_ingress_bytes_per_second =
        item.value("current_ingress_bytes_per_second", 0ULL);
    capability.updated_at_ms = item.value("updated_at_ms", 0LL);
    *out = capability;
    return true;
}

nlohmann::json node_to_json(const NodeAddress& node,
                            const NodeCapability* capability = nullptr) {
    nlohmann::json item;
    item["host"] = node.endpoint.host;
    item["port"] = node.endpoint.port;
    item["public_key"] = convid_to_hex(node.id);
    if (capability != nullptr && capability->present) {
        item["capability"] = capability_to_json(*capability);
    }
    return item;
}

nlohmann::json route_to_json(const RouteEntry& route) {
    nlohmann::json item = node_to_json(route.node, &route.capability);
    item["hop_count"] = route.hop_count;
    return item;
}

std::uint8_t route_hop_count_from_json(const nlohmann::json& item) {
    const unsigned value = item.value("hop_count", 0U);
    return static_cast<std::uint8_t>(
        std::min<unsigned>(
            value,
            static_cast<unsigned>(std::numeric_limits<std::uint8_t>::max())
        )
    );
}

bool node_from_json(const nlohmann::json& item, NodeAddress* out) {
    if (out == nullptr || !item.is_object() ||
        !item.contains("host") ||
        !item.contains("port") ||
        !item["host"].is_string() ||
        !item["port"].is_number_unsigned()) {
        return false;
    }

    std::string id;
    if (item.contains("public_key") && item["public_key"].is_string()) {
        id = item["public_key"].get<std::string>();
    } else if (item.contains("id") && item["id"].is_string()) {
        id = item["id"].get<std::string>();
    } else {
        return false;
    }

    NodeAddress node;
    node.endpoint.host = item["host"].get<std::string>();
    const unsigned port = item["port"].get<unsigned>();
    if (port == 0 || port > 65535U ||
        !parse_convid_hex(id, &node.id)) {
        return false;
    }
    node.endpoint.port = static_cast<std::uint16_t>(port);
    *out = node;
    return true;
}

bool node_capability_from_json(const nlohmann::json& item,
                               NodeCapability* out) {
    if (out == nullptr || !item.is_object() ||
        !item.contains("capability")) {
        return false;
    }
    return capability_from_json(item["capability"], out);
}

std::string hello_signing_payload(const NodeAddress& node,
                                  const myring::kcp::KcpConv& target_id,
                                  std::int64_t timestamp_ms,
                                  const std::string& nonce_hex) {
    std::string payload;
    payload.reserve(256);
    payload.append("iota-discovery-v1\n");
    payload.append(convid_to_hex(target_id));
    payload.push_back('\n');
    payload.append(convid_to_hex(node.id));
    payload.push_back('\n');
    payload.append(node.endpoint.host);
    payload.push_back('\n');
    payload.append(std::to_string(node.endpoint.port));
    payload.push_back('\n');
    payload.append(std::to_string(timestamp_ms));
    payload.push_back('\n');
    payload.append(nonce_hex);
    return payload;
}

bool attach_hello_signature(nlohmann::json* hello,
                            const DiscoveryConfig& config,
                            const NodeAddress& node,
                            const myring::kcp::KcpConv& target_id) {
    if (hello == nullptr) {
        return false;
    }

    Key256 nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        return false;
    }

    const std::int64_t timestamp_ms = route_time_now_ms();
    const std::string nonce_hex = hex_256(nonce);
    const std::string payload =
        hello_signing_payload(node, target_id, timestamp_ms, nonce_hex);

    Signature512 signature{};
    if (!sign_node_message(
            config.identity.key_pair.private_key,
            make_byte_view(payload),
            &signature
        )) {
        return false;
    }

    (*hello)["timestamp_ms"] = timestamp_ms;
    (*hello)["nonce"] = nonce_hex;
    (*hello)["signature"] = hex_512(signature);
    return true;
}

bool verify_hello_signature(const nlohmann::json& hello,
                            const DiscoveryConfig& config,
                            const NodeAddress& peer) {
    if (!hello.contains("timestamp_ms") ||
        !hello.contains("nonce") ||
        !hello.contains("signature") ||
        !hello["timestamp_ms"].is_number_integer() ||
        !hello["nonce"].is_string() ||
        !hello["signature"].is_string()) {
        return false;
    }

    const std::int64_t timestamp_ms =
        hello["timestamp_ms"].get<std::int64_t>();
    const std::int64_t now_ms = route_time_now_ms();
    if (timestamp_ms < now_ms - kHelloClockSkewMs ||
        timestamp_ms > now_ms + kHelloClockSkewMs) {
        return false;
    }

    const std::string nonce_hex = hello["nonce"].get<std::string>();
    Key256 nonce_check{};
    if (!parse_hex_256(nonce_hex, &nonce_check)) {
        return false;
    }

    Signature512 signature{};
    if (!parse_hex_512(hello["signature"].get<std::string>(),
                       &signature)) {
        return false;
    }

    NodePublicKey public_key;
    if (!public_key_from_node_id(peer.id, &public_key)) {
        return false;
    }

    const std::string payload =
        hello_signing_payload(
            peer,
            config.identity.id,
            timestamp_ms,
            nonce_hex
        );
    return verify_node_message(public_key, make_byte_view(payload), signature);
}

NodeAddress self_node(const DiscoveryConfig& config) {
    NodeAddress self;
    self.endpoint = config.local_endpoint;
    self.id = config.identity.id;
    return self;
}

NodeCapability local_capability(
    const DiscoveryCapabilityProvider& provider) {
    if (provider) {
        NodeCapability capability = provider();
        capability.present = true;
        if (capability.updated_at_ms == 0) {
            capability.updated_at_ms = route_time_now_ms();
        }
        return capability;
    }
    NodeCapability capability;
    capability.present = true;
    capability.updated_at_ms = route_time_now_ms();
    return capability;
}

RouteManagerConfig route_manager_config(const DiscoveryConfig& config) {
    RouteManagerConfig route_config = config.route_manager;
    route_config.max_kcp_links = config.max_kcp_links;
    return route_config;
}

std::string remote_host_from_addr(const sockaddr_in& addr) {
    char buffer[INET_ADDRSTRLEN] = {0};
    const char* ret = ::inet_ntop(
        AF_INET,
        &addr.sin_addr,
        buffer,
        sizeof(buffer)
    );
    return ret == nullptr ? std::string() : std::string(buffer);
}

void encode_ipv4_endpoint_half(const Ipv4Endpoint& endpoint,
                               std::uint8_t* out) noexcept {
    std::memset(out, 0, 16);
    in_addr addr;
    if (::inet_pton(AF_INET, endpoint.host.c_str(), &addr) == 1) {
        std::memcpy(out, &addr.s_addr, 4);
    }
    out[4] = static_cast<std::uint8_t>((endpoint.port >> 8) & 0xffu);
    out[5] = static_cast<std::uint8_t>(endpoint.port & 0xffu);
}

}  // namespace

DiscoveryService::DiscoveryService(const DiscoveryConfig& config)
    : config_(config),
      running_(false),
      listen_fd_(-1),
      accept_thread_(),
      bootstrap_thread_(),
      feeler_thread_(),
      routes_(route_manager_config(config)),
      kcp_link_callback_(),
      control_broadcast_(),
      capability_provider_(),
      kcp_admission_callback_() {}

DiscoveryService::~DiscoveryService() {
    stop();
}

void DiscoveryService::set_kcp_link_callback(
    DiscoveryKcpLinkCallback callback) {
    kcp_link_callback_ = std::move(callback);
}

void DiscoveryService::set_control_broadcast(
    DiscoveryControlBroadcast callback) {
    control_broadcast_ = std::move(callback);
}

void DiscoveryService::set_capability_provider(
    DiscoveryCapabilityProvider callback) {
    capability_provider_ = std::move(callback);
}

void DiscoveryService::set_kcp_admission_callback(
    DiscoveryKcpAdmissionCallback callback) {
    kcp_admission_callback_ = std::move(callback);
}

bool DiscoveryService::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return true;
    }

    load_route_table();
    const NodeCapability capability = local_capability(capability_provider_);
    add_route(self_node(config_), nullptr, true, 0, &capability);

    if (!start_listener()) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    accept_thread_ = std::thread(&DiscoveryService::accept_loop, this);
    bootstrap_thread_ = std::thread(&DiscoveryService::bootstrap_loop, this);
    feeler_thread_ = std::thread(&DiscoveryService::feeler_loop, this);
    return true;
}

void DiscoveryService::stop() {
    const bool was_running =
        running_.exchange(false, std::memory_order_acq_rel);
    if (!was_running) {
        return;
    }

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    if (bootstrap_thread_.joinable()) {
        bootstrap_thread_.join();
    }
    if (feeler_thread_.joinable()) {
        feeler_thread_.join();
    }

    save_route_table();
}

void DiscoveryService::on_kcp_control(ByteView payload) {
    if (payload.empty()) {
        return;
    }

    handle_control_json(std::string(payload.data(), payload.size()));
}

std::vector<RouteEntry> DiscoveryService::routes() const {
    return routes_.snapshot();
}

std::size_t DiscoveryService::kcp_link_count() const {
    return routes_.kcp_link_count();
}

std::optional<RouteNextHop> DiscoveryService::select_next_hop(
    const myring::kcp::KcpConv& target_id) const {
    return routes_.select_next_hop(target_id);
}

std::optional<RouteNextHop> DiscoveryService::select_default_next_hop() const {
    return routes_.select_default_next_hop();
}

bool DiscoveryService::start_listener() {
    listen_fd_ = create_tcp_listener(
        config_.local_endpoint,
        config_.listen_backlog
    );
    return listen_fd_ >= 0;
}

void DiscoveryService::accept_loop() {
    while (running_.load(std::memory_order_acquire)) {
        sockaddr_in addr;
        socklen_t len = sizeof(addr);
        int fd = ::accept4(
            listen_fd_,
            reinterpret_cast<sockaddr*>(&addr),
            &len,
            SOCK_CLOEXEC
        );
        if (fd < 0) {
            if (!running_.load(std::memory_order_acquire) ||
                errno == EBADF ||
                errno == EINVAL) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        std::thread(
            &DiscoveryService::handle_tcp_session,
            this,
            fd,
            remote_host_from_addr(addr)
        ).detach();
    }
}

void DiscoveryService::bootstrap_loop() {
    for (const NodeAddress& node : config_.bootstrap_nodes) {
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }
        if (node.id == config_.identity.id) {
            continue;
        }
        connect_bootstrap_node(node);
    }

    std::vector<RouteEntry> persisted_routes = routes_.snapshot();
    std::sort(
        persisted_routes.begin(),
        persisted_routes.end(),
        [](const RouteEntry& lhs, const RouteEntry& rhs) {
            if (lhs.state != rhs.state) {
                return lhs.state == RouteState::Tried;
            }
            return lhs.last_success_ms > rhs.last_success_ms;
        }
    );
    for (const RouteEntry& entry : persisted_routes) {
        if (!running_.load(std::memory_order_acquire) ||
            !routes_.can_add_kcp_link()) {
            break;
        }
        if (entry.kcp_connected ||
            entry.node.id == config_.identity.id ||
            entry.state == RouteState::Bad) {
            continue;
        }
        connect_bootstrap_node(entry.node);
    }
}

void DiscoveryService::feeler_loop() {
    const std::uint32_t interval_seconds =
        config_.route_feeler_interval_seconds == 0
            ? 30
            : config_.route_feeler_interval_seconds;

    while (running_.load(std::memory_order_acquire)) {
        RouteEntry local_route;
        local_route.node = self_node(config_);
        local_route.capability = local_capability(capability_provider_);
        local_route.kcp_connected = true;
        announce_route(local_route);

        if (routes_.can_add_kcp_link()) {
            const std::optional<RouteEntry> candidate =
                routes_.select_for_feeler();
            if (candidate.has_value() &&
                candidate->node.id != config_.identity.id) {
                connect_bootstrap_node(candidate->node);
            }
        }

        for (std::uint32_t i = 0; i < interval_seconds; ++i) {
            if (!running_.load(std::memory_order_acquire)) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void DiscoveryService::handle_tcp_session(int fd,
                                          const std::string& remote_host) {
    std::string line;
    if (!read_line(fd, &line)) {
        ::close(fd);
        return;
    }

    nlohmann::json hello;
    try {
        hello = nlohmann::json::parse(line);
    } catch (...) {
        ::close(fd);
        return;
    }

    if (!hello.is_object() ||
        hello.value("type", std::string()) != "hello" ||
        hello.value("version", 0) != 1 ||
        hello.value("target_public_key", std::string()) !=
            convid_to_hex(config_.identity.id)) {
        ::close(fd);
        return;
    }

    NodeAddress peer;
    NodeCapability capability;
    if (!hello.contains("node") ||
        !node_from_json(hello["node"], &peer)) {
        ::close(fd);
        return;
    }
    node_capability_from_json(hello["node"], &capability);
    if (!verify_hello_signature(hello, config_, peer)) {
        std::cerr << "discovery: rejected invalid hello from "
                  << remote_host << "\n";
        ::close(fd);
        return;
    }
    if (peer.endpoint.host.empty() || peer.endpoint.host == "0.0.0.0") {
        peer.endpoint.host = remote_host;
    }

    add_route(peer,
              nullptr,
              false,
              0,
              capability.present ? &capability : nullptr);
    send_routes(fd);
    negotiate_kcp_link(peer, fd);
    ::close(fd);
}

void DiscoveryService::connect_bootstrap_node(const NodeAddress& node) {
    add_route(node, nullptr, false);
    routes_.mark_attempt(node.id);

    int fd = connect_tcp(node.endpoint);
    if (fd < 0) {
        routes_.mark_failure(node.id);
        return;
    }

    nlohmann::json hello;
    const NodeAddress local_node = self_node(config_);
    const NodeCapability capability = local_capability(capability_provider_);
    hello["type"] = "hello";
    hello["version"] = 1;
    hello["target_public_key"] = convid_to_hex(node.id);
    hello["node"] = node_to_json(local_node, &capability);
    if (!attach_hello_signature(&hello, config_, local_node, node.id) ||
        !write_json_line(fd, hello)) {
        routes_.mark_failure(node.id);
        ::close(fd);
        return;
    }

    routes_.mark_success(node.id, false);

    std::string line;
    while (read_line(fd, &line)) {
        nlohmann::json message;
        try {
            message = nlohmann::json::parse(line);
        } catch (...) {
            continue;
        }

        const std::string type = message.value("type", std::string());
        if (type == "routes" && message.contains("entries") &&
            message["entries"].is_array()) {
            for (const nlohmann::json& item : message["entries"]) {
                NodeAddress route;
                NodeCapability route_capability;
                if (node_from_json(item, &route)) {
                    node_capability_from_json(item, &route_capability);
                    add_route(
                        route,
                        &node,
                        false,
                        route_hop_count_from_json(item),
                        route_capability.present ? &route_capability : nullptr
                    );
                }
            }
        } else if (type == "node") {
            NodeAddress route;
            NodeCapability route_capability;
            if (message.contains("node") &&
                node_from_json(message["node"], &route)) {
                node_capability_from_json(
                    message["node"],
                    &route_capability
                );
                add_route(
                    route,
                    &node,
                    false,
                    route_hop_count_from_json(message["node"]),
                    route_capability.present ? &route_capability : nullptr
                );
            }
        } else if (type == "kcp_conv") {
            NodeAddress peer;
            NodeCapability peer_capability;
            myring::kcp::KcpConv conv;
            if (message.contains("node") &&
                node_from_json(message["node"], &peer) &&
                message.contains("convid") &&
                message["convid"].is_string() &&
                parse_convid_hex(message["convid"].get<std::string>(),
                                 &conv)) {
                node_capability_from_json(
                    message["node"],
                    &peer_capability
                );
                if (peer.endpoint.host.empty() ||
                    peer.endpoint.host == "0.0.0.0") {
                    peer.endpoint.host = node.endpoint.host;
                }
                if (peer.endpoint.port == 0) {
                    peer.endpoint.port = node.endpoint.port;
                }
                if (kcp_link_callback_ && kcp_link_callback_(peer, conv)) {
                    add_route(
                        peer,
                        &node,
                        true,
                        0,
                        peer_capability.present ? &peer_capability : nullptr
                    );
                    routes_.mark_success(peer.id, true);
                    const std::optional<RouteEntry> connected =
                        routes_.find(peer.id);
                    if (connected.has_value()) {
                        announce_route(*connected);
                    }
                    std::cerr << "discovery: connected kcp peer "
                              << endpoint_to_string(peer.endpoint)
                              << " id=" << convid_to_hex(peer.id) << "\n";
                } else {
                    routes_.mark_failure(peer.id);
                }
            }
        }
    }

    ::close(fd);
}

bool DiscoveryService::add_route(const NodeAddress& node,
                                 const NodeAddress* source,
                                 bool kcp_connected,
                                 std::uint8_t advertised_hop_count,
                                 const NodeCapability* capability) {
    if (node.endpoint.host.empty() || node.endpoint.port == 0 ||
        node.id.empty() || node.id == config_.identity.id) {
        return false;
    }
    return routes_.add_route(
        node,
        source,
        kcp_connected,
        advertised_hop_count,
        capability
    );
}

void DiscoveryService::announce_route(const RouteEntry& route) {
    if (!control_broadcast_) {
        return;
    }

    nlohmann::json message;
    message["type"] = "node";
    message["node"] = route_to_json(route);
    const std::string payload = message.dump();
    control_broadcast_(ByteView(payload.data(), payload.size()));
}

void DiscoveryService::send_routes(int fd) {
    nlohmann::json message;
    message["type"] = "routes";
    message["entries"] = nlohmann::json::array();

    const std::vector<RouteEntry> snapshot =
        routes_.select_routes_for_response();
    for (const RouteEntry& entry : snapshot) {
        if (entry.node.id != config_.identity.id) {
            message["entries"].push_back(route_to_json(entry));
        }
    }

    write_json_line(fd, message);
}

bool DiscoveryService::negotiate_kcp_link(const NodeAddress& peer, int fd) {
    if (!routes_.can_add_kcp_link()) {
        return false;
    }
    if (kcp_admission_callback_ && !kcp_admission_callback_(peer)) {
        nlohmann::json message;
        message["type"] = "admission_rejected";
        message["reason"] = "kcp capacity exhausted";
        write_json_line(fd, message);
        return false;
    }

    const myring::kcp::KcpConv conv =
        derive_kcp_convid(peer.endpoint, config_.local_endpoint);
    if (kcp_link_callback_ && !kcp_link_callback_(peer, conv)) {
        routes_.mark_failure(peer.id);
        return false;
    }
    add_route(peer, nullptr, true);
    routes_.mark_success(peer.id, true);
    const std::optional<RouteEntry> connected = routes_.find(peer.id);
    if (connected.has_value()) {
        announce_route(*connected);
    }
    std::cerr << "discovery: negotiated kcp peer "
              << endpoint_to_string(peer.endpoint)
              << " id=" << convid_to_hex(peer.id) << "\n";

    nlohmann::json message;
    message["type"] = "kcp_conv";
    const NodeCapability capability = local_capability(capability_provider_);
    message["node"] = node_to_json(self_node(config_), &capability);
    message["convid"] = convid_to_hex(conv);
    return write_json_line(fd, message);
}

void DiscoveryService::handle_control_json(const std::string& payload) {
    nlohmann::json message;
    try {
        message = nlohmann::json::parse(payload);
    } catch (...) {
        return;
    }

    const std::string type = message.value("type", std::string());
    if (type == "node") {
        NodeAddress node;
        NodeCapability capability;
        if (message.contains("node") &&
            node_from_json(message["node"], &node)) {
            node_capability_from_json(message["node"], &capability);
            add_route(
                node,
                nullptr,
                false,
                route_hop_count_from_json(message["node"]),
                capability.present ? &capability : nullptr
            );
        }
    }
}

bool DiscoveryService::load_route_table() {
    if (config_.route_table_file.empty()) {
        return true;
    }

    std::string error;
    if (!routes_.load_json_file(
            config_.route_table_file,
            config_.identity.id,
            &error)) {
        std::cerr << "discovery: failed to load route table "
                  << config_.route_table_file << ": " << error << "\n";
        return false;
    }
    return true;
}

bool DiscoveryService::save_route_table() const {
    if (config_.route_table_file.empty()) {
        return true;
    }

    std::string error;
    if (!routes_.save_json_file(
            config_.route_table_file,
            config_.identity.id,
            &error)) {
        std::cerr << "discovery: failed to save route table "
                  << config_.route_table_file << ": " << error << "\n";
        return false;
    }
    return true;
}

myring::kcp::KcpConv derive_kcp_convid(const Ipv4Endpoint& peer,
                                       const Ipv4Endpoint& local) noexcept {
    myring::kcp::KcpConv conv;
    encode_ipv4_endpoint_half(peer, conv.bytes.data());
    encode_ipv4_endpoint_half(local, conv.bytes.data() + 16);
    return conv;
}

}  // namespace iota_proxy
