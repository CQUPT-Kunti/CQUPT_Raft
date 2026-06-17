#include "cluster/cluster_config.h"
#include "cluster/node_identity.h"
#include "metadata.grpc.pb.h"
#include "raft/common/config.h"
#include "raft/node/raft_node.h"
#include "view/view_client.h"

#include <grpcpp/grpcpp.h>

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

    struct MetadataNodeStartupConfig
    {
        std::string cluster_id;
        std::string node_id;
        std::int32_t raft_id{0};
        std::string listen_endpoint;
        std::filesystem::path data_dir;
        std::filesystem::path snapshot_dir;
        clusterdemo::MetadataNodeInitialRole initial_role{
            clusterdemo::MetadataNodeInitialRole::kUnknown};
        clusterdemo::NodeIdentitySource identity_source{
            clusterdemo::NodeIdentitySource::kConfigGenerator};
        clusterdemo::InitialRaftQuorumSummary initial_quorum;
    };

    struct ViewRegistrationTarget
    {
        std::string endpoint;
        std::shared_ptr<viewdemo::ViewNodeClient> client;
        bool registered{false};
        std::uint64_t next_sequence{1};
        std::string last_error_key;
    };

    struct DynamicJoinAttempt
    {
        std::string endpoint;
        grpc::Status rpc_status;
        raft::JoinMetadataClusterResponse response;
    };

    struct DynamicJoinTarget
    {
        std::string endpoint;
        std::string source;
        std::string observed_view_node_id;
    };

    struct DynamicJoinDiscoveryResult
    {
        std::vector<DynamicJoinTarget> targets;
        std::vector<std::string> diagnostics;
        bool used_view_discovery{false};
    };

    struct DynamicJoinRoundResult
    {
        bool accepted{false};
        bool retryable{false};
        bool used_view_discovery{false};
        std::vector<std::string> diagnostics;
        std::optional<DynamicJoinAttempt> accepted_attempt;
        std::optional<DynamicJoinTarget> accepted_target;
        std::optional<DynamicJoinAttempt> last_not_leader;
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
            return port > 0 && port <= 65535;
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

    [[nodiscard]] bool IsDynamicJoinCandidateMode(
        const MetadataNodeStartupConfig &startup)
    {
        return startup.initial_role ==
               clusterdemo::MetadataNodeInitialRole::kCandidate;
    }

    [[nodiscard]] clusterdemo::NodeIdentityMembershipState
    ResolveMetadataIdentityMembershipState(
        const MetadataNodeStartupConfig &startup)
    {
        switch (startup.initial_role)
        {
        case clusterdemo::MetadataNodeInitialRole::kVoter:
            return clusterdemo::NodeIdentityMembershipState::kVoter;
        case clusterdemo::MetadataNodeInitialRole::kLearner:
            return clusterdemo::NodeIdentityMembershipState::kLearner;
        case clusterdemo::MetadataNodeInitialRole::kCandidate:
            return clusterdemo::NodeIdentityMembershipState::kCandidate;
        case clusterdemo::MetadataNodeInitialRole::kUnknown:
        default:
            return clusterdemo::NodeIdentityMembershipState::kUnknown;
        }
    }

    [[nodiscard]] clusterdemo::NodeIdentitySource ResolveMetadataIdentitySource(
        const MetadataNodeStartupConfig &startup)
    {
        if (IsDynamicJoinCandidateMode(startup))
        {
            return clusterdemo::NodeIdentitySource::kExplicitOverride;
        }
        return clusterdemo::NodeIdentitySource::kConfigGenerator;
    }

    void PrintUsage(std::ostream &out)
    {
        out << "Usage: metadata_node_app --config <path> [--node_id <id>] "
               "[--data_dir <path>] [--listen <host:port>]\n"
            << "  --config   Unified cluster config json path\n"
            << "  --node_id  Controlled override to select the MetadataNode entry\n"
            << "  --data_dir MetadataNode validates this path against the config-generated"
               " durable identity and rejects drift\n"
            << "  --listen   MetadataNode validates this endpoint against the config-generated"
               " raft identity and rejects drift\n";
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

    void ValidateLocalMembershipBoundary(
        const clusterdemo::ClusterConfig &config,
        MetadataNodeStartupConfig *startup)
    {
        if (startup == nullptr)
        {
            throw std::runtime_error("startup config must not be null");
        }

        const auto quorum_result = clusterdemo::ComputeInitialRaftQuorum(config);
        if (!quorum_result.ok())
        {
            throw std::runtime_error(
                "failed to compute initial metadata quorum: " +
                quorum_result.error_detail);
        }

        startup->initial_quorum = *quorum_result.summary;
        const bool in_voter_set =
            std::find(startup->initial_quorum.voter_raft_ids.begin(),
                      startup->initial_quorum.voter_raft_ids.end(),
                      startup->raft_id) != startup->initial_quorum.voter_raft_ids.end();
        const bool in_learner_set =
            std::find(config.initial_raft_membership.learner_raft_ids.begin(),
                      config.initial_raft_membership.learner_raft_ids.end(),
                      startup->raft_id) !=
            config.initial_raft_membership.learner_raft_ids.end();

        // app startup 只做配置和身份边界校验，不修改真实 membership authority。
        if (startup->initial_role == clusterdemo::MetadataNodeInitialRole::kVoter &&
            (!in_voter_set || in_learner_set))
        {
            throw std::runtime_error(
                "metadata node initial_role=voter but raft_id is not present in voter set"
                " node_id=" +
                startup->node_id + " raft_id=" + std::to_string(startup->raft_id));
        }
        if (startup->initial_role == clusterdemo::MetadataNodeInitialRole::kLearner &&
            (!in_learner_set || in_voter_set))
        {
            throw std::runtime_error(
                "metadata node initial_role=learner but raft_id is not present in learner set"
                " node_id=" +
                startup->node_id + " raft_id=" + std::to_string(startup->raft_id));
        }
        if (startup->initial_role ==
                clusterdemo::MetadataNodeInitialRole::kCandidate &&
            (in_voter_set || in_learner_set))
        {
            throw std::runtime_error(
                "metadata node initial_role=candidate must not appear in initial committed membership"
                " node_id=" +
                startup->node_id + " raft_id=" + std::to_string(startup->raft_id));
        }
    }

    void ValidateLocalOverrideSafety(
        const ParsedArgs &args,
        const clusterdemo::ResolvedClusterNodeConfig &resolved,
        const MetadataNodeStartupConfig &startup)
    {
        if (startup.node_id.empty())
        {
            throw std::runtime_error("metadata node node_id must not be empty");
        }
        if (startup.raft_id <= 0)
        {
            throw std::runtime_error("metadata node raft_id must be > 0");
        }
        if (startup.data_dir.empty())
        {
            throw std::runtime_error("metadata node data_dir must not be empty");
        }
        if (startup.snapshot_dir.empty())
        {
            throw std::runtime_error("metadata node snapshot_dir must not be empty");
        }
        if (!IsValidEndpoint(startup.listen_endpoint))
        {
            throw std::runtime_error("metadata node endpoint is invalid: " +
                                     startup.listen_endpoint);
        }

        if (args.node_id.has_value() && *args.node_id != startup.node_id)
        {
            throw std::runtime_error(
                "--node_id resolved to an unexpected MetadataNode identity: requested=" +
                *args.node_id + " actual=" + startup.node_id);
        }

        const std::string configured_data_dir =
            NormalizePathKey(resolved.data_dir);
        const std::string startup_data_dir =
            NormalizePathKey(startup.data_dir);
        if (args.data_dir_override.has_value() &&
            startup_data_dir != configured_data_dir)
        {
            throw std::runtime_error(
                "--data_dir override is rejected for MetadataNode: config-generated "
                "data_dir=" +
                resolved.data_dir.generic_string() +
                " override=" +
                args.data_dir_override->generic_string() +
                "; refusing to move durable node_id/raft_id state to another directory");
        }

        if (args.listen_override.has_value() &&
            startup.listen_endpoint != resolved.endpoint)
        {
            throw std::runtime_error(
                "--listen override is rejected for MetadataNode: config-generated "
                "endpoint=" +
                resolved.endpoint + " override=" + *args.listen_override +
                "; refusing to start raft_id=" +
                std::to_string(startup.raft_id) +
                " on a different runtime endpoint");
        }

        if (startup.initial_role != clusterdemo::MetadataNodeInitialRole::kVoter &&
            startup.initial_role != clusterdemo::MetadataNodeInitialRole::kLearner &&
            startup.initial_role != clusterdemo::MetadataNodeInitialRole::kCandidate)
        {
            throw std::runtime_error(
                "metadata node initial_role must be voter, learner or candidate");
        }
    }

    [[nodiscard]] MetadataNodeStartupConfig ResolveStartupConfig(
        const clusterdemo::ClusterConfig &config,
        const ParsedArgs &args)
    {
        MetadataNodeStartupConfig startup;
        startup.cluster_id = config.cluster_id;

        std::string selected_node_id;
        if (args.node_id.has_value())
        {
            selected_node_id = *args.node_id;
        }
        else if (config.metadata_nodes.size() == 1)
        {
            selected_node_id = config.metadata_nodes.front().node_id;
        }
        else
        {
            throw std::runtime_error(
                "--node_id is required when cluster config contains multiple MetadataNode entries");
        }

        const auto resolved = clusterdemo::ResolveClusterNodeConfig(
            config,
            clusterdemo::ClusterNodeType::kMetadata,
            selected_node_id);
        if (!resolved.ok())
        {
            throw std::runtime_error(
                "failed to resolve metadata node from cluster config: " +
                resolved.error_detail);
        }
        if (!resolved.resolved.has_value())
        {
            throw std::runtime_error(
                "failed to resolve metadata node from cluster config: empty result");
        }

        const auto &resolved_node = *resolved.resolved;

        if (!resolved_node.raft_id.has_value() || *resolved_node.raft_id <= 0)
        {
            throw std::runtime_error(
                "metadata node config must provide a positive raft_id"
                " node_id=" +
                resolved_node.node_id);
        }
        if (!resolved_node.snapshot_dir.has_value() ||
            resolved_node.snapshot_dir->empty())
        {
            throw std::runtime_error(
                "metadata node config must provide snapshot_dir"
                " node_id=" +
                resolved_node.node_id);
        }
        if (!resolved_node.metadata_initial_role.has_value() ||
            *resolved_node.metadata_initial_role ==
                clusterdemo::MetadataNodeInitialRole::kUnknown)
        {
            throw std::runtime_error(
                "metadata node config must provide initial_role as voter, learner or candidate"
                " node_id=" +
                resolved_node.node_id);
        }

        startup.node_id = resolved_node.node_id;
        startup.raft_id = *resolved_node.raft_id;
        startup.listen_endpoint = resolved_node.endpoint;
        startup.data_dir = resolved_node.data_dir;
        startup.snapshot_dir = *resolved_node.snapshot_dir;
        startup.initial_role = *resolved_node.metadata_initial_role;
        startup.identity_source = ResolveMetadataIdentitySource(startup);

        if (args.data_dir_override.has_value())
        {
            startup.data_dir = *args.data_dir_override;
        }
        if (args.listen_override.has_value())
        {
            startup.listen_endpoint = *args.listen_override;
        }

        ValidateLocalOverrideSafety(args, resolved_node, startup);
        ValidateLocalMembershipBoundary(config, &startup);
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

    [[nodiscard]] clusterdemo::NodeIdentity EnsureNodeIdentity(
        const MetadataNodeStartupConfig &startup)
    {
        // bootstrap voter 与 dynamic join candidate 都通过统一 durable identity
        // 流程进入，但本地 identity 不能越权表达 committed membership authority。
        const auto membership_state =
            ResolveMetadataIdentityMembershipState(startup);
        const clusterdemo::NodeIdentity identity_to_create{
            .cluster_id = startup.cluster_id,
            .node_id = startup.node_id,
            .node_type = clusterdemo::ClusterNodeType::kMetadata,
            .raft_id = startup.raft_id,
            .membership_state = membership_state,
            .identity_version = clusterdemo::kNodeIdentityCurrentVersion,
            .created_at_unix_ms = static_cast<std::int64_t>(NowUnixMs()),
            .source = startup.identity_source,
        };

        const clusterdemo::ExpectedNodeIdentity expected{
            .cluster_id = startup.cluster_id,
            .node_id = startup.node_id,
            .node_type = clusterdemo::ClusterNodeType::kMetadata,
            .raft_id = startup.raft_id,
            .membership_state = membership_state,
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
        return *load_or_create.identity;
    }

    [[nodiscard]] clusterdemo::ProcessIncarnation EnsureProcessIncarnation(
        const clusterdemo::NodeIdentity &identity)
    {
        const auto incarnation = clusterdemo::CreateProcessIncarnation(identity);
        if (!incarnation.ok())
        {
            throw IdentityStartupError(
                incarnation.status,
                "process incarnation startup check failed: " +
                    incarnation.diagnostic);
        }
        return *incarnation.incarnation;
    }

    [[nodiscard]] raftdemo::NodeConfig BuildRaftNodeConfig(
        const clusterdemo::ClusterConfig &config,
        const MetadataNodeStartupConfig &startup)
    {
        raftdemo::NodeConfig node_config;
        node_config.node_id = startup.raft_id;
        node_config.address = startup.listen_endpoint;
        node_config.data_dir = startup.data_dir.string();

        if (config.timeouts.metadata_rpc_timeout > std::chrono::milliseconds::zero())
        {
            node_config.rpc_deadline = config.timeouts.metadata_rpc_timeout;
        }

        // cluster config 里的 heartbeat_interval_ms 用于 ViewNode / StorageNode
        // 的注册与观测心跳，不应直接复用成 Raft heartbeat。
        // 否则当观测心跳配置到 1s 级别时，Raft follower 会在默认 300-600ms
        // election timeout 内先超时发起选举，导致 leader 高频抖动。
        //
        // Metadata Raft 这里保持 NodeConfig 自带的毫秒级默认 heartbeat /
        // election window，避免把 control-plane liveness 配置误注入到
        // Raft consensus timing。

        node_config.peers.reserve(config.metadata_nodes.size());
        for (const auto &metadata_node : config.metadata_nodes)
        {
            if (metadata_node.raft_id == startup.raft_id)
            {
                continue;
            }

            node_config.peers.push_back(raftdemo::PeerConfig{
                .node_id = metadata_node.raft_id,
                .address = metadata_node.endpoint,
            });
        }

        return node_config;
    }

    [[nodiscard]] raftdemo::snapshotConfig BuildSnapshotConfig(
        const MetadataNodeStartupConfig &startup)
    {
        raftdemo::snapshotConfig snapshot_config;
        snapshot_config.snapshot_dir = startup.snapshot_dir.string();
        return snapshot_config;
    }

    [[nodiscard]] viewdemo::ViewNodeClientConfig BuildViewNodeClientConfig(
        const clusterdemo::ClusterTimeoutConfig &timeouts)
    {
        viewdemo::ViewNodeClientConfig config;
        if (timeouts.registration_timeout > std::chrono::milliseconds::zero())
        {
            config.register_timeout = timeouts.registration_timeout;
            config.heartbeat_timeout = timeouts.registration_timeout;
        }
        if (timeouts.discovery_rpc_timeout > std::chrono::milliseconds::zero())
        {
            config.cluster_view_timeout = timeouts.discovery_rpc_timeout;
            config.discovery_timeout = timeouts.discovery_rpc_timeout;
        }
        config.wait_for_ready = false;
        return config;
    }

    [[nodiscard]] std::string MakeViewRequestId(const std::string_view action,
                                                const MetadataNodeStartupConfig &startup,
                                                const std::uint64_t nonce)
    {
        return "metadata-node-" + std::string(action) + "-" + startup.node_id +
               "-" + std::to_string(nonce);
    }

    [[nodiscard]] std::vector<ViewRegistrationTarget> BuildViewTargets(
        const clusterdemo::ClusterConfig &config);

    [[nodiscard]] std::vector<std::string> BuildDynamicJoinSeedEndpoints(
        const clusterdemo::ClusterConfig &config,
        const MetadataNodeStartupConfig &startup)
    {
        std::vector<std::string> endpoints;
        std::unordered_set<std::string> seen;
        endpoints.reserve(config.metadata_nodes.size());

        for (const auto &metadata_node : config.metadata_nodes)
        {
            if (metadata_node.node_id == startup.node_id)
            {
                continue;
            }
            if (metadata_node.initial_role ==
                clusterdemo::MetadataNodeInitialRole::kCandidate)
            {
                continue;
            }
            if (metadata_node.endpoint.empty())
            {
                continue;
            }
            if (!seen.insert(metadata_node.endpoint).second)
            {
                continue;
            }
            endpoints.push_back(metadata_node.endpoint);
        }
        return endpoints;
    }

    [[nodiscard]] std::string ResolveMetadataJoinTargetEndpoint(
        const viewdemo::ViewNodeSnapshot &snapshot)
    {
        if (!snapshot.control_plane_endpoint.empty())
        {
            return snapshot.control_plane_endpoint;
        }
        return snapshot.endpoint;
    }

    void AppendDynamicJoinTarget(
        std::vector<DynamicJoinTarget> *targets,
        std::unordered_set<std::string> *seen,
        const MetadataNodeStartupConfig &startup,
        std::string endpoint,
        std::string source,
        std::string observed_view_node_id)
    {
        if (targets == nullptr || seen == nullptr)
        {
            return;
        }
        if (endpoint.empty() || endpoint == startup.listen_endpoint)
        {
            return;
        }
        if (!seen->insert(endpoint).second)
        {
            return;
        }
        targets->push_back(DynamicJoinTarget{
            .endpoint = std::move(endpoint),
            .source = std::move(source),
            .observed_view_node_id = std::move(observed_view_node_id),
        });
    }

    [[nodiscard]] DynamicJoinDiscoveryResult DiscoverDynamicJoinTargets(
        const clusterdemo::ClusterConfig &config,
        const MetadataNodeStartupConfig &startup)
    {
        DynamicJoinDiscoveryResult result;
        std::unordered_set<std::string> seen;
        const auto view_targets = BuildViewTargets(config);
        std::uint64_t request_nonce = NowUnixMs();

        for (const auto &view_target : view_targets)
        {
            const auto discovery = view_target.client->DiscoverMetadata(
                viewdemo::DiscoverMetadataRequest{
                    .request_id = MakeViewRequestId("discover-metadata",
                                                   startup,
                                                   request_nonce++),
                    .cluster_id = startup.cluster_id,
                    .prefer_leader = true,
                    .live_only = true,
                    .limit = static_cast<std::uint32_t>(
                        std::max<std::size_t>(config.metadata_nodes.size(), 3U)),
                });

            std::ostringstream diagnostic;
            diagnostic << "view_endpoint=" << view_target.endpoint;
            if (!discovery.transport_ok())
            {
                diagnostic << " transport_status="
                           << static_cast<int>(discovery.rpc.grpc_status_code)
                           << " transport_message="
                           << discovery.rpc.grpc_error_message;
                result.diagnostics.push_back(diagnostic.str());
                continue;
            }

            diagnostic << " status="
                       << viewdemo::ToString(
                              discovery.result.summary.status)
                       << " metadata_candidates="
                       << discovery.result.metadata_nodes.size()
                       << " message="
                       << discovery.result.summary.message;
            if (discovery.result.leader_hint.has_value())
            {
                diagnostic << " leader_hint_endpoint="
                           << discovery.result.leader_hint->endpoint;
            }
            result.diagnostics.push_back(diagnostic.str());

            if (!discovery.result.ok())
            {
                continue;
            }

            const std::string observed_view_node_id =
                !discovery.result.summary.node_id.empty()
                    ? discovery.result.summary.node_id
                    : view_target.endpoint;

            if (discovery.result.leader_hint.has_value())
            {
                AppendDynamicJoinTarget(&result.targets,
                                        &seen,
                                        startup,
                                        discovery.result.leader_hint->endpoint,
                                        "view_leader_hint",
                                        observed_view_node_id);
            }

            for (const auto &snapshot : discovery.result.metadata_nodes)
            {
                AppendDynamicJoinTarget(&result.targets,
                                        &seen,
                                        startup,
                                        ResolveMetadataJoinTargetEndpoint(snapshot),
                                        "view_metadata_candidate",
                                        observed_view_node_id);
            }
        }

        result.used_view_discovery = !result.targets.empty();
        if (result.used_view_discovery)
        {
            return result;
        }

        for (const auto &endpoint : BuildDynamicJoinSeedEndpoints(config, startup))
        {
            AppendDynamicJoinTarget(&result.targets,
                                    &seen,
                                    startup,
                                    endpoint,
                                    "config_seed_fallback",
                                    {});
        }
        return result;
    }

    [[nodiscard]] raft::JoinMetadataClusterRequest BuildJoinMetadataClusterRequest(
        const MetadataNodeStartupConfig &startup,
        const clusterdemo::NodeIdentity &identity,
        const clusterdemo::ProcessIncarnation &incarnation,
        const DynamicJoinTarget &target)
    {
        raft::JoinMetadataClusterRequest request;
        const std::uint64_t now_unix_ms = NowUnixMs();
        request.set_request_id("metadata-node-app-join-" + startup.node_id + "-" +
                               std::to_string(now_unix_ms));
        request.set_cluster_id(startup.cluster_id);
        request.set_node_id(identity.node_id);
        request.set_candidate_raft_id(startup.raft_id);
        request.set_candidate_client_address(startup.listen_endpoint);
        request.set_candidate_raft_address(startup.listen_endpoint);
        request.set_candidate_incarnation_id(incarnation.incarnation_id);
        request.set_candidate_sequence(incarnation.startup_sequence_base);
        request.set_persistent_generation(identity.persistent_generation);
        request.set_data_dir_fingerprint(NormalizePathKey(startup.data_dir));
        request.set_local_state_hint(
            raft::JOIN_METADATA_CANDIDATE_STATE_HINT_CANDIDATE);
        request.set_observed_view_node_id(target.observed_view_node_id);
        request.set_observed_time_unix_ms(now_unix_ms);
        request.set_observed_metadata_endpoint(target.endpoint);
        return request;
    }

    [[nodiscard]] DynamicJoinAttempt AttemptDynamicJoinValidation(
        const std::string &endpoint,
        const raft::JoinMetadataClusterRequest &request)
    {
        DynamicJoinAttempt attempt;
        attempt.endpoint = endpoint;

        auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
        auto stub = raft::MetadataService::NewStub(channel);
        grpc::ClientContext context;
        attempt.rpc_status =
            stub->JoinMetadataCluster(&context, request, &attempt.response);
        return attempt;
    }

    [[nodiscard]] std::string DescribeJoinAttempt(
        const DynamicJoinAttempt &attempt)
    {
        std::ostringstream oss;
        oss << "endpoint=" << attempt.endpoint;
        if (!attempt.rpc_status.ok())
        {
            oss << " transport_status="
                << static_cast<int>(attempt.rpc_status.error_code())
                << " transport_message=" << attempt.rpc_status.error_message();
            return oss.str();
        }

        oss << " status="
            << raft::MetadataStatusCode_Name(attempt.response.summary().code())
            << " disposition="
            << raft::JoinMetadataClusterDisposition_Name(
                   attempt.response.disposition())
            << " requested_membership="
            << raft::JoinMetadataTargetMembership_Name(
                   attempt.response.requested_membership())
            << " committed_membership_changed="
            << (attempt.response.committed_membership_changed() ? "true" : "false")
            << " membership_epoch=" << attempt.response.membership_epoch()
            << " message=" << attempt.response.summary().message();
        if (attempt.response.summary().has_leader_hint())
        {
            oss << " leader_hint_id="
                << attempt.response.summary().leader_hint().leader_id()
                << " leader_hint_address="
                << attempt.response.summary().leader_hint().leader_address();
        }
        return oss.str();
    }

    [[nodiscard]] DynamicJoinRoundResult ExecuteDynamicJoinRound(
        const clusterdemo::ClusterConfig &config,
        const MetadataNodeStartupConfig &startup,
        const clusterdemo::NodeIdentity &identity,
        const clusterdemo::ProcessIncarnation &incarnation)
    {
        DynamicJoinRoundResult result;
        const auto discovery = DiscoverDynamicJoinTargets(config, startup);
        result.used_view_discovery = discovery.used_view_discovery;
        result.diagnostics.reserve(discovery.targets.size() +
                                   discovery.diagnostics.size());
        result.diagnostics.insert(result.diagnostics.end(),
                                  discovery.diagnostics.begin(),
                                  discovery.diagnostics.end());
        if (discovery.targets.empty())
        {
            result.retryable = true;
            return result;
        }

        std::vector<DynamicJoinTarget> pending_targets = discovery.targets;
        std::unordered_set<std::string> queued_endpoints;
        std::unordered_set<std::string> attempted_endpoints;
        for (const auto &target : pending_targets)
        {
            queued_endpoints.insert(target.endpoint);
        }

        for (std::size_t index = 0; index < pending_targets.size(); ++index)
        {
            const auto &target = pending_targets[index];
            if (!attempted_endpoints.insert(target.endpoint).second)
            {
                continue;
            }

            const auto request = BuildJoinMetadataClusterRequest(startup,
                                                                 identity,
                                                                 incarnation,
                                                                 target);
            const DynamicJoinAttempt attempt =
                AttemptDynamicJoinValidation(target.endpoint, request);
            result.diagnostics.push_back("source=" + target.source + " " +
                                         DescribeJoinAttempt(attempt));

            if (!attempt.rpc_status.ok())
            {
                result.retryable = true;
                continue;
            }

            switch (attempt.response.disposition())
            {
            case raft::JOIN_METADATA_CLUSTER_DISPOSITION_NOT_LEADER:
                result.retryable = true;
                result.last_not_leader = attempt;
                if (attempt.response.summary().has_leader_hint() &&
                    !attempt.response.summary().leader_hint().leader_address().empty())
                {
                    AppendDynamicJoinTarget(&pending_targets,
                                            &queued_endpoints,
                                            startup,
                                            attempt.response.summary().leader_hint().leader_address(),
                                            "metadata_not_leader_hint",
                                            target.observed_view_node_id);
                }
                continue;
            case raft::JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT:
            case raft::JOIN_METADATA_CLUSTER_DISPOSITION_DUPLICATE:
            case raft::JOIN_METADATA_CLUSTER_DISPOSITION_PENDING_MEMBERSHIP_CHANGE:
                result.accepted = true;
                result.accepted_attempt = attempt;
                result.accepted_target = target;
                return result;
            case raft::JOIN_METADATA_CLUSTER_DISPOSITION_INVALID_CANDIDATE:
            case raft::JOIN_METADATA_CLUSTER_DISPOSITION_REJECTED:
            case raft::JOIN_METADATA_CLUSTER_DISPOSITION_UNSPECIFIED:
            default:
                result.retryable = false;
                result.accepted = false;
                result.diagnostics.push_back("rejected " +
                                             DescribeJoinAttempt(attempt));
                return result;
            }
        }

        result.retryable = true;
        return result;
    }

    [[nodiscard]] std::optional<std::string> FindMetadataNodeIdByRaftId(
        const clusterdemo::ClusterConfig &config,
        const std::int32_t raft_id)
    {
        for (const auto &node : config.metadata_nodes)
        {
            if (node.raft_id == raft_id)
            {
                return node.node_id;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] viewdemo::MetadataMembershipObservedState
    MapObservedMembershipState(
        const raftdemo::CommittedMembershipRole local_role,
        const clusterdemo::MetadataNodeInitialRole initial_role)
    {
        switch (local_role)
        {
        case raftdemo::CommittedMembershipRole::kVoter:
            return viewdemo::MetadataMembershipObservedState::kVoter;
        case raftdemo::CommittedMembershipRole::kLearner:
            return viewdemo::MetadataMembershipObservedState::kLearner;
        case raftdemo::CommittedMembershipRole::kNonMember:
            return viewdemo::MetadataMembershipObservedState::kRegistered;
        case raftdemo::CommittedMembershipRole::kUnknown:
        default:
            break;
        }

        if (initial_role == clusterdemo::MetadataNodeInitialRole::kVoter)
        {
            return viewdemo::MetadataMembershipObservedState::kVoter;
        }
        if (initial_role == clusterdemo::MetadataNodeInitialRole::kLearner)
        {
            return viewdemo::MetadataMembershipObservedState::kLearner;
        }
        return viewdemo::MetadataMembershipObservedState::kRegistered;
    }

    [[nodiscard]] bool RuntimeSummaryContainsLearner(
        const raftdemo::RuntimeMembershipSummary &runtime_summary,
        const std::int32_t raft_id)
    {
        return std::binary_search(runtime_summary.learner_ids.begin(),
                                  runtime_summary.learner_ids.end(),
                                  raft_id);
    }

    [[nodiscard]] viewdemo::MetadataRaftObservedRole MapObservedRaftRole(
        const raftdemo::NodeStatusSnapshot &status,
        const raftdemo::CommittedMembershipRole local_role)
    {
        if (local_role == raftdemo::CommittedMembershipRole::kLearner)
        {
            return viewdemo::MetadataRaftObservedRole::kLearner;
        }
        if (status.role == "Leader")
        {
            return viewdemo::MetadataRaftObservedRole::kLeader;
        }
        if (status.role == "Candidate")
        {
            return viewdemo::MetadataRaftObservedRole::kCandidate;
        }
        if (status.role == "Follower")
        {
            return viewdemo::MetadataRaftObservedRole::kFollower;
        }
        return viewdemo::MetadataRaftObservedRole::kUnknown;
    }

    [[nodiscard]] std::optional<viewdemo::MetadataLeaderHint> BuildLeaderHint(
        const clusterdemo::ClusterConfig &config,
        const MetadataNodeStartupConfig &startup,
        const raftdemo::NodeStatusSnapshot &status,
        const std::uint64_t now_unix_ms)
    {
        int leader_id = status.leader_id;
        std::string leader_endpoint = status.leader_address;
        if (leader_id < 0 && status.role == "Leader")
        {
            leader_id = startup.raft_id;
        }
        if (leader_endpoint.empty() && leader_id == startup.raft_id)
        {
            leader_endpoint = startup.listen_endpoint;
        }
        if (leader_id < 0 || leader_endpoint.empty())
        {
            return std::nullopt;
        }

        viewdemo::MetadataLeaderHint hint;
        hint.raft_id = leader_id;
        hint.endpoint = leader_endpoint;
        hint.observed_term = status.term;
        hint.observed_at_unix_ms = now_unix_ms;

        const auto leader_node_id = FindMetadataNodeIdByRaftId(config, leader_id);
        if (leader_node_id.has_value())
        {
            hint.node_id = *leader_node_id;
        }
        else if (leader_id == startup.raft_id)
        {
            hint.node_id = startup.node_id;
        }
        return hint;
    }

    [[nodiscard]] viewdemo::MetadataNodeObservation BuildMetadataObservation(
        const clusterdemo::ClusterConfig &config,
        const MetadataNodeStartupConfig &startup,
        const raftdemo::NodeStatusSnapshot &status,
        const raftdemo::CommittedMembershipQuorumSummary &quorum_summary,
        const raftdemo::RuntimeMembershipSummary &runtime_summary,
        const std::uint64_t now_unix_ms)
    {
        viewdemo::MetadataNodeObservation observation;
        observation.raft_id = startup.raft_id;
        const bool runtime_observes_learner =
            RuntimeSummaryContainsLearner(runtime_summary, startup.raft_id);
        const bool admitted_dynamic_learner =
            IsDynamicJoinCandidateMode(startup);
        observation.raft_role = (runtime_observes_learner ||
                                 admitted_dynamic_learner)
                                    ? viewdemo::MetadataRaftObservedRole::kLearner
                                    : MapObservedRaftRole(status, quorum_summary.local_role);
        observation.membership_state = (runtime_observes_learner ||
                                        admitted_dynamic_learner)
                                           ? viewdemo::MetadataMembershipObservedState::kLearner
                                           : MapObservedMembershipState(quorum_summary.local_role,
                                                                       startup.initial_role);
        observation.leader_hint =
            BuildLeaderHint(config, startup, status, now_unix_ms);
        observation.observed_term = status.term;
        observation.commit_index = status.commit_index;
        observation.membership_epoch =
            config.initial_raft_membership.membership_epoch;
        return observation;
    }

    [[nodiscard]] viewdemo::NodeRegistration BuildMetadataRegistration(
        const clusterdemo::ClusterConfig &config,
        const MetadataNodeStartupConfig &startup,
        const raftdemo::NodeStatusSnapshot &status,
        const raftdemo::CommittedMembershipQuorumSummary &quorum_summary,
        const raftdemo::RuntimeMembershipSummary &runtime_summary,
        const std::uint64_t now_unix_ms)
    {
        // ViewNode registration 只上报由 cluster config 确认过的 metadata identity，
        // 不能分配、覆盖或漂移 MetadataNode 的 raft_id。
        viewdemo::NodeRegistration registration;
        registration.cluster_id = startup.cluster_id;
        registration.node_id = startup.node_id;
        registration.node_type = viewdemo::ViewNodeType::kMetadata;
        registration.endpoint = startup.listen_endpoint;
        registration.control_plane_endpoint = startup.listen_endpoint;
        registration.data_plane_endpoint.clear();
        registration.data_dir_fingerprint = NormalizePathKey(startup.data_dir);
        registration.observed_at_unix_ms = now_unix_ms;
        registration.health.health = viewdemo::ViewNodeHealth::kHealthy;
        registration.health.disk_pressure = viewdemo::ViewNodeDiskPressure::kLow;
        registration.health.io_error_count = 0;
        registration.load.active_reads = 0;
        registration.load.active_writes = 0;
        registration.load.queued_ops = 0;
        registration.load.write_admission_overloaded = false;
        registration.load.read_admission_overloaded = false;
        registration.metadata = BuildMetadataObservation(config,
                                                         startup,
                                                         status,
                                                         quorum_summary,
                                                         runtime_summary,
                                                         now_unix_ms);
        return registration;
    }

    [[nodiscard]] std::vector<ViewRegistrationTarget> BuildViewTargets(
        const clusterdemo::ClusterConfig &config)
    {
        std::vector<ViewRegistrationTarget> targets;
        targets.reserve(config.view_nodes.size());

        const auto client_config = BuildViewNodeClientConfig(config.timeouts);
        for (const auto &view_node : config.view_nodes)
        {
            auto channel = grpc::CreateChannel(
                view_node.endpoint,
                grpc::InsecureChannelCredentials());
            targets.push_back(ViewRegistrationTarget{
                .endpoint = view_node.endpoint,
                .client = std::make_shared<viewdemo::ViewNodeClient>(
                    std::move(channel),
                    view_node.endpoint,
                    client_config),
            });
        }
        return targets;
    }

    [[nodiscard]] std::string DescribeViewDiagnostics(
        const std::vector<viewdemo::ViewRegistryDiagnostic> &diagnostics)
    {
        if (diagnostics.empty())
        {
            return {};
        }

        std::ostringstream oss;
        for (std::size_t index = 0; index < diagnostics.size(); ++index)
        {
            if (index > 0)
            {
                oss << "; ";
            }
            oss << viewdemo::ToString(diagnostics[index].code) << ":"
                << diagnostics[index].message;
        }
        return oss.str();
    }

    void ReportViewTargetFailure(
        ViewRegistrationTarget *target,
        const std::string_view stage,
        const std::string &detail)
    {
        if (target == nullptr)
        {
            return;
        }

        const std::string error_key =
            std::string(stage) + "|" + detail;
        if (target->last_error_key == error_key)
        {
            return;
        }
        target->last_error_key = error_key;
        std::cerr << "metadata_node_app view warning: endpoint="
                  << target->endpoint
                  << " stage=" << stage
                  << " message=" << detail << '\n';
    }

    void ClearViewTargetFailure(
        ViewRegistrationTarget *target,
        const std::string_view stage)
    {
        if (target == nullptr || target->last_error_key.empty())
        {
            return;
        }

        std::cout << "metadata_node_app view recovered"
                  << " endpoint=" << target->endpoint
                  << " stage=" << stage << '\n';
        target->last_error_key.clear();
    }

    bool EnsureRegisteredWithViewNode(
        const clusterdemo::ClusterConfig &config,
        const MetadataNodeStartupConfig &startup,
        const std::shared_ptr<raftdemo::RaftNode> &node,
        ViewRegistrationTarget *target)
    {
        if (node == nullptr || target == nullptr)
        {
            return false;
        }

        const std::uint64_t now_unix_ms = NowUnixMs();
        const auto status = node->GetStatusSnapshot();
        const auto quorum_summary = node->GetCommittedMembershipQuorumSummary();
        const auto runtime_summary = node->GetRuntimeMembershipSummary();
        const auto registration = BuildMetadataRegistration(config,
                                                            startup,
                                                            status,
                                                            quorum_summary,
                                                            runtime_summary,
                                                            now_unix_ms);
        const auto result = target->client->RegisterNode(
            viewdemo::RegisterNodeRequest{
                .request_id = MakeViewRequestId("register", startup, now_unix_ms),
                .registration = registration,
            });

        if (!result.transport_ok())
        {
            ReportViewTargetFailure(
                target,
                "register",
                "transport status=" +
                    std::to_string(static_cast<int>(result.rpc.grpc_status_code)) +
                    " message=" + result.rpc.grpc_error_message);
            return false;
        }
        if (!result.result.ok())
        {
            std::string detail =
                "status=" + std::string(viewdemo::ToString(result.result.summary.status)) +
                " message=" + result.result.summary.message;
            const std::string diagnostics =
                DescribeViewDiagnostics(result.result.diagnostics);
            if (!diagnostics.empty())
            {
                detail += " diagnostics=" + diagnostics;
            }
            ReportViewTargetFailure(target, "register", detail);
            return false;
        }

        target->registered = true;
        target->next_sequence = std::max<std::uint64_t>(target->next_sequence, 1);
        ClearViewTargetFailure(target, "register");
        return true;
    }

    void SendHeartbeatToViewNode(const clusterdemo::ClusterConfig &config,
                                 const MetadataNodeStartupConfig &startup,
                                 const std::shared_ptr<raftdemo::RaftNode> &node,
                                 ViewRegistrationTarget *target)
    {
        if (node == nullptr || target == nullptr)
        {
            return;
        }

        const std::uint64_t now_unix_ms = NowUnixMs();
        const auto status = node->GetStatusSnapshot();
        const auto quorum_summary = node->GetCommittedMembershipQuorumSummary();
        const auto runtime_summary = node->GetRuntimeMembershipSummary();
        const auto observation = BuildMetadataRegistration(config,
                                                           startup,
                                                           status,
                                                           quorum_summary,
                                                           runtime_summary,
                                                           now_unix_ms);
        const std::uint64_t sequence = target->next_sequence;
        const auto result = target->client->HeartbeatNode(
            viewdemo::HeartbeatNodeRequest{
                .request_id = MakeViewRequestId("heartbeat", startup, sequence),
                .cluster_id = startup.cluster_id,
                .node_id = startup.node_id,
                .node_type = viewdemo::ViewNodeType::kMetadata,
                .sequence = sequence,
                .observation = observation,
            });

        if (!result.transport_ok())
        {
            ReportViewTargetFailure(
                target,
                "heartbeat",
                "transport status=" +
                    std::to_string(static_cast<int>(result.rpc.grpc_status_code)) +
                    " message=" + result.rpc.grpc_error_message);
            return;
        }
        if (!result.result.ok())
        {
            std::string detail =
                "status=" + std::string(viewdemo::ToString(result.result.summary.status)) +
                " message=" + result.result.summary.message;
            const std::string diagnostics =
                DescribeViewDiagnostics(result.result.diagnostics);
            if (!diagnostics.empty())
            {
                detail += " diagnostics=" + diagnostics;
            }
            ReportViewTargetFailure(target, "heartbeat", detail);
            if (result.result.summary.status == viewdemo::ViewRegistryStatusCode::kNotFound ||
                result.result.summary.status == viewdemo::ViewRegistryStatusCode::kConflict)
            {
                target->registered = false;
                target->next_sequence = 1;
            }
            return;
        }

        ClearViewTargetFailure(target, "heartbeat");
        target->registered = true;
        if (result.result.applied || result.result.idempotent ||
            result.result.stale_ignored)
        {
            const std::uint64_t accepted_sequence =
                result.result.accepted_sequence == 0
                    ? sequence
                    : result.result.accepted_sequence;
            target->next_sequence = accepted_sequence + 1;
        }
        else
        {
            target->next_sequence = sequence + 1;
        }
    }

    [[nodiscard]] int Run(const ParsedArgs &args)
    {
        const auto loaded_config =
            clusterdemo::LoadClusterConfigFromJsonFile(args.config_path);
        if (!loaded_config.ok())
        {
            std::cerr << "metadata_node_app config error: "
                      << loaded_config.error_detail << '\n';
            return static_cast<int>(ExitCode::kConfigError);
        }

        MetadataNodeStartupConfig startup;
        try
        {
            startup = ResolveStartupConfig(*loaded_config.config, args);
        }
        catch (const std::exception &ex)
        {
            std::cerr << "metadata_node_app config error: " << ex.what() << '\n';
            return static_cast<int>(ExitCode::kConfigError);
        }

        if (!IsValidEndpoint(startup.listen_endpoint))
        {
            std::cerr << "metadata_node_app config error: resolved endpoint is invalid: "
                      << startup.listen_endpoint << '\n';
            return static_cast<int>(ExitCode::kConfigError);
        }

        clusterdemo::NodeIdentity identity;
        try
        {
            identity = EnsureNodeIdentity(startup);
        }
        catch (const IdentityStartupError &ex)
        {
            std::cerr << ex.what() << '\n';
            return static_cast<int>(MapIdentityExitCode(ex.status()));
        }

        clusterdemo::ProcessIncarnation incarnation;
        try
        {
            incarnation = EnsureProcessIncarnation(identity);
        }
        catch (const IdentityStartupError &ex)
        {
            std::cerr << ex.what() << '\n';
            return static_cast<int>(MapIdentityExitCode(ex.status()));
        }

        const bool dynamic_join_candidate_mode =
            IsDynamicJoinCandidateMode(startup);
        if (dynamic_join_candidate_mode)
        {
            const auto bootstrap_join = ExecuteDynamicJoinRound(*loaded_config.config,
                                                                startup,
                                                                identity,
                                                                incarnation);
            if (!bootstrap_join.accepted || !bootstrap_join.accepted_attempt.has_value())
            {
                std::cerr << "metadata_node_app dynamic join failed: no metadata leader accepted join admission"
                          << " cluster_id=" << startup.cluster_id
                          << " node_id=" << identity.node_id
                          << " raft_id=" << startup.raft_id
                          << " discovery_source="
                          << (bootstrap_join.used_view_discovery ? "view_candidates"
                                                                : "config_seed_fallback");
                if (bootstrap_join.last_not_leader.has_value() &&
                    bootstrap_join.last_not_leader->response.summary().has_leader_hint())
                {
                    std::cerr << " last_leader_hint_id="
                              << bootstrap_join.last_not_leader->response.summary().leader_hint().leader_id()
                              << " last_leader_hint_address="
                              << bootstrap_join.last_not_leader->response.summary().leader_hint().leader_address();
                }
                if (!bootstrap_join.diagnostics.empty())
                {
                    std::cerr << " attempts=[";
                    for (std::size_t index = 0; index < bootstrap_join.diagnostics.size(); ++index)
                    {
                        if (index > 0)
                        {
                            std::cerr << " | ";
                        }
                        std::cerr << bootstrap_join.diagnostics[index];
                    }
                    std::cerr << "]";
                }
                std::cerr << '\n';
                return static_cast<int>(bootstrap_join.retryable
                                            ? ExitCode::kStartupError
                                            : ExitCode::kConfigError);
            }

            std::cout << "metadata_node_app candidate join bootstrap"
                      << " cluster_id=" << startup.cluster_id
                      << " node_id=" << identity.node_id
                      << " raft_id=" << startup.raft_id
                      << " endpoint=" << startup.listen_endpoint
                      << " identity_membership_state="
                      << clusterdemo::ToString(identity.membership_state)
                      << " identity_source="
                      << clusterdemo::ToString(identity.source)
                      << " persistent_generation=" << identity.persistent_generation
                      << " incarnation_id=" << incarnation.incarnation_id
                      << " discovery_source="
                      << (bootstrap_join.used_view_discovery ? "view_candidates"
                                                            : "config_seed_fallback")
                      << " result=" << DescribeJoinAttempt(*bootstrap_join.accepted_attempt)
                      << '\n';
        }

        const raftdemo::NodeConfig node_config =
            BuildRaftNodeConfig(*loaded_config.config, startup);
        const raftdemo::snapshotConfig snapshot_config =
            BuildSnapshotConfig(startup);
        std::vector<ViewRegistrationTarget> view_targets =
            BuildViewTargets(*loaded_config.config);

        std::shared_ptr<raftdemo::RaftNode> node;
        try
        {
            node = std::make_shared<raftdemo::RaftNode>(node_config, snapshot_config);
            node->Start();
        }
        catch (const std::exception &ex)
        {
            std::cerr << "metadata_node_app startup error: " << ex.what() << '\n';
            return static_cast<int>(ExitCode::kStartupError);
        }

        // ViewNode registration / heartbeat 只上报 discovery / observation facts。
        // 即使 ViewNode 不可用，也不能反向改变 metadata authority、membership 或 quorum。
        for (auto &target : view_targets)
        {
            (void)EnsureRegisteredWithViewNode(*loaded_config.config,
                                              startup,
                                              node,
                                              &target);
        }

        std::cout << "metadata_node_app OK"
                  << " cluster_id=" << startup.cluster_id
                  << " node_id=" << identity.node_id
                  << " raft_id=" << startup.raft_id
                  << " endpoint=" << startup.listen_endpoint
                  << " data_dir=" << startup.data_dir.generic_string()
                  << " snapshot_dir=" << startup.snapshot_dir.generic_string()
                  << " initial_role=" << clusterdemo::ToString(startup.initial_role)
                  << " initial_voters=" << startup.initial_quorum.voter_count
                  << " initial_commit_quorum=" << startup.initial_quorum.commit_quorum
                  << " identity_membership_state="
                  << clusterdemo::ToString(identity.membership_state)
                  << " identity_source=" << clusterdemo::ToString(identity.source)
                  << " incarnation_id=" << incarnation.incarnation_id
                  << '\n';

        std::signal(SIGINT, HandleSignal);
#ifdef SIGTERM
        std::signal(SIGTERM, HandleSignal);
#endif

        std::thread wait_thread([&node]() {
            node->Wait();
        });

        const auto heartbeat_interval =
            loaded_config.config->timeouts.heartbeat_interval;
        std::thread view_registration_thread([&loaded_config, &startup, &node, &view_targets, heartbeat_interval]() {
            while (!g_stop_requested.load())
            {
                for (auto &target : view_targets)
                {
                    if (!target.registered)
                    {
                        const bool registered =
                            EnsureRegisteredWithViewNode(*loaded_config.config,
                                                         startup,
                                                         node,
                                                         &target);
                        if (!registered)
                        {
                            continue;
                        }
                    }

                    SendHeartbeatToViewNode(*loaded_config.config,
                                            startup,
                                            node,
                                            &target);
                }

                if (heartbeat_interval > std::chrono::milliseconds::zero())
                {
                    std::this_thread::sleep_for(heartbeat_interval);
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }
        });

        std::thread candidate_join_thread;
        if (dynamic_join_candidate_mode)
        {
            candidate_join_thread = std::thread([&loaded_config, &startup, &identity, &incarnation, heartbeat_interval]() {
                std::string last_join_state;
                while (!g_stop_requested.load())
                {
                    const auto join_round = ExecuteDynamicJoinRound(*loaded_config.config,
                                                                    startup,
                                                                    identity,
                                                                    incarnation);
                    std::string next_join_state;
                    if (join_round.accepted && join_round.accepted_attempt.has_value())
                    {
                        next_join_state =
                            DescribeJoinAttempt(*join_round.accepted_attempt);
                        if (next_join_state != last_join_state)
                        {
                            std::cout << "metadata_node_app candidate join status"
                                      << " cluster_id=" << startup.cluster_id
                                      << " node_id=" << identity.node_id
                                      << " raft_id=" << startup.raft_id
                                      << " endpoint=" << startup.listen_endpoint
                                      << " result=" << next_join_state
                                      << '\n';
                            last_join_state = next_join_state;
                        }
                    }
                    else if (!join_round.diagnostics.empty())
                    {
                        std::ostringstream oss;
                        for (std::size_t index = 0; index < join_round.diagnostics.size(); ++index)
                        {
                            if (index > 0)
                            {
                                oss << " | ";
                            }
                            oss << join_round.diagnostics[index];
                        }
                        next_join_state = oss.str();
                        if (next_join_state != last_join_state)
                        {
                            std::cerr << "metadata_node_app candidate join warning"
                                      << " cluster_id=" << startup.cluster_id
                                      << " node_id=" << identity.node_id
                                      << " raft_id=" << startup.raft_id
                                      << " attempts=[" << next_join_state << "]"
                                      << '\n';
                            last_join_state = next_join_state;
                        }
                    }

                    if (heartbeat_interval > std::chrono::milliseconds::zero())
                    {
                        std::this_thread::sleep_for(heartbeat_interval);
                    }
                    else
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }
                }
            });
        }

        while (!g_stop_requested.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (view_registration_thread.joinable())
        {
            view_registration_thread.join();
        }
        if (candidate_join_thread.joinable())
        {
            candidate_join_thread.join();
        }
        node->Stop();
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
        std::cerr << "metadata_node_app argument error: " << ex.what() << '\n';
        PrintUsage(std::cerr);
        return static_cast<int>(ExitCode::kInvalidArgument);
    }
}
