#ifndef IOTA_PROXY_BUSINESS_KAFKA_SINK_HPP
#define IOTA_PROXY_BUSINESS_KAFKA_SINK_HPP

#include <cstdint>
#include <mutex>
#include <string>

#include <librdkafka/rdkafka.h>

namespace iota_proxy {

struct KafkaSinkConfig {
    std::string brokers;
    std::string topic;
    std::string client_id = "iota-proxy";
    std::string acks = "all";
    std::uint32_t message_timeout_ms = 5000;
    std::uint32_t flush_timeout_ms = 5000;

    bool enabled() const noexcept {
        return !brokers.empty() && !topic.empty();
    }
};

class KafkaSink {
public:
    explicit KafkaSink(const KafkaSinkConfig& config);
    ~KafkaSink();

    bool produce(const std::string& key, const std::string& value);
    void stop();

private:
    bool ensure_started_locked();
    void close_locked() noexcept;

private:
    KafkaSinkConfig config_;
    std::mutex mutex_;
    rd_kafka_t* producer_;
};

}  // namespace iota_proxy

#endif
