#pragma once

#include "raft/metadata/metadata_records.h"
#include "store/transfer/object_transfer.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "metadata.grpc.pb.h"

namespace storedemo
{
    enum class MetadataTransferStatusCode : std::uint8_t
    {
        kOk = 0,
        kNotLeader = 1,
        kInvalidArgument = 2,
        kNotFound = 3,
        kIdempotentReplay = 4,
        kIdempotencyConflict = 5,
        kStateConflict = 6,
        kObjectNotVisible = 7,
        kQuorumUnavailable = 8,
        kTimeout = 9,
        kOverloaded = 10,
        kServiceUnavailable = 11,
        kUnsupported = 12,
        kInternalError = 13,
    };

    [[nodiscard]] inline bool IsSuccessfulMetadataTransferStatus(
        const MetadataTransferStatusCode status)
    {
        return status == MetadataTransferStatusCode::kOk ||
               status == MetadataTransferStatusCode::kIdempotentReplay;
    }

    struct MetadataTransferLeaderHint
    {
        std::int32_t leader_id{0};
        std::string leader_address;
    };

    struct MetadataTransferSummary
    {
        MetadataTransferStatusCode status{MetadataTransferStatusCode::kOk};
        std::string message;
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        raftdemo::ObjectState object_state{raftdemo::ObjectState::PENDING};
        std::uint64_t term{0};
        std::uint64_t log_index{0};
        std::optional<MetadataTransferLeaderHint> leader_hint;

        [[nodiscard]] bool ok() const
        {
            return IsSuccessfulMetadataTransferStatus(status);
        }
    };

    struct MetadataTransferDiagnostic
    {
        MetadataTransferStatusCode status{MetadataTransferStatusCode::kOk};
        std::string message;
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::string endpoint;
        bool retryable{false};
        std::optional<MetadataTransferLeaderHint> leader_hint;
    };

    struct MetadataTransferClientConfig
    {
        // 0 表示不额外设置 deadline，由调用方或 gRPC 默认策略决定。
        std::chrono::milliseconds create_write_plan_timeout{0};
        std::chrono::milliseconds commit_object_timeout{0};
        std::chrono::milliseconds head_object_timeout{0};
        std::chrono::milliseconds get_manifest_timeout{0};
        bool wait_for_ready{false};
        // 为空时默认使用 insecure channel credentials。
        std::shared_ptr<grpc::ChannelCredentials> channel_credentials;
    };

    struct MetadataTransferClientCallOptions
    {
        // 非空时覆盖本次 RPC 的默认 timeout。
        std::optional<std::chrono::milliseconds> timeout;
        std::optional<bool> wait_for_ready;
    };

    struct MetadataTransferClientCallDiagnostics
    {
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::string target_endpoint;
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
    struct MetadataTransferClientCallResult
    {
        Result result;
        MetadataTransferClientCallDiagnostics rpc;

        [[nodiscard]] bool transport_ok() const
        {
            return rpc.transport_ok();
        }

        [[nodiscard]] bool ok() const
        {
            return transport_ok() && result.ok();
        }
    };

    // HeadObject 只表达 metadata 可见性和对象概要，不承载 chunk payload。
    struct TransferObjectHead
    {
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        TransferObjectChecksumFacts object_checksum;
        raftdemo::ObjectState state{raftdemo::ObjectState::PENDING};
        std::uint64_t created_at_unix_ms{0};
        std::optional<std::uint64_t> committed_at_unix_ms;
    };

    // CreateWritePlan 是 transfer 侧的逻辑边界。
    // T032 可以把它映射到当前 MetadataService 的一个或多个 RPC，
    // 但这里不暴露底层 transport 细节，也不承载真实 payload。
    struct MetadataTransferCreateWritePlanRequest
    {
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        TransferObjectChecksumFacts expected_object_checksum;
        std::uint64_t chunk_size{0};
        std::uint32_t desired_replica_count{0};
        std::uint32_t minimum_successful_writes{0};
        std::uint64_t client_time_unix_ms{0};
    };

    struct MetadataTransferCreateWritePlanResult
    {
        MetadataTransferSummary summary;
        std::optional<TransferWritePlan> write_plan;
        std::vector<MetadataTransferDiagnostic> diagnostics;
        bool created_pending{false};
        bool idempotent{false};

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    struct MetadataTransferCommitObjectRequest
    {
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        TransferObjectChecksumFacts object_checksum;
        std::vector<TransferCommittedChunk> committed_chunks;
        std::uint64_t client_time_unix_ms{0};
    };

    struct MetadataTransferCommitObjectResult
    {
        MetadataTransferSummary summary;
        std::optional<TransferCommittedManifest> committed_manifest;
        std::vector<MetadataTransferDiagnostic> diagnostics;
        bool committed{false};
        bool idempotent{false};
        bool visible_for_read{false};

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    struct MetadataTransferHeadObjectRequest
    {
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::optional<std::uint64_t> version;
        bool require_committed_visible{true};
    };

