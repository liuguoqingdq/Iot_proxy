#include "iota_proxy/business/RedisStateCache.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <vector>

namespace iota_proxy {
namespace {

std::string bulk(const char* data, std::size_t len) {
    std::string out;
    out.reserve(32 + len);
    out.push_back('$');
    out.append(std::to_string(len));
    out.append("\r\n");
    if (len != 0) {
        out.append(data, len);
    }
    out.append("\r\n");
    return out;
}

std::string bulk(const std::string& value) {
    return bulk(value.data(), value.size());
}

std::string command(std::initializer_list<std::string> args) {
    std::string out;
    out.append("*");
    out.append(std::to_string(args.size()));
    out.append("\r\n");
    for (const std::string& arg : args) {
        out.append(bulk(arg));
    }
    return out;
}

std::string set_command(const std::string& key,
                        const std::string& value,
                        std::uint32_t ttl_seconds) {
    if (ttl_seconds == 0) {
        return command({"SET", key, value});
    }
    return command(
        {"SET", key, value, "EX", std::to_string(ttl_seconds)}
    );
}

std::string set_nx_command(const std::string& key,
                           const std::string& value,
                           std::uint32_t ttl_seconds) {
    if (ttl_seconds == 0) {
        return command({"SET", key, value, "NX"});
    }
    return command(
        {"SET", key, value, "EX", std::to_string(ttl_seconds), "NX"}
    );
}

bool wait_fd(int fd, short events, std::uint32_t timeout_ms) {
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    const int wait_ms =
        timeout_ms == 0 ? -1 : static_cast<int>(timeout_ms);
    int rc = 0;
    do {
        rc = ::poll(&pfd, 1, wait_ms);
    } while (rc < 0 && errno == EINTR);
    return rc > 0 && (pfd.revents & events) != 0;
}

bool write_all(int fd, const char* data, std::size_t len, std::uint32_t ms) {
    while (len > 0) {
        if (!wait_fd(fd, POLLOUT, ms)) {
            return false;
        }
        const ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        data += static_cast<std::size_t>(n);
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

bool read_byte(int fd, char* out, std::uint32_t ms) {
    if (out == nullptr || !wait_fd(fd, POLLIN, ms)) {
        return false;
    }
    while (true) {
        const ssize_t n = ::recv(fd, out, 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        return n == 1;
    }
}

bool read_line(int fd, std::string* out, std::uint32_t ms) {
    if (out == nullptr) {
        return false;
    }
    out->clear();
    char previous = 0;
    while (out->size() < 1024 * 1024) {
        char c = 0;
        if (!read_byte(fd, &c, ms)) {
            return false;
        }
        out->push_back(c);
        if (previous == '\r' && c == '\n') {
            out->resize(out->size() - 2);
            return true;
        }
        previous = c;
    }
    return false;
}

bool discard_bytes(int fd, std::size_t len, std::uint32_t ms) {
    char buffer[4096];
    while (len > 0) {
        if (!wait_fd(fd, POLLIN, ms)) {
            return false;
        }
        const std::size_t chunk = std::min(len, sizeof(buffer));
        const ssize_t n = ::recv(fd, buffer, chunk, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

int connect_with_timeout(const Ipv4Endpoint& endpoint,
                         std::uint32_t timeout_ms) {
    sockaddr_in addr;
    if (!make_sockaddr(endpoint, &addr)) {
        return -1;
    }

    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        return -1;
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 ||
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd);
        return -1;
    }

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) !=
        0) {
        if (errno != EINPROGRESS) {
            ::close(fd);
            return -1;
        }
        if (!wait_fd(fd, POLLOUT, timeout_ms)) {
            ::close(fd);
            errno = ETIMEDOUT;
            return -1;
        }

        int error = 0;
        socklen_t error_len = sizeof(error);
        if (getsockopt(
                fd,
                SOL_SOCKET,
                SO_ERROR,
                &error,
                &error_len
            ) != 0 ||
            error != 0) {
            ::close(fd);
            errno = error == 0 ? errno : error;
            return -1;
        }
    }

    if (::fcntl(fd, F_SETFL, flags) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

RedisStateCache::RedisStateCache(const RedisStateCacheConfig& config)
    : config_(config),
      mutex_(),
      fd_(-1) {}

RedisStateCache::~RedisStateCache() {
    stop();
}

bool RedisStateCache::record_edge_message(
    const EdgeDataMessage& message,
    const std::string& serialized_message,
    bool* duplicate) {
    if (duplicate != nullptr) {
        *duplicate = false;
    }
    if (!config_.enabled()) {
        return true;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    Reply reply;
    if (!message.message_id.empty()) {
        const std::string seen_key = "seen:" + message.message_id;
        if (!execute_locked(
                set_nx_command(
                    seen_key,
                    "1",
                    config_.seen_ttl_seconds
                ),
                &reply)) {
            return false;
        }
        if (reply.nil) {
            if (duplicate != nullptr) {
                *duplicate = true;
            }
            return true;
        }
    }

    const std::string device_prefix = "device:" + message.mac_hex;
    if (!execute_locked(
            set_command(
                device_prefix + ":state",
                "online",
                config_.device_state_ttl_seconds
            ),
            &reply)) {
        return false;
    }
    if (!execute_locked(
            set_command(
                device_prefix + ":latest",
                serialized_message,
                config_.device_latest_ttl_seconds
            ),
            &reply)) {
        return false;
    }

    return true;
}

void RedisStateCache::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    close_locked();
}

bool RedisStateCache::ensure_connected() {
    return fd_ >= 0 || connect_locked();
}

bool RedisStateCache::connect_locked() {
    if (!config_.enabled()) {
        return false;
    }

    fd_ = connect_with_timeout(
        config_.endpoint,
        config_.connect_timeout_ms
    );
    if (fd_ < 0) {
        std::cerr << "redis state cache: failed to connect "
                  << endpoint_to_string(config_.endpoint) << ": "
                  << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool RedisStateCache::execute_locked(const std::string& command,
                                     Reply* reply) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!ensure_connected()) {
            return false;
        }
        if (send_command_locked(command) && read_reply_locked(reply)) {
            if (reply != nullptr && reply->type == '-') {
                std::cerr << "redis state cache: command failed: "
                          << reply->text << "\n";
                return false;
            }
            return true;
        }
        close_locked();
    }
    return false;
}

bool RedisStateCache::send_command_locked(const std::string& command) {
    return fd_ >= 0 &&
           write_all(
               fd_,
               command.data(),
               command.size(),
               config_.io_timeout_ms
           );
}

bool RedisStateCache::read_reply_locked(Reply* reply) {
    if (fd_ < 0) {
        return false;
    }

    Reply parsed;
    if (!read_byte(fd_, &parsed.type, config_.io_timeout_ms)) {
        return false;
    }

    std::string line;
    if (parsed.type == '+' || parsed.type == ':' || parsed.type == '-') {
        if (!read_line(fd_, &parsed.text, config_.io_timeout_ms)) {
            return false;
        }
        if (reply != nullptr) {
            *reply = parsed;
        }
        return true;
    }

    if (parsed.type == '$') {
        if (!read_line(fd_, &line, config_.io_timeout_ms)) {
            return false;
        }
        char* end = nullptr;
        const long len = std::strtol(line.c_str(), &end, 10);
        if (end == line.c_str() || *end != '\0' || len < -1) {
            return false;
        }
        if (len < 0) {
            parsed.nil = true;
        } else if (!discard_bytes(
                       fd_,
                       static_cast<std::size_t>(len) + 2,
                       config_.io_timeout_ms
                   )) {
            return false;
        }
        if (reply != nullptr) {
            *reply = parsed;
        }
        return true;
    }

    return false;
}

void RedisStateCache::close_locked() noexcept {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace iota_proxy
