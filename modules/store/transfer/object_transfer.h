#pragma once

#include "store/common/store_types.h"
#include "store/maintenance/garbage_collector.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace viewdemo
{
    class ViewNodeClient;
}

namespace storedemo
{
    class MetadataTransferClient;
    class StorageTransferClient;

    enum class ObjectTransferDirection : std::uint8_t
    {
        kUnknown = 0,
        kUpload = 1,
        kDownload = 2,
    };

    enum class ObjectTransferStage : std::uint8_t
    {
        kUnknown = 0,
        kPreparing = 1,
        kDiscoveringMetadata = 2,
        kPlanningWrite = 3,
        kUploadingChunks = 4,
        kCommittingObject = 5,
        kFetchingManifest = 6,
        kDownloadingChunks = 7,
        kVerifyingChecksums = 8,
        kCompleted = 9,
        kFailed = 10,
        kCancelled = 11,
    };

    enum class ObjectTransferStatusCode : std::uint8_t
    {
        kOk = 0,
        kInvalidArgument = 1,
        kNotFound = 2,
        kConflict = 3,
        kDiscoveryUnavailable = 4,
        kMetadataNotLeader = 5,
        kMetadataRejected = 6,
        kStorageRejected = 7,
        kChecksumMismatch = 8,
        kIoError = 9,
        kTimeout = 10,
        kCancelled = 11,
        kUnsupported = 12,
        kInternalError = 13,
    };

    [[nodiscard]] inline bool IsSuccessfulObjectTransferStatus(
        const ObjectTransferStatusCode status)
    {
        return status == ObjectTransferStatusCode::kOk;
    }

    // 对象级 facts 只包含 size/checksum/etag 等 metadata 信息，
    // 不包含真实 payload，也不能作为 Raft inline payload 通道。
    struct TransferObjectChecksumFacts
    {
        std::uint64_t size{0};
        ChunkChecksum checksum;
        std::string etag;
    };

    // Upload WritePlan 的 chunk metadata facts。
    // 这里只表达 index/offset/size/checksum/placement，不承载 chunk bytes。
    struct TransferChunkPlan
    {
        ChunkIdentity identity;
        std::uint64_t offset{0};
        std::uint64_t expected_size{0};
        ChunkChecksum expected_checksum;
        std::vector<StorageNodeId> selected_replica_nodes;
        std::vector<StorageNodeId> candidate_nodes;
        std::uint32_t required_replica_count{0};
        std::uint32_t minimum_successful_writes{0};
    };

    // CommitObject 和 COMMITTED manifest 共享的 chunk facts 边界。
    struct TransferCommittedChunk
    {
        ChunkIdentity identity;
        std::uint64_t size{0};
        ChunkChecksum checksum;
        std::vector<StorageNodeId> replica_nodes;
    };

    // upload 本地准备出的 chunk facts。
    // 这里只记录 chunk index/offset/size/checksum，不伪造 chunk_id、replica 或 COMMITTED 状态。
    struct TransferPreparedChunk
    {
        std::uint32_t chunk_index{0};
        std::uint64_t offset{0};
        std::uint64_t size{0};
        ChunkChecksum checksum;
    };

    // MetadataNode CreateWritePlan 返回的 metadata facts 边界。
    struct TransferWritePlan
    {
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        std::uint64_t chunk_size_bytes{0};
        std::uint32_t total_chunks{0};
        std::uint32_t replica_count{0};
        std::uint32_t minimum_successful_writes{0};
        std::uint64_t placement_epoch{0};
        TransferObjectChecksumFacts object_checksum;
        std::vector<TransferChunkPlan> chunks;
        std::uint64_t created_at_unix_ms{0};
        std::uint64_t expires_at_unix_ms{0};
    };

    // Download 只接受 MetadataNode 返回的 COMMITTED manifest facts。
    struct TransferCommittedManifest
    {
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        TransferObjectChecksumFacts object_checksum;
        std::vector<TransferCommittedChunk> chunks;
        std::uint64_t committed_at_unix_ms{0};
    };

    struct ObjectTransferDiagnostic
    {
        ObjectTransferStatusCode status{ObjectTransferStatusCode::kOk};
        std::string message;
        std::string request_id;
        std::string node_id;
        std::string endpoint;
        ChunkId chunk_id;
        std::uint32_t chunk_index{0};
        std::uint64_t offset{0};
        bool retryable{false};
    };

    struct TransferFailureSummary
    {
        ObjectTransferStatusCode status{ObjectTransferStatusCode::kOk};
        std::string error_detail;
        std::string node_id;
        std::string endpoint;
        ChunkId chunk_id;
        std::uint32_t chunk_index{0};
        std::uint64_t offset{0};
        bool retryable{false};

        [[nodiscard]] bool ok() const
        {
            return IsSuccessfulObjectTransferStatus(status);
        }
    };

