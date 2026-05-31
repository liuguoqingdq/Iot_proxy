#ifndef IOTA_PROXY_BUSINESS_EDGE_DATA_PIPELINE_HPP
#define IOTA_PROXY_BUSINESS_EDGE_DATA_PIPELINE_HPP

#include "iota_proxy/business/KafkaSink.hpp"
#include "iota_proxy/business/RedisStateCache.hpp"
#include "iota_proxy/common/ByteView.hpp"
#include "iota_proxy/frame/TunnelFrame.hpp"

namespace iota_proxy {

struct EdgeDataPipelineConfig {
    RedisStateCacheConfig redis;
    KafkaSinkConfig kafka;
};

class EdgeDataPipeline {
public:
    explicit EdgeDataPipeline(const EdgeDataPipelineConfig& config);
    ~EdgeDataPipeline();

    bool write_edge_data(std::uint64_t stream_id,
                         ByteView payload,
                         const BroadcastMetadata& metadata = {});
    void stop();

private:
    EdgeDataPipelineConfig config_;
    RedisStateCache redis_;
    KafkaSink kafka_;
};

}  // namespace iota_proxy

#endif
