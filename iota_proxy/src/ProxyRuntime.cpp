#include "iota_proxy/ProxyRuntime.hpp"

#include <openssl/rand.h>

#include <optional>

namespace iota_proxy {
namespace {

bool random_message_id(myring::kcp::KcpConv* out) {
    if (out == nullptr) {
        return false;
    }
    if (RAND_bytes(out->bytes.data(), static_cast<int>(out->bytes.size())) !=
        1) {
        return false;
    }
    if (out->empty()) {
        out->bytes.back() = 1;
    }
    return true;
}

}  // namespace

ProxyRuntime::ProxyRuntime(const ProxyRuntimeConfig& config)
    : config_(config),
      admission_(config.admission),
      tcp_(config.tcp),
      egress_(config.egress),
      edge_data_(config.edge_data),
      kcp_(config.kcp),
      discovery_(config.discovery),
      business_() {
    tcp_.set_event_handler(this);
    egress_.set_event_handler(this);

    business_.set_kcp_sender(
        [this](TunnelFrameType type,
               std::uint64_t stream_id,
               ByteView payload) {
            const std::optional<RouteNextHop> next =
                discovery_.select_default_next_hop();
            if (next.has_value()) {
                return kcp_.send_frame_to(
                    next->next_hop.id,
                    type,
                    stream_id,
                    payload
                );
            }
            return kcp_.send_frame(type, stream_id, payload);
        }
    );
    business_.set_tcp_sender(
        [this](std::uint64_t stream_id, ByteView payload) {
            return tcp_.send_to_stream(stream_id, payload);
        }
    );
    business_.set_tcp_write_shutdown(
        [this](std::uint64_t stream_id) {
            tcp_.shutdown_stream_write(stream_id);
        }
    );
    business_.set_tcp_shutdown(
        [this](std::uint64_t stream_id) {
            tcp_.shutdown_stream(stream_id);
        }
    );
    business_.set_tcp_egress_open(
        [this](std::uint64_t stream_id) {
            return egress_.open_stream(stream_id);
        }
    );
    business_.set_tcp_egress_sender(
        [this](std::uint64_t stream_id, ByteView payload) {
            return egress_.send_to_stream(stream_id, payload);
        }
    );
    business_.set_tcp_egress_write_shutdown(
        [this](std::uint64_t stream_id) {
            egress_.shutdown_stream_write(stream_id);
        }
    );
    business_.set_tcp_egress_shutdown(
        [this](std::uint64_t stream_id) {
            egress_.close_stream(stream_id);
        }
    );
    business_.set_edge_data_sink(
        [this](std::uint64_t stream_id,
               ByteView payload,
               const BroadcastMetadata& metadata) {
            return edge_data_.write_edge_data(stream_id, payload, metadata);
        }
    );
    business_.set_replicate_sender(
        [this](std::uint64_t stream_id,
               ByteView payload,
               const BroadcastMetadata& metadata) {
            return kcp_.broadcast_frame(
                TunnelFrameType::Replicate,
                stream_id,
                payload,
                &metadata);
        }
    );
    business_.set_broadcast_metadata_factory(
        [this]() {
            BroadcastMetadata metadata;
            metadata.origin = config_.kcp.identity.id;
            metadata.valid =
                !metadata.origin.empty() &&
                random_message_id(&metadata.message_id);
            return metadata;
        }
    );

    kcp_.set_frame_callback(
        [this](const TunnelFrameView& frame) {
            if (!admission_.try_accept_ingress_bytes(frame.payload.size())) {
                return;
            }
            business_.on_kcp_frame(frame);
        }
    );
    kcp_.set_control_callback(
        [this](const myring::kcp::KcpSessionKey&, ByteView payload) {
            if (!admission_.try_accept_ingress_bytes(payload.size())) {
                return;
            }
            discovery_.on_kcp_control(payload);
        }
    );

    discovery_.set_kcp_link_callback(
        [this](const NodeAddress& peer,
               const myring::kcp::KcpConv& conv) {
            if (!admission_.can_accept_kcp_link(kcp_.peer_link_count())) {
                return false;
            }
            return kcp_.add_peer_link(peer, conv);
        }
    );
    discovery_.set_control_broadcast(
        [this](ByteView payload) {
            return kcp_.broadcast_control(payload);
        }
    );
    discovery_.set_capability_provider(
        [this]() {
            return admission_.snapshot(kcp_.peer_link_count());
        }
    );
    discovery_.set_kcp_admission_callback(
        [this](const NodeAddress&) {
            return admission_.can_accept_kcp_link(kcp_.peer_link_count());
        }
    );
}

ProxyRuntime::~ProxyRuntime() {
    stop();
}

bool ProxyRuntime::start() {
    if (!kcp_.start()) {
        return false;
    }

    if (!discovery_.start()) {
        kcp_.stop();
        return false;
    }

    try {
        tcp_.start();
    } catch (...) {
        discovery_.stop();
        kcp_.stop();
        throw;
    }

    return true;
}

void ProxyRuntime::stop() {
    tcp_.stop();
    egress_.stop();
    edge_data_.stop();
    discovery_.stop();
    kcp_.stop();
}

void ProxyRuntime::on_tcp_event(const TcpEvent& event) {
    if (event.source == TcpEventSource::Ingress) {
        if (event.type == TcpEventType::Connected) {
            if (!admission_.try_accept_ingress_stream(event.stream_id)) {
                tcp_.shutdown_stream(event.stream_id);
                return;
            }
        } else if (event.type == TcpEventType::Closed) {
            admission_.release_ingress_stream(event.stream_id);
        } else if (event.type == TcpEventType::Data &&
                   !admission_.try_accept_ingress_bytes(
                       event.payload.size()
                   )) {
            admission_.release_ingress_stream(event.stream_id);
            tcp_.shutdown_stream(event.stream_id);
            return;
        }
    }

    business_.on_tcp_event(event);
}

}  // namespace iota_proxy
