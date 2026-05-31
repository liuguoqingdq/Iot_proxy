#include "iota_proxy/business/NodeIdentity.hpp"

#include <openssl/evp.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <unistd.h>

namespace iota_proxy {
namespace {

using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using PKeyCtxPtr =
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message == nullptr ? std::string() : std::string(message);
    }
}

bool set_identity_from_private_key(const NodePrivateKey& private_key,
                                   NodeIdentity* out,
                                   std::string* error) {
    if (out == nullptr) {
        set_error(error, "invalid identity output");
        return false;
    }

    NodePublicKey public_key;
    if (!derive_public_key(private_key, &public_key, error)) {
        return false;
    }

    out->key_pair.private_key = private_key;
    out->key_pair.public_key = public_key;
    out->id = node_id_from_public_key(public_key);
    out->configured = true;
    return true;
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool read_raw_private_key(EVP_PKEY* key,
                          NodePrivateKey* out,
                          std::string* error) {
    if (key == nullptr || out == nullptr) {
        set_error(error, "invalid private key output");
        return false;
    }

    std::size_t len = out->bytes.size();
    if (EVP_PKEY_get_raw_private_key(key, out->bytes.data(), &len) != 1 ||
        len != out->bytes.size()) {
        set_error(error, "failed to read raw Ed25519 private key");
        return false;
    }

    return true;
}

bool read_raw_public_key(EVP_PKEY* key,
                         NodePublicKey* out,
                         std::string* error) {
    if (key == nullptr || out == nullptr) {
        set_error(error, "invalid public key output");
        return false;
    }

    std::size_t len = out->bytes.size();
    if (EVP_PKEY_get_raw_public_key(key, out->bytes.data(), &len) != 1 ||
        len != out->bytes.size()) {
        set_error(error, "failed to read raw Ed25519 public key");
        return false;
    }

    return true;
}

}  // namespace

bool generate_node_key_pair(NodeKeyPair* out, std::string* error) {
    if (out == nullptr) {
        set_error(error, "invalid key pair output");
        return false;
    }

    PKeyCtxPtr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr),
                   EVP_PKEY_CTX_free);
    if (!ctx) {
        set_error(error, "failed to create Ed25519 keygen context");
        return false;
    }

    if (EVP_PKEY_keygen_init(ctx.get()) != 1) {
        set_error(error, "failed to initialize Ed25519 keygen");
        return false;
    }

    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw_key) != 1 || raw_key == nullptr) {
        set_error(error, "failed to generate Ed25519 key pair");
        return false;
    }

    PKeyPtr key(raw_key, EVP_PKEY_free);
    return read_raw_private_key(key.get(), &out->private_key, error) &&
           read_raw_public_key(key.get(), &out->public_key, error);
}

bool derive_public_key(const NodePrivateKey& private_key,
                       NodePublicKey* out,
                       std::string* error) {
    PKeyPtr key(
        EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519,
            nullptr,
            private_key.bytes.data(),
            private_key.bytes.size()
        ),
        EVP_PKEY_free
    );
    if (!key) {
        set_error(error, "failed to import raw Ed25519 private key");
        return false;
    }

    return read_raw_public_key(key.get(), out, error);
}

myring::kcp::KcpConv node_id_from_public_key(
    const NodePublicKey& public_key) noexcept {
    myring::kcp::KcpConv id;
    for (std::size_t i = 0; i < id.bytes.size(); ++i) {
        id.bytes[i] = public_key.bytes[i];
    }
    return id;
}

bool public_key_from_node_id(const myring::kcp::KcpConv& id,
                             NodePublicKey* out) noexcept {
    if (out == nullptr || id.empty()) {
        return false;
    }

    for (std::size_t i = 0; i < out->bytes.size(); ++i) {
        out->bytes[i] = id.bytes[i];
    }
    return true;
}

