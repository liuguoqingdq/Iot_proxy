#include "iota_proxy/discovery/RouteManager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <unistd.h>

namespace iota_proxy {
namespace {

// The timing and chance rules are adapted from Bitcoin Core's addrman
// behavior. The storage model is narrowed to Iota's ip:port:256-bit-id routes.
constexpr std::int64_t kSecondsToMs = 1000;
constexpr std::int64_t kFutureSkewMs = 10 * 60 * kSecondsToMs;
constexpr std::int64_t kRecentTryPenaltyMs = 10 * 60 * kSecondsToMs;
constexpr std::int64_t kMinFailMs = 7 * 24 * 60 * 60 * kSecondsToMs;

bool valid_route_node(const NodeAddress& node) noexcept {
    return !node.endpoint.host.empty() &&
           node.endpoint.port != 0 &&
           !node.id.empty();
}

std::uint8_t local_hop_count(const NodeAddress* source,
                             bool kcp_connected,
                             std::uint8_t advertised_hop_count) noexcept {
    if (kcp_connected) {
        return 1;
    }
    if (source != nullptr && valid_route_node(*source)) {
        if (advertised_hop_count ==
            std::numeric_limits<std::uint8_t>::max()) {
            return advertised_hop_count;
        }
        return static_cast<std::uint8_t>(advertised_hop_count + 1);
    }
    return advertised_hop_count;
}

const char* route_state_to_string(RouteState state) noexcept {
    switch (state) {
        case RouteState::New:
            return "new";
        case RouteState::Tried:
            return "tried";
        case RouteState::Bad:
            return "bad";
    }
    return "new";
}

RouteState route_state_from_string(const std::string& text) noexcept {
    if (text == "tried") {
        return RouteState::Tried;
    }
    if (text == "bad") {
        return RouteState::Bad;
    }
    return RouteState::New;
}

nlohmann::json node_to_json(const NodeAddress& node) {
    nlohmann::json item;
    item["host"] = node.endpoint.host;
    item["port"] = node.endpoint.port;
    item["public_key"] = convid_to_hex(node.id);
    return item;
}

std::uint64_t json_size_value(const nlohmann::json& item,
                              const char* key,
                              std::uint64_t fallback = 0) {
    if (!item.contains(key) || !item[key].is_number_unsigned()) {
        return fallback;
    }
    return item[key].get<std::uint64_t>();
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
        static_cast<std::size_t>(json_size_value(item, "max_kcp_links"));
    capability.current_kcp_links =
        static_cast<std::size_t>(json_size_value(item, "current_kcp_links"));
    capability.max_ingress_streams =
        static_cast<std::size_t>(json_size_value(item, "max_ingress_streams"));
    capability.current_ingress_streams =
        static_cast<std::size_t>(
            json_size_value(item, "current_ingress_streams")
        );
    capability.max_ingress_bytes_per_second =
        json_size_value(item, "max_ingress_bytes_per_second");
    capability.current_ingress_bytes_per_second =
        json_size_value(item, "current_ingress_bytes_per_second");
    capability.updated_at_ms = item.value("updated_at_ms", 0LL);
    *out = capability;
    return true;
}

bool node_from_json(const nlohmann::json& item, NodeAddress* out) {
    if (out == nullptr || !item.is_object() ||
        !item.contains("host") ||
        !item.contains("port") ||
        !item["host"].is_string() ||
        !item["port"].is_number_unsigned()) {
        return false;
    }

    std::string id_text;
    if (item.contains("public_key") && item["public_key"].is_string()) {
        id_text = item["public_key"].get<std::string>();
    } else if (item.contains("id") && item["id"].is_string()) {
        id_text = item["id"].get<std::string>();
    } else if (item.contains("convid") && item["convid"].is_string()) {
        id_text = item["convid"].get<std::string>();
    } else {
        return false;
    }

    NodeAddress node;
    node.endpoint.host = item["host"].get<std::string>();
    const unsigned port = item["port"].get<unsigned>();
    if (port == 0 || port > 65535U ||
        !parse_convid_hex(id_text, &node.id)) {
        return false;
    }
    node.endpoint.port = static_cast<std::uint16_t>(port);
    *out = node;
    return true;
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

}  // namespace

RouteManager::RouteManager(const RouteManagerConfig& config)
    : config_(config),
      mutex_(),
      routes_(),
      kcp_link_count_(0),
      rng_(std::random_device{}()) {}

bool RouteManager::add_route(const NodeAddress& node,
                             const NodeAddress* source,
                             bool kcp_connected,
                             std::uint8_t advertised_hop_count,
                             const NodeCapability* capability) {
    if (!valid_route_node(node)) {
        return false;
    }
    const std::uint8_t hops =
        local_hop_count(source, kcp_connected, advertised_hop_count);
    if (hops > config_.max_route_hops) {
        return false;
    }

    const std::int64_t now_ms = route_time_now_ms();
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = routes_.find(node.id);
    if (it != routes_.end()) {
        RouteEntry& entry = it->second;
        entry.node = node;
        entry.last_seen_ms = now_ms;
        if (capability != nullptr && capability->present) {
            entry.capability = *capability;
        }
        if (source != nullptr && valid_route_node(*source)) {
            const bool shorter_path =
                entry.hop_count == 0 || hops < entry.hop_count;
            if (!entry.kcp_connected && shorter_path) {
                entry.source = *source;
                entry.has_source = true;
                entry.hop_count = hops;
            }
        }
        if (entry.state == RouteState::Bad) {
            entry.state = RouteState::New;
        }
        if (kcp_connected && !entry.kcp_connected) {
            entry.kcp_connected = true;
            ++kcp_link_count_;
        }
        if (kcp_connected) {
            entry.state = RouteState::Tried;
            entry.last_success_ms = now_ms;
            entry.failures = 0;
            entry.hop_count = 1;
            entry.has_source = false;
        }
        return false;
    }

    trim_if_needed(now_ms);
    if (routes_.size() >= config_.max_routes) {
        trim_if_needed(now_ms);
    }
    if (routes_.size() >= config_.max_routes) {
        return false;
    }

    RouteEntry entry;
    entry.node = node;
    if (capability != nullptr && capability->present) {
        entry.capability = *capability;
    }
    entry.last_seen_ms = now_ms;
    entry.kcp_connected = kcp_connected;
    entry.hop_count = hops;
    if (kcp_connected) {
        entry.state = RouteState::Tried;
        entry.last_success_ms = now_ms;
        ++kcp_link_count_;
    }
    if (source != nullptr && valid_route_node(*source)) {
        entry.source = *source;
        entry.has_source = true;
    }
    routes_.emplace(node.id, entry);
    return true;
}

bool RouteManager::mark_attempt(const myring::kcp::KcpConv& node_id) {
    const std::int64_t now_ms = route_time_now_ms();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = routes_.find(node_id);
    if (it == routes_.end()) {
        return false;
    }

    RouteEntry& entry = it->second;
    entry.last_try_ms = now_ms;
    if (entry.attempts < std::numeric_limits<std::uint32_t>::max()) {
        ++entry.attempts;
    }
    return true;
}

bool RouteManager::mark_success(const myring::kcp::KcpConv& node_id,
                                bool kcp_connected) {
    const std::int64_t now_ms = route_time_now_ms();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = routes_.find(node_id);
    if (it == routes_.end()) {
        return false;
    }

    RouteEntry& entry = it->second;
    entry.state = RouteState::Tried;
    entry.failures = 0;
    entry.attempts = 0;
    entry.last_seen_ms = now_ms;
    entry.last_try_ms = now_ms;
    entry.last_success_ms = now_ms;
    if (kcp_connected && !entry.kcp_connected) {
        entry.kcp_connected = true;
        ++kcp_link_count_;
    }
    if (kcp_connected) {
        entry.hop_count = 1;
        entry.has_source = false;
    }
    return true;
}

bool RouteManager::mark_failure(const myring::kcp::KcpConv& node_id) {
    const std::int64_t now_ms = route_time_now_ms();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = routes_.find(node_id);
    if (it == routes_.end()) {
        return false;
    }

    RouteEntry& entry = it->second;
    entry.last_try_ms = now_ms;
    if (entry.failures < std::numeric_limits<std::uint32_t>::max()) {
        ++entry.failures;
    }
    if (entry.kcp_connected) {
        entry.kcp_connected = false;
        if (kcp_link_count_ > 0) {
            --kcp_link_count_;
        }
    }
    if (is_terrible(entry, now_ms) ||
        entry.failures >= config_.bad_failure_threshold) {
        entry.state = RouteState::Bad;
    } else if (entry.last_success_ms == 0) {
        entry.state = RouteState::New;
    }
    return true;
}

bool RouteManager::mark_kcp_disconnected(
    const myring::kcp::KcpConv& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = routes_.find(node_id);
    if (it == routes_.end()) {
        return false;
    }

    if (it->second.kcp_connected) {
        it->second.kcp_connected = false;
        if (kcp_link_count_ > 0) {
            --kcp_link_count_;
        }
    }
    return true;
}

bool RouteManager::can_add_kcp_link() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kcp_link_count_ < config_.max_kcp_links;
}

std::size_t RouteManager::route_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return routes_.size();
}

