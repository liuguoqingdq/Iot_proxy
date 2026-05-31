#include "iota_proxy/ProxyRuntime.hpp"
#include "iota_proxy/business/NodeIdentity.hpp"
#include "iota_proxy/common/Endpoint.hpp"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace {

std::sig_atomic_t g_running = 1;

void handle_signal(int) {
    g_running = 0;
}

void print_usage(const char* program) {
    std::cerr
        << "usage: " << program << " "
        << "[options]\n"
        << "\n"
        << "options:\n"
        << "  --tcp-host <ip>          default 0.0.0.0\n"
        << "  --tcp-port <port>        default 7000\n"
        << "  --disable-tcp-ingress   disable device TCP ingress\n"
        << "  --edge-frame-max-payload <bytes> default 1048576\n"
        << "  --egress-host <ip>       upstream TCP target host\n"
        << "  --egress-port <port>     upstream TCP target port\n"
        << "  --egress-connect-timeout-ms <ms> default 3000\n"
        << "  --redis-host <ip>        Redis state/cache host, disabled by default\n"
        << "  --redis-port <port>      Redis port\n"
        << "  --redis-config <path>    edge pipeline config, default config/edge_pipeline.json\n"
        << "  --edge-config <path>     alias for --redis-config\n"
        << "  --redis-connect-timeout-ms <ms> default 1000\n"
        << "  --redis-io-timeout-ms <ms> default 1000\n"
        << "  --redis-state-ttl-seconds <s> default 60\n"
        << "  --redis-latest-ttl-seconds <s> default 86400\n"
        << "  --redis-seen-ttl-seconds <s> default 600\n"
        << "  --kafka-brokers <list>   Kafka bootstrap servers\n"
        << "  --kafka-topic <name>     Kafka topic for edge data\n"
        << "  --kafka-client-id <id>   default iota-proxy\n"
        << "  --kafka-acks <value>     default all\n"
        << "  --kcp-host <ip>          default 0.0.0.0\n"
        << "  --kcp-port <port>        default 9000\n"
        << "  --kcp-peer-host <ip>     optional static peer host\n"
        << "  --kcp-peer-port <port>   optional static peer port\n"
        << "  --peer-id-hex <hex>      peer 256-bit id, 64 hex chars\n"
        << "  --conv <id>              uint32 shortcut, default 1\n"
        << "  --conv-hex <hex>         64 hex chars, overrides --conv\n"
        << "  --private-key-hex <hex>  local Ed25519 private key\n"
        << "  --identity-file <path>   default $HOME/.iota_proxy/identity.json\n"
        << "  --bootstrap-node <addr>  host:port:public_key\n"
        << "  --route-table-file <p>   default next to identity file\n"
        << "  --max-kcp-links <count>  default 512\n"
        << "  --max-ingress-streams <count> default 4096, 0 unlimited\n"
        << "  --max-ingress-bytes-per-second <bytes> default 0 unlimited\n"
        << "  --broadcast-ttl <hops>   default 4\n"
        << "  --broadcast-fanout <n>   default 32\n"
        << "  --broadcast-seen-cache <n> default 8192\n"
        << "  --broadcast-seen-seconds <s> default 600\n"
        << "  --generate-identity      print a new private/public/id set\n"
        << "  --node-id <id>           default 1\n"
        << "  --workers <count>        default 1\n";
}

bool parse_u16(const std::string& text, std::uint16_t* out) {
    if (out == nullptr || text.empty()) {
        return false;
    }

    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value == 0 ||
        value > 65535UL) {
        return false;
    }

    *out = static_cast<std::uint16_t>(value);
    return true;
}

bool parse_u32(const std::string& text, std::uint32_t* out) {
    if (out == nullptr || text.empty()) {
        return false;
    }

    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value == 0 ||
        value > 0xffffffffUL) {
        return false;
    }

    *out = static_cast<std::uint32_t>(value);
    return true;
}

bool parse_u8(const std::string& text, std::uint8_t* out) {
    std::uint32_t value = 0;
    if (out == nullptr || !parse_u32(text, &value) || value > 255U) {
        return false;
    }
    *out = static_cast<std::uint8_t>(value);
    return true;
}

