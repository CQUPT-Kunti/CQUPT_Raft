#include "cluster/cluster_config.h"
#include "cluster/node_identity.h"
#include "view/view_registry.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

    struct ViewNodeStartupConfig
    {
        std::string cluster_id;
        std::string node_id;
        std::string listen_endpoint;
        std::filesystem::path data_dir;
        clusterdemo::NodeIdentitySource identity_source{
            clusterdemo::NodeIdentitySource::kConfigGenerator};
        viewdemo::ViewRegistryConfig registry_config;
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

    std::uint64_t NowUnixMs()
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
            return port > 0 && port <= 65535;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    [[nodiscard]] viewdemo::ViewRegistryConfig BuildRegistryConfig(
        const clusterdemo::ClusterTimeoutConfig &timeouts)
    {
        viewdemo::ViewRegistryConfig config;
        config.stale_timeout = timeouts.liveness_stale_timeout;
        config.dead_timeout = timeouts.liveness_dead_timeout;

        // startup 只做装配：suspect 超时从 cluster config 的 stale/dead 边界中间值推导，
        // 不把这段逻辑扩展成新的 discovery 业务语义。
        if (timeouts.liveness_dead_timeout > timeouts.liveness_stale_timeout)
        {
            const auto delta =
                (timeouts.liveness_dead_timeout - timeouts.liveness_stale_timeout) / 2;
            config.suspect_timeout = timeouts.liveness_stale_timeout + delta;
        }
        else
        {
            config.suspect_timeout = timeouts.liveness_stale_timeout;
        }
        return config;
    }

    void PrintUsage(std::ostream &out)
    {
        out << "Usage: view_node_app --config <path> [--node_id <id>] "
               "[--data_dir <path>] [--listen <host:port>]\n"
            << "  --config   Unified cluster config json path\n"
            << "  --node_id  Controlled override to select the ViewNode entry\n"
            << "  --data_dir Safe local testing override for node.identity storage\n"
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
                args.data_dir_override = std::filesystem::path(require_value("--data_dir"));
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

    [[nodiscard]] std::optional<viewdemo::NodeRegistration> MakeSelfRegistration(
        const ViewNodeStartupConfig &startup)
    {
        viewdemo::NodeRegistration registration;
        registration.cluster_id = startup.cluster_id;
        registration.node_id = startup.node_id;
        registration.node_type = viewdemo::ViewNodeType::kView;
        registration.endpoint = startup.listen_endpoint;
        registration.control_plane_endpoint = startup.listen_endpoint;
        registration.data_plane_endpoint = "";
        registration.data_dir_fingerprint = startup.data_dir.lexically_normal().generic_string();
        registration.observed_at_unix_ms = NowUnixMs();
        registration.health.health = viewdemo::ViewNodeHealth::kHealthy;
        registration.health.disk_pressure = viewdemo::ViewNodeDiskPressure::kLow;
        return registration;
    }

    [[nodiscard]] std::optional<clusterdemo::ViewNodeConfig> FindSingleUnnamedViewNode(
        const clusterdemo::ClusterConfig &config)
    {
        std::optional<clusterdemo::ViewNodeConfig> selected;
        for (const auto &node : config.view_nodes)
        {
            if (node.node_id.has_value())
            {
                continue;
            }
            if (selected.has_value())
            {
                return std::nullopt;
            }
            selected = node;
        }
        return selected;
    }

    [[nodiscard]] ViewNodeStartupConfig ResolveStartupConfig(
        const clusterdemo::ClusterConfig &config,
        const ParsedArgs &args)
    {
        ViewNodeStartupConfig startup;
        startup.cluster_id = config.cluster_id;
        startup.registry_config = BuildRegistryConfig(config.timeouts);

        if (args.node_id.has_value())
        {
            const auto resolved = clusterdemo::ResolveClusterNodeConfig(
                config,
                clusterdemo::ClusterNodeType::kView,
                *args.node_id);
            if (resolved.ok())
            {
                startup.node_id = resolved.resolved->node_id;
                startup.listen_endpoint = resolved.resolved->endpoint;
                startup.data_dir = resolved.resolved->data_dir;
                startup.identity_source = clusterdemo::NodeIdentitySource::kConfigGenerator;
            }
            else
            {
                const auto unnamed_view = FindSingleUnnamedViewNode(config);
                if (!unnamed_view.has_value())
                {
                    throw std::runtime_error(
                        "failed to resolve view node by --node_id: " +
                        resolved.error_detail);
                }

                // 显式 --node_id 可以把单个未命名 ViewNode 绑定为受控本地启动实例，
                // 但不会静默覆盖多个未命名条目或其它 role 的节点。
                startup.node_id = *args.node_id;
                startup.listen_endpoint = unnamed_view->endpoint;
                startup.data_dir = unnamed_view->data_dir;
                startup.identity_source = clusterdemo::NodeIdentitySource::kExplicitOverride;
            }
        }
        else
        {
            if (config.view_nodes.size() != 1 || !config.view_nodes.front().node_id.has_value())
            {
                throw std::runtime_error(
                    "--node_id is required when cluster config does not contain exactly one named ViewNode");
            }

            const auto resolved = clusterdemo::ResolveClusterNodeConfig(
                config,
                clusterdemo::ClusterNodeType::kView,
                *config.view_nodes.front().node_id);
            if (!resolved.ok())
            {
                throw std::runtime_error(
                    "failed to resolve the only ViewNode entry: " +
                    resolved.error_detail);
            }

            startup.node_id = resolved.resolved->node_id;
            startup.listen_endpoint = resolved.resolved->endpoint;
            startup.data_dir = resolved.resolved->data_dir;
            startup.identity_source = clusterdemo::NodeIdentitySource::kConfigGenerator;
        }

        if (args.data_dir_override.has_value())
        {
            startup.data_dir = *args.data_dir_override;
        }
        if (args.listen_override.has_value())
        {
            startup.listen_endpoint = *args.listen_override;
        }
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

    [[nodiscard]] IdentityStartupState EnsureNodeIdentity(
        const ViewNodeStartupConfig &startup)
    {
        const clusterdemo::NodeIdentity identity_to_create{
            .cluster_id = startup.cluster_id,
            .node_id = startup.node_id,
            .node_type = clusterdemo::ClusterNodeType::kView,
            .raft_id = std::nullopt,
            .identity_version = clusterdemo::kNodeIdentityCurrentVersion,
            .created_at_unix_ms = static_cast<std::int64_t>(NowUnixMs()),
            .source = startup.identity_source,
        };

        const clusterdemo::ExpectedNodeIdentity expected{
            .cluster_id = startup.cluster_id,
            .node_id = startup.node_id,
            .node_type = clusterdemo::ClusterNodeType::kView,
            .raft_id = std::nullopt,
            .source = startup.identity_source,
            .require_raft_id_for_metadata = true,
            .forbid_raft_id_for_non_metadata = true,
        };

        // ViewNode startup 只负责受控地 load/create durable identity。
        // 已有 identity 不匹配时必须失败，不能在 app 层静默覆盖。
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
                "node.identity startup check failed for data_dir=" +
                    startup.data_dir.lexically_normal().generic_string() +
                    ": " + load_or_create.diagnostic);
        }
        return IdentityStartupState{
            .identity = *load_or_create.identity,
            .identity_path = load_or_create.identity_path,
            .loaded_existing = load_or_create.loaded_existing,
            .created_new = load_or_create.created_new,
            .durable = load_or_create.durable,
        };
    }

    [[nodiscard]] int Run(const ParsedArgs &args)
    {
        const auto loaded_config =
            clusterdemo::LoadClusterConfigFromJsonFile(args.config_path);
        if (!loaded_config.ok())
        {
            std::cerr << "view_node_app config error: " << loaded_config.error_detail
                      << '\n';
            return static_cast<int>(ExitCode::kConfigError);
        }

        ViewNodeStartupConfig startup;
        try
        {
            startup = ResolveStartupConfig(*loaded_config.config, args);
        }
        catch (const std::exception &ex)
        {
            std::cerr << "view_node_app config error: " << ex.what() << '\n';
            return static_cast<int>(ExitCode::kConfigError);
        }

        if (!IsValidEndpoint(startup.listen_endpoint))
        {
            std::cerr << "view_node_app config error: resolved endpoint is invalid: "
                      << startup.listen_endpoint << '\n';
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

        auto registry = std::make_shared<viewdemo::ViewNodeRegistry>(
            startup.registry_config);

        if (const auto registration = MakeSelfRegistration(startup);
            registration.has_value())
        {
            const auto result = registry->RegisterNode(
                viewdemo::RegisterNodeRequest{
                    .request_id = "view-node-startup-register-" + startup.node_id,
                    .registration = *registration,
                });
            if (!result.summary.ok())
            {
                std::cerr << "view_node_app startup error: failed to register local view node"
                          << " status=" << viewdemo::ToString(result.summary.status)
                          << " message=" << result.summary.message << '\n';
                return static_cast<int>(ExitCode::kStartupError);
            }
        }

        grpc::ServerBuilder builder;
        int selected_port = 0;
        grpc::EnableDefaultHealthCheckService(true);
        builder.AddListeningPort(startup.listen_endpoint,
                                 grpc::InsecureServerCredentials(),
                                 &selected_port);
        std::unique_ptr<grpc::ServerCompletionQueue> completion_queue =
            builder.AddCompletionQueue();

        // T045 只负责 thin startup：这里先建立 gRPC 生命周期边界与本地 registry，
        // 不把 ViewNode service 业务逻辑扩展进 app，也不在此改动 target wiring。

        std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
        if (server == nullptr || selected_port <= 0)
        {
            std::cerr << "view_node_app startup error: failed to bind endpoint "
                      << startup.listen_endpoint << '\n';
            return static_cast<int>(ExitCode::kStartupError);
        }

        std::cout << "view_node_app OK"
                  << " cluster_id=" << startup.cluster_id
                  << " node_id=" << identity_state.identity.node_id
                  << " endpoint=" << startup.listen_endpoint
                  << " data_dir=" << startup.data_dir.generic_string()
                  << " identity_path=" <<
            identity_state.identity_path.lexically_normal().generic_string()
                  << " identity_source=" <<
            clusterdemo::ToString(identity_state.identity.source)
                  << " identity_state=" <<
            (identity_state.created_new ? "created" : "loaded")
                  << " identity_durable=" <<
            (identity_state.durable ? "true" : "false")
                  << '\n';

        std::signal(SIGINT, HandleSignal);
#ifdef SIGTERM
        std::signal(SIGTERM, HandleSignal);
#endif

        std::thread completion_queue_thread([&completion_queue]() {
            void *tag = nullptr;
            bool ok = false;
            while (completion_queue->Next(&tag, &ok))
            {
            }
        });

        std::thread wait_thread([&server]() {
            server->Wait();
        });

        while (!g_stop_requested.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        server->Shutdown();
        completion_queue->Shutdown();
        wait_thread.join();
        completion_queue_thread.join();
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
        std::cerr << "view_node_app argument error: " << ex.what() << '\n';
        PrintUsage(std::cerr);
        return static_cast<int>(ExitCode::kInvalidArgument);
    }
}
