#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clusterdemo
{
    using ClusterId = std::string;
    using ClusterNodeId = std::string;

    enum class ClusterNodeType : std::uint8_t
    {
        kUnknown = 0,
        kView = 1,
        kMetadata = 2,
        kStorage = 3,
    };

    enum class MetadataNodeInitialRole : std::uint8_t
    {
        kUnknown = 0,
        kVoter = 1,
        kLearner = 2,
    };

    enum class ClusterChecksumAlgorithm : std::uint8_t
    {
        kUnknown = 0,
        kSha256 = 1,
    };

    // validation issue 只表达配置、身份和 durability contract 的错误边界，
    // 不承载 Raft 共识、对象可见性或真实 payload 处理结果。
    enum class ClusterConfigIssueCode : std::uint16_t
    {
        kUnknown = 0,
        kMissingClusterId = 1,
        kInvalidNodeCount = 2,
        kInvalidNodeType = 3,
        kInvalidNodeId = 4,
        kDuplicateNodeId = 5,
        kInvalidEndpoint = 6,
        kDuplicateEndpoint = 7,
        kMissingDataDir = 8,
        kMissingSnapshotDir = 9,
        kSharedDataDir = 10,
        kInvalidCapacity = 11,
        kInvalidChunkPolicy = 12,
        kInvalidTimeoutPolicy = 13,
        kInvalidRaftId = 14,
        kDuplicateRaftId = 15,
        kInvalidRaftVoterCount = 16,
        kInvalidInitialMembership = 17,
        kIdentityConfigMismatch = 18,
        kUnsupportedDurabilityMode = 19,
    };

    enum class ClusterConfigStatusCode : std::uint8_t
    {
        kOk = 0,
        kInvalidArgument = 1,
        kConflict = 2,
        kUnsupported = 3,
        kInternalError = 4,
    };

    struct FailureDomainConfig
    {
        std::string zone;
        std::string rack;
    };

    struct ChunkPolicyConfig
    {
        std::uint64_t chunk_size_bytes{0};
        std::uint32_t replica_count{0};
        std::uint32_t minimum_successful_writes{0};
        ClusterChecksumAlgorithm checksum_algorithm{
            ClusterChecksumAlgorithm::kUnknown};
    };

    struct ClusterTimeoutConfig
    {
        std::chrono::milliseconds discovery_rpc_timeout{0};
        std::chrono::milliseconds metadata_rpc_timeout{0};
        std::chrono::milliseconds storage_rpc_timeout{0};
        std::chrono::milliseconds heartbeat_interval{0};
        std::chrono::milliseconds registration_timeout{0};
        std::chrono::milliseconds commit_deadline{0};
        std::chrono::milliseconds liveness_stale_timeout{0};
        std::chrono::milliseconds liveness_dead_timeout{0};
    };

    struct ViewNodeConfig
    {
        // 允许为空；后续可由配置生成器或 identity 流程补全稳定 node_id。
        std::optional<ClusterNodeId> node_id;
        std::string endpoint;
        std::filesystem::path data_dir;
    };

    struct MetadataNodeConfig
    {
        ClusterNodeId node_id;
        std::int32_t raft_id{0};
        std::string endpoint;
        std::filesystem::path data_dir;
        std::filesystem::path snapshot_dir;
        MetadataNodeInitialRole initial_role{
            MetadataNodeInitialRole::kUnknown};
    };

    struct StorageNodeConfig
    {
        // 允许为空；StorageNode 首次启动时可通过 node.identity / 分配流程获得身份。
        std::optional<ClusterNodeId> node_id;
        std::string endpoint;
        std::filesystem::path data_dir;
        std::uint64_t capacity_bytes{0};
        FailureDomainConfig failure_domain;
    };

    // 这里只描述初始 membership 配置边界；运行时 membership change
    // 仍必须通过 Raft leader 提交并在已提交日志中生效。
    struct InitialRaftMembershipConfig
    {
        std::vector<std::int32_t> voter_raft_ids;
        std::vector<std::int32_t> learner_raft_ids;
        std::uint64_t membership_epoch{0};
    };

    struct ClusterConfig
    {
        ClusterId cluster_id;
        std::filesystem::path base_dir;
        std::vector<ViewNodeConfig> view_nodes;
        std::vector<MetadataNodeConfig> metadata_nodes;
        std::vector<StorageNodeConfig> storage_nodes;
        InitialRaftMembershipConfig initial_raft_membership;
        ChunkPolicyConfig chunk_policy;
        ClusterTimeoutConfig timeouts;
    };

    struct ClusterConfigValidationIssue
    {
        ClusterConfigIssueCode code{ClusterConfigIssueCode::kUnknown};
        std::string field_path;
        std::string message;
        ClusterNodeType node_type{ClusterNodeType::kUnknown};
        ClusterNodeId node_id;
        std::string endpoint;
        std::filesystem::path path;
    };

    struct ClusterConfigValidationResult
    {
        std::vector<ClusterConfigValidationIssue> issues;

        [[nodiscard]] bool ok() const
        {
            return issues.empty();
        }
    };

    // generation request 用于 T011 的确定性配置生成。
    // 具体 endpoint / data_dir / snapshot_dir 的展开规则由实现决定，
    // 但相同输入应生成可重复、可诊断的结果。
    struct ClusterConfigGenerationRequest
    {
        ClusterId cluster_id;
        std::filesystem::path base_dir;
        std::string bind_host;
        std::string advertise_host;
        std::size_t view_node_count{0};
        std::size_t metadata_node_count{0};
        std::size_t metadata_voter_count{0};
        std::size_t storage_node_count{0};
        std::uint16_t view_port_base{0};
        std::uint16_t metadata_port_base{0};
        std::uint16_t storage_port_base{0};
        std::uint64_t default_storage_capacity_bytes{0};
        ChunkPolicyConfig chunk_policy;
        ClusterTimeoutConfig timeouts;
        std::vector<ClusterNodeId> fixed_view_node_ids;
        std::vector<ClusterNodeId> fixed_metadata_node_ids;
        std::vector<std::int32_t> fixed_metadata_raft_ids;
        std::vector<ClusterNodeId> fixed_storage_node_ids;
        std::vector<std::uint64_t> storage_capacity_overrides_bytes;
        std::optional<std::uint64_t> generation_seed;
    };

    struct ClusterConfigGenerationResult
    {
        ClusterConfigStatusCode status{ClusterConfigStatusCode::kOk};
        std::string error_detail;
        ClusterConfig config;
        ClusterConfigValidationResult validation;

        [[nodiscard]] bool ok() const
        {
            return status == ClusterConfigStatusCode::kOk &&
                   validation.ok();
        }
    };

    struct ClusterEndpointAssignment
    {
        ClusterNodeType node_type{ClusterNodeType::kUnknown};
        ClusterNodeId node_id;
        std::size_t ordinal{0};
        std::string endpoint;
    };

    struct ClusterEndpointAllocationResult
    {
        ClusterConfigStatusCode status{ClusterConfigStatusCode::kOk};
        std::string error_detail;
        std::vector<ClusterEndpointAssignment> assignments;
        ClusterConfigValidationResult validation;

        [[nodiscard]] bool ok() const
        {
            return status == ClusterConfigStatusCode::kOk &&
                   validation.ok();
        }
    };

    struct ResolvedClusterNodeConfig
    {
        ClusterNodeType node_type{ClusterNodeType::kUnknown};
        ClusterNodeId node_id;
        std::string endpoint;
        std::filesystem::path data_dir;
        std::optional<std::filesystem::path> snapshot_dir;
        std::optional<std::int32_t> raft_id;
        std::optional<MetadataNodeInitialRole> metadata_initial_role;
        std::optional<std::uint64_t> capacity_bytes;
        FailureDomainConfig failure_domain;
    };

    struct ClusterNodeResolutionResult
    {
        ClusterConfigStatusCode status{ClusterConfigStatusCode::kOk};
        std::string error_detail;
        std::optional<ResolvedClusterNodeConfig> resolved;
        ClusterConfigValidationResult validation;

        [[nodiscard]] bool ok() const
        {
            return status == ClusterConfigStatusCode::kOk &&
                   validation.ok() &&
                   resolved.has_value();
        }
    };

    struct InitialRaftQuorumSummary
    {
        std::size_t voter_count{0};
        std::size_t election_quorum{0};
        std::size_t commit_quorum{0};
        std::vector<std::int32_t> voter_raft_ids;
    };

    struct InitialRaftQuorumComputationResult
    {
        ClusterConfigStatusCode status{ClusterConfigStatusCode::kOk};
        std::string error_detail;
        std::optional<InitialRaftQuorumSummary> summary;
        ClusterConfigValidationResult validation;

        [[nodiscard]] bool ok() const
        {
            return status == ClusterConfigStatusCode::kOk &&
                   validation.ok() &&
                   summary.has_value();
        }
    };

    [[nodiscard]] ClusterConfigValidationResult ValidateClusterConfig(
        const ClusterConfig &config);

    [[nodiscard]] ClusterConfigValidationResult ValidateInitialRaftMembership(
        const ClusterConfig &config);

    // endpoint 分配只基于配置输入生成稳定结果，不负责 app startup、
    // 真实节点启动或运行时服务发现。
    [[nodiscard]] ClusterEndpointAllocationResult AllocateClusterEndpoints(
        const ClusterConfigGenerationRequest &request);

    [[nodiscard]] ClusterConfigGenerationResult GenerateDeterministicClusterConfig(
        const ClusterConfigGenerationRequest &request);

    // 单节点解析必须显式按 role + node_id 命中，不允许静默 fallback
    // 到“第一个节点”或任何默认 demo 拓扑。
    [[nodiscard]] ClusterNodeResolutionResult ResolveClusterNodeConfig(
        const ClusterConfig &config,
        ClusterNodeType node_type,
        std::string_view node_id);

    // 只根据 initial voter membership 计算 quorum，用于配置校验、测试和诊断；
    // 它不是运行时 Raft membership authority，也不会改变 election / commit 行为。
    [[nodiscard]] InitialRaftQuorumComputationResult ComputeInitialRaftQuorum(
        const InitialRaftMembershipConfig &membership);

    [[nodiscard]] InitialRaftQuorumComputationResult ComputeInitialRaftQuorum(
        const ClusterConfig &config);

    // quorum 必须基于初始 voter 总数计算，不因当前存活节点减少而缩小。
    [[nodiscard]] std::size_t ComputeInitialRaftQuorumSize(
        std::size_t voter_count);

    [[nodiscard]] std::size_t ComputeInitialRaftQuorumSize(
        const InitialRaftMembershipConfig &membership);

    // 生成器输出的文本配置只描述 cluster/config 边界，不承载 app startup
    // 或运行时 discovery / quorum authority。
    [[nodiscard]] std::string SerializeClusterConfigToJson(
        const ClusterConfig &config);

    [[nodiscard]] const char *ToString(ClusterNodeType node_type);
    [[nodiscard]] const char *ToString(MetadataNodeInitialRole role);
    [[nodiscard]] const char *ToString(ClusterChecksumAlgorithm algorithm);
    [[nodiscard]] const char *ToString(ClusterConfigIssueCode code);
    [[nodiscard]] const char *ToString(ClusterConfigStatusCode code);

    [[nodiscard]] std::string DescribeClusterConfigIssue(
        const ClusterConfigValidationIssue &issue);

} // namespace clusterdemo