std::size_t RouteManager::kcp_link_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kcp_link_count_;
}

std::vector<RouteEntry> RouteManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RouteEntry> out;
    out.reserve(routes_.size());
    for (const auto& item : routes_) {
        out.push_back(item.second);
    }
    return out;
}

std::vector<RouteEntry> RouteManager::select_routes_for_response() const {
    const std::int64_t now_ms = route_time_now_ms();
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<RouteEntry> candidates;
    candidates.reserve(routes_.size());
    for (const auto& item : routes_) {
        const RouteEntry& entry = item.second;
        if (!is_terrible(entry, now_ms) && entry.state != RouteState::Bad) {
            candidates.push_back(entry);
        }
    }
    if (candidates.empty()) {
        return {};
    }

    std::shuffle(candidates.begin(), candidates.end(), rng_);
    const std::size_t pct_limit =
        (candidates.size() * config_.max_response_percent + 99) / 100;
    const std::size_t limit = std::max<std::size_t>(
        1,
        std::min(config_.max_response_routes, pct_limit)
    );
    if (candidates.size() > limit) {
        candidates.resize(limit);
    }
    return candidates;
}

std::optional<RouteEntry> RouteManager::select_for_feeler() const {
    const std::int64_t now_ms = route_time_now_ms();
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<ScoredRoute> candidates;
    candidates.reserve(routes_.size());
    for (const auto& item : routes_) {
        const RouteEntry& entry = item.second;
        if (entry.kcp_connected ||
            entry.state == RouteState::Bad ||
            is_terrible(entry, now_ms) ||
            in_retry_backoff(entry, now_ms)) {
            continue;
        }

        ScoredRoute scored;
        scored.entry = entry;
        scored.score = route_score(entry, now_ms);
        if (scored.score <= 0.0) {
            continue;
        }
        scored.tie_breaker = rng_();
        candidates.push_back(scored);
    }
    if (candidates.empty()) {
        return std::nullopt;
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const ScoredRoute& lhs, const ScoredRoute& rhs) {
            if (lhs.score != rhs.score) {
                return lhs.score > rhs.score;
            }
            return lhs.tie_breaker < rhs.tie_breaker;
        }
    );
    return candidates.front().entry;
}

