#ifndef IOTA_PROXY_ADMISSION_ADMISSION_CONTROL_HPP
#define IOTA_PROXY_ADMISSION_ADMISSION_CONTROL_HPP

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "iota_proxy/discovery/RouteManager.hpp"

namespace iota_proxy {

struct AdmissionControlConfig {
    std::size_t max_ingress_streams = 4096;
    std::uint64_t max_ingress_bytes_per_second = 0;
    std::size_t max_kcp_links = 512;
};

class AdmissionControl {
public:
    explicit AdmissionControl(const AdmissionControlConfig& config = {});

    bool try_accept_ingress_stream(std::uint64_t stream_id);
    void release_ingress_stream(std::uint64_t stream_id);
    bool try_accept_ingress_bytes(std::size_t bytes);
    bool can_accept_kcp_link(std::size_t current_kcp_links) const;

    NodeCapability snapshot(std::size_t current_kcp_links) const;

private:
    void refill_tokens_locked(std::int64_t now_ms) const;

private:
    AdmissionControlConfig config_;
    mutable std::mutex mutex_;
    std::unordered_set<std::uint64_t> ingress_streams_;
    mutable double ingress_byte_tokens_;
    mutable std::int64_t token_refill_ms_;
    mutable std::uint64_t current_second_;
    mutable std::uint64_t current_second_bytes_;
    mutable std::uint64_t last_complete_second_bytes_;
};

}  // namespace iota_proxy

#endif
