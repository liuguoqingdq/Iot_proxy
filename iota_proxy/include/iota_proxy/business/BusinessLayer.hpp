#ifndef IOTA_PROXY_BUSINESS_BUSINESS_LAYER_HPP
#define IOTA_PROXY_BUSINESS_BUSINESS_LAYER_HPP

#include <cstdint>
#include <functional>

#include "iota_proxy/common/ByteView.hpp"
#include "iota_proxy/frame/TunnelFrame.hpp"
#include "iota_proxy/network/TcpEvent.hpp"

namespace iota_proxy {

using KcpFrameSender =
    std::function<bool(TunnelFrameType, std::uint64_t, ByteView)>;
using TcpDataSender = std::function<bool(std::uint64_t, ByteView)>;
using TcpShutdown = std::function<void(std::uint64_t)>;
using TcpStreamOpen = std::function<bool(std::uint64_t)>;
using EdgeDataSink =
    std::function<bool(std::uint64_t, ByteView, const BroadcastMetadata&)>;
using ReplicateSender = std::function<std::size_t(
    std::uint64_t,
    ByteView,
    const BroadcastMetadata&)>;
using BroadcastMetadataFactory = std::function<BroadcastMetadata()>;

class BusinessLayer {
public:
    virtual ~BusinessLayer() = default;

    virtual void on_tcp_event(const TcpEvent& event) = 0;
    virtual void on_kcp_frame(const TunnelFrameView& frame) = 0;
};

class PassThroughBusiness final : public BusinessLayer {
public:
    PassThroughBusiness();

    void set_kcp_sender(KcpFrameSender sender);
    void set_tcp_sender(TcpDataSender sender);
    void set_tcp_write_shutdown(TcpShutdown shutdown);
    void set_tcp_shutdown(TcpShutdown shutdown);
    void set_tcp_egress_open(TcpStreamOpen open);
    void set_tcp_egress_sender(TcpDataSender sender);
    void set_tcp_egress_write_shutdown(TcpShutdown shutdown);
    void set_tcp_egress_shutdown(TcpShutdown shutdown);
    void set_edge_data_sink(EdgeDataSink sink);
    void set_replicate_sender(ReplicateSender sender);
    void set_broadcast_metadata_factory(BroadcastMetadataFactory factory);

    void on_tcp_event(const TcpEvent& event) override;
    void on_kcp_frame(const TunnelFrameView& frame) override;

private:
    KcpFrameSender kcp_sender_;
    TcpDataSender tcp_sender_;
    TcpShutdown tcp_write_shutdown_;
    TcpShutdown tcp_shutdown_;
    TcpStreamOpen tcp_egress_open_;
    TcpDataSender tcp_egress_sender_;
    TcpShutdown tcp_egress_write_shutdown_;
    TcpShutdown tcp_egress_shutdown_;
    EdgeDataSink edge_data_sink_;
    ReplicateSender replicate_sender_;
    BroadcastMetadataFactory broadcast_metadata_factory_;
};

}  // namespace iota_proxy

#endif