std::optional<RouteNextHop> RouteManager::select_next_hop(
    const myring::kcp::KcpConv& target_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto target_it = routes_.find(target_id);
    if (target_it == routes_.end() ||
        target_it->second.state == RouteState::Bad) {
        return std::nullopt;
    }

    const RouteEntry& target = target_it->second;
    if (target.kcp_connected) {
        RouteNextHop next;
        next.target = target.node;
        next.next_hop = target.node;
        next.direct = true;
        next.hop_count = 1;
        return next;
    }

    if (!target.has_source) {
        return std::nullopt;
    }

    auto source_it = routes_.find(target.source.id);
    if (source_it == routes_.end() ||
        !source_it->second.kcp_connected ||
        source_it->second.state == RouteState::Bad) {
        return std::nullopt;
    }

    RouteNextHop next;
    next.target = target.node;
    next.next_hop = source_it->second.node;
    next.direct = false;
    next.hop_count = target.hop_count;
    return next;
}

std::optional<RouteNextHop> RouteManager::select_default_next_hop() const {
    const std::int64_t now_ms = route_time_now_ms();
    std::lock_guard<std::mutex> lock(mutex_);

    std::optional<RouteNextHop> best;
    double best_score = -1.0;
    for (const auto& item : routes_) {
        const RouteEntry& entry = item.second;
        if (!entry.kcp_connected || entry.state == RouteState::Bad) {
            continue;
        }

        const double score = route_score(entry, now_ms);
        if (score <= 0.0) {
            continue;
        }
        if (!best.has_value() || score > best_score) {
            RouteNextHop next;
            next.target = entry.node;
            next.next_hop = entry.node;
            next.direct = true;
            next.hop_count = 1;
            best = next;
            best_score = score;
        }
    }
    return best;
}