bool sign_node_message(const NodePrivateKey& private_key,
                       ByteView message,
                       Signature512* out,
                       std::string* error) {
    if (out == nullptr) {
        set_error(error, "invalid signature output");
        return false;
    }

    PKeyPtr key(
        EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519,
            nullptr,
            private_key.bytes.data(),
            private_key.bytes.size()
        ),
        EVP_PKEY_free
    );
    if (!key) {
        set_error(error, "failed to import raw Ed25519 private key");
        return false;
    }

    MdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) {
        set_error(error, "failed to create Ed25519 signing context");
        return false;
    }
    if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) !=
        1) {
        set_error(error, "failed to initialize Ed25519 signing");
        return false;
    }

    std::size_t signature_len = out->size();
    if (EVP_DigestSign(
            ctx.get(),
            out->data(),
            &signature_len,
            reinterpret_cast<const unsigned char*>(message.data()),
            message.size()
        ) != 1 ||
        signature_len != out->size()) {
        set_error(error, "failed to sign Ed25519 message");
        return false;
    }

    return true;
}

bool verify_node_message(const NodePublicKey& public_key,
                         ByteView message,
                         const Signature512& signature,
                         std::string* error) {
    PKeyPtr key(
        EVP_PKEY_new_raw_public_key(
            EVP_PKEY_ED25519,
            nullptr,
            public_key.bytes.data(),
            public_key.bytes.size()
        ),
        EVP_PKEY_free
    );
    if (!key) {
        set_error(error, "failed to import raw Ed25519 public key");
        return false;
    }

    MdCtxPtr ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) {
        set_error(error, "failed to create Ed25519 verify context");
        return false;
    }
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) !=
        1) {
        set_error(error, "failed to initialize Ed25519 verify");
        return false;
    }

    const int rc = EVP_DigestVerify(
        ctx.get(),
        signature.data(),
        signature.size(),
        reinterpret_cast<const unsigned char*>(message.data()),
        message.size()
    );
    if (rc != 1) {
        set_error(error, "Ed25519 signature verification failed");
        return false;
    }

    return true;
}

