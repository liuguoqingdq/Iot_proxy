#include "iota_proxy/common/Endpoint.hpp"

#include <arpa/inet.h>

#include <cstring>

namespace iota_proxy {

bool make_sockaddr(const Ipv4Endpoint& endpoint, sockaddr_in* out) noexcept {
    if (out == nullptr || endpoint.host.empty() || endpoint.port == 0) {
        return false;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port);

    if (::inet_pton(AF_INET, endpoint.host.c_str(), &addr.sin_addr) != 1) {
        return false;
    }

    *out = addr;
    return true;
}

std::string endpoint_to_string(const Ipv4Endpoint& endpoint) {
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

}  // namespace iota_proxy