    struct TransferSessionSnapshot
    {
        ObjectTransferDirection direction{ObjectTransferDirection::kUnknown};
        ObjectTransferStage stage{ObjectTransferStage::kUnknown};
        std::string request_id;
        std::string cluster_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        std::filesystem::path source_path;
        std::filesystem::path destination_path;
        std::uint64_t chunk_size{0};
        std::uint32_t concurrency{0};
        std::uint64_t bytes_completed{0};
        std::uint64_t total_bytes{0};
        std::uint32_t chunks_completed{0};
        std::uint32_t total_chunks{0};
        bool metadata_commit_attempted{false};
        bool committed_visible{false};
        bool final_checksum_verified{false};
        std::optional<TransferFailureSummary> failure;
    };

    struct UploadObjectRequest
    {
        std::string request_id;
        // transfer 必须通过 ViewNode 在指定 cluster 内做 discovery，
        // 不能依赖硬编码 MetadataNode / StorageNode 地址。
        std::string cluster_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::filesystem::path source_path;
        std::uint64_t chunk_size{0};
        std::uint32_t concurrency{1};
        std::uint64_t max_inflight_bytes{0};
        std::uint32_t replica_fanout_concurrency{0};
        std::uint64_t replica_write_timeout_ms{0};
        // 这些副本策略是 CreateWritePlan 的 metadata facts 输入，
        // 不是本地自行决定对象可见性的 authority。
        std::uint32_t desired_replica_count{0};
        std::uint32_t minimum_successful_writes{0};
        std::uint64_t client_time_unix_ms{0};
        std::optional<TransferObjectChecksumFacts> expected_object_checksum;
    };

    struct DownloadObjectRequest
    {
        std::string request_id;
        // download 也必须先在指定 cluster 内发现 MetadataNode，
        // 对象可见性仍只能来自 MetadataNode 的 COMMITTED manifest。
        std::string cluster_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::optional<std::uint64_t> version;
        std::filesystem::path destination_path;
        std::uint32_t concurrency{1};
        std::optional<TransferObjectChecksumFacts> expected_object_checksum;
    };

    struct UploadObjectResult
    {
        ObjectTransferStatusCode status{ObjectTransferStatusCode::kOk};
        std::string error_detail;
        TransferSessionSnapshot session;
        std::vector<TransferPreparedChunk> prepared_chunks;
        std::optional<TransferWritePlan> write_plan;
        // 这里记录“已经 durable 且原本准备提交给 metadata”的 chunk facts；
        // 即使 CommitObject 失败，也可作为 cleanup candidate 的来源。
        std::vector<TransferCommittedChunk> committed_chunks;
        std::optional<TransferCommittedManifest> committed_manifest;
        // cleanup candidate 只表示“可能需要后续清理的未提交 durable/orphan chunk”；
        // 不能被解释成对象已 COMMITTED，也不能直接替代 metadata authority。
        std::vector<CleanupCandidate> cleanup_candidates;
        std::vector<ObjectTransferDiagnostic> diagnostics;
        bool commit_attempted{false};
        bool committed{false};
        // 用于表达“存在需要后续 cleanup/GC 关注的风险”，包括 uncertain write。
        bool cleanup_candidate_possible{false};

        [[nodiscard]] bool ok() const
        {
            return IsSuccessfulObjectTransferStatus(status);
        }
    };

    struct DownloadObjectResult
    {
        ObjectTransferStatusCode status{ObjectTransferStatusCode::kOk};
        std::string error_detail;
        TransferSessionSnapshot session;
        std::optional<TransferCommittedManifest> manifest;
        TransferObjectChecksumFacts downloaded_object_checksum;
        std::vector<ObjectTransferDiagnostic> diagnostics;
        bool checksum_verified{false};

        [[nodiscard]] bool ok() const
        {
            return IsSuccessfulObjectTransferStatus(status);
        }
    };

    struct TransferChunkReaderOpenRequest
    {
        std::filesystem::path source_path;
        std::uint64_t chunk_size{0};
        std::uint64_t start_offset{0};
    };

    struct TransferChunkReadResult
    {
        ObjectTransferStatusCode status{ObjectTransferStatusCode::kOk};
        std::string error_detail;
        std::uint32_t chunk_index{0};
        std::uint64_t offset{0};
        // payload 只能表示单个 bounded chunk 的 buffer，
        // 不得要求实现把完整对象常驻内存后再切片。
        std::string payload;
        bool eof{false};
        bool last_chunk{false};

        [[nodiscard]] bool ok() const
        {
            return IsSuccessfulObjectTransferStatus(status);
        }
    };

    // chunk reader 只负责 bounded file IO 边界，不参与 metadata authority、
    // StorageNode WriteChunk 或对象 COMMITTED 判定。
    class TransferChunkReader
    {
    public:
        virtual ~TransferChunkReader();

        virtual ObjectTransferStatusCode Open(
            const TransferChunkReaderOpenRequest &request,
            std::string *error_detail) = 0;
        virtual TransferChunkReadResult ReadNextChunk() = 0;
        virtual void Close() = 0;
    };

    // 返回默认的 bounded 文件 chunk reader。
    // 它只负责本地文件分块读取，不负责 metadata authority 或 StorageNode RPC。
    [[nodiscard]] std::unique_ptr<TransferChunkReader> CreateFileTransferChunkReader();

