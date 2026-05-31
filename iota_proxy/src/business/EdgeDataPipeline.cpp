#include "iota_proxy/business/EdgeDataPipeline.hpp"

#include "iota_proxy/business/EdgeDataMessage.hpp"

#include <iostream>

namespace iota_proxy {

EdgeDataPipeline::EdgeDataPipeline(const EdgeDataPipelineConfig& config)
    : config_(config),
      redis_(config.redis),
      kafka_(config.kafka) {}

EdgeDataPipeline::~EdgeDataPipeline() {
    stop();
}

bool EdgeDataPipeline::write_edge_data(std::uint64_t stream_id,
                                       ByteView payload,
                                       const BroadcastMetadata& metadata) {
    if (payload.empty()) {
        return true;
    }

    EdgeDataMessage message;
    if (!make_edge_data_message(stream_id, payload, metadata, &message)) {
        std::cerr << "edge pipeline: invalid edge frame, skip write\n";
        return false;
    }

    const std::string serialized =
        serialize_edge_data_message_json(message);

    bool duplicate = false;
    if (!redis_.record_edge_message(message, serialized, &duplicate)) {
        std::cerr << "edge pipeline: failed to update redis state cache\n";
    }
    if (duplicate) {
        return true;
    }

    return kafka_.produce(message.mac_hex, serialized);
}

void EdgeDataPipeline::stop() {
    kafka_.stop();
    redis_.stop();
}

}  // namespace iota_proxy
