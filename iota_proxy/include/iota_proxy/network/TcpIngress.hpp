#ifndef IOTA_PROXY_NETWORK_TCP_INGRESS_HPP
#define IOTA_PROXY_NETWORK_TCP_INGRESS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "TcpServer.h"
#include "iota_proxy/common/ByteView.hpp"
#include "iota_proxy/network/TcpEvent.hpp"

namespace iota_proxy {

struct TcpIngressConfig {
    bool enabled = true;
    unsigned entries = 256;
    std::string host = "0.0.0.0";
    std::uint16_t port = 7000;
    std::size_t worker_threads = 1;
    int backlog = 1024;
    std::size_t recv_buffer_size = 16 * 1024;
    std::size_t max_events = 64;
    std::uint32_t node_id = 1;
    std::size_t max_edge_frame_payload_size = 1024 * 1024;
};

class TcpIngress {
public:
    explicit TcpIngress(const TcpIngressConfig& config);
    ~TcpIngress();

    void set_event_handler(TcpEventHandler* handler) noexcept;

    void start();
    void stop();

    bool send_to_stream(std::uint64_t stream_id, ByteView payload);
    void shutdown_stream_write(std::uint64_t stream_id);
    void shutdown_stream(std::uint64_t stream_id);

private:
    void handle_connection(const myring::TcpConnectionPtr& connection);
    void handle_read_closed(const myring::TcpConnectionPtr& connection);
    void handle_data(const myring::TcpConnectionPtr& connection,
                     const char* data,
                     std::size_t len);
    std::uint64_t find_stream_id_by_fd(int fd) const;

private:
    TcpIngressConfig config_;
    std::shared_ptr<myring::TcpServer> server_;
    TcpEventHandler* handler_;
    mutable std::mutex mutex_;
    std::unordered_map<int, std::uint64_t> stream_by_fd_;
    std::unordered_map<int, std::string> frame_buffer_by_fd_;
    std::unordered_map<std::uint64_t, myring::TcpConnectionPtr>
        connection_by_stream_;
    std::unordered_set<std::uint64_t> read_closed_streams_;
    std::atomic<std::uint64_t> next_stream_id_;
};

}  // namespace iota_proxy

#endif
