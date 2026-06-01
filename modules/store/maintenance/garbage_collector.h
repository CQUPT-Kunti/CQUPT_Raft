#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "store/chunk/chunk_store.h"

namespace storedemo
{
    enum class GarbageCollectionReason : std::uint8_t
    {
        kUnspecified = 0,
        kDeletedObjectCleanup = 1,
        kOrphanChunkCleanup = 2,
        kFailedUploadCleanup = 3,
        kAbortCleanup = 4,
    };

    enum class GarbageCollectorTaskState : std::uint8_t
    {
        kQueued = 0,
        kRunning = 1,
        kRetryPending = 2,
        kCompleted = 3,
        kFailed = 4,
        kCancelled = 5,
    };

    enum class GarbageCollectorSubmitCode : std::uint8_t
    {
        kAccepted = 0,
        kOverloaded = 1,
        kStopped = 2,
        kInvalidArgument = 3,
        kAlreadyExists = 4,
    };

    enum class GarbageCollectorStopMode : std::uint8_t
    {
        kDrain = 0,
        kCancelPending = 1,
    };

    enum class CleanupCandidateSource : std::uint8_t
    {
        kDeletedObject = 0,
        kPendingTimeout = 1,
        kFailedUpload = 2,
        kAbortCleanup = 3,
    };

    enum class CleanupObjectState : std::uint8_t
    {
        kUnspecified = 0,
        kPending = 1,
        kCommitted = 2,
        kDeleted = 3,
        kAborted = 4,
    };

    const char *ToString(GarbageCollectionReason reason);
    const char *ToString(GarbageCollectorTaskState state);
    const char *ToString(GarbageCollectorSubmitCode code);
    const char *ToString(GarbageCollectorStopMode mode);
    const char *ToString(CleanupCandidateSource source);
    const char *ToString(CleanupObjectState state);

    struct GarbageCollectorConfig
    {
        std::size_t worker_count{1};
        std::size_t queue_capacity{64};
        std::uint32_t default_max_attempts{3};
    };

    struct GarbageCollectorTask
    {
        std::string task_id;
        ChunkId chunk_id;
        std::string object_id;
        std::uint64_t version{0};
        std::uint32_t chunk_index{0};
        GarbageCollectionReason reason{GarbageCollectionReason::kUnspecified};
        std::string metadata_boundary;
        std::uint32_t attempts{0};
        std::uint32_t max_attempts{0};
        StorageNodeStatusCode last_error{StorageNodeStatusCode::kOk};
        std::string last_error_detail;
        GarbageCollectorTaskState state{GarbageCollectorTaskState::kQueued};
        bool retryable{false};
        std::uint64_t next_retry_after_ms{0};
    };

    struct GarbageCollectorSafetyCheckResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::uint64_t retry_after_ms{0};

        [[nodiscard]] bool allowed() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    struct CleanupChunkFact
    {
        ChunkIdentity identity;
        std::uint64_t size{0};
        ChunkChecksum checksum;
        std::vector<StorageNodeId> replica_nodes;
    };

    struct CleanupCandidate
    {
        CleanupCandidateSource source{CleanupCandidateSource::kDeletedObject};
        CleanupObjectState object_state{CleanupObjectState::kUnspecified};
        GarbageCollectionReason reason{GarbageCollectionReason::kUnspecified};
        std::string bucket;
        std::string object_key;
        ChunkIdentity identity;
        std::uint64_t size{0};
        ChunkChecksum checksum;
        std::vector<StorageNodeId> replica_nodes;
        std::string metadata_boundary;
        std::uint64_t created_at_unix_ms{0};
        std::uint64_t deadline_unix_ms{0};
    };

    struct PendingTimeoutCleanupRequest
    {
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        CleanupObjectState object_state{CleanupObjectState::kPending};
        std::uint64_t created_at_unix_ms{0};
        std::uint64_t now_unix_ms{0};
        std::uint64_t timeout_ms{0};
        std::vector<CleanupChunkFact> durable_chunks;
    };

