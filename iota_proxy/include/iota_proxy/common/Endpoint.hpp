#ifndef IOTA_PROXY_COMMON_ENDPOINT_HPP
#define IOTA_PROXY_COMMON_ENDPOINT_HPP

#include <netinet/in.h>

#include <cstdint>
#include <string>

namespace iota_proxy {

struct Ipv4Endpoint {
    std::string host;
    std::uint16_t port = 0;
};

bool make_sockaddr(const Ipv4Endpoint& endpoint, sockaddr_in* out) noexcept;
std::string endpoint_to_string(const Ipv4Endpoint& endpoint);

}  // namespace iota_proxy

#endif
