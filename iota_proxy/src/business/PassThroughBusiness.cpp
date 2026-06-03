#include "iota_proxy/business/BusinessLayer.hpp"

#include <iostream>
#include <utility>

namespace iota_proxy {

PassThroughBusiness::PassThroughBusiness()
    : kcp_sender_(),
      tcp_sender_(),
      tcp_write_shutdown_(),
      tcp_shutdown_(),
      tcp_egress_open_(),
      tcp_egress_sender_(),
      tcp_egress_write_shutdown_(),
      tcp_egress_shutdown_(),
      edge_data_sink_(),
      replicate_sender_(),
      broadcast_metadata_factory_() {}

void PassThroughBusiness::set_kcp_sender(KcpFrameSender sender) {
    kcp_sender_ = std::move(sender);
}

void PassThroughBusiness::set_tcp_sender(TcpDataSender sender) {
    tcp_sender_ = std::move(sender);
}

void PassThroughBusiness::set_tcp_write_shutdown(TcpShutdown shutdown) {
    tcp_write_shutdown_ = std::move(shutdown);
}

void PassThroughBusiness::set_tcp_shutdown(TcpShutdown shutdown) {
    tcp_shutdown_ = std::move(shutdown);
}

void PassThroughBusiness::set_tcp_egress_open(TcpStreamOpen open) {
    tcp_egress_open_ = std::move(open);
}

void PassThroughBusiness::set_tcp_egress_sender(TcpDataSender sender) {
    tcp_egress_sender_ = std::move(sender);
}

void PassThroughBusiness::set_tcp_egress_write_shutdown(TcpShutdown shutdown) {
    tcp_egress_write_shutdown_ = std::move(shutdown);
}

void PassThroughBusiness::set_tcp_egress_shutdown(TcpShutdown shutdown) {
    tcp_egress_shutdown_ = std::move(shutdown);
}

void PassThroughBusiness::set_edge_data_sink(EdgeDataSink sink) {
    edge_data_sink_ = std::move(sink);
}

void PassThroughBusiness::set_replicate_sender(ReplicateSender sender) {
    replicate_sender_ = std::move(sender);
}

void PassThroughBusiness::set_broadcast_metadata_factory(
    BroadcastMetadataFactory factory) {
    broadcast_metadata_factory_ = std::move(factory);
}

void PassThroughBusiness::on_tcp_event(const TcpEvent& event) {
    if (!kcp_sender_ || event.stream_id == 0) {
        return;
    }

    switch (event.type) {
        case TcpEventType::Connected:
            kcp_sender_(TunnelFrameType::Open, event.stream_id, ByteView());
            break;
        case TcpEventType::Data: {
            BroadcastMetadata metadata;
            if (event.source == TcpEventSource::Ingress &&
                broadcast_metadata_factory_) {
                metadata = broadcast_metadata_factory_();
            }
            if (event.source == TcpEventSource::Ingress &&
                edge_data_sink_) {
                if (!edge_data_sink_(
                        event.stream_id,
                        event.payload,
                        metadata)) {
                    std::cerr << "edge_data_sink: failed to write stream "
                              << event.stream_id << "\n";
                }
            }
            if (event.source == TcpEventSource::Ingress &&
                replicate_sender_) {
                replicate_sender_(event.stream_id, event.payload, metadata);
            }
            kcp_sender_(
                TunnelFrameType::Data,
                event.stream_id,
                event.payload
            );
            break;
        }
        case TcpEventType::Closed:
            kcp_sender_(TunnelFrameType::Finish, event.stream_id, ByteView());
            break;
    }
}

void PassThroughBusiness::on_kcp_frame(const TunnelFrameView& frame) {
    if (frame.stream_id == 0) {
        return;
    }

    switch (frame.type) {
        case TunnelFrameType::Open:
            if ((!tcp_egress_open_ ||
                 !tcp_egress_open_(frame.stream_id)) &&
                kcp_sender_) {
                kcp_sender_(
                    TunnelFrameType::Close,
                    frame.stream_id,
                    ByteView()
                );
            }
            break;
        case TunnelFrameType::Data:
            if (frame.payload.empty()) {
                break;
            }
            if (tcp_egress_sender_ &&
                tcp_egress_sender_(frame.stream_id, frame.payload)) {
                break;
            }
            if (tcp_sender_) {
                if (!tcp_sender_(frame.stream_id, frame.payload) &&
                    kcp_sender_) {
                    kcp_sender_(
                        TunnelFrameType::Close,
                        frame.stream_id,
                        ByteView()
                    );
                }
            }
            break;
        case TunnelFrameType::Close:
            if (tcp_egress_shutdown_) {
                tcp_egress_shutdown_(frame.stream_id);
            }
            if (tcp_shutdown_) {
                tcp_shutdown_(frame.stream_id);
            }
            break;
        case TunnelFrameType::Finish:
            if (tcp_egress_write_shutdown_) {
                tcp_egress_write_shutdown_(frame.stream_id);
            }
            if (tcp_write_shutdown_) {
                tcp_write_shutdown_(frame.stream_id);
            }
            break;
        case TunnelFrameType::Control:
            break;
        case TunnelFrameType::Replicate:
            // Received replicated edge data from a peer proxy.
            // KcpTunnel has already handled bounded gossip relay and duplicate
            // suppression before delivering the frame here.
            if (!frame.payload.empty() && edge_data_sink_) {
                if (!edge_data_sink_(
                        frame.stream_id,
                        frame.payload,
                        frame.broadcast)) {
                    std::cerr << "replicate: failed to write stream "
                              << frame.stream_id << " to edge pipeline\n";
                }
            }
            break;
        case TunnelFrameType::KafkaBroadcast:
            break;
    }
}

}  // namespace iota_proxy