bool parse_hex_256(const std::string& text, Key256* out) noexcept {
    if (out == nullptr) {
        return false;
    }

    std::string value = text;
    if (value.size() >= 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value.erase(0, 2);
    }

    if (value.size() != out->size() * 2) {
        return false;
    }

    Key256 parsed{};
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        const int hi = hex_value(value[i * 2]);
        const int lo = hex_value(value[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        parsed[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }

    *out = parsed;
    return true;
}

std::string hex_256(const Key256& value) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (std::uint8_t byte : value) {
        out.push_back(digits[(byte >> 4) & 0x0f]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

bool parse_hex_512(const std::string& text, Signature512* out) noexcept {
    if (out == nullptr) {
        return false;
    }

    std::string value = text;
    if (value.size() >= 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        value.erase(0, 2);
    }

    if (value.size() != out->size() * 2) {
        return false;
    }

    Signature512 parsed{};
    for (std::size_t i = 0; i < parsed.size(); ++i) {
        const int hi = hex_value(value[i * 2]);
        const int lo = hex_value(value[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        parsed[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }

    *out = parsed;
    return true;
}

std::string hex_512(const Signature512& value) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(value.size() * 2);
    for (std::uint8_t byte : value) {
        out.push_back(digits[(byte >> 4) & 0x0f]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

bool parse_convid_hex(const std::string& text,
                      myring::kcp::KcpConv* out) noexcept {
    Key256 value{};
    if (out == nullptr || !parse_hex_256(text, &value)) {
        return false;
    }

    myring::kcp::KcpConv conv;
    for (std::size_t i = 0; i < conv.bytes.size(); ++i) {
        conv.bytes[i] = value[i];
    }

    if (conv.empty()) {
        return false;
    }

    *out = conv;
    return true;
}

std::string convid_to_hex(const myring::kcp::KcpConv& conv) {
    Key256 value{};
    for (std::size_t i = 0; i < value.size(); ++i) {
        value[i] = conv.bytes[i];
    }
    return hex_256(value);
}

std::string default_identity_file_path() {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return (std::filesystem::path(home) /
                ".iota_proxy" /
                "identity.json").string();
    }

    return "iota_identity.json";
}

bool load_node_identity_from_json(const std::string& path,
                                  NodeIdentity* out,
                                  std::string* error) {
    if (path.empty() || out == nullptr) {
        set_error(error, "invalid identity file path");
        return false;
    }

    std::ifstream input(path);
    if (!input) {
        set_error(error, "identity file is not readable");
        return false;
    }

    nlohmann::json doc;
    try {
        input >> doc;
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("identity json parse failed: ") + ex.what();
        }
        return false;
    }

    if (!doc.is_object() ||
        doc.value("version", 0) != 1 ||
        doc.value("algorithm", std::string()) != "ed25519" ||
        !doc.contains("private_key") ||
        !doc["private_key"].is_string()) {
        set_error(error, "identity json schema is invalid");
        return false;
    }

    NodePrivateKey private_key;
    if (!parse_hex_256(doc["private_key"].get<std::string>(),
                       &private_key.bytes)) {
        set_error(error, "identity private_key is invalid");
        return false;
    }

    NodeIdentity loaded;
    if (!set_identity_from_private_key(private_key, &loaded, error)) {
        return false;
    }

    if (doc.contains("public_key")) {
        if (!doc["public_key"].is_string() ||
            doc["public_key"].get<std::string>() !=
                hex_256(loaded.key_pair.public_key.bytes)) {
            set_error(error, "identity public_key does not match private_key");
            return false;
        }
    }

    if (doc.contains("id")) {
        if (!doc["id"].is_string() ||
            doc["id"].get<std::string>() != convid_to_hex(loaded.id)) {
            set_error(error, "identity id does not match public_key");
            return false;
        }
    }

    *out = loaded;
    return true;
}

bool save_node_identity_to_json(const std::string& path,
                                const NodeIdentity& identity,
                                std::string* error) {
    if (path.empty() || !identity.configured) {
        set_error(error, "identity is not configured");
        return false;
    }

    const std::filesystem::path target(path);
    const std::filesystem::path parent = target.parent_path();
    std::error_code ec;
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error != nullptr) {
                *error = "failed to create identity directory: " +
                         ec.message();
            }
            return false;
        }
    }

    nlohmann::json existing;
    {
        std::ifstream input(path);
        if (input) {
            try {
                input >> existing;
            } catch (...) {
                existing = nlohmann::json();
            }
        }
    }

    nlohmann::json doc;
    doc["version"] = 1;
    doc["algorithm"] = "ed25519";
    doc["private_key"] = hex_256(identity.key_pair.private_key.bytes);
    doc["public_key"] = hex_256(identity.key_pair.public_key.bytes);
    doc["id"] = convid_to_hex(identity.id);
    if (existing.is_object() &&
        existing.contains("bootstrap_nodes") &&
        existing["bootstrap_nodes"].is_array()) {
        doc["bootstrap_nodes"] = existing["bootstrap_nodes"];
    } else {
        doc["bootstrap_nodes"] = nlohmann::json::array();
    }

    const std::filesystem::path temp =
        target.string() + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream output(temp, std::ios::trunc);
        if (!output) {
            set_error(error, "identity temp file is not writable");
            return false;
        }
        output << doc.dump(2) << "\n";
        if (!output) {
            set_error(error, "failed to write identity json");
            return false;
        }
    }

    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        if (error != nullptr) {
            *error = "failed to install identity file: " + ec.message();
        }
        return false;
    }

    std::filesystem::permissions(
        target,
        std::filesystem::perms::owner_read |
            std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        ec
    );

    return true;
}

bool load_or_create_node_identity(const std::string& path,
                                  NodeIdentity* out,
                                  bool* created,
                                  std::string* error) {
    if (created != nullptr) {
        *created = false;
    }
    if (path.empty() || out == nullptr) {
        set_error(error, "invalid identity file path");
        return false;
    }

    const std::filesystem::path identity_path(path);
    if (file_exists(identity_path)) {
        return load_node_identity_from_json(path, out, error);
    }

    NodeKeyPair key_pair;
    if (!generate_node_key_pair(&key_pair, error)) {
        return false;
    }

    NodeIdentity generated;
    generated.key_pair = key_pair;
    generated.id = node_id_from_public_key(key_pair.public_key);
    generated.configured = true;

    if (!save_node_identity_to_json(path, generated, error)) {
        return false;
    }

    *out = generated;
    if (created != nullptr) {
        *created = true;
    }
    return true;
}

bool load_bootstrap_nodes_from_json(const std::string& path,
                                    std::vector<NodeAddress>* out,
                                    std::string* error) {
    if (out == nullptr) {
        set_error(error, "invalid bootstrap output");
        return false;
    }

    out->clear();

    std::ifstream input(path);
    if (!input) {
        return true;
    }

    nlohmann::json doc;
    try {
        input >> doc;
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = std::string("identity json parse failed: ") + ex.what();
        }
        return false;
    }

    if (!doc.is_object() || !doc.contains("bootstrap_nodes")) {
        return true;
    }
    if (!doc["bootstrap_nodes"].is_array()) {
        set_error(error, "bootstrap_nodes must be an array");
        return false;
    }

    for (const nlohmann::json& item : doc["bootstrap_nodes"]) {
        if (!item.is_object() ||
            !item.contains("host") ||
            !item.contains("port") ||
            !item["host"].is_string() ||
            !item["port"].is_number_unsigned()) {
            set_error(error, "bootstrap node schema is invalid");
            return false;
        }

        NodeAddress node;
        node.endpoint.host = item["host"].get<std::string>();
        const unsigned port = item["port"].get<unsigned>();
        if (port == 0 || port > 65535U) {
            set_error(error, "bootstrap node port is invalid");
            return false;
        }
        node.endpoint.port = static_cast<std::uint16_t>(port);

        std::string id;
        if (item.contains("id") && item["id"].is_string()) {
            id = item["id"].get<std::string>();
        } else if (item.contains("public_key") && item["public_key"].is_string()) {
            id = item["public_key"].get<std::string>();
        } else if (item.contains("convid") && item["convid"].is_string()) {
            id = item["convid"].get<std::string>();
        } else {
            set_error(error, "bootstrap node id is missing");
            return false;
        }

        if (!parse_convid_hex(id, &node.id)) {
            set_error(error, "bootstrap node id is invalid");
            return false;
        }

        out->push_back(node);
    }

    return true;
}

bool parse_node_address(const std::string& text, NodeAddress* out) noexcept {
    if (out == nullptr) {
        return false;
    }

    const std::size_t first = text.find(':');
    const std::size_t second =
        first == std::string::npos ? std::string::npos
                                   : text.find(':', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        text.find(':', second + 1) != std::string::npos) {
        return false;
    }

    const std::string host = text.substr(0, first);
    const std::string port_text = text.substr(first + 1, second - first - 1);
    const std::string id_text = text.substr(second + 1);
    if (host.empty() || port_text.empty() || id_text.empty()) {
        return false;
    }

    char* end = nullptr;
    const unsigned long port = std::strtoul(port_text.c_str(), &end, 10);
    if (end == port_text.c_str() || *end != '\0' || port == 0 ||
        port > 65535UL) {
        return false;
    }

    NodeAddress node;
    node.endpoint.host = host;
    node.endpoint.port = static_cast<std::uint16_t>(port);
    if (!parse_convid_hex(id_text, &node.id)) {
        return false;
    }

    *out = node;
    return true;
}

}  // namespace iota_proxy
