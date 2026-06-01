#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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

    const char *ToString(GarbageCollectionReason reason);
    const char *ToString(GarbageCollectorTaskState state);
    const char *ToString(GarbageCollectorSubmitCode code);
    const char *ToString(GarbageCollectorStopMode mode);

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

    using GarbageCollectorDeleteHandler =
        std::function<DeleteChunkResponse(const GarbageCollectorTask &)>;
    using GarbageCollectorSafetyChecker =
        std::function<GarbageCollectorSafetyCheckResult(const GarbageCollectorTask &)>;

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