bool parse_size(const std::string& text, std::size_t* out) {
    if (out == nullptr || text.empty()) {
        return false;
    }

    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value == 0) {
        return false;
    }

    *out = static_cast<std::size_t>(value);
    return true;
}

bool parse_size_allow_zero(const std::string& text, std::size_t* out) {
    if (out == nullptr || text.empty()) {
        return false;
    }

    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }

    *out = static_cast<std::size_t>(value);
    return true;
}

bool parse_u64_allow_zero(const std::string& text, std::uint64_t* out) {
    if (out == nullptr || text.empty()) {
        return false;
    }

    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }

    *out = static_cast<std::uint64_t>(value);
    return true;
}

bool require_value(int argc, char** argv, int* index, std::string* value) {
    if (index == nullptr || value == nullptr || *index + 1 >= argc) {
        return false;
    }

    ++(*index);
    *value = argv[*index];
    return true;
}

bool configure_private_key(const std::string& value,
                           iota_proxy::NodeIdentity* identity) {
    if (identity == nullptr) {
        return false;
    }

    iota_proxy::NodePrivateKey private_key;
    if (!iota_proxy::parse_hex_256(value, &private_key.bytes)) {
        return false;
    }

    iota_proxy::NodePublicKey public_key;
    std::string error;
    if (!iota_proxy::derive_public_key(private_key, &public_key, &error)) {
        if (!error.empty()) {
            std::cerr << "identity error: " << error << "\n";
        }
        return false;
    }

    identity->key_pair.private_key = private_key;
    identity->key_pair.public_key = public_key;
    identity->id = iota_proxy::node_id_from_public_key(public_key);
    identity->configured = true;
    return true;
}

std::string default_route_table_file(const std::string& identity_file) {
    const std::filesystem::path identity_path(identity_file);
    const std::filesystem::path parent = identity_path.parent_path();
    if (!parent.empty()) {
        return (parent / "routes.json").string();
    }
    return "iota_routes.json";
}

std::string default_redis_config_file() {
#ifdef IOTA_PROXY_DEFAULT_CONFIG_DIR
    return (std::filesystem::path(IOTA_PROXY_DEFAULT_CONFIG_DIR) /
            "edge_pipeline.json").string();
#else
    return "config/edge_pipeline.json";
#endif
}

bool has_help_arg(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            return true;
        }
    }
    return false;
}

bool find_redis_config_arg(int argc,
                           char** argv,
                           std::string* redis_config_file,
                           bool* explicit_config) {
    if (redis_config_file == nullptr || explicit_config == nullptr) {
        return false;
    }

    *explicit_config = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--redis-config" || arg == "--edge-config") {
            if (i + 1 >= argc) {
                return false;
            }
            *redis_config_file = argv[++i];
            *explicit_config = true;
        }
    }
    return true;
}

const nlohmann::json* redis_config_value(const nlohmann::json& doc,
                                         const char* primary,
                                         const char* fallback) {
    auto it = doc.find(primary);
    if (it != doc.end()) {
        return &(*it);
    }
    if (fallback != nullptr) {
        it = doc.find(fallback);
        if (it != doc.end()) {
            return &(*it);
        }
    }
    return nullptr;
}

bool json_scalar_to_string(const nlohmann::json& value, std::string* out) {
    if (out == nullptr) {
        return false;
    }
    if (value.is_string()) {
        *out = value.get<std::string>();
        return true;
    }
    if (value.is_number_integer()) {
        *out = std::to_string(value.get<long long>());
        return true;
    }
    if (value.is_number_unsigned()) {
        *out = std::to_string(value.get<unsigned long long>());
        return true;
    }
    return false;
}

