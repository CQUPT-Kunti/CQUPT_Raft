#include "cluster/cluster_config.h"
#include "cluster/node_identity.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/node/storage_node_registry.h"
#include "store/node/storage_node_service.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace
{
    enum class ExitCode : int
    {
        kOk = 0,
        kInvalidArgument = 2,
        kConfigError = 3,
        kIdentityError = 4,
        kUnsupported = 5,
        kStartupError = 6,
        kInternalError = 10,
    };

    struct ParsedArgs
    {
        std::filesystem::path config_path;
        std::optional<std::string> node_id;
        std::optional<std::filesystem::path> data_dir_override;
        std::optional<std::string> listen_override;
        bool show_help{false};
    };

    struct StorageNodeStartupConfig
    {
        std::string cluster_id;
        std::string node_id;
        std::string listen_endpoint;
        std::filesystem::path data_dir;
        std::uint64_t capacity_bytes{0};
        clusterdemo::FailureDomainConfig failure_domain;
        clusterdemo::NodeIdentitySource identity_source{
            clusterdemo::NodeIdentitySource::kConfigGenerator};
        storedemo::StorageNodeRegistryConfig registry_config;
        int grpc_message_limit_bytes{4 * 1024 * 1024};
        std::size_t selected_storage_index{0};
    };

    struct IdentityStartupState
    {
        clusterdemo::NodeIdentity identity;
        std::filesystem::path identity_path;
        bool loaded_existing{false};
        bool created_new{false};
        bool durable{false};
    };

    class IdentityStartupError final : public std::runtime_error
    {
    public:
        IdentityStartupError(clusterdemo::NodeIdentityStatusCode status,
                             std::string message)
            : std::runtime_error(std::move(message)),
              status_(status)
        {
        }

        [[nodiscard]] clusterdemo::NodeIdentityStatusCode status() const
        {
            return status_;
        }

    private:
        clusterdemo::NodeIdentityStatusCode status_;
    };

    std::atomic<bool> g_stop_requested{false};

    void HandleSignal(int)
    {
        g_stop_requested.store(true);
    }

    [[nodiscard]] std::uint64_t NowUnixMs()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    [[nodiscard]] bool IsValidEndpoint(const std::string_view endpoint)
    {
        if (endpoint.empty())
        {
            return false;
        }

        const auto separator = endpoint.rfind(':');
        if (separator == std::string_view::npos || separator == 0 ||
            separator + 1 >= endpoint.size())
        {
            return false;
        }

        const std::string_view port_text = endpoint.substr(separator + 1);
        for (const char ch : port_text)
        {
            if (ch < '0' || ch > '9')
            {
                return false;
            }
        }

        try
        {
            const unsigned long port = std::stoul(std::string(port_text));
            return port > 0 &&
                   port <= static_cast<unsigned long>(
                               std::numeric_limits<std::uint16_t>::max());
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    [[nodiscard]] std::string NormalizePathKey(
        const std::filesystem::path &path)
    {
        return path.lexically_normal().generic_string();
    }

    [[nodiscard]] int ComputeGrpcMessageLimitBytes(
        const std::uint64_t chunk_size_bytes)
    {
        constexpr std::uint64_t kGrpcEnvelopeHeadroomBytes = 1024ULL * 1024ULL;
        constexpr std::uint64_t kGrpcMinimumMessageBytes = 4ULL * 1024ULL * 1024ULL;

        if (chunk_size_bytes == 0)
        {
            throw std::runtime_error(
                "chunk_policy.chunk_size_bytes must be > 0 for storage_node_app startup");
        }

        std::uint64_t message_limit = chunk_size_bytes + kGrpcEnvelopeHeadroomBytes;
        if (message_limit < kGrpcMinimumMessageBytes)
        {
            message_limit = kGrpcMinimumMessageBytes;
        }
        if (message_limit >
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        {
            throw std::runtime_error(
                "chunk_policy.chunk_size_bytes is too large for gRPC message limit");
        }
        return static_cast<int>(message_limit);
    }

    [[nodiscard]] storedemo::StorageNodeRegistryConfig BuildRegistryConfig(
        const clusterdemo::ClusterTimeoutConfig &timeouts)
    {
        storedemo::StorageNodeRegistryConfig config;
        if (timeouts.liveness_stale_timeout >
            std::chrono::milliseconds::zero())
        {
            config.stale_timeout_ms = static_cast<std::uint64_t>(
                timeouts.liveness_stale_timeout.count());
        }
        if (timeouts.liveness_dead_timeout > std::chrono::milliseconds::zero())
        {
            config.dead_timeout_ms = static_cast<std::uint64_t>(
                timeouts.liveness_dead_timeout.count());
        }
        config.enforce_unique_endpoints = true;
        return config;
    }

    void PrintUsage(std::ostream &out)
    {
        out << "Usage: storage_node_app --config <path> [--node_id <id>] "
               "[--data_dir <path>] [--listen <host:port>]\n"
            << "  --config   Unified cluster config json path\n"
            << "  --node_id  Controlled override to select the StorageNode entry\n"
            << "  --data_dir Safe local testing override for node.identity and chunk data\n"
            << "  --listen   Safe local testing override for startup endpoint\n";
    }

    [[nodiscard]] ParsedArgs ParseArgs(int argc, char **argv)
    {
        ParsedArgs args;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view flag = argv[index];
            auto require_value = [&](const std::string_view name) -> std::string {
                if (index + 1 >= argc)
                {
                    throw std::runtime_error(std::string(name) + " requires a value");
                }
                ++index;
                return argv[index];
            };

            if (flag == "--help" || flag == "-h")
            {
                args.show_help = true;
            }
            else if (flag == "--config")
            {
                args.config_path = require_value("--config");
            }
            else if (flag == "--node_id")
            {
                args.node_id = require_value("--node_id");
            }
            else if (flag == "--data_dir")
            {
                args.data_dir_override =
                    std::filesystem::path(require_value("--data_dir"));
            }
            else if (flag == "--listen")
            {
                args.listen_override = require_value("--listen");
            }
            else
            {
                throw std::runtime_error("unknown argument: " + std::string(flag));
            }
        }

        if (!args.show_help && args.config_path.empty())
        {
            throw std::runtime_error("--config is required");
        }
        if (args.listen_override.has_value() &&
            !IsValidEndpoint(*args.listen_override))
        {
            throw std::runtime_error("--listen must use host:port with port in 1..65535");
        }
        return args;
    }

    [[nodiscard]] std::optional<std::size_t> FindSingleUnnamedStorageNodeIndex(
        const clusterdemo::ClusterConfig &config)
    {
        std::optional<std::size_t> selected_index;
        for (std::size_t index = 0; index < config.storage_nodes.size(); ++index)
        {
            if (config.storage_nodes[index].node_id.has_value())
            {
                continue;
            }
            if (selected_index.has_value())
            {
                return std::nullopt;
            }
            selected_index = index;
        }
        return selected_index;
    }

    void ValidateLocalOverrideSafety(
        const clusterdemo::ClusterConfig &config,
        const StorageNodeStartupConfig &startup)
    {
        if (startup.data_dir.empty())
        {
            throw std::runtime_error("storage node data_dir must not be empty");
        }
        if (!IsValidEndpoint(startup.listen_endpoint))
        {
            throw std::runtime_error("storage node endpoint is invalid: " +
                                     startup.listen_endpoint);
        }
        if (startup.capacity_bytes == 0)
        {
            throw std::runtime_error("storage node capacity_bytes must be > 0");
        }

        const std::string startup_data_dir = NormalizePathKey(startup.data_dir);

        for (const auto &node : config.view_nodes)
        {
            if (node.endpoint == startup.listen_endpoint)
            {
                throw std::runtime_error(
                    "storage node endpoint conflicts with ViewNode endpoint: " +
                    startup.listen_endpoint);
            }
            if (NormalizePathKey(node.data_dir) == startup_data_dir)
            {
                throw std::runtime_error(
                    "storage node data_dir conflicts with ViewNode data_dir: " +
                    startup.data_dir.generic_string());
            }
        }

        for (const auto &node : config.metadata_nodes)
        {
            if (node.endpoint == startup.listen_endpoint)
            {
                throw std::runtime_error(
                    "storage node endpoint conflicts with MetadataNode endpoint: " +
                    startup.listen_endpoint);
            }
            if (NormalizePathKey(node.data_dir) == startup_data_dir)
            {
                throw std::runtime_error(
                    "storage node data_dir conflicts with MetadataNode data_dir: " +
                    startup.data_dir.generic_string());
            }
            if (NormalizePathKey(node.snapshot_dir) == startup_data_dir)
            {
                throw std::runtime_error(
                    "storage node data_dir conflicts with MetadataNode snapshot_dir: " +
                    startup.data_dir.generic_string());
            }
        }

        for (std::size_t index = 0; index < config.storage_nodes.size(); ++index)
        {
            if (index == startup.selected_storage_index)
            {
                continue;
            }

            const auto &node = config.storage_nodes[index];
            if (node.endpoint == startup.listen_endpoint)
            {
                throw std::runtime_error(
                    "storage node endpoint conflicts with another StorageNode endpoint: " +
                    startup.listen_endpoint);
            }
            if (NormalizePathKey(node.data_dir) == startup_data_dir)
            {
                throw std::runtime_error(
                    "storage node data_dir conflicts with another StorageNode data_dir: " +
                    startup.data_dir.generic_string());
            }
        }
    }

    [[nodiscard]] StorageNodeStartupConfig ResolveStartupConfig(
        const clusterdemo::ClusterConfig &config,
        const ParsedArgs &args)
    {
        StorageNodeStartupConfig startup;
        startup.cluster_id = config.cluster_id;
        startup.registry_config = BuildRegistryConfig(config.timeouts);
        startup.grpc_message_limit_bytes =
            ComputeGrpcMessageLimitBytes(config.chunk_policy.chunk_size_bytes);

        if (args.node_id.has_value())
        {
            const auto resolved = clusterdemo::ResolveClusterNodeConfig(
                config,
                clusterdemo::ClusterNodeType::kStorage,
                *args.node_id);
            if (resolved.ok())
            {
                startup.node_id = resolved.resolved->node_id;
                startup.listen_endpoint = resolved.resolved->endpoint;
                startup.data_dir = resolved.resolved->data_dir;
                startup.capacity_bytes =
                    resolved.resolved->capacity_bytes.value_or(0);
                startup.failure_domain = resolved.resolved->failure_domain;
                startup.identity_source =
                    clusterdemo::NodeIdentitySource::kConfigGenerator;

                for (std::size_t index = 0; index < config.storage_nodes.size(); ++index)
                {
                    const auto &node = config.storage_nodes[index];
                    if (node.node_id.has_value() && *node.node_id == startup.node_id)
                    {
                        startup.selected_storage_index = index;
                        break;
                    }
                }
            }
            else
            {
                const auto unnamed_index =
                    FindSingleUnnamedStorageNodeIndex(config);
                if (!unnamed_index.has_value())
                {
                    throw std::runtime_error(
                        "failed to resolve storage node by --node_id: " +
                        resolved.error_detail);
                }

                const auto &node = config.storage_nodes[*unnamed_index];
                // 只允许把显式 --node_id 绑定到单个未命名 StorageNode，
                // 避免把多条未命名配置或其它 role 静默当成当前节点。
                startup.node_id = *args.node_id;
                startup.listen_endpoint = node.endpoint;
                startup.data_dir = node.data_dir;
                startup.capacity_bytes = node.capacity_bytes;
                startup.failure_domain = node.failure_domain;
                startup.identity_source =
                    clusterdemo::NodeIdentitySource::kExplicitOverride;
                startup.selected_storage_index = *unnamed_index;
            }
        }
        else
        {
            if (config.storage_nodes.size() != 1 ||
                !config.storage_nodes.front().node_id.has_value())
            {
                throw std::runtime_error(
                    "--node_id is required when cluster config does not contain exactly one named StorageNode");
            }

            const auto resolved = clusterdemo::ResolveClusterNodeConfig(
                config,
                clusterdemo::ClusterNodeType::kStorage,
                *config.storage_nodes.front().node_id);
            if (!resolved.ok())
            {
                throw std::runtime_error(
                    "failed to resolve the only StorageNode entry: " +
                    resolved.error_detail);
            }

            startup.node_id = resolved.resolved->node_id;
            startup.listen_endpoint = resolved.resolved->endpoint;
            startup.data_dir = resolved.resolved->data_dir;
            startup.capacity_bytes =
                resolved.resolved->capacity_bytes.value_or(0);
            startup.failure_domain = resolved.resolved->failure_domain;
            startup.identity_source =
                clusterdemo::NodeIdentitySource::kConfigGenerator;
            startup.selected_storage_index = 0;
        }

        if (args.data_dir_override.has_value())
        {
            startup.data_dir = *args.data_dir_override;
        }
        if (args.listen_override.has_value())
        {
            startup.listen_endpoint = *args.listen_override;
        }

        ValidateLocalOverrideSafety(config, startup);
        return startup;
    }

    [[nodiscard]] ExitCode MapIdentityExitCode(
        const clusterdemo::NodeIdentityStatusCode status)
    {
        switch (status)
        {
        case clusterdemo::NodeIdentityStatusCode::kUnsupported:
        case clusterdemo::NodeIdentityStatusCode::kDurabilityError:
            return ExitCode::kUnsupported;
        case clusterdemo::NodeIdentityStatusCode::kConflict:
        case clusterdemo::NodeIdentityStatusCode::kCorrupt:
        case clusterdemo::NodeIdentityStatusCode::kIoError:
        case clusterdemo::NodeIdentityStatusCode::kInvalidArgument:
            return ExitCode::kIdentityError;
        case clusterdemo::NodeIdentityStatusCode::kInternalError:
            return ExitCode::kInternalError;
        case clusterdemo::NodeIdentityStatusCode::kOk:
        case clusterdemo::NodeIdentityStatusCode::kNotFound:
        default:
            return ExitCode::kIdentityError;
        }
    }

    [[nodiscard]] ExitCode MapStoreExitCode(
        const storedemo::StorageNodeStatusCode status)
    {
        switch (status)
        {
        case storedemo::StorageNodeStatusCode::kUnsupported:
            return ExitCode::kUnsupported;
        case storedemo::StorageNodeStatusCode::kInvalidArgument:
        case storedemo::StorageNodeStatusCode::kConflict:
        case storedemo::StorageNodeStatusCode::kDiskFull:
        case storedemo::StorageNodeStatusCode::kPermissionDenied:
        case storedemo::StorageNodeStatusCode::kIoError:
        case storedemo::StorageNodeStatusCode::kNodeUnavailable:
            return ExitCode::kStartupError;
        case storedemo::StorageNodeStatusCode::kOk:
        case storedemo::StorageNodeStatusCode::kAlreadyExists:
        case storedemo::StorageNodeStatusCode::kNotFound:
        case storedemo::StorageNodeStatusCode::kChecksumMismatch:
        case storedemo::StorageNodeStatusCode::kCorrupted:
        case storedemo::StorageNodeStatusCode::kTimeout:
        case storedemo::StorageNodeStatusCode::kCancelled:
        case storedemo::StorageNodeStatusCode::kOverloaded:
        default:
            return ExitCode::kStartupError;
        }
    }

    [[nodiscard]] IdentityStartupState EnsureNodeIdentity(
        const StorageNodeStartupConfig &startup)
    {
        const clusterdemo::NodeIdentity identity_to_create{
            .cluster_id = startup.cluster_id,
            .node_id = startup.node_id,
            .node_type = clusterdemo::ClusterNodeType::kStorage,
            .raft_id = std::nullopt,
            .identity_version = clusterdemo::kNodeIdentityCurrentVersion,
            .created_at_unix_ms = static_cast<std::int64_t>(NowUnixMs()),
            .source = startup.identity_source,
        };

        const clusterdemo::ExpectedNodeIdentity expected{
            .cluster_id = startup.cluster_id,
            .node_id = startup.node_id,
            .node_type = clusterdemo::ClusterNodeType::kStorage,
            .raft_id = std::nullopt,
            .source = startup.identity_source,
            .require_raft_id_for_metadata = true,
            .forbid_raft_id_for_non_metadata = true,
        };

        const auto load_or_create = clusterdemo::LoadOrCreateNodeIdentity(
            clusterdemo::NodeIdentityLoadOrCreateRequest{
                .load_options = clusterdemo::NodeIdentityLoadOptions{
                    .data_dir = startup.data_dir,
                    .expected = expected,
                    .require_existing = false,
                },
                .identity_to_create = identity_to_create,
                .store_options = clusterdemo::NodeIdentityStoreOptions{
                    .data_dir = startup.data_dir,
                    .durability_mode = clusterdemo::NodeIdentityDurabilityMode::kRequired,
                    .store_mode = clusterdemo::NodeIdentityStoreMode::kCreateNewOnly,
                    .expected_existing = expected,
                },
            });

        if (!load_or_create.ok())
        {
            throw IdentityStartupError(
                load_or_create.status,
                "node.identity startup check failed: " + load_or_create.diagnostic);
        }

        return IdentityStartupState{
            .identity = *load_or_create.identity,
            .identity_path = load_or_create.identity_path,
            .loaded_existing = load_or_create.loaded_existing,
            .created_new = load_or_create.created_new,
            .durable = load_or_create.durable,
        };
    }

    [[nodiscard]] storedemo::StorageNodeRegistryFacts BuildRegistryFacts(
        const StorageNodeStartupConfig &startup)
    {
        storedemo::StorageNodeRegistryFacts facts;
        facts.capacity.total_capacity_bytes = startup.capacity_bytes;
        facts.capacity.available_capacity_bytes = startup.capacity_bytes;
        facts.capacity.used_capacity_bytes = 0;
        facts.capacity.chunk_count = 0;
        facts.health.health = storedemo::StorageNodeHealth::kHealthy;
        facts.health.disk_pressure = storedemo::StorageNodeDiskPressure::kLow;
        facts.health.io_error_count = 0;
        facts.failure_domain.zone = startup.failure_domain.zone;
        facts.failure_domain.rack = startup.failure_domain.rack;
        return facts;
    }

    [[nodiscard]] int Run(const ParsedArgs &args)
    {
        const auto loaded_config =
            clusterdemo::LoadClusterConfigFromJsonFile(args.config_path);
        if (!loaded_config.ok())
        {
            std::cerr << "storage_node_app config error: "
                      << loaded_config.error_detail << '\n';
            return static_cast<int>(ExitCode::kConfigError);
        }

        StorageNodeStartupConfig startup;
        try
        {
            startup = ResolveStartupConfig(*loaded_config.config, args);
        }
        catch (const std::exception &ex)
        {
            std::cerr << "storage_node_app config error: " << ex.what() << '\n';
            return static_cast<int>(ExitCode::kConfigError);
        }

        IdentityStartupState identity_state;
        try
        {
            identity_state = EnsureNodeIdentity(startup);
        }
        catch (const IdentityStartupError &ex)
        {
            std::cerr << ex.what() << '\n';
            return static_cast<int>(MapIdentityExitCode(ex.status()));
        }

        auto chunk_store = std::make_shared<storedemo::LocalDiskChunkStore>(
            storedemo::LocalDiskChunkStoreConfig{
                .data_dir = startup.data_dir,
                .node_id = startup.node_id,
            });
        const auto init_result = chunk_store->Initialize();
        if (!init_result.ok())
        {
            std::cerr << "storage_node_app startup error: chunk store init failed"
                      << " status=" << storedemo::ToString(init_result.status)
                      << " message=" << init_result.error_detail << '\n';
            return static_cast<int>(MapStoreExitCode(init_result.status));
        }

        auto registry = std::make_shared<storedemo::StorageNodeRegistry>(
            startup.registry_config);
        const auto local_facts = BuildRegistryFacts(startup);
        const auto registration_result = registry->RegisterStorageNode(
            storedemo::RegisterStorageNodeRequest{
                .node_id = startup.node_id,
                .endpoint = startup.listen_endpoint,
                .observed_at_unix_ms = NowUnixMs(),
                .facts = local_facts,
            });
        if (!registration_result.ok())
        {
            std::cerr << "storage_node_app startup error: local registry seed failed"
                      << " status=" << storedemo::ToString(registration_result.status)
                      << " message=" << registration_result.error_detail << '\n';
            return static_cast<int>(ExitCode::kStartupError);
        }

        auto service = std::make_shared<storedemo::StorageNodeService>(
            chunk_store,
            startup.node_id,
            registry);

        grpc::ServerBuilder builder;
        builder.SetMaxReceiveMessageSize(startup.grpc_message_limit_bytes);
        builder.SetMaxSendMessageSize(startup.grpc_message_limit_bytes);

        int selected_port = 0;
        builder.AddListeningPort(startup.listen_endpoint,
                                 grpc::InsecureServerCredentials(),
                                 &selected_port);
        builder.RegisterService(service.get());

        // T047 只装配 StorageNode data-plane 已有边界：chunk store、service
        // 和 gRPC 生命周期；不在 app 中实现 placement、heartbeat loop
        // 或 metadata control-plane 逻辑。
        std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
        if (server == nullptr || selected_port <= 0)
        {
            std::cerr << "storage_node_app startup error: failed to bind endpoint "
                      << startup.listen_endpoint << '\n';
            return static_cast<int>(ExitCode::kStartupError);
        }

        std::cout << "storage_node_app OK"
                  << " cluster_id=" << startup.cluster_id
                  << " node_id=" << identity_state.identity.node_id
                  << " endpoint=" << startup.listen_endpoint
                  << " data_dir=" << startup.data_dir.generic_string()
                  << " capacity_bytes=" << startup.capacity_bytes
                  << " failure_domain.zone=" << startup.failure_domain.zone
                  << " failure_domain.rack=" << startup.failure_domain.rack
                  << " identity_path="
                  << identity_state.identity_path.generic_string()
                  << " identity_source="
                  << clusterdemo::ToString(identity_state.identity.source)
                  << " identity_loaded_existing="
                  << (identity_state.loaded_existing ? "true" : "false")
                  << " identity_created_new="
                  << (identity_state.created_new ? "true" : "false")
                  << " chunk_root="
                  << init_result.paths.data_root.generic_string()
                  << '\n';

        std::signal(SIGINT, HandleSignal);
#ifdef SIGTERM
        std::signal(SIGTERM, HandleSignal);
#endif

        std::thread wait_thread([&server]() {
            server->Wait();
        });

        while (!g_stop_requested.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        server->Shutdown();
        wait_thread.join();
        return static_cast<int>(ExitCode::kOk);
    }
} // namespace

int main(int argc, char **argv)
{
    try
    {
        const ParsedArgs args = ParseArgs(argc, argv);
        if (args.show_help)
        {
            PrintUsage(std::cout);
            return static_cast<int>(ExitCode::kOk);
        }
        return Run(args);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "storage_node_app argument error: " << ex.what() << '\n';
        PrintUsage(std::cerr);
        return static_cast<int>(ExitCode::kInvalidArgument);
    }
}
