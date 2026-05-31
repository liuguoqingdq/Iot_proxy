#ifndef IOTA_PROXY_NETWORK_TCP_EGRESS_HPP
#define IOTA_PROXY_NETWORK_TCP_EGRESS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "iota_proxy/common/ByteView.hpp"
#include "iota_proxy/common/Endpoint.hpp"
#include "iota_proxy/network/TcpEvent.hpp"

namespace iota_proxy {

struct TcpEgressConfig {
    Ipv4Endpoint target;
    std::size_t recv_buffer_size = 16 * 1024;
    std::size_t max_streams = 4096;
    std::uint32_t connect_timeout_ms = 3000;
};

class TcpEgress {
public:
    explicit TcpEgress(const TcpEgressConfig& config);
    ~TcpEgress();

    void set_event_handler(TcpEventHandler* handler) noexcept;

    bool open_stream(std::uint64_t stream_id);
    bool send_to_stream(std::uint64_t stream_id, ByteView payload);
    void shutdown_stream_write(std::uint64_t stream_id);
    void close_stream(std::uint64_t stream_id);
    void stop();

private:
    struct Stream {
        explicit Stream(std::uint64_t stream_id, int socket_fd);

        std::uint64_t id;
        int fd;
        std::atomic<bool> open;
        std::mutex write_mutex;
    };

    void read_loop(std::shared_ptr<Stream> stream);
    std::shared_ptr<Stream> remove_stream(std::uint64_t stream_id);
    void close_fd(std::shared_ptr<Stream> stream);

private:
    TcpEgressConfig config_;
    TcpEventHandler* handler_;
    std::atomic<bool> stopping_;
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Stream>> streams_;
    std::vector<std::thread> threads_;
};

}  // namespace iota_proxy

#endif
