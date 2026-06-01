#include "store/maintenance/garbage_collector.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "store/runtime/storage_executor.h"

namespace storedemo
{
    namespace
    {
        GarbageCollectorConfig SanitizeGarbageCollectorConfig(GarbageCollectorConfig config)
        {
            if (config.worker_count == 0)
            {
                config.worker_count = 1;
            }
            if (config.queue_capacity == 0)
            {
                config.queue_capacity = 1;
            }
            if (config.default_max_attempts == 0)
            {
                config.default_max_attempts = 1;
            }
            return config;
        }

        bool IsTerminalTaskState(const GarbageCollectorTaskState state)
        {
            return state == GarbageCollectorTaskState::kCompleted ||
                   state == GarbageCollectorTaskState::kFailed ||
                   state == GarbageCollectorTaskState::kCancelled;
        }

        bool HasPendingWork(const std::unordered_map<std::string, GarbageCollectorTask> &tasks)
        {
            for (const auto &[task_id, task] : tasks)
            {
                (void)task_id;
                if (!IsTerminalTaskState(task.state))
                {
                    return true;
                }
            }
            return false;
        }

        struct GarbageCollectorAttemptResult
        {
            StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
            std::string error_detail;
            std::uint64_t retry_after_ms{0};
        };

        StorageNodeStatusCode ResolveTaskChunkId(GarbageCollectorTask *task,
                                                 std::string *error_detail)
        {
            if (task == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "garbage collector task must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (!task->chunk_id.empty())
            {
                std::string validation_error;
                const auto status =
                    ValidateChunkId(task->chunk_id, &validation_error);
                if (status != StorageNodeStatusCode::kOk)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = validation_error;
                    }
                    return status;
                }

                if (!task->object_id.empty())
                {
                    ChunkId derived_chunk_id;
                    const auto derive_status = MakeChunkId(task->object_id,
                                                           task->version,
                                                           task->chunk_index,
                                                           &derived_chunk_id,
                                                           error_detail);
                    if (derive_status != StorageNodeStatusCode::kOk)
                    {
                        return derive_status;
                    }
                    if (derived_chunk_id != task->chunk_id)
                    {
                        if (error_detail != nullptr)
                        {
                            *error_detail =
                                "garbage collector task chunk identity does not match chunk_id";
                        }
                        return StorageNodeStatusCode::kInvalidArgument;
                    }
                }

                return StorageNodeStatusCode::kOk;
            }

