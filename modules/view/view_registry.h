#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewdemo
{
    using ClusterId = std::string;
    using NodeId = std::string;
    using RequestId = std::string;
    using Endpoint = std::string;

    enum class ViewNodeType : std::uint8_t
    {
        kUnknown = 0,
        kView = 1,
        kMetadata = 2,
        kStorage = 3,
    };

    enum class ViewNodeLivenessState : std::uint8_t
    {
        kUnknown = 0,
        kLive = 1,
        kStale = 2,
        kSuspect = 3,
        kDead = 4,
    };

    enum class ViewNodeHealth : std::uint8_t
    {
        kUnknown = 0,
        kHealthy = 1,
        kDegraded = 2,
        kReadOnly = 3,
        kDraining = 4,
        kUnavailable = 5,
    };

    enum class ViewNodeDiskPressure : std::uint8_t
    {
        kUnknown = 0,
        kLow = 1,
        kMedium = 2,
        kHigh = 3,
        kFull = 4,
    };

    // 仅表示 ViewNode 观测到的 MetadataNode membership 状态。
    // 这里不是 Raft membership authority，不能据此修改 voter 集合。
    enum class MetadataMembershipObservedState : std::uint8_t
    {
        kUnknown = 0,
        kRegistered = 1,
        kJoining = 2,
        kLearner = 3,
        kVoter = 4,
        kDown = 5,
    };

    enum class MetadataRaftObservedRole : std::uint8_t
    {
        kUnknown = 0,
        kFollower = 1,
        kCandidate = 2,
        kLeader = 3,
        kLearner = 4,
        kObserver = 5,
    };

    enum class ViewRegistryStatusCode : std::uint8_t
    {
        kOk = 0,
        kIdempotentReplay = 1,
        kInvalidArgument = 2,
        kNotFound = 3,
        kConflict = 4,
        kStaleIgnored = 5,
        kInternalError = 6,
        kTimeout = 7,
        kOverloaded = 8,
        kServiceUnavailable = 9,
        kUnsupported = 10,
    };

    enum class ViewRegistryIssueCode : std::uint16_t
    {
        kUnknown = 0,
        kMissingClusterId = 1,
        kMissingNodeId = 2,
        kInvalidNodeType = 3,
        kMissingEndpoint = 4,
        kEndpointConflict = 5,
        kNodeIdConflict = 6,
        kClusterMismatch = 7,
        kNodeTypeMismatch = 8,
        kDataDirFingerprintConflict = 9,
        kStaleHeartbeat = 10,
        kNodeUnavailable = 11,
        kLivenessExcluded = 12,
        kCapacityInsufficient = 13,
        kHealthExcluded = 14,
        kLeaderHintStale = 15,
        kNonAuthorityBoundary = 16,
    };

    [[nodiscard]] inline bool IsSuccessfulStatus(
        ViewRegistryStatusCode status)
    {
        return status == ViewRegistryStatusCode::kOk ||
               status == ViewRegistryStatusCode::kIdempotentReplay ||
               status == ViewRegistryStatusCode::kStaleIgnored;
    }

    struct ViewRegistryFailureDomain
    {
        std::string zone;
        std::string rack;
    };

    struct ViewRegistryCapacityReport
    {
        std::uint64_t total_capacity_bytes{0};
        std::uint64_t used_capacity_bytes{0};
        std::uint64_t available_capacity_bytes{0};
        std::uint64_t chunk_count{0};
    };

    struct ViewRegistryHealthReport
    {
        ViewNodeHealth health{ViewNodeHealth::kUnknown};
        ViewNodeDiskPressure disk_pressure{ViewNodeDiskPressure::kUnknown};
        std::uint64_t io_error_count{0};
    };

    struct ViewRegistryLoadReport
    {
        std::uint32_t active_reads{0};
        std::uint32_t active_writes{0};
        std::uint32_t queued_ops{0};
        bool write_admission_overloaded{false};
        bool read_admission_overloaded{false};
    };

    // leader hint 是减少 Client 重试的观测提示。
    // Client 仍必须处理 MetadataNode 的 NOT_LEADER / quorum failure。
    struct MetadataLeaderHint
    {
        NodeId node_id;
        std::optional<std::int32_t> raft_id;
        Endpoint endpoint;
        std::uint64_t observed_term{0};
        std::uint64_t observed_at_unix_ms{0};
    };

    struct MetadataNodeObservation
    {
        std::optional<std::int32_t> raft_id;
        MetadataRaftObservedRole raft_role{
            MetadataRaftObservedRole::kUnknown};
        MetadataMembershipObservedState membership_state{
            MetadataMembershipObservedState::kUnknown};
        std::optional<MetadataLeaderHint> leader_hint;
        std::uint64_t observed_term{0};
        std::uint64_t commit_index{0};
        std::uint64_t membership_epoch{0};
    };

    // 节点注册只表达 discovery / observation facts。
    // 不包含 object manifest、chunk payload 或 Raft membership 写入意图。
    struct NodeRegistration
    {
        ClusterId cluster_id;
        NodeId node_id;
        ViewNodeType node_type{ViewNodeType::kUnknown};
        Endpoint endpoint;
        Endpoint control_plane_endpoint;
        Endpoint data_plane_endpoint;
        std::string data_dir_fingerprint;
        std::uint64_t observed_at_unix_ms{0};
        ViewRegistryFailureDomain failure_domain;
        ViewRegistryHealthReport health;
        ViewRegistryCapacityReport capacity;
        ViewRegistryLoadReport load;
        std::optional<MetadataNodeObservation> metadata;
    };

    struct ViewNodeSnapshot
    {
        ClusterId cluster_id;
        NodeId node_id;
        ViewNodeType node_type{ViewNodeType::kUnknown};
        Endpoint endpoint;
        Endpoint control_plane_endpoint;
        Endpoint data_plane_endpoint;
        std::string data_dir_fingerprint;
        std::uint64_t registered_at_unix_ms{0};
        std::uint64_t last_seen_unix_ms{0};
        std::uint64_t last_sequence{0};
        ViewNodeLivenessState liveness{ViewNodeLivenessState::kUnknown};
        ViewRegistryFailureDomain failure_domain;
        ViewRegistryHealthReport health;
        ViewRegistryCapacityReport capacity;
        ViewRegistryLoadReport load;
        std::optional<MetadataNodeObservation> metadata;
    };

    struct ViewRegistryDiagnostic
    {
        ViewRegistryIssueCode code{ViewRegistryIssueCode::kUnknown};
        std::string message;
        RequestId request_id;
        ClusterId cluster_id;
        NodeId node_id;
        Endpoint endpoint;
        std::uint64_t sequence{0};
    };

    struct ViewRegistryResponseSummary
    {
        ViewRegistryStatusCode status{ViewRegistryStatusCode::kOk};
        std::string message;
        RequestId request_id;
        ClusterId cluster_id;
        NodeId node_id;
        std::uint64_t retry_after_ms{0};

        [[nodiscard]] bool ok() const
        {
            return IsSuccessfulStatus(status);
        }
    };

    struct ViewRegistryConfig
    {
        std::chrono::milliseconds stale_timeout{30'000};
        std::chrono::milliseconds suspect_timeout{60'000};
        std::chrono::milliseconds dead_timeout{90'000};
        bool enforce_unique_endpoints{true};
        bool keep_dead_nodes_for_cluster_view{true};
    };

    struct RegisterNodeRequest
    {
        RequestId request_id;
        NodeRegistration registration;
    };

    struct RegisterNodeResult
    {
        ViewRegistryResponseSummary summary;
        bool created{false};
        bool idempotent{false};
        bool conflict{false};
        std::optional<ViewNodeSnapshot> snapshot;
        std::vector<ViewRegistryDiagnostic> diagnostics;

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    struct HeartbeatNodeRequest
    {
        RequestId request_id;
        ClusterId cluster_id;
        NodeId node_id;
        ViewNodeType node_type{ViewNodeType::kUnknown};
        std::uint64_t sequence{0};
        NodeRegistration observation;
    };

    struct HeartbeatNodeResult
    {
        ViewRegistryResponseSummary summary;
        std::uint64_t accepted_sequence{0};
        bool applied{false};
        bool idempotent{false};
        bool stale_ignored{false};
        std::optional<ViewNodeSnapshot> snapshot;
        std::vector<ViewRegistryDiagnostic> diagnostics;

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    struct LookupNodeResult
    {
        ViewRegistryResponseSummary summary;
        std::optional<ViewNodeSnapshot> snapshot;
        std::vector<ViewRegistryDiagnostic> diagnostics;

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    struct DiscoverMetadataRequest
    {
        RequestId request_id;
        ClusterId cluster_id;
        bool prefer_leader{true};
        bool live_only{true};
        // 0 表示不限制返回数量。
        std::uint32_t limit{0};
    };

    struct DiscoverMetadataResult
    {
        ViewRegistryResponseSummary summary;
        std::vector<ViewNodeSnapshot> metadata_nodes;
        std::optional<MetadataLeaderHint> leader_hint;
        std::uint64_t observed_at_unix_ms{0};
        std::uint64_t membership_epoch{0};
        std::vector<ViewRegistryDiagnostic> diagnostics;

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    struct DiscoverStorageRequest
    {
        RequestId request_id;
        ClusterId cluster_id;
        bool live_only{true};
        std::uint64_t minimum_available_capacity_bytes{0};
        std::string zone;
        std::string rack;
        // 0 表示不限制返回数量。
        std::uint32_t limit{0};
        bool require_writable{false};
    };

    struct DiscoverStorageResult
    {
        ViewRegistryResponseSummary summary;
        std::vector<ViewNodeSnapshot> storage_nodes;
        std::uint64_t observed_at_unix_ms{0};
        std::vector<ViewRegistryDiagnostic> diagnostics;

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    struct GetClusterViewRequest
    {
        RequestId request_id;
        ClusterId cluster_id;
        bool include_dead_nodes{true};
        bool include_warnings{true};
    };

    struct ClusterViewSnapshot
    {
        std::vector<ViewNodeSnapshot> view_nodes;
        std::vector<ViewNodeSnapshot> metadata_nodes;
        std::vector<ViewNodeSnapshot> storage_nodes;
        std::optional<MetadataLeaderHint> leader_hint;
        std::uint64_t observed_at_unix_ms{0};
        std::vector<ViewRegistryDiagnostic> diagnostics;
    };

    struct GetClusterViewResult
    {
        ViewRegistryResponseSummary summary;
        ClusterViewSnapshot snapshot;

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    // ViewNodeRegistry 是 discovery-only / observation-only registry。
    // T016 在 .cpp 中实现注册幂等、heartbeat sequence 排序、
    // liveness transition 和 discovery snapshot。这里不包含 gRPC 映射。
    class ViewNodeRegistry
    {
    public:
        explicit ViewNodeRegistry(ViewRegistryConfig config = {});
        ~ViewNodeRegistry();

        ViewNodeRegistry(const ViewNodeRegistry &) = delete;
        ViewNodeRegistry &operator=(const ViewNodeRegistry &) = delete;
        ViewNodeRegistry(ViewNodeRegistry &&) noexcept;
        ViewNodeRegistry &operator=(ViewNodeRegistry &&) noexcept;

        RegisterNodeResult RegisterNode(const RegisterNodeRequest &request);

        HeartbeatNodeResult HeartbeatNode(const HeartbeatNodeRequest &request);

        // self refresh 复用与普通 heartbeat 相同的 registry update 语义：
        // 调用方必须提供当前 observed_at_unix_ms 和递增 sequence。
        // 该入口只刷新本节点的 observed state，不绕过 TTL，也不授予
        // 任何 membership authority。
        HeartbeatNodeResult RefreshSelfNode(const HeartbeatNodeRequest &request);

        [[nodiscard]] LookupNodeResult LookupNode(
            std::string_view cluster_id,
            std::string_view node_id,
            std::uint64_t now_unix_ms) const;

        [[nodiscard]] DiscoverMetadataResult DiscoverMetadata(
            const DiscoverMetadataRequest &request,
            std::uint64_t now_unix_ms) const;

        [[nodiscard]] DiscoverStorageResult DiscoverStorage(
            const DiscoverStorageRequest &request,
            std::uint64_t now_unix_ms) const;

        [[nodiscard]] GetClusterViewResult GetClusterView(
            const GetClusterViewRequest &request,
            std::uint64_t now_unix_ms) const;

        [[nodiscard]] std::size_t size() const;
        [[nodiscard]] const ViewRegistryConfig &config() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] const char *ToString(ViewNodeType node_type);
    [[nodiscard]] const char *ToString(ViewNodeLivenessState liveness);
    [[nodiscard]] const char *ToString(ViewNodeHealth health);
    [[nodiscard]] const char *ToString(ViewNodeDiskPressure pressure);
    [[nodiscard]] const char *ToString(
        MetadataMembershipObservedState state);
    [[nodiscard]] const char *ToString(MetadataRaftObservedRole role);
    [[nodiscard]] const char *ToString(ViewRegistryStatusCode status);
    [[nodiscard]] const char *ToString(ViewRegistryIssueCode code);

    [[nodiscard]] std::string DescribeViewRegistryDiagnostic(
        const ViewRegistryDiagnostic &diagnostic);

} // namespace viewdemo