std::optional<RouteEntry> RouteManager::find(
    const myring::kcp::KcpConv& node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = routes_.find(node_id);
    if (it == routes_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool RouteManager::load_json_file(const std::string& path,
                                  const myring::kcp::KcpConv& local_id,
                                  std::string* error) {
    if (path.empty()) {
        return true;
    }

    std::ifstream input(path);
    if (!input) {
        return true;
    }

    nlohmann::json doc;
    try {
        input >> doc;
    } catch (const std::exception& ex) {
        set_error(error, std::string("route table parse failed: ") + ex.what());
        return false;
    }

    if (!doc.is_object() ||
        doc.value("version", 0) != 1 ||
        !doc.contains("entries") ||
        !doc["entries"].is_array()) {
        set_error(error, "route table schema is invalid");
        return false;
    }

    const std::int64_t now_ms = route_time_now_ms();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const nlohmann::json& item : doc["entries"]) {
        if (!item.is_object() || !item.contains("node")) {
            continue;
        }

        NodeAddress node;
        if (!node_from_json(item["node"], &node) ||
            node.id == local_id ||
            node.id.empty()) {
            continue;
        }

        RouteEntry entry;
        entry.node = node;
        entry.state = route_state_from_string(
            item.value("state", std::string("new"))
        );
        entry.kcp_connected = false;
        entry.attempts = item.value("attempts", 0U);
        entry.failures = item.value("failures", 0U);
        if (item.contains("capability")) {
            capability_from_json(item["capability"], &entry.capability);
        }
        entry.hop_count = static_cast<std::uint8_t>(
            std::min<unsigned>(
                item.value("hop_count", 0U),
                static_cast<unsigned>(config_.max_route_hops)
            )
        );
        entry.last_seen_ms = item.value("last_seen_ms", now_ms);
        entry.last_try_ms = item.value("last_try_ms", 0LL);
        entry.last_success_ms = item.value("last_success_ms", 0LL);
        if (item.contains("source") &&
            node_from_json(item["source"], &entry.source) &&
            entry.source.id != node.id &&
            entry.source.id != local_id) {
            entry.has_source = true;
        }

        if (entry.last_seen_ms == 0) {
            entry.last_seen_ms = now_ms;
        }
        if (entry.state == RouteState::Tried &&
            entry.last_success_ms == 0) {
            entry.state = RouteState::New;
        }
        if (entry.hop_count > config_.max_route_hops) {
            continue;
        }

        routes_[entry.node.id] = entry;
    }
    trim_if_needed(now_ms);
    return true;
}

bool RouteManager::save_json_file(const std::string& path,
                                  const myring::kcp::KcpConv& local_id,
                                  std::string* error) const {
    if (path.empty()) {
        return true;
    }

    nlohmann::json doc;
    doc["version"] = 1;
    doc["entries"] = nlohmann::json::array();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : routes_) {
            const RouteEntry& entry = item.second;
            if (entry.node.id == local_id || !valid_route_node(entry.node)) {
                continue;
            }

            nlohmann::json route;
            route["node"] = node_to_json(entry.node);
            route["state"] = route_state_to_string(entry.state);
            route["attempts"] = entry.attempts;
            route["failures"] = entry.failures;
            if (entry.capability.present) {
                route["capability"] = capability_to_json(entry.capability);
            }
            route["hop_count"] = entry.hop_count;
            route["last_seen_ms"] = entry.last_seen_ms;
            route["last_try_ms"] = entry.last_try_ms;
            route["last_success_ms"] = entry.last_success_ms;
            if (entry.has_source && valid_route_node(entry.source)) {
                route["source"] = node_to_json(entry.source);
            }
            doc["entries"].push_back(std::move(route));
        }
    }

    const std::filesystem::path target(path);
    const std::filesystem::path parent = target.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            set_error(error, "failed to create route table directory: " +
                                 ec.message());
            return false;
        }
    }

    const std::filesystem::path temp =
        target.string() + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream output(temp, std::ios::trunc);
        if (!output) {
            set_error(error, "route table temp file is not writable");
            return false;
        }
        output << doc.dump(2) << "\n";
        if (!output) {
            set_error(error, "failed to write route table");
            return false;
        }
    }

    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        set_error(error, "failed to install route table: " + ec.message());
        return false;
    }
    return true;
}

bool RouteManager::is_terrible(const RouteEntry& entry,
                               std::int64_t now_ms) const {
    if (entry.last_try_ms != 0 &&
        entry.last_try_ms >= now_ms -
            static_cast<std::int64_t>(config_.retry_backoff_seconds) *
                kSecondsToMs) {
        return false;
    }
    if (entry.last_seen_ms > now_ms + kFutureSkewMs) {
        return true;
    }
    if (entry.last_seen_ms == 0 ||
        entry.last_seen_ms <
            now_ms - static_cast<std::int64_t>(config_.stale_after_seconds) *
                kSecondsToMs) {
        return true;
    }
    if (entry.last_success_ms == 0 &&
        entry.attempts >= config_.give_up_attempts) {
        return true;
    }
    if (entry.last_success_ms != 0 &&
        entry.last_success_ms < now_ms - kMinFailMs &&
        entry.failures >= config_.bad_failure_threshold) {
        return true;
    }
    return false;
}

