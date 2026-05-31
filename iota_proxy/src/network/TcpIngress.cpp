#include "iota_proxy/network/TcpIngress.hpp"

#include <iostream>
#include <utility>
#include <vector>

#include "iota_proxy/frame/EdgeDataFrame.hpp"

namespace iota_proxy {

TcpIngress::TcpIngress(const TcpIngressConfig& config)
    : config_(config),
      server_(
          config.enabled
              ? myring::TcpServer::create(
                    config.entries,
                    config.host.c_str(),
                    config.port,
                    config.worker_threads,
                    config.backlog,
                    config.recv_buffer_size
                )
              : nullptr
      ),
      handler_(nullptr),
      mutex_(),
      stream_by_fd_(),
      frame_buffer_by_fd_(),
      connection_by_stream_(),
      read_closed_streams_(),
      next_stream_id_(1) {
    if (!server_) {
        return;
    }

    server_->set_connection_callback(
        [this](const myring::TcpConnectionPtr& connection) {
            handle_connection(connection);
        }
    );
    server_->set_raw_message_callback(
        [this](const myring::TcpConnectionPtr& connection,
               const char* data,
               std::size_t len) {
            handle_data(connection, data, len);
        }
    );
    server_->set_read_closed_callback(
        [this](const myring::TcpConnectionPtr& connection) {
            handle_read_closed(connection);
        }
    );
}

TcpIngress::~TcpIngress() {
    stop();
}

void TcpIngress::set_event_handler(TcpEventHandler* handler) noexcept {
    handler_ = handler;
}

void TcpIngress::start() {
    if (!server_) {
        return;
    }
    server_->start(config_.max_events);
}

void TcpIngress::stop() {
    if (server_) {
        server_->stop();
    }
}

bool TcpIngress::send_to_stream(std::uint64_t stream_id, ByteView payload) {
    if (!server_ || stream_id == 0 || payload.data() == nullptr ||
        payload.empty()) {
        return false;
    }

    myring::TcpConnectionPtr connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connection_by_stream_.find(stream_id);
        if (it == connection_by_stream_.end()) {
            return false;
        }
        connection = it->second;
    }

    return connection && connection->connected() &&
           connection->send(payload.data(), payload.size());
}

void TcpIngress::shutdown_stream_write(std::uint64_t stream_id) {
    if (stream_id == 0) {
        return;
    }

    myring::TcpConnectionPtr connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connection_by_stream_.find(stream_id);
        if (it == connection_by_stream_.end()) {
            return;
        }
        connection = it->second;
    }

    if (connection) {
        connection->shutdown_write();
    }
}

void TcpIngress::shutdown_stream(std::uint64_t stream_id) {
    if (stream_id == 0) {
        return;
    }

    myring::TcpConnectionPtr connection;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connection_by_stream_.find(stream_id);
        if (it == connection_by_stream_.end()) {
            return;
        }
        connection = it->second;
    }

    if (connection) {
        connection->shutdown();
    }
}

void TcpIngress::handle_connection(
    const myring::TcpConnectionPtr& connection) {
    if (!connection) {
        return;
    }

    TcpEvent event;
    event.source = TcpEventSource::Ingress;
    if (connection->connected()) {
        event.type = TcpEventType::Connected;
        std::uint64_t local_id = 0;
        do {
            local_id =
                next_stream_id_.fetch_add(1, std::memory_order_relaxed) &
                0xffffffffULL;
        } while (local_id == 0);
        event.stream_id =
            (static_cast<std::uint64_t>(config_.node_id) << 32) | local_id;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            stream_by_fd_[connection->fd()] = event.stream_id;
            frame_buffer_by_fd_[connection->fd()] = std::string();
            connection_by_stream_[event.stream_id] = connection;
        }
    } else {
        event.type = TcpEventType::Closed;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = stream_by_fd_.find(connection->fd());
            if (it == stream_by_fd_.end()) {
                return;
            }

            event.stream_id = it->second;
            stream_by_fd_.erase(it);
            frame_buffer_by_fd_.erase(connection->fd());
            connection_by_stream_.erase(event.stream_id);
            const bool already_read_closed =
                read_closed_streams_.erase(event.stream_id) > 0;
            if (already_read_closed) {
                event.stream_id = 0;
            }
        }
    }

    if (handler_ != nullptr && event.stream_id != 0) {
        handler_->on_tcp_event(event);
    }
}

void TcpIngress::handle_read_closed(
    const myring::TcpConnectionPtr& connection) {
    if (!connection) {
        return;
    }

    TcpEvent event;
    event.type = TcpEventType::Closed;
    event.source = TcpEventSource::Ingress;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = stream_by_fd_.find(connection->fd());
        if (it == stream_by_fd_.end()) {
            return;
        }
        event.stream_id = it->second;
        frame_buffer_by_fd_.erase(connection->fd());
        if (!read_closed_streams_.insert(event.stream_id).second) {
            return;
        }
    }

    if (handler_ != nullptr) {
        handler_->on_tcp_event(event);
    }
}

void TcpIngress::handle_data(const myring::TcpConnectionPtr& connection,
                             const char* data,
                             std::size_t len) {
    if (!connection || data == nullptr || len == 0) {
        return;
    }

    if (handler_ == nullptr) {
        return;
    }

    const int fd = connection->fd();
    std::uint64_t stream_id = 0;
    std::vector<std::string> frames;
    bool close_connection = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto stream_it = stream_by_fd_.find(fd);
        if (stream_it == stream_by_fd_.end()) {
            return;
        }
        stream_id = stream_it->second;

        std::string& buffer = frame_buffer_by_fd_[fd];
        const std::size_t max_buffer_size =
            kEdgeDataFrameHeaderSize + config_.max_edge_frame_payload_size;
        if (buffer.size() > max_buffer_size ||
            len > max_buffer_size ||
            buffer.size() > max_buffer_size - len) {
            close_connection = true;
        } else {
            buffer.append(data, len);
        }

        while (!close_connection &&
               buffer.size() >= kEdgeDataFrameHeaderSize) {
            std::uint32_t payload_size = 0;
            if (!edge_data_frame_payload_size(
                    ByteView(buffer.data(), buffer.size()),
                    &payload_size)) {
                close_connection = true;
                break;
            }
            if (payload_size > config_.max_edge_frame_payload_size) {
                close_connection = true;
                break;
            }

            const std::size_t frame_size =
                kEdgeDataFrameHeaderSize +
                static_cast<std::size_t>(payload_size);
            if (buffer.size() < frame_size) {
                break;
            }

            frames.emplace_back(buffer.data(), frame_size);
            buffer.erase(0, frame_size);
        }
    }

    if (close_connection) {
        std::cerr << "tcp ingress: invalid edge frame from fd "
                  << fd << ", closing connection\n";
        connection->shutdown();
        return;
    }

    for (const std::string& frame : frames) {
        TcpEvent event;
        event.type = TcpEventType::Data;
        event.source = TcpEventSource::Ingress;
        event.stream_id = stream_id;
        event.payload = ByteView(frame.data(), frame.size());
        handler_->on_tcp_event(event);
    }
}

std::uint64_t TcpIngress::find_stream_id_by_fd(int fd) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = stream_by_fd_.find(fd);
    if (it == stream_by_fd_.end()) {
        return 0;
    }

    return it->second;
}

}  // namespace iota_proxy
