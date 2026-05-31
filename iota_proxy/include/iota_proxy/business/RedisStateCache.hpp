#ifndef IOTA_PROXY_BUSINESS_REDIS_STATE_CACHE_HPP
#define IOTA_PROXY_BUSINESS_REDIS_STATE_CACHE_HPP

#include <cstdint>
#include <mutex>
#include <string>

#include "iota_proxy/business/EdgeDataMessage.hpp"
#include "iota_proxy/common/Endpoint.hpp"

namespace iota_proxy {

struct RedisStateCacheConfig {
    Ipv4Endpoint endpoint;
    std::uint32_t connect_timeout_ms = 1000;
    std::uint32_t io_timeout_ms = 1000;
    std::uint32_t device_state_ttl_seconds = 60;
    std::uint32_t device_latest_ttl_seconds = 24 * 60 * 60;
    std::uint32_t seen_ttl_seconds = 10 * 60;

    bool enabled() const noexcept {
        return !endpoint.host.empty() && endpoint.port != 0;
    }
};

class RedisStateCache {
public:
    explicit RedisStateCache(const RedisStateCacheConfig& config);
    ~RedisStateCache();

    bool record_edge_message(const EdgeDataMessage& message,
                             const std::string& serialized_message,
                             bool* duplicate);
    void stop();

private:
    struct Reply {
        char type = 0;
        bool nil = false;
        std::string text;
    };

    bool ensure_connected();
    bool connect_locked();
    bool execute_locked(const std::string& command, Reply* reply);
    bool send_command_locked(const std::string& command);
    bool read_reply_locked(Reply* reply);
    void close_locked() noexcept;

private:
    RedisStateCacheConfig config_;
    std::mutex mutex_;
    int fd_;
};

}  // namespace iota_proxy

#endif