bool load_redis_config_json(const std::string& path,
                            iota_proxy::ProxyRuntimeConfig* config,
                            std::string* error) {
    if (config == nullptr || path.empty()) {
        return false;
    }

    try {
        std::ifstream input(path);
        if (!input.is_open()) {
            if (error != nullptr) {
                *error = "failed to open file";
            }
            return false;
        }

        nlohmann::json doc;
        input >> doc;
        if (!doc.is_object()) {
            if (error != nullptr) {
                *error = "redis config must be a JSON object";
            }
            return false;
        }

        const nlohmann::json* host =
            redis_config_value(doc, "redis-host", "host");
        if (host != nullptr) {
            if (!host->is_string()) {
                if (error != nullptr) {
                    *error = "redis-host must be a string";
                }
                return false;
            }
            config->edge_data.redis.endpoint.host = host->get<std::string>();
        }

        const nlohmann::json* port =
            redis_config_value(doc, "redis-port", "port");
        if (port != nullptr) {
            std::string port_text;
            std::uint16_t parsed_port = 0;
            if (!json_scalar_to_string(*port, &port_text) ||
                !parse_u16(port_text, &parsed_port)) {
                if (error != nullptr) {
                    *error = "redis-port must be 1..65535";
                }
                return false;
            }
            config->edge_data.redis.endpoint.port = parsed_port;
        }

        const auto read_u32_field =
            [&](const char* primary,
                const char* fallback,
                std::uint32_t* out) {
                const nlohmann::json* value =
                    redis_config_value(doc, primary, fallback);
                if (value == nullptr) {
                    return true;
                }
                std::string text;
                std::uint32_t parsed = 0;
                if (!json_scalar_to_string(*value, &text) ||
                    !parse_u32(text, &parsed)) {
                    if (error != nullptr) {
                        *error = std::string(primary) +
                                 " must be a positive uint32";
                    }
                    return false;
                }
                *out = parsed;
                return true;
            };

        if (!read_u32_field(
                "redis-connect-timeout-ms",
                "connect_timeout_ms",
                &config->edge_data.redis.connect_timeout_ms
            ) ||
            !read_u32_field(
                "redis-io-timeout-ms",
                "io_timeout_ms",
                &config->edge_data.redis.io_timeout_ms
            ) ||
            !read_u32_field(
                "redis-state-ttl-seconds",
                "device_state_ttl_seconds",
                &config->edge_data.redis.device_state_ttl_seconds
            ) ||
            !read_u32_field(
                "redis-latest-ttl-seconds",
                "device_latest_ttl_seconds",
                &config->edge_data.redis.device_latest_ttl_seconds
            ) ||
            !read_u32_field(
                "redis-seen-ttl-seconds",
                "seen_ttl_seconds",
                &config->edge_data.redis.seen_ttl_seconds
            ) ||
            !read_u32_field(
                "kafka-message-timeout-ms",
                "message_timeout_ms",
                &config->edge_data.kafka.message_timeout_ms
            ) ||
            !read_u32_field(
                "kafka-flush-timeout-ms",
                "flush_timeout_ms",
                &config->edge_data.kafka.flush_timeout_ms
            )) {
            return false;
        }

        const nlohmann::json* kafka_brokers =
            redis_config_value(doc, "kafka-brokers", "brokers");
        if (kafka_brokers != nullptr) {
            if (!kafka_brokers->is_string()) {
                if (error != nullptr) {
                    *error = "kafka-brokers must be a string";
                }
                return false;
            }
            config->edge_data.kafka.brokers =
                kafka_brokers->get<std::string>();
        }

        const nlohmann::json* kafka_topic =
            redis_config_value(doc, "kafka-topic", "topic");
        if (kafka_topic != nullptr) {
            if (!kafka_topic->is_string()) {
                if (error != nullptr) {
                    *error = "kafka-topic must be a string";
                }
                return false;
            }
            config->edge_data.kafka.topic =
                kafka_topic->get<std::string>();
        }

        const nlohmann::json* kafka_client_id =
            redis_config_value(doc, "kafka-client-id", "client_id");
        if (kafka_client_id != nullptr) {
            if (!kafka_client_id->is_string()) {
                if (error != nullptr) {
                    *error = "kafka-client-id must be a string";
                }
                return false;
            }
            config->edge_data.kafka.client_id =
                kafka_client_id->get<std::string>();
        }

        const nlohmann::json* kafka_acks =
            redis_config_value(doc, "kafka-acks", "acks");
        if (kafka_acks != nullptr) {
            if (!kafka_acks->is_string()) {
                if (error != nullptr) {
                    *error = "kafka-acks must be a string";
                }
                return false;
            }
            config->edge_data.kafka.acks =
                kafka_acks->get<std::string>();
        }

        return true;
    } catch (const std::exception& ex) {
        if (error != nullptr) {
            *error = ex.what();
        }
        return false;
    }
}

