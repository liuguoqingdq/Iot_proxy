#ifndef IOTA_PROXY_BUSINESS_KAFKA_BROADCAST_SOURCE_HPP
#define IOTA_PROXY_BUSINESS_KAFKA_BROADCAST_SOURCE_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include <librdkafka/rdkafka.h>

#include "iota_proxy/common/ByteView.hpp"

namespace iota_proxy {

struct KafkaBroadcastSourceConfig {
    bool enabled = false;
    std::string brokers;
    std::string topic;
    std::string group_id = "iota-proxy-kcp-broadcast";
    std::string client_id = "iota-proxy-kcp-broadcast";
    std::string auto_offset_reset = "latest";
    std::uint32_t poll_timeout_ms = 1000;
};

using KafkaBroadcastSender = std::function<std::size_t(ByteView)>;

class KafkaBroadcastSource {
public:
    explicit KafkaBroadcastSource(const KafkaBroadcastSourceConfig& config);
    ~KafkaBroadcastSource();

    void set_sender(KafkaBroadcastSender sender);
    bool start();
    void stop();

private:
    bool create_consumer();
    void run();
    void close_consumer() noexcept;

private:
    KafkaBroadcastSourceConfig config_;
    KafkaBroadcastSender sender_;
    rd_kafka_t* consumer_;
    rd_kafka_topic_partition_list_t* subscription_;
    std::thread worker_;
    std::atomic<bool> running_;
};

}  // namespace iota_proxy

#endif
