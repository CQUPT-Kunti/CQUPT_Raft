#pragma once

#include "cluster/cluster_config.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clusterdemo
{
    inline constexpr std::uint32_t kNodeIdentityCurrentVersion{2};
    inline constexpr std::string_view kNodeIdentityFileName{"node.identity"};
    inline constexpr std::uint64_t kProcessIncarnationInitialSequence{1};

    enum class NodeIdentitySource : std::uint8_t
    {
        kUnknown = 0,
        kConfigGenerator = 1,
        kViewNodeAllocator = 2,
        kExplicitOverride = 3,
    };

    enum class NodeIdentityStatusCode : std::uint8_t
    {
        kOk = 0,
        kNotFound = 1,
        kInvalidArgument = 2,
        kConflict = 3,
        kCorrupt = 4,
        kUnsupported = 5,
        kIoError = 6,
        kDurabilityError = 7,
        kInternalError = 8,
    };

    enum class NodeIdentityMembershipState : std::uint8_t
    {
        kUnknown = 0,
        kNonRaft = 1,
        kJoining = 2,
        kCandidate = 3,
        kLearner = 4,
        kVoter = 5,
    };

    // issue code 只描述身份文件和配置匹配边界，不表达 Raft membership
    // 变更、对象可见性或 StorageNode payload 状态。
    enum class NodeIdentityIssueCode : std::uint16_t
    {
        kUnknown = 0,
        kMissingClusterId = 1,
        kMissingNodeId = 2,
        kInvalidNodeType = 3,
        kMissingRaftId = 4,
        kUnexpectedRaftId = 5,
        kInvalidIdentityVersion = 6,
        kUnsupportedIdentityVersion = 7,
        kIdentityFileNotFound = 8,
        kIdentityFileCorrupt = 9,
        kClusterIdMismatch = 10,
        kNodeTypeMismatch = 11,
        kNodeIdMismatch = 12,
        kRaftIdMismatch = 13,
        kSourceMismatch = 14,
        kExistingIdentityConflict = 15,
        kUnsupportedDurabilityMode = 16,
        kDurabilityPublishFailed = 17,
        kIoFailure = 18,
        kInvalidMembershipState = 19,
        kMembershipStateMismatch = 20,
        kInvalidPersistentGeneration = 21,
    };

    enum class NodeIdentityDurabilityMode : std::uint8_t
    {
        kRequired = 0,
        kBestEffortForTests = 1,
    };

    enum class NodeIdentityStoreMode : std::uint8_t
    {
        kCreateNewOnly = 0,
        kReplaceOnlyIfMatchesExpected = 1,
    };

    // 节点本地持久身份。MetadataNode 使用 node_id 表示集群身份，
    // 使用 raft_id 表示 Raft membership 身份；StorageNode / ViewNode 不应携带 raft_id。
    struct NodeIdentity
    {
        ClusterId cluster_id;
        ClusterNodeId node_id;
        ClusterNodeType node_type{ClusterNodeType::kUnknown};
        std::optional<std::int32_t> raft_id;
        NodeIdentityMembershipState membership_state{
            NodeIdentityMembershipState::kUnknown};
        std::uint64_t persistent_generation{1};
        std::uint32_t identity_version{kNodeIdentityCurrentVersion};
        std::int64_t created_at_unix_ms{0};
        NodeIdentitySource source{NodeIdentitySource::kUnknown};
    };

    // 启动配置对本地 identity 的期望。字段为空表示该维度不由调用方约束；
    // 已存在 identity 与非空期望冲突时必须显式失败，不能静默覆盖。
    struct ExpectedNodeIdentity
    {
        std::optional<ClusterId> cluster_id;
        std::optional<ClusterNodeId> node_id;
        ClusterNodeType node_type{ClusterNodeType::kUnknown};
        std::optional<std::int32_t> raft_id;
        std::optional<NodeIdentityMembershipState> membership_state;
        std::optional<NodeIdentitySource> source;
        bool require_raft_id_for_metadata{true};
        bool forbid_raft_id_for_non_metadata{true};
    };

    struct NodeIdentityIssue
    {
        NodeIdentityIssueCode code{NodeIdentityIssueCode::kUnknown};
        std::string field_path;
        std::string message;
        ClusterNodeType node_type{ClusterNodeType::kUnknown};
        ClusterNodeId node_id;
        std::optional<std::int32_t> raft_id;
        std::filesystem::path path;
    };

    struct NodeIdentityValidationResult
    {
        std::vector<NodeIdentityIssue> issues;

        [[nodiscard]] bool ok() const
        {
            return issues.empty();
        }
    };

    struct NodeIdentityLoadOptions
    {
        std::filesystem::path data_dir;
        ExpectedNodeIdentity expected;
        bool require_existing{false};
    };

    struct NodeIdentityLoadResult
    {
        NodeIdentityStatusCode status{NodeIdentityStatusCode::kOk};
        std::optional<NodeIdentity> identity;
        std::filesystem::path identity_path;
        NodeIdentityValidationResult validation;
        std::string diagnostic;

        [[nodiscard]] bool ok() const
        {
            return status == NodeIdentityStatusCode::kOk &&
                   identity.has_value() && validation.ok();
        }
    };

    struct NodeIdentityStoreOptions
    {
        std::filesystem::path data_dir;
        NodeIdentityDurabilityMode durability_mode{
            NodeIdentityDurabilityMode::kRequired};
        NodeIdentityStoreMode store_mode{
            NodeIdentityStoreMode::kCreateNewOnly};
        ExpectedNodeIdentity expected_existing;
    };

    struct NodeIdentityStoreResult
    {
        NodeIdentityStatusCode status{NodeIdentityStatusCode::kOk};
        std::optional<NodeIdentity> identity;
        std::filesystem::path identity_path;
        NodeIdentityValidationResult validation;
        bool created{false};
        bool replaced{false};
        bool durable{false};
        std::string diagnostic;

        [[nodiscard]] bool ok() const
        {
            return status == NodeIdentityStatusCode::kOk &&
                   validation.ok();
        }
    };

    struct NodeIdentityLoadOrCreateRequest
    {
        NodeIdentityLoadOptions load_options;
        NodeIdentity identity_to_create;
        NodeIdentityStoreOptions store_options;
    };

    struct NodeIdentityLoadOrCreateResult
    {
        NodeIdentityStatusCode status{NodeIdentityStatusCode::kOk};
        std::optional<NodeIdentity> identity;
        NodeIdentityValidationResult validation;
        bool loaded_existing{false};
        bool created_new{false};
        bool durable{false};
        std::filesystem::path identity_path;
        std::string diagnostic;

        [[nodiscard]] bool ok() const
        {
            return status == NodeIdentityStatusCode::kOk &&
                   identity.has_value() && validation.ok();
        }
    };

    // 单次进程启动实例身份。它绑定长期 node_id，但不写回 node.identity，
    // 也不表达 Raft membership authority。
    struct ProcessIncarnation
    {
        ClusterId cluster_id;
        ClusterNodeId node_id;
        ClusterNodeType node_type{ClusterNodeType::kUnknown};
        std::string incarnation_id;
        std::int64_t started_at_unix_ms{0};
        std::uint64_t startup_sequence_base{
            kProcessIncarnationInitialSequence};
    };

    struct ProcessIncarnationResult
    {
        NodeIdentityStatusCode status{NodeIdentityStatusCode::kOk};
        std::optional<ProcessIncarnation> incarnation;
        NodeIdentityValidationResult validation;
        std::string diagnostic;

        [[nodiscard]] bool ok() const
        {
            return status == NodeIdentityStatusCode::kOk &&
                   incarnation.has_value() && validation.ok();
        }
    };

    [[nodiscard]] std::filesystem::path ResolveNodeIdentityPath(
        const std::filesystem::path &data_dir);

    [[nodiscard]] NodeIdentityValidationResult ValidateNodeIdentity(
        const NodeIdentity &identity);

    [[nodiscard]] NodeIdentityValidationResult ValidateNodeIdentityMatches(
        const NodeIdentity &identity,
        const ExpectedNodeIdentity &expected);

    // 以下接口声明 009 阶段 durable identity 的 load/store/load-or-create 边界。
    // 当前实现已覆盖解析、临时文件写入、flush、atomic publish、restart
    // validation、目录 durability，以及基于 durable identity 的 process
    // incarnation / boot epoch 生成边界。
    [[nodiscard]] NodeIdentityLoadResult LoadNodeIdentity(
        const NodeIdentityLoadOptions &options);

    [[nodiscard]] NodeIdentityStoreResult StoreNodeIdentity(
        const NodeIdentity &identity,
        const NodeIdentityStoreOptions &options);

    [[nodiscard]] NodeIdentityLoadOrCreateResult LoadOrCreateNodeIdentity(
        const NodeIdentityLoadOrCreateRequest &request);

    // T013 只生成单次进程启动实例身份，不修改长期 durable identity。
    // 调用方必须先成功 load/create NodeIdentity，再据此生成 incarnation。
    [[nodiscard]] ProcessIncarnationResult CreateProcessIncarnation(
        const NodeIdentity &identity);

    [[nodiscard]] const char *ToString(NodeIdentitySource source);
    [[nodiscard]] const char *ToString(NodeIdentityMembershipState state);
    [[nodiscard]] const char *ToString(NodeIdentityStatusCode code);
    [[nodiscard]] const char *ToString(NodeIdentityIssueCode code);
    [[nodiscard]] const char *ToString(NodeIdentityDurabilityMode mode);
    [[nodiscard]] const char *ToString(NodeIdentityStoreMode mode);

    [[nodiscard]] std::string DescribeNodeIdentityIssue(
        const NodeIdentityIssue &issue);

} // namespace clusterdemo
