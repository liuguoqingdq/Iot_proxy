#ifndef IOTA_PROXY_PROXY_RUNTIME_HPP
#define IOTA_PROXY_PROXY_RUNTIME_HPP

#include "iota_proxy/admission/AdmissionControl.hpp"
#include "iota_proxy/business/BusinessLayer.hpp"
#include "iota_proxy/business/EdgeDataPipeline.hpp"
#include "iota_proxy/discovery/DiscoveryService.hpp"
#include "iota_proxy/kcp/KcpTunnel.hpp"
#include "iota_proxy/network/TcpEgress.hpp"
#include "iota_proxy/network/TcpIngress.hpp"

namespace iota_proxy {

struct ProxyRuntimeConfig {
    AdmissionControlConfig admission;
    TcpIngressConfig tcp;
    TcpEgressConfig egress;
    EdgeDataPipelineConfig edge_data;
    KcpTunnelConfig kcp;
    DiscoveryConfig discovery;
};

class ProxyRuntime final : public TcpEventHandler {
public:
    explicit ProxyRuntime(const ProxyRuntimeConfig& config);
    ~ProxyRuntime();

    bool start();
    void stop();

    void on_tcp_event(const TcpEvent& event) override;

private:
    ProxyRuntimeConfig config_;
    AdmissionControl admission_;
    TcpIngress tcp_;
    TcpEgress egress_;
    EdgeDataPipeline edge_data_;
    KcpTunnel kcp_;
    DiscoveryService discovery_;
    PassThroughBusiness business_;
};

}  // namespace iota_proxy

#endif
