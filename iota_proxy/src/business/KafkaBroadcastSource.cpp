#include "iota_proxy/business/KafkaBroadcastSource.hpp"

#include <chrono>
#include <iostream>
#include <utility>

namespace iota_proxy {
namespace {

bool set_conf(rd_kafka_conf_t* conf,
              const char* name,
              const std::string& value,
              std::string* error) {
    char errstr[512] = {};
    if (rd_kafka_conf_set(
            conf,
            name,
            value.c_str(),
            errstr,
            sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        if (error != nullptr) {
            *error = errstr;
        }
        return false;
    }
    return true;
}

}  // namespace

KafkaBroadcastSource::KafkaBroadcastSource(
    const KafkaBroadcastSourceConfig& config)
    : config_(config),
      sender_(),
      consumer_(nullptr),
      subscription_(nullptr),
      worker_(),
      running_(false) {}

KafkaBroadcastSource::~KafkaBroadcastSource() {
    stop();
}

void KafkaBroadcastSource::set_sender(KafkaBroadcastSender sender) {
    sender_ = std::move(sender);
}

bool KafkaBroadcastSource::start() {
    if (!config_.enabled) {
        return true;
    }
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }
    if (config_.brokers.empty() || config_.topic.empty() || !sender_) {
        std::cerr << "kafka broadcast: brokers, topic, and sender are required\n";
        return false;
    }
    if (!create_consumer()) {
        return false;
    }

    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this]() { run(); });
    return true;
}

void KafkaBroadcastSource::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    close_consumer();
}

bool KafkaBroadcastSource::create_consumer() {
    rd_kafka_conf_t* conf = rd_kafka_conf_new();
    std::string error;
    const bool ok =
        set_conf(conf, "bootstrap.servers", config_.brokers, &error) &&
        set_conf(conf, "group.id", config_.group_id, &error) &&
        set_conf(conf, "client.id", config_.client_id, &error) &&
        set_conf(conf, "enable.auto.commit", "false", &error) &&
        set_conf(conf, "auto.offset.reset", config_.auto_offset_reset, &error);
    if (!ok) {
        std::cerr << "kafka broadcast: invalid config: " << error << "\n";
        rd_kafka_conf_destroy(conf);
        return false;
    }

    char errstr[512] = {};
    consumer_ = rd_kafka_new(
        RD_KAFKA_CONSUMER,
        conf,
        errstr,
        sizeof(errstr));
    if (consumer_ == nullptr) {
        std::cerr << "kafka broadcast: failed to create consumer: "
                  << errstr << "\n";
        rd_kafka_conf_destroy(conf);
        return false;
    }

    rd_kafka_poll_set_consumer(consumer_);
    subscription_ = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(
        subscription_,
        config_.topic.c_str(),
        RD_KAFKA_PARTITION_UA);

    const rd_kafka_resp_err_t err =
        rd_kafka_subscribe(consumer_, subscription_);
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) {
        std::cerr << "kafka broadcast: subscribe failed: "
                  << rd_kafka_err2str(err) << "\n";
        close_consumer();
        return false;
    }
    return true;
}

void KafkaBroadcastSource::run() {
    std::cout << "kafka broadcast: consuming topic " << config_.topic
              << " as group " << config_.group_id << "\n";

    while (running_.load(std::memory_order_acquire)) {
        rd_kafka_message_t* message =
            rd_kafka_consumer_poll(consumer_, config_.poll_timeout_ms);
        if (message == nullptr) {
            continue;
        }

        if (message->err != RD_KAFKA_RESP_ERR_NO_ERROR) {
            if (message->err != RD_KAFKA_RESP_ERR__PARTITION_EOF &&
                message->err != RD_KAFKA_RESP_ERR__TIMED_OUT &&
                message->err != RD_KAFKA_RESP_ERR__INTR) {
                std::cerr << "kafka broadcast: consume failed: "
                          << rd_kafka_message_errstr(message) << "\n";
            }
            rd_kafka_message_destroy(message);
            continue;
        }

        bool processed = true;
        if (message->payload != nullptr && message->len > 0) {
            const char* payload =
                static_cast<const char*>(message->payload);
            processed = sender_(ByteView(payload, message->len)) != 0;
        }
        if (!processed) {
            rd_kafka_message_destroy(message);
            continue;
        }

        const rd_kafka_resp_err_t commit_err =
            rd_kafka_commit_message(consumer_, message, 0);
        if (commit_err != RD_KAFKA_RESP_ERR_NO_ERROR) {
            std::cerr << "kafka broadcast: commit failed: "
                      << rd_kafka_err2str(commit_err) << "\n";
        }
        rd_kafka_message_destroy(message);
    }
}

void KafkaBroadcastSource::close_consumer() noexcept {
    if (consumer_ != nullptr) {
        rd_kafka_consumer_close(consumer_);
    }
    if (subscription_ != nullptr) {
        rd_kafka_topic_partition_list_destroy(subscription_);
        subscription_ = nullptr;
    }
    if (consumer_ != nullptr) {
        rd_kafka_destroy(consumer_);
        consumer_ = nullptr;
    }
}

}  // namespace iota_proxy