    struct MetadataTransferHeadObjectResult
    {
        MetadataTransferSummary summary;
        std::optional<TransferObjectHead> object;
        std::vector<MetadataTransferDiagnostic> diagnostics;
        bool found{false};
        bool visible_for_read{false};

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    // 下载路径最终依赖 COMMITTED manifest；PENDING / ABORTED / 不可见对象
    // 不能由 adapter 本地擅自解释为可下载对象。
    struct MetadataTransferGetObjectManifestRequest
    {
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::optional<std::uint64_t> version;
        bool require_committed_visible{true};
    };

    struct MetadataTransferGetObjectManifestResult
    {
        MetadataTransferSummary summary;
        std::optional<TransferCommittedManifest> manifest;
        std::vector<MetadataTransferDiagnostic> diagnostics;
        bool found{false};
        bool visible_for_read{false};

        [[nodiscard]] bool ok() const
        {
            return summary.ok();
        }
    };

    using MetadataTransferClientCreateWritePlanCallResult =
        MetadataTransferClientCallResult<MetadataTransferCreateWritePlanResult>;
    using MetadataTransferClientCommitObjectCallResult =
        MetadataTransferClientCallResult<MetadataTransferCommitObjectResult>;
    using MetadataTransferClientHeadObjectCallResult =
        MetadataTransferClientCallResult<MetadataTransferHeadObjectResult>;
    using MetadataTransferClientGetObjectManifestCallResult =
        MetadataTransferClientCallResult<MetadataTransferGetObjectManifestResult>;

    // MetadataTransferClient 只定义 transfer -> MetadataService 的适配边界。
    // 它负责表达 CreateWritePlan / CommitObject / HeadObject / GetObjectManifest
    // 的请求、结果和诊断，不实现 RPC 调用逻辑，不实现 ViewNode discovery，
    // 不保存 object manifest 权威副本，也不承担 upload/download 编排。
    class MetadataTransferClient
    {
    public:
        explicit MetadataTransferClient(
            std::unique_ptr<raft::MetadataService::StubInterface> stub,
            std::string target_endpoint,
            MetadataTransferClientConfig config = {});
        explicit MetadataTransferClient(std::shared_ptr<grpc::Channel> channel,
                                       std::string target_endpoint,
                                       MetadataTransferClientConfig config = {});

        virtual ~MetadataTransferClient() = default;

        MetadataTransferClient(const MetadataTransferClient &) = delete;
        MetadataTransferClient &operator=(const MetadataTransferClient &) = delete;
        MetadataTransferClient(MetadataTransferClient &&) noexcept = default;
        MetadataTransferClient &operator=(MetadataTransferClient &&) noexcept =
            default;

        // 申请写入计划只返回 metadata/control-plane facts。
        // 这里不返回真实 payload，也不授予对象可见性 authority。
        MetadataTransferClientCreateWritePlanCallResult CreateWritePlan(
            const MetadataTransferCreateWritePlanRequest &request,
            MetadataTransferClientCallOptions options = {});

        // CommitObject 只提交 chunk manifest facts；对象是否真正 COMMITTED 可见
        // 仍由 MetadataNode / Raft quorum 的权威结果决定。
        MetadataTransferClientCommitObjectCallResult CommitObject(
            const MetadataTransferCommitObjectRequest &request,
            MetadataTransferClientCallOptions options = {});

        // HeadObject 只返回对象概要与可见性诊断，不驱动下载编排。
        MetadataTransferClientHeadObjectCallResult HeadObject(
            const MetadataTransferHeadObjectRequest &request,
            MetadataTransferClientCallOptions options = {});

        // GetObjectManifest 只返回下载所需的 COMMITTED manifest facts。
        // 它不负责读取 chunk payload，也不实现副本选择或 checksum 校验。
        MetadataTransferClientGetObjectManifestCallResult GetObjectManifest(
            const MetadataTransferGetObjectManifestRequest &request,
            MetadataTransferClientCallOptions options = {});

        [[nodiscard]] std::string_view target_endpoint() const;
        [[nodiscard]] const MetadataTransferClientConfig &config() const;

    protected:
        std::unique_ptr<raft::MetadataService::StubInterface> stub_;
        std::string target_endpoint_;
        MetadataTransferClientConfig config_;
    };

    // 返回一个基于 MetadataService gRPC control-plane 的 transfer adapter。
    // 它只负责单 endpoint 的 metadata RPC 映射，不负责 ViewNode discovery 或 upload/download 编排。
    std::shared_ptr<MetadataTransferClient> CreateGrpcMetadataTransferClient(
        std::string target_endpoint,
        MetadataTransferClientConfig config = {});

} // namespace storedemo