bool RouteManager::in_retry_backoff(const RouteEntry& entry,
                                    std::int64_t now_ms) const {
    if (entry.last_try_ms == 0) {
        return false;
    }
    const std::int64_t backoff_ms =
        static_cast<std::int64_t>(config_.retry_backoff_seconds) *
        kSecondsToMs;
    return entry.last_try_ms > now_ms - backoff_ms;
}

double RouteManager::route_chance(const RouteEntry& entry,
                                  std::int64_t now_ms) const {
    double chance = 1.0;
    if (entry.last_try_ms != 0 &&
        entry.last_try_ms > now_ms - kRecentTryPenaltyMs) {
        chance *= 0.01;
    }

    const std::uint32_t attempts = std::min<std::uint32_t>(entry.attempts, 8);
    chance *= std::pow(0.66, static_cast<double>(attempts));
    if (entry.state == RouteState::Tried) {
        chance *= 1.25;
    } else if (entry.state == RouteState::Bad) {
        chance *= 0.05;
    }
    return chance;
}

double RouteManager::capability_score(const RouteEntry& entry,
                                      std::int64_t now_ms) const {
    const NodeCapability& cap = entry.capability;
    if (!cap.present) {
        return 1.0;
    }
    if (!cap.accepting_ingress) {
        return 0.0;
    }
    if (cap.updated_at_ms != 0 &&
        cap.updated_at_ms <
            now_ms - static_cast<std::int64_t>(
                         config_.capability_stale_after_seconds
                     ) *
                         kSecondsToMs) {
        return 0.5;
    }

    double load = 0.0;
    if (cap.max_kcp_links != 0) {
        if (cap.current_kcp_links >= cap.max_kcp_links) {
            return 0.0;
        }
        load = std::max(
            load,
            static_cast<double>(cap.current_kcp_links) /
                static_cast<double>(cap.max_kcp_links)
        );
    }
    if (cap.max_ingress_streams != 0) {
        if (cap.current_ingress_streams >= cap.max_ingress_streams) {
            return 0.0;
        }
        load = std::max(
            load,
            static_cast<double>(cap.current_ingress_streams) /
                static_cast<double>(cap.max_ingress_streams)
        );
    }
    if (cap.max_ingress_bytes_per_second != 0) {
        if (cap.current_ingress_bytes_per_second >=
            cap.max_ingress_bytes_per_second) {
            return 0.0;
        }
        load = std::max(
            load,
            static_cast<double>(cap.current_ingress_bytes_per_second) /
                static_cast<double>(cap.max_ingress_bytes_per_second)
        );
    }

    load = std::max(0.0, std::min(1.0, load));
    return 0.1 + 0.9 * (1.0 - load);
}

double RouteManager::route_score(const RouteEntry& entry,
                                 std::int64_t now_ms) const {
    double score = route_chance(entry, now_ms) *
                   capability_score(entry, now_ms);
    if (entry.hop_count > 1) {
        score /= static_cast<double>(entry.hop_count);
    }
    return score;
}

void RouteManager::trim_if_needed(std::int64_t now_ms) {
    if (routes_.size() < config_.max_routes) {
        return;
    }

    for (auto it = routes_.begin(); it != routes_.end();) {
        if (!it->second.kcp_connected && is_terrible(it->second, now_ms)) {
            it = routes_.erase(it);
        } else {
            ++it;
        }
    }
    if (routes_.size() < config_.max_routes) {
        return;
    }

    auto victim = routes_.end();
    double victim_score = std::numeric_limits<double>::max();
    for (auto it = routes_.begin(); it != routes_.end(); ++it) {
        const RouteEntry& entry = it->second;
        if (entry.kcp_connected) {
            continue;
        }
        double score = route_score(entry, now_ms);
        if (entry.state == RouteState::Bad) {
            score *= 0.01;
        }
        if (score < victim_score) {
            victim_score = score;
            victim = it;
        }
    }
    if (victim != routes_.end()) {
        routes_.erase(victim);
    }
}

std::int64_t route_time_now_ms() noexcept {
    using Clock = std::chrono::system_clock;
    const auto now = Clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

}  // namespace iota_proxy