    struct FailedUploadCleanupRequest
    {
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        CleanupObjectState object_state{CleanupObjectState::kPending};
        std::uint64_t created_at_unix_ms{0};
        std::vector<CleanupChunkFact> durable_chunks;
    };

    struct AbortCleanupRequest
    {
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        CleanupObjectState object_state{CleanupObjectState::kAborted};
        std::uint64_t created_at_unix_ms{0};
        std::vector<CleanupChunkFact> durable_chunks;
    };

    struct DeletedObjectCleanupRequest
    {
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        CleanupObjectState object_state{CleanupObjectState::kDeleted};
        std::uint64_t created_at_unix_ms{0};
        std::vector<CleanupChunkFact> durable_chunks;
    };

    using GarbageCollectorDeleteHandler =
        std::function<DeleteChunkResponse(const GarbageCollectorTask &)>;
    using GarbageCollectorSafetyChecker =
        std::function<GarbageCollectorSafetyCheckResult(const GarbageCollectorTask &)>;

    std::vector<CleanupCandidate> BuildPendingTimeoutCleanupCandidates(
        const PendingTimeoutCleanupRequest &request);
    std::vector<CleanupCandidate> BuildFailedUploadCleanupCandidates(
        const FailedUploadCleanupRequest &request);
    std::vector<CleanupCandidate> BuildAbortCleanupCandidates(
        const AbortCleanupRequest &request);
    std::vector<CleanupCandidate> BuildDeletedObjectCleanupCandidates(
        const DeletedObjectCleanupRequest &request);
    GarbageCollectorTask CleanupCandidateToGarbageCollectorTask(
        const CleanupCandidate &candidate);

    struct GarbageCollectorSubmitResult
    {
        GarbageCollectorSubmitCode code{GarbageCollectorSubmitCode::kAccepted};
        std::string error_detail;
        std::uint64_t retry_after_ms{0};
        std::size_t queue_depth{0};

        [[nodiscard]] bool accepted() const
        {
            return code == GarbageCollectorSubmitCode::kAccepted;
        }

        [[nodiscard]] StorageNodeStatusCode status_code() const;
    };

    struct GarbageCollectorDrainResult
    {
        bool drained{false};
        std::string error_detail;
    };

    struct GarbageCollectorStats
    {
        bool accepting_new_tasks{false};
        bool stop_requested{false};
        std::size_t worker_count{0};
        std::size_t queue_capacity{0};
        std::size_t queued_tasks{0};
        std::size_t running_tasks{0};
        std::size_t retry_pending_tasks{0};
        std::size_t completed_tasks{0};
        std::size_t failed_tasks{0};
        std::size_t cancelled_tasks{0};
        std::uint64_t submitted_tasks{0};
        std::uint64_t rejected_tasks{0};
        std::uint64_t total_attempts{0};
        std::string last_error_detail;
    };

    struct GarbageCollectorStopRequest
    {
        GarbageCollectorStopMode mode{GarbageCollectorStopMode::kDrain};
    };

    struct GarbageCollectorStopResult
    {
        bool stopped{false};
        bool drained{false};
        std::string error_detail;
        GarbageCollectorStats stats;
    };

    class GarbageCollector
    {
    public:
        explicit GarbageCollector(GarbageCollectorDeleteHandler delete_handler,
                                  GarbageCollectorSafetyChecker safety_checker,
                                  GarbageCollectorConfig config = {});
        ~GarbageCollector();

        GarbageCollector(const GarbageCollector &) = delete;
        GarbageCollector &operator=(const GarbageCollector &) = delete;

        GarbageCollectorSubmitResult SubmitTask(GarbageCollectorTask task);
        GarbageCollectorDrainResult Drain();
        GarbageCollectorStopResult Stop(GarbageCollectorStopRequest request = {});

        [[nodiscard]] std::optional<GarbageCollectorTask> FindTask(
            std::string_view task_id) const;
        [[nodiscard]] GarbageCollectorStats SnapshotStats() const;
        [[nodiscard]] const GarbageCollectorConfig &config() const;

    private:
        struct Impl;

        std::unique_ptr<Impl> impl_;
        GarbageCollectorConfig config_;
    };
}