bool parse_args(int argc,
                char** argv,
                iota_proxy::ProxyRuntimeConfig* config,
                bool* help,
                bool* generate_identity,
                std::string* identity_file,
                bool* private_key_override,
                std::string* redis_config_file) {
    if (config == nullptr || help == nullptr || generate_identity == nullptr ||
        identity_file == nullptr || private_key_override == nullptr ||
        redis_config_file == nullptr) {
        return false;
    }

    *help = false;
    *generate_identity = false;
    *private_key_override = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;

        if (arg == "--help" || arg == "-h") {
            *help = true;
            return true;
        }

        if (arg == "--disable-tcp-ingress") {
            config->tcp.enabled = false;
            continue;
        }

        if (arg == "--generate-identity") {
            *generate_identity = true;
            continue;
        }

        if (arg == "--tcp-host") {
            if (!require_value(argc, argv, &i, &value)) {
                return false;
            }
            config->tcp.host = value;
        } else if (arg == "--tcp-port") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u16(value, &config->tcp.port)) {
                return false;
            }
        } else if (arg == "--edge-frame-max-payload") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_size(value, &config->tcp.max_edge_frame_payload_size)) {
                return false;
            }
        } else if (arg == "--egress-host") {
            if (!require_value(argc, argv, &i, &value)) {
                return false;
            }
            config->egress.target.host = value;
        } else if (arg == "--egress-port") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u16(value, &config->egress.target.port)) {
                return false;
            }
        } else if (arg == "--egress-connect-timeout-ms") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u32(value, &config->egress.connect_timeout_ms)) {
                return false;
            }
        } else if (arg == "--redis-host") {
            if (!require_value(argc, argv, &i, &value)) {
                return false;
            }
            config->edge_data.redis.endpoint.host = value;
        } else if (arg == "--redis-port") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u16(value, &config->edge_data.redis.endpoint.port)) {
                return false;
            }
        } else if (arg == "--redis-config" || arg == "--edge-config") {
            if (!require_value(argc, argv, &i, &value) || value.empty()) {
                return false;
            }
            *redis_config_file = value;
        } else if (arg == "--redis-connect-timeout-ms") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u32(
                    value,
                    &config->edge_data.redis.connect_timeout_ms
                )) {
                return false;
            }
        } else if (arg == "--redis-io-timeout-ms") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u32(value, &config->edge_data.redis.io_timeout_ms)) {
                return false;
            }
        } else if (arg == "--redis-state-ttl-seconds") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u32(
                    value,
                    &config->edge_data.redis.device_state_ttl_seconds
                )) {
                return false;
            }
        } else if (arg == "--redis-latest-ttl-seconds") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u32(
                    value,
                    &config->edge_data.redis.device_latest_ttl_seconds
                )) {
                return false;
            }
        } else if (arg == "--redis-seen-ttl-seconds") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u32(
                    value,
                    &config->edge_data.redis.seen_ttl_seconds
                )) {
                return false;
            }
        } else if (arg == "--kafka-brokers") {
            if (!require_value(argc, argv, &i, &value)) {
                return false;
            }
            config->edge_data.kafka.brokers = value;
        } else if (arg == "--kafka-topic") {
            if (!require_value(argc, argv, &i, &value)) {
                return false;
            }
            config->edge_data.kafka.topic = value;
        } else if (arg == "--kafka-client-id") {
            if (!require_value(argc, argv, &i, &value)) {
                return false;
            }
            config->edge_data.kafka.client_id = value;
        } else if (arg == "--kafka-acks") {
            if (!require_value(argc, argv, &i, &value)) {
                return false;
            }
            config->edge_data.kafka.acks = value;
        } else if (arg == "--kcp-host") {
            if (!require_value(argc, argv, &i, &value)) {
                return false;
            }
            config->kcp.local.host = value;
        } else if (arg == "--kcp-port") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u16(value, &config->kcp.local.port)) {
                return false;
            }
        } else if (arg == "--kcp-peer-host") {
            if (!require_value(argc, argv, &i, &value)) {
                return false;
            }
            config->kcp.peer.endpoint.host = value;
        } else if (arg == "--kcp-peer-port") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u16(value, &config->kcp.peer.endpoint.port)) {
                return false;
            }
        } else if (arg == "--peer-id-hex") {
            if (!require_value(argc, argv, &i, &value) ||
                !iota_proxy::parse_convid_hex(value, &config->kcp.peer.id)) {
                return false;
            }
        } else if (arg == "--conv") {
            std::uint32_t conv32 = 0;
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u32(value, &conv32)) {
                return false;
            }
            config->kcp.peer.id = myring::kcp::KcpConv::from_uint32(conv32);
        } else if (arg == "--conv-hex") {
            if (!require_value(argc, argv, &i, &value) ||
                !iota_proxy::parse_convid_hex(value, &config->kcp.peer.id)) {
                return false;
            }
        } else if (arg == "--private-key-hex") {
            if (!require_value(argc, argv, &i, &value) ||
                !configure_private_key(value, &config->kcp.identity)) {
                return false;
            }
            *private_key_override = true;
        } else if (arg == "--identity-file") {
            if (!require_value(argc, argv, &i, &value) || value.empty()) {
                return false;
            }
            *identity_file = value;
        } else if (arg == "--bootstrap-node") {
            iota_proxy::NodeAddress node;
            if (!require_value(argc, argv, &i, &value) ||
                !iota_proxy::parse_node_address(value, &node)) {
                return false;
            }
            config->discovery.bootstrap_nodes.push_back(node);
        } else if (arg == "--route-table-file") {
            if (!require_value(argc, argv, &i, &value)) {
                return false;
            }
            config->discovery.route_table_file = value;
        } else if (arg == "--max-kcp-links") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_size(value, &config->discovery.max_kcp_links)) {
                return false;
            }
        } else if (arg == "--max-ingress-streams") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_size_allow_zero(
                    value,
                    &config->admission.max_ingress_streams
                )) {
                return false;
            }
        } else if (arg == "--max-ingress-bytes-per-second") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u64_allow_zero(
                    value,
                    &config->admission.max_ingress_bytes_per_second
                )) {
                return false;
            }
        } else if (arg == "--broadcast-ttl") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u8(value, &config->kcp.broadcast_ttl)) {
                return false;
            }
        } else if (arg == "--broadcast-fanout") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_size(value, &config->kcp.max_broadcast_fanout)) {
                return false;
            }
        } else if (arg == "--broadcast-seen-cache") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_size(value, &config->kcp.max_seen_broadcasts)) {
                return false;
            }
        } else if (arg == "--broadcast-seen-seconds") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u32(value, &config->kcp.seen_broadcast_seconds)) {
                return false;
            }
        } else if (arg == "--node-id") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_u32(value, &config->tcp.node_id)) {
                return false;
            }
        } else if (arg == "--workers") {
            if (!require_value(argc, argv, &i, &value) ||
                !parse_size(value, &config->tcp.worker_threads)) {
                return false;
            }
        } else {
            return false;
        }
    }

    return true;
}

}  // namespace

