#include "iota_proxy/business/KafkaSink.hpp"

#include <iostream>

namespace iota_proxy {
namespace {

void delivery_report(rd_kafka_t*,
                     const rd_kafka_message_t* message,
                     void*) {
    if (message != nullptr && message->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        std::cerr << "kafka sink: delivery failed: "
                  << rd_kafka_err2str(message->err) << "\n";
    }
}

}  // namespace

KafkaSink::KafkaSink(const KafkaSinkConfig& config)
    : config_(config),
      mutex_(),
      producer_(nullptr) {}

KafkaSink::~KafkaSink() {
    stop();
}

bool KafkaSink::produce(const std::string& key, const std::string& value) {
    if (!config_.enabled()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensure_started_locked()) {
        return false;
    }

    rd_kafka_resp_err_t err = rd_kafka_producev(
        producer_,
        RD_KAFKA_V_TOPIC(config_.topic.c_str()),
        RD_KAFKA_V_KEY(key.data(), key.size()),
        RD_KAFKA_V_VALUE(
            const_cast<char*>(value.data()),
            value.size()
        ),
        RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
        RD_KAFKA_V_END
    );

    if (err == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
        rd_kafka_poll(producer_, 100);
        err = rd_kafka_producev(
            producer_,
            RD_KAFKA_V_TOPIC(config_.topic.c_str()),
            RD_KAFKA_V_KEY(key.data(), key.size()),
            RD_KAFKA_V_VALUE(
                const_cast<char*>(value.data()),
                value.size()
            ),
            RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
            RD_KAFKA_V_END
        );
    }

    rd_kafka_poll(producer_, 0);
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        std::cerr << "kafka sink: failed to produce: "
                  << rd_kafka_err2str(err) << "\n";
        return false;
    }

    return true;
}

void KafkaSink::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

bool KafkaSink::ensure_started_locked() {
    if (producer_ != nullptr) {
        return true;
    }

    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    rd_kafka_conf_set_dr_msg_cb(conf, delivery_report);
    char errstr[512] = {};
    const auto set_conf = [&](const char* name, const std::string& value) {
        if (value.empty()) {
            return true;
        }
        if (rd_kafka_conf_set(
                conf,
                name,
                value.c_str(),
                errstr,
                sizeof(errstr)
            ) != RD_KAFKA_CONF_OK) {
            std::cerr << "kafka sink: invalid config " << name << ": "
                      << errstr << "\n";
            return false;
        }
        return true;
    };

    if (!set_conf("bootstrap.servers", config_.brokers) ||
        !set_conf("client.id", config_.client_id) ||
        !set_conf("request.required.acks", config_.acks) ||
        !set_conf(
            "message.timeout.ms",
            std::to_string(config_.message_timeout_ms)
        )) {
        rd_kafka_conf_destroy(conf);
        return false;
    }

    producer_ = rd_kafka_new(
        RD_KAFKA_PRODUCER,
        conf,
        errstr,
        sizeof(errstr)
    );
    if (producer_ == nullptr) {
        rd_kafka_conf_destroy(conf);
        std::cerr << "kafka sink: failed to create producer: "
                  << errstr << "\n";
        return false;
    }

    return true;
}

void KafkaSink::close_locked() noexcept {
    if (producer_ == nullptr) {
        return;
    }

    rd_kafka_flush(producer_, config_.flush_timeout_ms);
    rd_kafka_destroy(producer_);
    producer_ = nullptr;
}

}  // namespace iota_proxy
