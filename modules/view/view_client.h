#pragma once

#include "view/view_registry.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <grpcpp/grpcpp.h>

#include "view.grpc.pb.h"

namespace viewdemo
{
    struct ViewNodeClientConfig
    {
        // 0 表示不额外设置 deadline，由调用方或 gRPC 默认策略决定。
        std::chrono::milliseconds register_timeout{0};
        std::chrono::milliseconds heartbeat_timeout{0};
        std::chrono::milliseconds discovery_timeout{0};
        std::chrono::milliseconds cluster_view_timeout{0};
        std::chrono::milliseconds peer_sync_timeout{0};
        bool wait_for_ready{false};
    };

    struct ViewNodeClientCallOptions
    {
        // 非空时覆盖本次 RPC 的默认 timeout。
        std::optional<std::chrono::milliseconds> timeout;
        std::optional<bool> wait_for_ready;
    };

    struct ViewNodeClientCallDiagnostics
    {
        RequestId request_id;
        ClusterId cluster_id;
        NodeId node_id;
        Endpoint target_endpoint;
        grpc::StatusCode grpc_status_code{grpc::StatusCode::OK};
        std::string grpc_error_message;
        std::string grpc_error_details;
        std::chrono::milliseconds effective_timeout{0};
        bool wait_for_ready{false};
        bool retryable{false};

        [[nodiscard]] bool transport_ok() const
        {
            return grpc_status_code == grpc::StatusCode::OK;
        }
    };

    template <typename Result>
    struct ViewNodeClientCallResult
    {
        Result result;
        ViewNodeClientCallDiagnostics rpc;

        [[nodiscard]] bool transport_ok() const
        {
            return rpc.transport_ok();
        }

        [[nodiscard]] bool ok() const
        {
            return transport_ok() && result.ok();
        }
    };

    using ViewNodeClientRegisterNodeResult =
        ViewNodeClientCallResult<RegisterNodeResult>;
    using ViewNodeClientHeartbeatNodeResult =
        ViewNodeClientCallResult<HeartbeatNodeResult>;
    using ViewNodeClientDiscoverMetadataResult =
        ViewNodeClientCallResult<DiscoverMetadataResult>;
    using ViewNodeClientDiscoverStorageResult =
        ViewNodeClientCallResult<DiscoverStorageResult>;
    using ViewNodeClientGetClusterViewResult =
        ViewNodeClientCallResult<GetClusterViewResult>;

    struct ViewPeerSyncSnapshot
    {
        ClusterId cluster_id;
        std::uint64_t generated_at_unix_ms{0};
        std::vector<ViewNodeSnapshot> view_nodes;
        std::vector<ViewNodeSnapshot> metadata_nodes;
        std::vector<ViewNodeSnapshot> storage_nodes;
        std::optional<MetadataLeaderHint> leader_hint;
    };

    struct PullPeerViewSnapshotRequest
    {
        RequestId request_id;
        ClusterId cluster_id;
        bool include_dead_nodes{true};
        bool include_warnings{true};
    };

    struct PullPeerViewSnapshotResult
    {
        ViewRegistryResponseSummary summary;
        ViewPeerSyncSnapshot snapshot;
        std::vector<ViewRegistryDiagnostic> diagnostics;

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    struct PushPeerViewSnapshotRequest
    {
        RequestId request_id;
        ClusterId cluster_id;
        ViewPeerSyncSnapshot snapshot;
    };

    struct PushPeerViewSnapshotResult
    {
        ViewRegistryResponseSummary summary;
        std::uint32_t received_node_count{0};
        std::uint32_t accepted_node_count{0};
        std::uint32_t applied_node_count{0};
        std::uint32_t stale_ignored_node_count{0};
        std::uint32_t conflict_node_count{0};
        std::vector<ViewRegistryDiagnostic> diagnostics;

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    using ViewNodeClientPullPeerViewSnapshotResult =
        ViewNodeClientCallResult<PullPeerViewSnapshotResult>;
    using ViewNodeClientPushPeerViewSnapshotResult =
        ViewNodeClientCallResult<PushPeerViewSnapshotResult>;

    // ViewNodeClient 只负责把调用方的注册、心跳、发现和 cluster view 请求
    // 映射到 ViewNodeService RPC，并返回 transport + observation 诊断边界。
    // 它不负责对象 COMMITTED 可见性、Raft membership 变更、quorum 计算、
    // StorageNode payload 操作、transfer 编排或 app 启动循环。
    class ViewNodeClient
    {
    public:
        explicit ViewNodeClient(
            std::unique_ptr<view::ViewNodeService::StubInterface> stub,
            std::string target_endpoint,
            ViewNodeClientConfig config = {});
        explicit ViewNodeClient(std::shared_ptr<grpc::Channel> channel,
                                std::string target_endpoint,
                                ViewNodeClientConfig config = {});

        ~ViewNodeClient() = default;

        ViewNodeClient(const ViewNodeClient &) = delete;
        ViewNodeClient &operator=(const ViewNodeClient &) = delete;
        ViewNodeClient(ViewNodeClient &&) noexcept = default;
        ViewNodeClient &operator=(ViewNodeClient &&) noexcept = default;

        // 注册只上报 discovery / observation facts。
        // 注册成功不等于加入 Raft membership，也不改变 quorum。
        ViewNodeClientRegisterNodeResult RegisterNode(
            const RegisterNodeRequest &request,
            ViewNodeClientCallOptions options = {});

        // heartbeat 只刷新观测事实与 liveness，不决定 leader 最终归属。
        ViewNodeClientHeartbeatNodeResult HeartbeatNode(
            const HeartbeatNodeRequest &request,
            ViewNodeClientCallOptions options = {});

        // 返回 MetadataNode 候选地址和 leader hint。
        // leader hint 只是观测提示，调用方仍必须处理 MetadataService NOT_LEADER。
        ViewNodeClientDiscoverMetadataResult DiscoverMetadata(
            const DiscoverMetadataRequest &request,
            ViewNodeClientCallOptions options = {});

        // 返回 StorageNode endpoint snapshot、容量和健康观测。
        // 结果不是对象 manifest，也不是对象可见性的权威依据。
        ViewNodeClientDiscoverStorageResult DiscoverStorage(
            const DiscoverStorageRequest &request,
            ViewNodeClientCallOptions options = {});

        // 返回集群观测快照，供 status / diagnostics / tests 使用。
        // 它不授予任何 membership 或对象状态 authority。
        ViewNodeClientGetClusterViewResult GetClusterView(
            const GetClusterViewRequest &request,
            ViewNodeClientCallOptions options = {});

        // Pull 只导出 observed registry snapshot，不能直接决定 membership。
        ViewNodeClientPullPeerViewSnapshotResult PullPeerViewSnapshot(
            const PullPeerViewSnapshotRequest &request,
            ViewNodeClientCallOptions options = {});

        // Push 只把 peer observed-state replay 到本地 registry adapter 边界。
        // 它不能绕过 registry merge 语义，也不能修改 Raft membership。
        ViewNodeClientPushPeerViewSnapshotResult PushPeerViewSnapshot(
            const PushPeerViewSnapshotRequest &request,
            ViewNodeClientCallOptions options = {});

        [[nodiscard]] std::string_view target_endpoint() const;
        [[nodiscard]] const ViewNodeClientConfig &config() const;

    private:
        std::unique_ptr<view::ViewNodeService::StubInterface> stub_;
        Endpoint target_endpoint_;
        ViewNodeClientConfig config_;
    };

} // namespace viewdemo
