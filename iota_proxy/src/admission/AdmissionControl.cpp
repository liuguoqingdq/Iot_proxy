#include "iota_proxy/admission/AdmissionControl.hpp"

#include <algorithm>
#include <chrono>

namespace iota_proxy {
namespace {

std::int64_t steady_now_ms() noexcept {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::int64_t wall_now_ms() noexcept {
    using Clock = std::chrono::system_clock;
    const auto now = Clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

std::uint64_t steady_second(std::int64_t ms) noexcept {
    if (ms <= 0) {
        return 0;
    }
    return static_cast<std::uint64_t>(ms / 1000);
}

}  // namespace

AdmissionControl::AdmissionControl(const AdmissionControlConfig& config)
    : config_(config),
      mutex_(),
      ingress_streams_(),
      ingress_byte_tokens_(
          static_cast<double>(config.max_ingress_bytes_per_second)
      ),
      token_refill_ms_(steady_now_ms()),
      current_second_(steady_second(token_refill_ms_)),
      current_second_bytes_(0),
      last_complete_second_bytes_(0) {}

bool AdmissionControl::try_accept_ingress_stream(std::uint64_t stream_id) {
    if (stream_id == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (ingress_streams_.find(stream_id) != ingress_streams_.end()) {
        return true;
    }
    if (config_.max_ingress_streams != 0 &&
        ingress_streams_.size() >= config_.max_ingress_streams) {
        return false;
    }

    ingress_streams_.insert(stream_id);
    return true;
}

void AdmissionControl::release_ingress_stream(std::uint64_t stream_id) {
    if (stream_id == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ingress_streams_.erase(stream_id);
}

bool AdmissionControl::try_accept_ingress_bytes(std::size_t bytes) {
    if (bytes == 0 || config_.max_ingress_bytes_per_second == 0) {
        return true;
    }

    const std::int64_t now = steady_now_ms();
    std::lock_guard<std::mutex> lock(mutex_);
    refill_tokens_locked(now);

    const double needed = static_cast<double>(bytes);
    if (ingress_byte_tokens_ < needed) {
        return false;
    }

    ingress_byte_tokens_ -= needed;
    current_second_bytes_ += static_cast<std::uint64_t>(bytes);
    return true;
}

bool AdmissionControl::can_accept_kcp_link(
    std::size_t current_kcp_links) const {
    return config_.max_kcp_links == 0 ||
           current_kcp_links < config_.max_kcp_links;
}

NodeCapability AdmissionControl::snapshot(
    std::size_t current_kcp_links) const {
    const std::int64_t now = steady_now_ms();
    std::lock_guard<std::mutex> lock(mutex_);
    refill_tokens_locked(now);

    NodeCapability capability;
    capability.present = true;
    capability.accepting_ingress =
        (config_.max_ingress_streams == 0 ||
         ingress_streams_.size() < config_.max_ingress_streams) &&
        can_accept_kcp_link(current_kcp_links);
    capability.max_kcp_links = config_.max_kcp_links;
    capability.current_kcp_links = current_kcp_links;
    capability.max_ingress_streams = config_.max_ingress_streams;
    capability.current_ingress_streams = ingress_streams_.size();
    capability.max_ingress_bytes_per_second =
        config_.max_ingress_bytes_per_second;
    capability.current_ingress_bytes_per_second = std::max(
        current_second_bytes_,
        last_complete_second_bytes_
    );
    capability.updated_at_ms = wall_now_ms();
    return capability;
}

void AdmissionControl::refill_tokens_locked(std::int64_t now_ms) const {
    const std::uint64_t second = steady_second(now_ms);
    if (second != current_second_) {
        last_complete_second_bytes_ = current_second_bytes_;
        current_second_bytes_ = 0;
        current_second_ = second;
    }

    if (config_.max_ingress_bytes_per_second == 0) {
        return;
    }

    const std::int64_t elapsed_ms =
        std::max<std::int64_t>(0, now_ms - token_refill_ms_);
    if (elapsed_ms == 0) {
        return;
    }

    const double capacity =
        static_cast<double>(config_.max_ingress_bytes_per_second);
    const double refill =
        capacity * static_cast<double>(elapsed_ms) / 1000.0;
    ingress_byte_tokens_ = std::min(capacity, ingress_byte_tokens_ + refill);
    token_refill_ms_ = now_ms;
}

}  // namespace iota_proxy
