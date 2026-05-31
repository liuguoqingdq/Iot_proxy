#ifndef IOTA_PROXY_BUSINESS_NODE_IDENTITY_HPP
#define IOTA_PROXY_BUSINESS_NODE_IDENTITY_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Kcp/KcpConv.hpp"
#include "iota_proxy/common/ByteView.hpp"
#include "iota_proxy/common/Endpoint.hpp"

namespace iota_proxy {

using Key256 = std::array<std::uint8_t, 32>;
using Signature512 = std::array<std::uint8_t, 64>;

struct NodePrivateKey {
    Key256 bytes{};
};

struct NodePublicKey {
    Key256 bytes{};
};

struct NodeKeyPair {
    NodePrivateKey private_key;
    NodePublicKey public_key;
};

struct NodeIdentity {
    NodeKeyPair key_pair;
    myring::kcp::KcpConv id;
    bool configured = false;
};

struct NodeAddress {
    Ipv4Endpoint endpoint;
    myring::kcp::KcpConv id;
};

bool generate_node_key_pair(NodeKeyPair* out, std::string* error = nullptr);
bool derive_public_key(const NodePrivateKey& private_key,
                       NodePublicKey* out,
                       std::string* error = nullptr);
myring::kcp::KcpConv node_id_from_public_key(
    const NodePublicKey& public_key) noexcept;
bool public_key_from_node_id(const myring::kcp::KcpConv& id,
                             NodePublicKey* out) noexcept;

bool sign_node_message(const NodePrivateKey& private_key,
                       ByteView message,
                       Signature512* out,
                       std::string* error = nullptr);
bool verify_node_message(const NodePublicKey& public_key,
                         ByteView message,
                         const Signature512& signature,
                         std::string* error = nullptr);

bool parse_hex_256(const std::string& text, Key256* out) noexcept;
std::string hex_256(const Key256& value);
bool parse_hex_512(const std::string& text, Signature512* out) noexcept;
std::string hex_512(const Signature512& value);

bool parse_convid_hex(const std::string& text,
                      myring::kcp::KcpConv* out) noexcept;
std::string convid_to_hex(const myring::kcp::KcpConv& conv);

std::string default_identity_file_path();
bool load_node_identity_from_json(const std::string& path,
                                  NodeIdentity* out,
                                  std::string* error = nullptr);
bool save_node_identity_to_json(const std::string& path,
                                const NodeIdentity& identity,
                                std::string* error = nullptr);
bool load_or_create_node_identity(const std::string& path,
                                  NodeIdentity* out,
                                  bool* created = nullptr,
                                  std::string* error = nullptr);
bool load_bootstrap_nodes_from_json(const std::string& path,
                                    std::vector<NodeAddress>* out,
                                    std::string* error = nullptr);
bool parse_node_address(const std::string& text, NodeAddress* out) noexcept;

}  // namespace iota_proxy

#endif
