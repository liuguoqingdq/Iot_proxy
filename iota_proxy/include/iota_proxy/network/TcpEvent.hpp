#ifndef IOTA_PROXY_NETWORK_TCP_EVENT_HPP
#define IOTA_PROXY_NETWORK_TCP_EVENT_HPP

#include <cstdint>

#include "iota_proxy/common/ByteView.hpp"

namespace iota_proxy {

enum class TcpEventType {
    Connected,
    Data,
    Closed
};

enum class TcpEventSource {
    Ingress,
    Egress
};

struct TcpEvent {
    TcpEventType type = TcpEventType::Data;
    TcpEventSource source = TcpEventSource::Ingress;
    std::uint64_t stream_id = 0;
    ByteView payload;
};

class TcpEventHandler {
public:
    virtual ~TcpEventHandler() = default;
    virtual void on_tcp_event(const TcpEvent& event) = 0;
};

}  // namespace iota_proxy

#endif
