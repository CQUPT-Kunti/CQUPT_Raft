#include "cluster/cluster_config.h"
#include "cluster/node_identity.h"
#include "view/view_client.h"
#include "view/view_service_impl.h"
#include "view/view_registry.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
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
        std::chrono::milliseconds self_refresh_interval{0};
        std::vector<std::string> peer_seed_endpoints;
        viewdemo::ViewNodeClientConfig peer_client_config;
        std::chrono::milliseconds peer_sync_interval{0};
        std::chrono::milliseconds peer_sync_max_backoff{0};
    };

    struct IdentityStartupState
    {
        clusterdemo::NodeIdentity identity;
        std::filesystem::path identity_path;
        bool loaded_existing{false};
        bool created_new{false};
        bool durable{false};
    };

    struct ProcessIncarnationStartupState
    {
        clusterdemo::ProcessIncarnation incarnation;
    };

    struct PeerSyncTarget
    {
        std::string endpoint;
        std::shared_ptr<viewdemo::ViewNodeClient> client;
        std::uint32_t consecutive_failures{0};
        std::chrono::steady_clock::time_point next_attempt_at{};
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

    [[nodiscard]] bool SleepWithStop(const std::chrono::milliseconds duration)
    {
        constexpr auto kPollInterval = std::chrono::milliseconds(100);
        auto remaining = duration;
        while (!g_stop_requested.load() &&
               remaining > std::chrono::milliseconds::zero())
        {
            const auto step = remaining < kPollInterval ? remaining : kPollInterval;
            std::this_thread::sleep_for(step);
            remaining -= step;
        }
        return !g_stop_requested.load();
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
        constexpr auto kDefaultLivenessStaleTimeout = std::chrono::milliseconds(5000);
        constexpr auto kDefaultLivenessDeadTimeout = std::chrono::milliseconds(15000);

        viewdemo::ViewRegistryConfig config;
        config.stale_timeout =
            timeouts.liveness_stale_timeout > std::chrono::milliseconds::zero()
                ? timeouts.liveness_stale_timeout
                : kDefaultLivenessStaleTimeout;
        config.dead_timeout =
            timeouts.liveness_dead_timeout > std::chrono::milliseconds::zero()
                ? timeouts.liveness_dead_timeout
                : kDefaultLivenessDeadTimeout;

        // startup 只做装配：suspect 超时从 cluster config 的 stale/dead 边界中间值推导，
        // 不把这段逻辑扩展成新的 discovery 业务语义。
        if (config.dead_timeout > config.stale_timeout)
        {
            const auto delta =
                (config.dead_timeout - config.stale_timeout) / 2;
            config.suspect_timeout = config.stale_timeout + delta;
        }
        else
        {
            config.suspect_timeout = config.stale_timeout;
        }
        return config;
    }

    [[nodiscard]] std::chrono::milliseconds ComputeSelfRefreshInterval(
        const clusterdemo::ClusterConfig &config,
        const viewdemo::ViewRegistryConfig &registry_config)
    {
        constexpr auto kDefaultSelfRefreshInterval = std::chrono::milliseconds(1000);
        constexpr auto kMinimumSelfRefreshInterval = std::chrono::milliseconds(1);

        auto interval =
            config.view_runtime.self_refresh_interval_ms.has_value()
                ? std::chrono::milliseconds(
                      *config.view_runtime.self_refresh_interval_ms)
                : (config.timeouts.heartbeat_interval > std::chrono::milliseconds::zero()
                       ? config.timeouts.heartbeat_interval
                       : kDefaultSelfRefreshInterval);
        if (interval <= std::chrono::milliseconds::zero())
        {
            interval = kDefaultSelfRefreshInterval;
        }

        if (interval >= registry_config.stale_timeout)
        {
            if (registry_config.stale_timeout > kMinimumSelfRefreshInterval)
            {
                interval = registry_config.stale_timeout / 2;
            }
            else
            {
                interval = kMinimumSelfRefreshInterval;
            }
        }

        if (interval >= registry_config.stale_timeout &&
            registry_config.stale_timeout > kMinimumSelfRefreshInterval)
        {
            interval = registry_config.stale_timeout - kMinimumSelfRefreshInterval;
        }

        if (interval <= std::chrono::milliseconds::zero())
        {
            interval = kMinimumSelfRefreshInterval;
        }
        return interval;
    }

    [[nodiscard]] std::chrono::milliseconds ComputePeerSyncInterval(
        const clusterdemo::ClusterConfig &config)
    {
        constexpr auto kDefaultPeerSyncInterval = std::chrono::milliseconds(1000);
        constexpr auto kMinimumPeerSyncInterval = std::chrono::milliseconds(100);

        auto interval =
            config.view_runtime.peer_sync_interval_ms.has_value()
                ? std::chrono::milliseconds(
                      *config.view_runtime.peer_sync_interval_ms)
                : (config.timeouts.heartbeat_interval > std::chrono::milliseconds::zero()
                       ? config.timeouts.heartbeat_interval
                       : kDefaultPeerSyncInterval);
        if (interval < kMinimumPeerSyncInterval)
        {
            interval = kMinimumPeerSyncInterval;
        }
        return interval;
    }

    [[nodiscard]] std::chrono::milliseconds ComputePeerSyncMaxBackoff(
        const std::chrono::milliseconds base_interval)
    {
        constexpr auto kDefaultMaxBackoff = std::chrono::milliseconds(10'000);
        return std::max(base_interval * 4, kDefaultMaxBackoff);
    }

    [[nodiscard]] std::chrono::milliseconds ComputePeerSyncBackoff(
        const std::chrono::milliseconds base_interval,
        const std::chrono::milliseconds max_backoff,
        const std::uint32_t consecutive_failures)
    {
        if (consecutive_failures == 0)
        {
            return base_interval;
        }

        auto delay = base_interval;
        for (std::uint32_t index = 1; index < consecutive_failures; ++index)
        {
            if (delay >= max_backoff / 2)
            {
                return max_backoff;
            }
            delay *= 2;
        }
        return std::min(delay, max_backoff);
    }

    [[nodiscard]] viewdemo::ViewNodeClientConfig BuildPeerSyncClientConfig(
        const clusterdemo::ClusterConfig &cluster_config)
    {
        constexpr auto kDefaultPeerSyncTimeout = std::chrono::milliseconds(2000);

        viewdemo::ViewNodeClientConfig config;
        config.peer_sync_timeout =
            cluster_config.view_runtime.peer_sync_timeout_ms.has_value()
                ? std::chrono::milliseconds(
                      *cluster_config.view_runtime.peer_sync_timeout_ms)
                : (cluster_config.timeouts.discovery_rpc_timeout >
                       std::chrono::milliseconds::zero()
                       ? cluster_config.timeouts.discovery_rpc_timeout
                       : kDefaultPeerSyncTimeout);
        config.wait_for_ready =
            cluster_config.view_runtime.wait_for_ready.value_or(false);
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
        const ViewNodeStartupConfig &startup,
        const clusterdemo::NodeIdentity &identity)
    {
        viewdemo::NodeRegistration registration;
        registration.cluster_id = identity.cluster_id;
        registration.node_id = identity.node_id;
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

    [[nodiscard]] viewdemo::HeartbeatNodeRequest MakeSelfRefreshRequest(
        const ViewNodeStartupConfig &startup,
        const clusterdemo::NodeIdentity &identity,
        const clusterdemo::ProcessIncarnation &process_incarnation,
        const std::uint64_t sequence)
    {
        auto observation = *MakeSelfRegistration(startup, identity);

        viewdemo::HeartbeatNodeRequest request;
        request.request_id =
            "view-node-self-refresh-" + identity.node_id + "-" +
            process_incarnation.incarnation_id + "-" +
            std::to_string(sequence);
        request.cluster_id = identity.cluster_id;
        request.node_id = identity.node_id;
        request.node_type = viewdemo::ViewNodeType::kView;
        request.sequence = sequence;
        request.observation = std::move(observation);
        return request;
    }

    [[nodiscard]] viewdemo::ViewRegistryPeerSnapshot
    ToRegistryPeerSnapshot(const viewdemo::ViewPeerSyncSnapshot &snapshot)
    {
        return viewdemo::ViewRegistryPeerSnapshot{
            .cluster_id = snapshot.cluster_id,
            .generated_at_unix_ms = snapshot.generated_at_unix_ms,
            .view_nodes = snapshot.view_nodes,
            .metadata_nodes = snapshot.metadata_nodes,
            .storage_nodes = snapshot.storage_nodes,
            .leader_hint = snapshot.leader_hint,
        };
    }

    [[nodiscard]] viewdemo::ViewPeerSyncSnapshot
    ToClientPeerSyncSnapshot(const viewdemo::ViewRegistryPeerSnapshot &snapshot)
    {
        return viewdemo::ViewPeerSyncSnapshot{
            .cluster_id = snapshot.cluster_id,
            .generated_at_unix_ms = snapshot.generated_at_unix_ms,
            .view_nodes = snapshot.view_nodes,
            .metadata_nodes = snapshot.metadata_nodes,
            .storage_nodes = snapshot.storage_nodes,
            .leader_hint = snapshot.leader_hint,
        };
    }

    [[nodiscard]] std::string DescribeViewDiagnostics(
        const std::vector<viewdemo::ViewRegistryDiagnostic> &diagnostics)
    {
        std::ostringstream oss;
        for (std::size_t index = 0; index < diagnostics.size(); ++index)
        {
            if (index != 0)
            {
                oss << "; ";
            }
            oss << viewdemo::DescribeViewRegistryDiagnostic(diagnostics[index]);
        }
        return oss.str();
    }

    [[nodiscard]] std::vector<PeerSyncTarget> BuildPeerSyncTargets(
        const ViewNodeStartupConfig &startup)
    {
        std::vector<PeerSyncTarget> targets;
        std::unordered_set<std::string> seen_endpoints;
        const auto now = std::chrono::steady_clock::now();

        for (const auto &endpoint : startup.peer_seed_endpoints)
        {
            if (endpoint == startup.listen_endpoint)
            {
                std::cerr << "view_node_app peer sync skip self endpoint"
                          << " endpoint=" << endpoint << '\n';
                continue;
            }
            if (!seen_endpoints.insert(endpoint).second)
            {
                continue;
            }

            auto channel = grpc::CreateChannel(endpoint,
                                               grpc::InsecureChannelCredentials());
            targets.push_back(PeerSyncTarget{
                .endpoint = endpoint,
                .client = std::make_shared<viewdemo::ViewNodeClient>(
                    std::move(channel),
                    endpoint,
                    startup.peer_client_config),
                .consecutive_failures = 0,
                .next_attempt_at = now,
            });
        }
        return targets;
    }

    [[nodiscard]] bool SyncPeerObservedState(
        const ViewNodeStartupConfig &startup,
        const clusterdemo::NodeIdentity &identity,
        const std::shared_ptr<viewdemo::ViewNodeRegistry> &registry,
        PeerSyncTarget *target)
    {
        if (registry == nullptr || target == nullptr || target->client == nullptr)
        {
            return false;
        }

        const std::uint64_t now_unix_ms = NowUnixMs();
        const std::string request_prefix =
            "view-node-peer-sync-" + identity.node_id + "-" + target->endpoint +
            "-" + std::to_string(now_unix_ms);

        bool pull_import_ok = false;
        const auto pull_result = target->client->PullPeerViewSnapshot(
            viewdemo::PullPeerViewSnapshotRequest{
                .request_id = request_prefix + "-pull",
                .cluster_id = startup.cluster_id,
                .include_dead_nodes = true,
                .include_warnings = true,
            });
        if (!pull_result.transport_ok())
        {
            std::cerr << "view_node_app peer sync pull failed"
                      << " node_id=" << identity.node_id
                      << " peer_endpoint=" << target->endpoint
                      << " grpc_status=" << static_cast<int>(pull_result.rpc.grpc_status_code)
                      << " grpc_message=" << pull_result.rpc.grpc_error_message
                      << '\n';
        }
        else if (!pull_result.result.ok())
        {
            std::cerr << "view_node_app peer sync pull rejected"
                      << " node_id=" << identity.node_id
                      << " peer_endpoint=" << target->endpoint
                      << " status=" << viewdemo::ToString(pull_result.result.summary.status)
                      << " message=" << pull_result.result.summary.message;
            const auto diagnostics =
                DescribeViewDiagnostics(pull_result.result.diagnostics);
            if (!diagnostics.empty())
            {
                std::cerr << " diagnostics=" << diagnostics;
            }
            std::cerr << '\n';
        }
        else
        {
            const auto import_result = registry->ImportPeerSnapshot(
                viewdemo::ImportPeerSnapshotRequest{
                    .request_id = request_prefix + "-import",
                    .cluster_id = startup.cluster_id,
                    .snapshot =
                        ToRegistryPeerSnapshot(pull_result.result.snapshot),
                });
            if (!import_result.summary.ok())
            {
                std::cerr << "view_node_app peer sync import failed"
                          << " node_id=" << identity.node_id
                          << " peer_endpoint=" << target->endpoint
                          << " status=" << viewdemo::ToString(import_result.summary.status)
                          << " message=" << import_result.summary.message;
                const auto diagnostics =
                    DescribeViewDiagnostics(import_result.diagnostics);
                if (!diagnostics.empty())
                {
                    std::cerr << " diagnostics=" << diagnostics;
                }
                std::cerr << '\n';
            }
            else
            {
                pull_import_ok = true;
            }
        }

        const auto export_result = registry->ExportPeerSnapshot(
            viewdemo::ExportPeerSnapshotRequest{
                .request_id = request_prefix + "-export",
                .cluster_id = startup.cluster_id,
                .include_dead_nodes = true,
                .include_warnings = true,
            },
            now_unix_ms);
        if (!export_result.ok())
        {
            std::cerr << "view_node_app peer sync export failed"
                      << " node_id=" << identity.node_id
                      << " peer_endpoint=" << target->endpoint
                      << " status=" << viewdemo::ToString(export_result.summary.status)
                      << " message=" << export_result.summary.message;
            const auto diagnostics =
                DescribeViewDiagnostics(export_result.diagnostics);
            if (!diagnostics.empty())
            {
                std::cerr << " diagnostics=" << diagnostics;
            }
            std::cerr << '\n';
            return false;
        }

        bool push_ok = false;
        const auto push_result = target->client->PushPeerViewSnapshot(
            viewdemo::PushPeerViewSnapshotRequest{
                .request_id = request_prefix + "-push",
                .cluster_id = startup.cluster_id,
                .snapshot = ToClientPeerSyncSnapshot(export_result.snapshot),
            });
        if (!push_result.transport_ok())
        {
            std::cerr << "view_node_app peer sync push failed"
                      << " node_id=" << identity.node_id
                      << " peer_endpoint=" << target->endpoint
                      << " grpc_status=" << static_cast<int>(push_result.rpc.grpc_status_code)
                      << " grpc_message=" << push_result.rpc.grpc_error_message
                      << '\n';
        }
        else if (!push_result.result.ok())
        {
            std::cerr << "view_node_app peer sync push rejected"
                      << " node_id=" << identity.node_id
                      << " peer_endpoint=" << target->endpoint
                      << " status=" << viewdemo::ToString(push_result.result.summary.status)
                      << " message=" << push_result.result.summary.message;
            const auto diagnostics =
                DescribeViewDiagnostics(push_result.result.diagnostics);
            if (!diagnostics.empty())
            {
                std::cerr << " diagnostics=" << diagnostics;
            }
            std::cerr << '\n';
        }
        else
        {
            push_ok = true;
        }

        return pull_import_ok && push_ok;
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
                startup.peer_seed_endpoints =
                    resolved.resolved->view_peer_seed_endpoints;
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
                startup.peer_seed_endpoints = unnamed_view->peer_seeds;
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
            startup.peer_seed_endpoints =
                resolved.resolved->view_peer_seed_endpoints;
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

    [[nodiscard]] ProcessIncarnationStartupState EnsureProcessIncarnation(
        const clusterdemo::NodeIdentity &identity,
        const std::filesystem::path &identity_path)
    {
        const auto incarnation_result =
            clusterdemo::CreateProcessIncarnation(identity);
        if (!incarnation_result.ok())
        {
            throw IdentityStartupError(
                incarnation_result.status,
                "process incarnation startup check failed for identity_path=" +
                    identity_path.lexically_normal().generic_string() +
                    ": " + incarnation_result.diagnostic);
        }

        return ProcessIncarnationStartupState{
            .incarnation = *incarnation_result.incarnation,
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
            startup.self_refresh_interval = ComputeSelfRefreshInterval(
                *loaded_config.config,
                startup.registry_config);
            startup.peer_client_config = BuildPeerSyncClientConfig(
                *loaded_config.config);
            startup.peer_sync_interval = ComputePeerSyncInterval(
                *loaded_config.config);
            startup.peer_sync_max_backoff = ComputePeerSyncMaxBackoff(
                startup.peer_sync_interval);
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
        if (startup.self_refresh_interval >= startup.registry_config.stale_timeout)
        {
            std::cerr
                << "view_node_app config error: self refresh interval must be less than stale timeout"
                << " interval_ms=" << startup.self_refresh_interval.count()
                << " stale_timeout_ms="
                << startup.registry_config.stale_timeout.count() << '\n';
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

        ProcessIncarnationStartupState process_state;
        try
        {
            process_state = EnsureProcessIncarnation(
                identity_state.identity,
                identity_state.identity_path);
        }
        catch (const IdentityStartupError &ex)
        {
            std::cerr << ex.what() << '\n';
            return static_cast<int>(MapIdentityExitCode(ex.status()));
        }

        auto registry = std::make_shared<viewdemo::ViewNodeRegistry>(
            startup.registry_config);

        if (const auto registration = MakeSelfRegistration(
                startup,
                identity_state.identity);
            registration.has_value())
        {
            const auto result = registry->RegisterNode(
                viewdemo::RegisterNodeRequest{
                    .request_id =
                        "view-node-startup-register-" + identity_state.identity.node_id,
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
        viewdemo::ViewNodeServiceImpl service(registry);
        // 入口层只负责把现有 ViewNode gRPC adapter 挂到 server，
        // 不在 app 中扩展 discovery/authority 业务逻辑。
        builder.RegisterService(&service);

        // T045 只负责 thin startup：这里建立 gRPC 生命周期边界并注册已有 adapter，
        // 不把 ViewNode service 业务逻辑扩展进 app，也不在此改动 target wiring。

        std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
        if (server == nullptr || selected_port <= 0)
        {
            std::cerr << "view_node_app startup error: failed to bind endpoint "
                      << startup.listen_endpoint << '\n';
            return static_cast<int>(ExitCode::kStartupError);
        }

        std::cout << "view_node_app OK"
                  << " cluster_id=" << identity_state.identity.cluster_id
                  << " node_type=view"
                  << " node_id=" << identity_state.identity.node_id
                  << " incarnation_id="
                  << process_state.incarnation.incarnation_id
                  << " self_refresh_interval_ms="
                  << startup.self_refresh_interval.count()
                  << " peer_sync_interval_ms="
                  << startup.peer_sync_interval.count()
                  << " peer_seed_count="
                  << startup.peer_seed_endpoints.size()
                  << " startup_sequence_base="
                  << process_state.incarnation.startup_sequence_base
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

        std::vector<PeerSyncTarget> peer_sync_targets =
            BuildPeerSyncTargets(startup);
        if (peer_sync_targets.empty())
        {
            std::cout << "view_node_app peer sync disabled"
                      << " node_id=" << identity_state.identity.node_id
                      << " reason=no_peer_seeds"
                      << '\n';
        }

        std::thread self_refresh_thread(
            [&startup, &identity_state, &process_state, &registry]() {
                std::uint64_t next_sequence =
                    process_state.incarnation.startup_sequence_base;
                while (!g_stop_requested.load())
                {
                    if (!SleepWithStop(startup.self_refresh_interval))
                    {
                        break;
                    }

                    const auto refresh_result = registry->RefreshSelfNode(
                        MakeSelfRefreshRequest(startup,
                                               identity_state.identity,
                                               process_state.incarnation,
                                               next_sequence));
                    if (!refresh_result.summary.ok())
                    {
                        std::cerr
                            << "view_node_app self refresh failed"
                            << " node_id=" << identity_state.identity.node_id
                            << " incarnation_id="
                            << process_state.incarnation.incarnation_id
                            << " sequence=" << next_sequence
                            << " status="
                            << viewdemo::ToString(refresh_result.summary.status)
                            << " message=" << refresh_result.summary.message
                            << '\n';
                        continue;
                    }

                    ++next_sequence;
                }
            });

        std::thread peer_sync_thread(
            [&startup,
             &identity_state,
             &registry,
             peer_sync_targets = std::move(peer_sync_targets)]() mutable {
                while (!g_stop_requested.load())
                {
                    if (peer_sync_targets.empty())
                    {
                        break;
                    }

                    const auto now = std::chrono::steady_clock::now();
                    auto next_wake_at = now + startup.peer_sync_interval;

                    for (auto &target : peer_sync_targets)
                    {
                        if (target.next_attempt_at > now)
                        {
                            next_wake_at =
                                std::min(next_wake_at, target.next_attempt_at);
                            continue;
                        }

                        const bool sync_ok = SyncPeerObservedState(
                            startup,
                            identity_state.identity,
                            registry,
                            &target);
                        const auto attempt_completed_at =
                            std::chrono::steady_clock::now();

                        if (sync_ok)
                        {
                            if (target.consecutive_failures > 0)
                            {
                                std::cout << "view_node_app peer sync recovered"
                                          << " node_id="
                                          << identity_state.identity.node_id
                                          << " peer_endpoint=" << target.endpoint
                                          << " previous_failures="
                                          << target.consecutive_failures
                                          << '\n';
                            }
                            target.consecutive_failures = 0;
                            target.next_attempt_at =
                                attempt_completed_at +
                                startup.peer_sync_interval;
                        }
                        else
                        {
                            ++target.consecutive_failures;
                            const auto backoff = ComputePeerSyncBackoff(
                                startup.peer_sync_interval,
                                startup.peer_sync_max_backoff,
                                target.consecutive_failures);
                            target.next_attempt_at =
                                attempt_completed_at + backoff;
                            std::cerr << "view_node_app peer sync backoff"
                                      << " node_id="
                                      << identity_state.identity.node_id
                                      << " peer_endpoint=" << target.endpoint
                                      << " failures="
                                      << target.consecutive_failures
                                      << " backoff_ms=" << backoff.count()
                                      << '\n';
                        }

                        next_wake_at =
                            std::min(next_wake_at, target.next_attempt_at);
                    }

                    const auto sleep_until =
                        next_wake_at > std::chrono::steady_clock::now()
                            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                  next_wake_at - std::chrono::steady_clock::now())
                            : std::chrono::milliseconds(50);
                    if (!SleepWithStop(sleep_until))
                    {
                        break;
                    }
                }
            });

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

        self_refresh_thread.join();
        peer_sync_thread.join();
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