            if (task->object_id.empty() || task->version == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "garbage collector task requires chunk_id or object identity";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            ChunkId chunk_id;
            const auto status = MakeChunkId(task->object_id,
                                            task->version,
                                            task->chunk_index,
                                            &chunk_id,
                                            error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }
            task->chunk_id = std::move(chunk_id);
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ValidateTaskForSubmission(
            GarbageCollectorTask *task,
            const GarbageCollectorConfig &config,
            std::string *error_detail)
        {
            if (task == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "garbage collector task must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (task->task_id.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "garbage collector task_id must not be empty";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (task->metadata_boundary.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "garbage collector metadata_boundary must not be empty";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (task->reason == GarbageCollectionReason::kUnspecified)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "garbage collector reason must not be unspecified";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (task->max_attempts == 0)
            {
                task->max_attempts = config.default_max_attempts;
            }
            if (task->max_attempts == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "garbage collector max_attempts must not be zero";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
            if (task->attempts >= task->max_attempts)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "garbage collector task attempts must be less than max_attempts";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return ResolveTaskChunkId(task, error_detail);
        }

        GarbageCollectorAttemptResult MakeAttemptResultFromSafetyCheck(
            GarbageCollectorSafetyCheckResult check_result)
        {
            return GarbageCollectorAttemptResult{
                .status = check_result.status,
                .error_detail = std::move(check_result.error_detail),
                .retry_after_ms = check_result.retry_after_ms};
        }

        GarbageCollectorAttemptResult MakeAttemptResultFromDeleteResponse(
            DeleteChunkResponse delete_response)
        {
            return GarbageCollectorAttemptResult{
                .status = delete_response.status,
                .error_detail = std::move(delete_response.error_detail),
                .retry_after_ms = delete_response.retry_after_ms};
        }
    }

    struct GarbageCollector::Impl
    {
        explicit Impl(GarbageCollectorDeleteHandler handler,
                      GarbageCollectorSafetyChecker checker,
                      const GarbageCollectorConfig &collector_config)
            : delete_handler(std::move(handler))
            , safety_checker(std::move(checker))
            , config(collector_config)
            , executor(StorageExecutorConfig{
                  .worker_count = collector_config.worker_count,
                  .queue_capacity = collector_config.queue_capacity})
        {
        }

        mutable std::mutex mutex;
        std::condition_variable cv;
        GarbageCollectorDeleteHandler delete_handler;
        GarbageCollectorSafetyChecker safety_checker;
        GarbageCollectorConfig config;
        BoundedStorageExecutor executor;
        bool accepting_new_tasks{true};
        bool stop_requested{false};
        GarbageCollectorStopMode stop_mode{GarbageCollectorStopMode::kDrain};
        std::uint64_t submitted_tasks{0};
        std::uint64_t rejected_tasks{0};
        std::uint64_t total_attempts{0};
        std::string last_error_detail;
        std::unordered_map<std::string, GarbageCollectorTask> tasks;

        [[nodiscard]] GarbageCollectorStats SnapshotStatsLocked() const
        {
            GarbageCollectorStats stats;
            stats.accepting_new_tasks = accepting_new_tasks;
            stats.stop_requested = stop_requested;
            stats.worker_count = config.worker_count;
            stats.queue_capacity = config.queue_capacity;
            stats.submitted_tasks = submitted_tasks;
            stats.rejected_tasks = rejected_tasks;
            stats.total_attempts = total_attempts;
            stats.last_error_detail = last_error_detail;

            for (const auto &[task_id, task] : tasks)
            {
                (void)task_id;
                switch (task.state)
                {
                case GarbageCollectorTaskState::kQueued:
                    ++stats.queued_tasks;
                    break;
                case GarbageCollectorTaskState::kRunning:
                    ++stats.running_tasks;
                    break;
                case GarbageCollectorTaskState::kRetryPending:
                    ++stats.retry_pending_tasks;
                    break;
                case GarbageCollectorTaskState::kCompleted:
                    ++stats.completed_tasks;
                    break;
                case GarbageCollectorTaskState::kFailed:
                    ++stats.failed_tasks;
                    break;
                case GarbageCollectorTaskState::kCancelled:
                    ++stats.cancelled_tasks;
                    break;
                }
            }

            return stats;
        }
    };

    const char *ToString(const GarbageCollectionReason reason)
    {
        switch (reason)
        {
        case GarbageCollectionReason::kUnspecified:
            return "Unspecified";
        case GarbageCollectionReason::kDeletedObjectCleanup:
            return "DeletedObjectCleanup";
        case GarbageCollectionReason::kOrphanChunkCleanup:
            return "OrphanChunkCleanup";
        case GarbageCollectionReason::kFailedUploadCleanup:
            return "FailedUploadCleanup";
        case GarbageCollectionReason::kAbortCleanup:
            return "AbortCleanup";
        }
        return "UnknownGarbageCollectionReason";
    }

    const char *ToString(const GarbageCollectorTaskState state)
    {
        switch (state)
        {
        case GarbageCollectorTaskState::kQueued:
            return "Queued";
        case GarbageCollectorTaskState::kRunning:
            return "Running";
        case GarbageCollectorTaskState::kRetryPending:
            return "RetryPending";
        case GarbageCollectorTaskState::kCompleted:
            return "Completed";
        case GarbageCollectorTaskState::kFailed:
            return "Failed";
        case GarbageCollectorTaskState::kCancelled:
            return "Cancelled";
        }
        return "UnknownGarbageCollectorTaskState";
    }

    const char *ToString(const GarbageCollectorSubmitCode code)
    {
        switch (code)
        {
        case GarbageCollectorSubmitCode::kAccepted:
            return "Accepted";
        case GarbageCollectorSubmitCode::kOverloaded:
            return "Overloaded";
        case GarbageCollectorSubmitCode::kStopped:
            return "Stopped";
        case GarbageCollectorSubmitCode::kInvalidArgument:
            return "InvalidArgument";
        case GarbageCollectorSubmitCode::kAlreadyExists:
            return "AlreadyExists";
        }
        return "UnknownGarbageCollectorSubmitCode";
    }

    const char *ToString(const GarbageCollectorStopMode mode)
    {
        switch (mode)
        {
        case GarbageCollectorStopMode::kDrain:
            return "Drain";
        case GarbageCollectorStopMode::kCancelPending:
            return "CancelPending";
        }
        return "UnknownGarbageCollectorStopMode";
    }

    StorageNodeStatusCode GarbageCollectorSubmitResult::status_code() const
    {
        switch (code)
        {
        case GarbageCollectorSubmitCode::kAccepted:
            return StorageNodeStatusCode::kOk;
        case GarbageCollectorSubmitCode::kOverloaded:
            return StorageNodeStatusCode::kOverloaded;
        case GarbageCollectorSubmitCode::kStopped:
            return StorageNodeStatusCode::kNodeUnavailable;
        case GarbageCollectorSubmitCode::kInvalidArgument:
            return StorageNodeStatusCode::kInvalidArgument;
        case GarbageCollectorSubmitCode::kAlreadyExists:
            return StorageNodeStatusCode::kAlreadyExists;
        }
        return StorageNodeStatusCode::kIoError;
    }

    namespace
    {
        GarbageCollectorSubmitCode TranslateSubmitCode(
            const StorageExecutorSubmitCode code)
        {
            switch (code)
            {
            case StorageExecutorSubmitCode::kAccepted:
                return GarbageCollectorSubmitCode::kAccepted;
            case StorageExecutorSubmitCode::kOverloaded:
                return GarbageCollectorSubmitCode::kOverloaded;
            case StorageExecutorSubmitCode::kStopped:
                return GarbageCollectorSubmitCode::kStopped;
            case StorageExecutorSubmitCode::kInvalidArgument:
                return GarbageCollectorSubmitCode::kInvalidArgument;
            }
            return GarbageCollectorSubmitCode::kInvalidArgument;
        }

        StorageExecutorStopMode TranslateStopMode(
            const GarbageCollectorStopMode mode)
        {
            switch (mode)
            {
            case GarbageCollectorStopMode::kCancelPending:
                return StorageExecutorStopMode::kCancelPending;
            case GarbageCollectorStopMode::kDrain:
            default:
                return StorageExecutorStopMode::kDrain;
            }
        }
    }

    GarbageCollector::GarbageCollector(GarbageCollectorDeleteHandler delete_handler,
                                       GarbageCollectorSafetyChecker safety_checker,
                                       GarbageCollectorConfig config)
        : impl_(std::make_unique<Impl>(std::move(delete_handler),
                                       std::move(safety_checker),
                                       SanitizeGarbageCollectorConfig(config)))
        , config_(SanitizeGarbageCollectorConfig(config))
    {
        if (!impl_->delete_handler)
        {
            throw std::invalid_argument(
                "GarbageCollector requires a non-null delete handler");
        }
        if (!impl_->safety_checker)
        {
            throw std::invalid_argument(
                "GarbageCollector requires a non-null metadata safety checker");
        }
    }

    GarbageCollector::~GarbageCollector()
    {
        (void)Stop(GarbageCollectorStopRequest{
            .mode = GarbageCollectorStopMode::kDrain});
    }

    GarbageCollectorSubmitResult GarbageCollector::SubmitTask(GarbageCollectorTask task)
    {
        GarbageCollectorSubmitResult result;
        std::string validation_error;
        const auto validation_status =
            ValidateTaskForSubmission(&task, config_, &validation_error);
        if (validation_status != StorageNodeStatusCode::kOk)
        {
            result.code = GarbageCollectorSubmitCode::kInvalidArgument;
            result.error_detail = std::move(validation_error);
            return result;
        }

        task.state = GarbageCollectorTaskState::kQueued;
        task.retryable = false;
        task.next_retry_after_ms = 0;

        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!impl_->accepting_new_tasks)
            {
                result.code = GarbageCollectorSubmitCode::kStopped;
                result.error_detail = "garbage collector is not accepting new tasks";
                ++impl_->rejected_tasks;
                result.queue_depth = impl_->SnapshotStatsLocked().queued_tasks;
                return result;
            }

            if (impl_->tasks.find(task.task_id) != impl_->tasks.end())
            {
                result.code = GarbageCollectorSubmitCode::kAlreadyExists;
                result.error_detail = "garbage collector task_id already exists";
                ++impl_->rejected_tasks;
                result.queue_depth = impl_->SnapshotStatsLocked().queued_tasks;
                return result;
            }

            impl_->tasks.emplace(task.task_id, task);
        }

        const std::string task_id = task.task_id;
        const auto executor_result = impl_->executor.Submit(StorageExecutorSubmitRequest{
            .task_name = "garbage-collector/" + task_id,
            .task =
                [this, task_id]()
                {
                    for (;;)
                    {
                        GarbageCollectorTask task_snapshot;
                        {
                            std::lock_guard<std::mutex> lock(impl_->mutex);
                            auto task_it = impl_->tasks.find(task_id);
                            if (task_it == impl_->tasks.end())
                            {
                                impl_->cv.notify_all();
                                return;
                            }

                            if (task_it->second.state == GarbageCollectorTaskState::kCancelled)
                            {
                                impl_->cv.notify_all();
                                return;
                            }

                            task_it->second.state = GarbageCollectorTaskState::kRunning;
                            task_it->second.retryable = false;
                            task_it->second.next_retry_after_ms = 0;
                            task_snapshot = task_it->second;
                        }
                        impl_->cv.notify_all();

                        GarbageCollectorAttemptResult attempt_result;
                        try
                        {
                            attempt_result = MakeAttemptResultFromSafetyCheck(
                                impl_->safety_checker(task_snapshot));
                        }
                        catch (const std::exception &ex)
                        {
                            attempt_result.status = StorageNodeStatusCode::kIoError;
                            attempt_result.error_detail = ex.what();
                        }
                        catch (...)
                        {
                            attempt_result.status = StorageNodeStatusCode::kIoError;
                            attempt_result.error_detail =
                                "unknown garbage collector safety checker exception";
                        }

                        if (attempt_result.status == StorageNodeStatusCode::kOk)
                        {
                            try
                            {
                                attempt_result = MakeAttemptResultFromDeleteResponse(
                                    impl_->delete_handler(task_snapshot));
                            }
                            catch (const std::exception &ex)
                            {
                                attempt_result.status = StorageNodeStatusCode::kIoError;
                                attempt_result.error_detail = ex.what();
                            }
                            catch (...)
                            {
                                attempt_result.status = StorageNodeStatusCode::kIoError;
                                attempt_result.error_detail =
                                    "unknown garbage collector handler exception";
                            }
                        }

                        bool should_retry = false;
                        {
                            std::lock_guard<std::mutex> lock(impl_->mutex);
                            auto task_it = impl_->tasks.find(task_id);
                            if (task_it == impl_->tasks.end())
                            {
                                impl_->cv.notify_all();
                                return;
                            }

                            ++task_it->second.attempts;
                            ++impl_->total_attempts;

                            if (attempt_result.status != StorageNodeStatusCode::kOk)
                            {
                                task_it->second.last_error = attempt_result.status;
                                task_it->second.last_error_detail =
                                    attempt_result.error_detail;
                                task_it->second.next_retry_after_ms =
                                    attempt_result.retry_after_ms;
                                impl_->last_error_detail =
                                    attempt_result.error_detail;
                            }

                            if (attempt_result.status == StorageNodeStatusCode::kOk)
                            {
                                task_it->second.state =
                                    GarbageCollectorTaskState::kCompleted;
                                task_it->second.retryable = false;
                                task_it->second.next_retry_after_ms = 0;
                                impl_->cv.notify_all();
                                return;
                            }

                            const bool stop_blocks_retry =
                                impl_->stop_requested &&
                                impl_->stop_mode == GarbageCollectorStopMode::kCancelPending;
                            const bool retryable_failure =
                                IsRetriableStatus(attempt_result.status) &&
                                task_it->second.attempts < task_it->second.max_attempts &&
                                !stop_blocks_retry;
                            if (retryable_failure)
                            {
                                task_it->second.state =
                                    GarbageCollectorTaskState::kRetryPending;
                                task_it->second.retryable = true;
                                should_retry = true;
                            }
                            else
                            {
                                task_it->second.state =
                                    GarbageCollectorTaskState::kFailed;
                                task_it->second.retryable = false;
                            }
                        }
                        impl_->cv.notify_all();

                        if (!should_retry)
                        {
                            return;
                        }
                    }
                }});

        result.code = TranslateSubmitCode(executor_result.code);
        result.error_detail = executor_result.error_detail;
        result.retry_after_ms = executor_result.retry_after_ms;
        result.queue_depth = executor_result.queue_depth;

        if (!executor_result.accepted())
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->tasks.erase(task_id);
            ++impl_->rejected_tasks;
            return result;
        }

        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            ++impl_->submitted_tasks;
        }
        impl_->cv.notify_all();
        return result;
    }

    GarbageCollectorDrainResult GarbageCollector::Drain()
    {
        GarbageCollectorDrainResult result;
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->cv.wait(lock,
                       [this]()
                       {
                           return !HasPendingWork(impl_->tasks);
                       });
        result.drained = true;
        return result;
    }

    GarbageCollectorStopResult GarbageCollector::Stop(GarbageCollectorStopRequest request)
    {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->accepting_new_tasks = false;
            impl_->stop_requested = true;
            impl_->stop_mode = request.mode;
            if (request.mode == GarbageCollectorStopMode::kCancelPending)
            {
                for (auto &[task_id, task] : impl_->tasks)
                {
                    (void)task_id;
                    if (task.state == GarbageCollectorTaskState::kQueued)
                    {
                        task.state = GarbageCollectorTaskState::kCancelled;
                        task.retryable = false;
                        task.last_error = StorageNodeStatusCode::kCancelled;
                        task.last_error_detail =
                            "garbage collector cancelled pending task during stop";
                        task.next_retry_after_ms = 0;
                        impl_->last_error_detail = task.last_error_detail;
                    }
                }
            }
        }
        impl_->cv.notify_all();

        const auto executor_result =
            impl_->executor.Shutdown(StorageExecutorShutdownRequest{
                .mode = TranslateStopMode(request.mode)});

        GarbageCollectorStopResult result;
        result.stopped = executor_result.stopped;
        result.drained = request.mode == GarbageCollectorStopMode::kDrain
                             ? !HasPendingWork(impl_->tasks)
                             : executor_result.drained;
        result.error_detail = executor_result.error_detail;
        result.stats = SnapshotStats();
        return result;
    }

    std::optional<GarbageCollectorTask> GarbageCollector::FindTask(
        std::string_view task_id) const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto it = impl_->tasks.find(std::string(task_id));
        if (it == impl_->tasks.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    GarbageCollectorStats GarbageCollector::SnapshotStats() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->SnapshotStatsLocked();
    }

    const GarbageCollectorConfig &GarbageCollector::config() const
    {
        return config_;
    }
}