    struct TransferChecksumUpdateRequest
    {
        std::uint32_t chunk_index{0};
        std::uint64_t offset{0};
        std::string_view payload;
        std::optional<ChunkChecksum> expected_chunk_checksum;
    };

    struct TransferChecksumUpdateResult
    {
        ObjectTransferStatusCode status{ObjectTransferStatusCode::kOk};
        std::string error_detail;
        ChunkChecksum chunk_checksum;
        std::uint64_t bytes_processed{0};
        std::uint32_t chunks_processed{0};
        bool chunk_checksum_verified{false};

        [[nodiscard]] bool ok() const
        {
            return IsSuccessfulObjectTransferStatus(status);
        }
    };

    struct TransferChecksumSnapshot
    {
        std::uint64_t bytes_processed{0};
        std::uint32_t chunks_processed{0};
        bool finalized{false};
        std::optional<TransferObjectChecksumFacts> object_checksum;
    };

    struct TransferChecksumFinalizeResult
    {
        ObjectTransferStatusCode status{ObjectTransferStatusCode::kOk};
        std::string error_detail;
        TransferObjectChecksumFacts object_checksum;

        [[nodiscard]] bool ok() const
        {
            return IsSuccessfulObjectTransferStatus(status);
        }
    };

    // checksum state 只维护增量 checksum facts。
    // 它不能要求调用方先把完整文件或完整对象 payload 一次性拼进内存。
    class TransferChecksumState
    {
    public:
        virtual ~TransferChecksumState();

        virtual TransferChecksumUpdateResult Append(
            const TransferChecksumUpdateRequest &request) = 0;
        virtual TransferChecksumFinalizeResult Finalize() = 0;
        virtual TransferChecksumSnapshot Snapshot() const = 0;
        virtual void Reset() = 0;
    };

    // 返回默认的增量对象 checksum state。
    // 它只维护 chunk/object checksum facts，不缓存完整对象 payload。
    [[nodiscard]] std::unique_ptr<TransferChecksumState> CreateTransferChecksumState();

    // TransferSession 只表示单次客户端 upload/download 的生命周期快照。
    // 它不是 Raft 持久状态，也不是 StorageNode 本地状态。
    class TransferSession
    {
    public:
        virtual ~TransferSession();

        [[nodiscard]] virtual ObjectTransferDirection direction() const = 0;
        [[nodiscard]] virtual TransferSessionSnapshot Snapshot() const = 0;
        [[nodiscard]] virtual bool finished() const = 0;
    };

    class UploadTransferSession : public TransferSession
    {
    public:
        ~UploadTransferSession() override;

        static void SetMaxInflightPayloadBytesOverrideForTesting(
            std::uint64_t max_bytes);

        [[nodiscard]] virtual const UploadObjectRequest &request() const = 0;
        virtual UploadObjectResult Execute(
            TransferChunkReader &reader,
            TransferChecksumState &checksum_state) = 0;
    };

    class DownloadTransferSession : public TransferSession
    {
    public:
        ~DownloadTransferSession() override;

        [[nodiscard]] virtual const DownloadObjectRequest &request() const = 0;
        virtual DownloadObjectResult Execute(
            TransferChecksumState &checksum_state) = 0;
    };

    // ObjectTransfer 是 storage_client 侧的传输编排入口。
    // 它依赖 ViewNode discovery、MetadataTransferClient 和
    // StorageTransferClient，但不成为 metadata authority，也不让 payload 进入 Raft。
    class ObjectTransfer
    {
    public:
        ObjectTransfer(
            std::shared_ptr<MetadataTransferClient> metadata_client,
            std::shared_ptr<StorageTransferClient> storage_client,
            std::shared_ptr<viewdemo::ViewNodeClient> view_client = nullptr);
        ~ObjectTransfer();

        ObjectTransfer(const ObjectTransfer &) = delete;
        ObjectTransfer &operator=(const ObjectTransfer &) = delete;
        ObjectTransfer(ObjectTransfer &&) noexcept;
        ObjectTransfer &operator=(ObjectTransfer &&) noexcept;

        [[nodiscard]] std::unique_ptr<UploadTransferSession> StartUploadSession(
            const UploadObjectRequest &request) const;
        [[nodiscard]] std::unique_ptr<DownloadTransferSession> StartDownloadSession(
            const DownloadObjectRequest &request) const;

        [[nodiscard]] const std::shared_ptr<MetadataTransferClient> &
        metadata_client() const;
        [[nodiscard]] const std::shared_ptr<StorageTransferClient> &
        storage_client() const;
        [[nodiscard]] const std::shared_ptr<viewdemo::ViewNodeClient> &
        view_client() const;

    private:
        std::shared_ptr<MetadataTransferClient> metadata_client_;
        std::shared_ptr<StorageTransferClient> storage_client_;
        std::shared_ptr<viewdemo::ViewNodeClient> view_client_;
    };

} // namespace storedemo