int main(int argc, char** argv) {
    iota_proxy::ProxyRuntimeConfig config;
    bool help = false;
    bool generate_identity = false;
    bool private_key_override = false;
    bool identity_created = false;
    std::string identity_file = iota_proxy::default_identity_file_path();
    std::string redis_config_file = default_redis_config_file();
    std::vector<iota_proxy::NodeAddress> cli_bootstrap_nodes;

    if (has_help_arg(argc, argv)) {
        print_usage(argv[0]);
        return 0;
    }

    bool explicit_redis_config = false;
    if (!find_redis_config_arg(
            argc,
            argv,
            &redis_config_file,
            &explicit_redis_config)) {
        print_usage(argv[0]);
        return 2;
    }

    if (explicit_redis_config ||
        std::filesystem::exists(redis_config_file)) {
        std::string error;
        if (!load_redis_config_json(redis_config_file, &config, &error)) {
            std::cerr << "edge pipeline config error: " << redis_config_file;
            if (!error.empty()) {
                std::cerr << ": " << error;
            }
            std::cerr << "\n";
            return 1;
        }
    }

    if (!parse_args(argc,
                    argv,
                    &config,
                    &help,
                    &generate_identity,
                    &identity_file,
                    &private_key_override,
                    &redis_config_file)) {
        print_usage(argv[0]);
        return 2;
    }

    if (help) {
        print_usage(argv[0]);
        return 0;
    }

    if (generate_identity) {
        iota_proxy::NodeKeyPair key_pair;
        std::string error;
        if (!iota_proxy::generate_node_key_pair(&key_pair, &error)) {
            std::cerr << "identity error: " << error << "\n";
            return 1;
        }

        const myring::kcp::KcpConv id =
            iota_proxy::node_id_from_public_key(key_pair.public_key);
        std::cout << "private=" << iota_proxy::hex_256(
                         key_pair.private_key.bytes
                     ) << "\n"
                  << "public=" << iota_proxy::hex_256(
                         key_pair.public_key.bytes
                     ) << "\n"
                  << "id=" << iota_proxy::convid_to_hex(id) << "\n";
        return 0;
    }

    if (private_key_override) {
        std::string error;
        if (!iota_proxy::save_node_identity_to_json(
                identity_file,
                config.kcp.identity,
                &error)) {
            std::cerr << "identity error: " << error << "\n";
            return 1;
        }
    } else {
        std::string error;
        if (!iota_proxy::load_or_create_node_identity(
                identity_file,
                &config.kcp.identity,
                &identity_created,
                &error)) {
            std::cerr << "identity error: " << error << "\n";
            return 1;
        }
    }

    cli_bootstrap_nodes = config.discovery.bootstrap_nodes;
    std::vector<iota_proxy::NodeAddress> file_bootstrap_nodes;
    {
        std::string error;
        if (!iota_proxy::load_bootstrap_nodes_from_json(
                identity_file,
                &file_bootstrap_nodes,
                &error)) {
            std::cerr << "identity error: " << error << "\n";
            return 1;
        }
    }
    config.discovery.bootstrap_nodes = file_bootstrap_nodes;
    config.discovery.bootstrap_nodes.insert(
        config.discovery.bootstrap_nodes.end(),
        cli_bootstrap_nodes.begin(),
        cli_bootstrap_nodes.end()
    );

    config.discovery.identity = config.kcp.identity;
    config.discovery.local_endpoint = config.kcp.local;
    if (config.discovery.route_table_file.empty()) {
        config.discovery.route_table_file =
            default_route_table_file(identity_file);
    }
    config.kcp.max_peer_links = config.discovery.max_kcp_links;
    config.admission.max_kcp_links = config.discovery.max_kcp_links;

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        iota_proxy::ProxyRuntime runtime(config);
        if (!runtime.start()) {
            std::cerr << "failed to start proxy runtime\n";
            return 1;
        }

        std::cout << "iota_proxy tcp="
                  << (config.tcp.enabled
                          ? iota_proxy::endpoint_to_string(
                                {config.tcp.host, config.tcp.port}
                            )
                          : std::string("disabled"))
                  << " kcp="
                  << iota_proxy::endpoint_to_string(config.kcp.local)
                  << " egress="
                  << iota_proxy::endpoint_to_string(config.egress.target)
                  << " redis="
                  << iota_proxy::endpoint_to_string(
                         config.edge_data.redis.endpoint
                     )
                  << " edge_config=" << redis_config_file
                  << " kafka_brokers=" << config.edge_data.kafka.brokers
                  << " kafka_topic=" << config.edge_data.kafka.topic
                  << " peer="
                  << iota_proxy::endpoint_to_string(
                         config.kcp.peer.endpoint
                     )
                  << " peer_id="
                  << iota_proxy::convid_to_hex(config.kcp.peer.id)
                  << " local_id="
                  << iota_proxy::convid_to_hex(config.kcp.identity.id)
                  << " identity_file=" << identity_file
                  << " route_table_file="
                  << config.discovery.route_table_file
                  << " identity_created="
                  << (identity_created ? "true" : "false")
                  << " bootstrap_nodes="
                  << config.discovery.bootstrap_nodes.size()
                  << " max_ingress_streams="
                  << config.admission.max_ingress_streams
                  << " max_ingress_bytes_per_second="
                  << config.admission.max_ingress_bytes_per_second
                  << " node=" << config.tcp.node_id << "\n";

        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        runtime.stop();
    } catch (const std::exception& ex) {
        std::cerr << "fatal: " << ex.what() << "\n";
        return 1;
    }

    return 0;
}
