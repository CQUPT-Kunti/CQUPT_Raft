#include "store/maintenance/rebalance_manager.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "store/node/storage_node_registry.h"

namespace storedemo
{
    namespace
    {
        RebalanceManagerConfig SanitizeRebalanceManagerConfig(
            RebalanceManagerConfig config)
        {
            if (config.max_active_tasks == 0)
            {
                config.max_active_tasks = 1;
            }
            if (config.max_tasks == 0)
            {
                config.max_tasks = config.max_active_tasks;
            }
            if (config.max_tasks < config.max_active_tasks)
            {
                config.max_tasks = config.max_active_tasks;
            }
            if (!config.now_unix_ms)
            {
                config.now_unix_ms = []()
                {
                    return static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
                };
            }
            return config;
        }

        bool IsActiveTaskState(const RebalanceTaskState state)
        {
            return state == RebalanceTaskState::kQueued ||
                   state == RebalanceTaskState::kRunning ||
                   state == RebalanceTaskState::kRetryPending;
        }

        const char *TaskReasonToken(const RebalanceTaskReason reason)
        {
            switch (reason)
            {
            case RebalanceTaskReason::kCapacityImbalance:
                return "capacity";
            case RebalanceTaskReason::kHotspot:
                return "hotspot";
            case RebalanceTaskReason::kNewNodeJoin:
                return "new-node-join";
            case RebalanceTaskReason::kDraining:
                return "draining";
            case RebalanceTaskReason::kMaintenance:
                return "maintenance";
            }
            return "unknown";
        }

        std::string BuildRebalanceTaskId(const ChunkId &chunk_id,
                                         const RebalanceTaskReason reason,
                                         const ChunkChecksum &expected_checksum,
                                         const std::uint64_t expected_size,
                                         const StorageNodeId &source_node,
                                         const StorageNodeId &target_node)
        {
            return chunk_id + "|rebalance|" + TaskReasonToken(reason) + "|" +
                   expected_checksum.value + "|" +
                   std::to_string(expected_size) + "|" + source_node + "|" +
                   target_node;
        }

        StorageNodeStatusCode ResolveChunkIdentity(ChunkIdentity *identity,
                                                   std::string *error_detail)
        {
            if (identity == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance identity must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (!identity->chunk_id.empty())
            {
                std::string validation_error;
                const auto status =
                    ValidateChunkId(identity->chunk_id, &validation_error);
                if (status != StorageNodeStatusCode::kOk)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = std::move(validation_error);
                    }
                    return status;
                }

                if (!identity->object_id.empty())
                {
                    ChunkId expected_chunk_id;
                    const auto derive_status = MakeChunkId(identity->object_id,
                                                           identity->version,
                                                           identity->chunk_index,
                                                           &expected_chunk_id,
                                                           error_detail);
                    if (derive_status != StorageNodeStatusCode::kOk)
                    {
                        return derive_status;
                    }
                    if (expected_chunk_id != identity->chunk_id)
                    {
                        if (error_detail != nullptr)
                        {
                            *error_detail =
                                "rebalance chunk identity does not match chunk_id";
                        }
                        return StorageNodeStatusCode::kInvalidArgument;
                    }
                }
                return StorageNodeStatusCode::kOk;
            }

            if (identity->object_id.empty() || identity->version == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "rebalance task requires chunk_id or object identity";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            ChunkId chunk_id;
            const auto status = MakeChunkId(identity->object_id,
                                            identity->version,
                                            identity->chunk_index,
                                            &chunk_id,
                                            error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }
            identity->chunk_id = std::move(chunk_id);
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ValidateSourceState(const ChunkState source_state,
                                                  std::string *error_detail)
        {
            switch (source_state)
            {
            case ChunkState::kLive:
                return StorageNodeStatusCode::kOk;
            case ChunkState::kCorrupted:
            case ChunkState::kQuarantined:
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "rebalance source chunk must not be corrupted or quarantined";
                }
                return StorageNodeStatusCode::kCorrupted;
            case ChunkState::kMissing:
            case ChunkState::kDeleted:
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance source chunk is missing";
                }
                return StorageNodeStatusCode::kNotFound;
            case ChunkState::kStaging:
            case ChunkState::kDeleting:
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance source chunk must be live";
                }
                return StorageNodeStatusCode::kConflict;
            }

            if (error_detail != nullptr)
            {
                *error_detail = "rebalance source chunk state is unknown";
            }
            return StorageNodeStatusCode::kInvalidArgument;
        }

        StorageNodeStatusCode ValidateSourceSnapshot(
            const StorageNodeRegistryNodeSnapshot *snapshot,
            std::string *error_detail)
        {
            if (snapshot == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance source registry snapshot is missing";
                }
                return StorageNodeStatusCode::kNodeUnavailable;
            }
            if (snapshot->liveness != StorageNodeRegistryLiveness::kLive)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance source is stale or unavailable";
                }
                return StorageNodeStatusCode::kNodeUnavailable;
            }
            if (snapshot->facts.health.health != StorageNodeHealth::kHealthy)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance source is unhealthy";
                }
                return StorageNodeStatusCode::kNodeUnavailable;
            }
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ValidateTargetSnapshot(
            const StorageNodeRegistryNodeSnapshot *snapshot,
            const std::uint64_t expected_size,
            std::string *error_detail)
        {
            if (snapshot == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance target registry snapshot is missing";
                }
                return StorageNodeStatusCode::kNodeUnavailable;
            }
            if (snapshot->liveness != StorageNodeRegistryLiveness::kLive)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance target is stale or unavailable";
                }
                return StorageNodeStatusCode::kNodeUnavailable;
            }
            if (snapshot->facts.health.health != StorageNodeHealth::kHealthy)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance target is unhealthy";
                }
                return StorageNodeStatusCode::kNodeUnavailable;
            }
            if (snapshot->facts.health.disk_pressure ==
                    StorageNodeDiskPressure::kHigh ||
                snapshot->facts.health.disk_pressure ==
                    StorageNodeDiskPressure::kFull)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance target disk pressure is too high";
                }
                return StorageNodeStatusCode::kDiskFull;
            }
            if (snapshot->facts.load.write_admission_overloaded)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance target write admission is overloaded";
                }
                return StorageNodeStatusCode::kOverloaded;
            }
            if (snapshot->facts.capacity.available_capacity_bytes < expected_size)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "rebalance target capacity is insufficient";
                }
                return StorageNodeStatusCode::kDiskFull;
            }
            return StorageNodeStatusCode::kOk;
        }

        RebalanceTask MakeRebalanceTask(const RebalanceTaskRequest &request,
                                        const std::uint64_t now_unix_ms)
        {
            RebalanceTask task;
            task.identity = request.identity;
            task.chunk_id = request.identity.chunk_id;
            task.source_node = request.source_node;
            task.target_node = request.target_node;
            task.reason = request.reason;
            task.expected_checksum = request.expected_checksum;
            task.expected_size = request.expected_size;
            task.source_state = request.source_state;
            task.context = request.context;
            task.task_id = BuildRebalanceTaskId(task.chunk_id,
                                                task.reason,
                                                task.expected_checksum,
                                                task.expected_size,
                                                task.source_node,
                                                task.target_node);
            task.submitted_at_unix_ms = now_unix_ms;
            return task;
        }

        RebalanceTaskOperationResult MakeTaskMutationNotFound(std::string_view task_id)
        {
            RebalanceTaskOperationResult result;
            result.code = RebalanceTaskOperationCode::kNotFound;
            result.error_detail = "rebalance task not found: " +
                                  std::string(task_id);
            return result;
        }
    }

    struct RebalanceManager::Impl
    {
        explicit Impl(const StorageNodeRegistry *registry_ptr,
                      RebalanceManagerConfig manager_config)
            : registry(registry_ptr), config(std::move(manager_config))
        {
        }

        mutable std::mutex mutex;
        const StorageNodeRegistry *registry{nullptr};
        RebalanceManagerConfig config;
        std::map<std::string, RebalanceTask, std::less<>> tasks;
        std::uint64_t submitted_tasks{0};
        std::uint64_t rejected_tasks{0};
        std::uint64_t total_attempts{0};
        std::string last_error_detail;
    };

    const char *ToString(const RebalanceTaskReason reason)
    {
        switch (reason)
        {
        case RebalanceTaskReason::kCapacityImbalance:
            return "CapacityImbalance";
        case RebalanceTaskReason::kHotspot:
            return "Hotspot";
        case RebalanceTaskReason::kNewNodeJoin:
            return "NewNodeJoin";
        case RebalanceTaskReason::kDraining:
            return "Draining";
        case RebalanceTaskReason::kMaintenance:
            return "Maintenance";
        }
        return "UnknownRebalanceTaskReason";
    }

    const char *ToString(const RebalanceTaskState state)
    {
        switch (state)
        {
        case RebalanceTaskState::kQueued:
            return "Queued";
        case RebalanceTaskState::kRunning:
            return "Running";
        case RebalanceTaskState::kCompleted:
            return "Completed";
        case RebalanceTaskState::kFailed:
            return "Failed";
        case RebalanceTaskState::kCancelled:
            return "Cancelled";
        case RebalanceTaskState::kRetryPending:
            return "RetryPending";
        }
        return "UnknownRebalanceTaskState";
    }

    const char *ToString(const RebalanceManagerSubmitCode code)
    {
        switch (code)
        {
        case RebalanceManagerSubmitCode::kAccepted:
            return "Accepted";
        case RebalanceManagerSubmitCode::kOverloaded:
            return "Overloaded";
        case RebalanceManagerSubmitCode::kInvalidArgument:
            return "InvalidArgument";
        case RebalanceManagerSubmitCode::kAlreadyExists:
            return "AlreadyExists";
        }
        return "UnknownRebalanceManagerSubmitCode";
    }

    const char *ToString(const RebalanceTaskOperationCode code)
    {
        switch (code)
        {
        case RebalanceTaskOperationCode::kOk:
            return "Ok";
        case RebalanceTaskOperationCode::kNotFound:
            return "NotFound";
        case RebalanceTaskOperationCode::kConflict:
            return "Conflict";
        case RebalanceTaskOperationCode::kInvalidArgument:
            return "InvalidArgument";
        }
        return "UnknownRebalanceTaskOperationCode";
    }

    StorageNodeStatusCode RebalanceManagerSubmitResult::status_code() const
    {
        switch (code)
        {
        case RebalanceManagerSubmitCode::kAccepted:
            return StorageNodeStatusCode::kOk;
        case RebalanceManagerSubmitCode::kOverloaded:
            return StorageNodeStatusCode::kOverloaded;
        case RebalanceManagerSubmitCode::kInvalidArgument:
            return StorageNodeStatusCode::kInvalidArgument;
        case RebalanceManagerSubmitCode::kAlreadyExists:
            return StorageNodeStatusCode::kAlreadyExists;
        }
        return StorageNodeStatusCode::kIoError;
    }

    StorageNodeStatusCode RebalanceTaskOperationResult::status_code() const
    {
        switch (code)
        {
        case RebalanceTaskOperationCode::kOk:
            return StorageNodeStatusCode::kOk;
        case RebalanceTaskOperationCode::kNotFound:
            return StorageNodeStatusCode::kNotFound;
        case RebalanceTaskOperationCode::kConflict:
            return StorageNodeStatusCode::kConflict;
        case RebalanceTaskOperationCode::kInvalidArgument:
            return StorageNodeStatusCode::kInvalidArgument;
        }
        return StorageNodeStatusCode::kIoError;
    }

    RebalanceManager::RebalanceManager(const StorageNodeRegistry *registry,
                                       RebalanceManagerConfig config)
        : config_(SanitizeRebalanceManagerConfig(std::move(config))),
          impl_(std::make_unique<Impl>(registry, config_))
    {
        if (registry == nullptr)
        {
            throw std::invalid_argument(
                "RebalanceManager requires a non-null registry");
        }
    }

    RebalanceManager::~RebalanceManager() = default;

    RebalanceManagerSubmitResult RebalanceManager::SubmitTask(
        const RebalanceTaskRequest &request)
    {
        RebalanceManagerSubmitResult result;
        auto normalized_request = request;
        const auto now_unix_ms = config_.now_unix_ms();

        auto status =
            ResolveChunkIdentity(&normalized_request.identity, &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            result.code = RebalanceManagerSubmitCode::kInvalidArgument;
        }
        else if (normalized_request.source_node.empty())
        {
            result.code = RebalanceManagerSubmitCode::kInvalidArgument;
            result.error_detail = "rebalance task requires source_node";
        }
        else if (normalized_request.target_node.empty())
        {
            result.code = RebalanceManagerSubmitCode::kInvalidArgument;
            result.error_detail = "rebalance task requires target_node";
        }
        else if (normalized_request.source_node == normalized_request.target_node)
        {
            result.code = RebalanceManagerSubmitCode::kInvalidArgument;
            result.error_detail = "rebalance source and target must differ";
        }
        else if (normalized_request.expected_size == 0)
        {
            result.code = RebalanceManagerSubmitCode::kInvalidArgument;
            result.error_detail = "rebalance task requires expected_size";
        }
        else if (!normalized_request.expected_checksum.IsSet())
        {
            result.code = RebalanceManagerSubmitCode::kInvalidArgument;
            result.error_detail = "rebalance task requires expected_checksum";
        }
        else
        {
            status = ValidateSourceState(normalized_request.source_state,
                                         &result.error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                result.code = RebalanceManagerSubmitCode::kInvalidArgument;
            }
        }

        if (result.code != RebalanceManagerSubmitCode::kAccepted)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        const auto snapshot = impl_->registry->Snapshot(now_unix_ms);
        if (!snapshot.ok())
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            result.code = RebalanceManagerSubmitCode::kInvalidArgument;
            result.error_detail = snapshot.error_detail;
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        const StorageNodeRegistryNodeSnapshot *source_snapshot = nullptr;
        const StorageNodeRegistryNodeSnapshot *target_snapshot = nullptr;
        for (const auto &node_snapshot : snapshot.nodes)
        {
            if (node_snapshot.node_id == normalized_request.source_node)
            {
                source_snapshot = &node_snapshot;
            }
            if (node_snapshot.node_id == normalized_request.target_node)
            {
                target_snapshot = &node_snapshot;
            }
        }

        status = ValidateSourceSnapshot(source_snapshot, &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            result.code = RebalanceManagerSubmitCode::kInvalidArgument;
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        status = ValidateTargetSnapshot(target_snapshot,
                                        normalized_request.expected_size,
                                        &result.error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            result.code = status == StorageNodeStatusCode::kOverloaded
                              ? RebalanceManagerSubmitCode::kOverloaded
                              : RebalanceManagerSubmitCode::kInvalidArgument;
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        const auto planned_task =
            MakeRebalanceTask(normalized_request, now_unix_ms);

        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->tasks.contains(planned_task.task_id))
        {
            result.code = RebalanceManagerSubmitCode::kAlreadyExists;
            result.error_detail = "rebalance task already exists";
            result.task = impl_->tasks.find(planned_task.task_id)->second;
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        std::size_t active_task_count = 0;
        for (const auto &[task_id, task] : impl_->tasks)
        {
            (void)task_id;
            if (IsActiveTaskState(task.state))
            {
                ++active_task_count;
            }
        }

        if (impl_->tasks.size() >= config_.max_tasks)
        {
            result.code = RebalanceManagerSubmitCode::kOverloaded;
            result.error_detail = "rebalance task registry is full";
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        if (active_task_count >= config_.max_active_tasks)
        {
            result.code = RebalanceManagerSubmitCode::kOverloaded;
            result.error_detail = "rebalance task queue is full";
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        auto [task_it, inserted] =
            impl_->tasks.emplace(planned_task.task_id, planned_task);
        (void)inserted;
        ++impl_->submitted_tasks;
        result.task = task_it->second;
        return result;
    }

    RebalanceTaskOperationResult RebalanceManager::MarkTaskRunning(
        std::string_view task_id)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return MakeTaskMutationNotFound(task_id);
        }
        if (task_it->second.state != RebalanceTaskState::kQueued &&
            task_it->second.state != RebalanceTaskState::kRetryPending)
        {
            RebalanceTaskOperationResult result;
            result.code = RebalanceTaskOperationCode::kConflict;
            result.error_detail = "rebalance task is not ready to run";
            result.task = task_it->second;
            return result;
        }

        task_it->second.state = RebalanceTaskState::kRunning;
        task_it->second.started_at_unix_ms = config_.now_unix_ms();
        task_it->second.progress_percent =
            std::max<std::uint32_t>(task_it->second.progress_percent, 1U);
        task_it->second.retry_after_ms = 0;
        task_it->second.last_error = StorageNodeStatusCode::kOk;
        task_it->second.last_error_detail.clear();
        ++task_it->second.attempts;
        ++impl_->total_attempts;

        RebalanceTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    RebalanceTaskOperationResult RebalanceManager::UpdateTaskProgress(
        std::string_view task_id,
        const std::uint32_t progress_percent)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return MakeTaskMutationNotFound(task_id);
        }
        if (progress_percent > 100)
        {
            RebalanceTaskOperationResult result;
            result.code = RebalanceTaskOperationCode::kInvalidArgument;
            result.error_detail = "rebalance task progress must be <= 100";
            result.task = task_it->second;
            return result;
        }
        if (task_it->second.state != RebalanceTaskState::kRunning)
        {
            RebalanceTaskOperationResult result;
            result.code = RebalanceTaskOperationCode::kConflict;
            result.error_detail =
                "rebalance task progress can only change while running";
            result.task = task_it->second;
            return result;
        }

        task_it->second.progress_percent = progress_percent;
        RebalanceTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    RebalanceTaskOperationResult RebalanceManager::CompleteTask(
        std::string_view task_id)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return MakeTaskMutationNotFound(task_id);
        }
        if (task_it->second.state != RebalanceTaskState::kRunning)
        {
            RebalanceTaskOperationResult result;
            result.code = RebalanceTaskOperationCode::kConflict;
            result.error_detail =
                "rebalance task can only complete from running";
            result.task = task_it->second;
            return result;
        }

        task_it->second.state = RebalanceTaskState::kCompleted;
        task_it->second.progress_percent = 100;
        task_it->second.completed_at_unix_ms = config_.now_unix_ms();
        task_it->second.last_error = StorageNodeStatusCode::kOk;
        task_it->second.last_error_detail.clear();
        task_it->second.retry_after_ms = 0;

        RebalanceTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    RebalanceTaskOperationResult RebalanceManager::FailTask(
        std::string_view task_id,
        const StorageNodeStatusCode error_code,
        std::string error_detail,
        const bool retryable,
        const std::uint64_t retry_after_ms)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return MakeTaskMutationNotFound(task_id);
        }
        if (task_it->second.state != RebalanceTaskState::kRunning &&
            task_it->second.state != RebalanceTaskState::kQueued)
        {
            RebalanceTaskOperationResult result;
            result.code = RebalanceTaskOperationCode::kConflict;
            result.error_detail =
                "rebalance task cannot fail from current state";
            result.task = task_it->second;
            return result;
        }
        if (error_code == StorageNodeStatusCode::kOk)
        {
            RebalanceTaskOperationResult result;
            result.code = RebalanceTaskOperationCode::kInvalidArgument;
            result.error_detail =
                "rebalance task failure requires non-ok error code";
            result.task = task_it->second;
            return result;
        }

        task_it->second.state =
            retryable ? RebalanceTaskState::kRetryPending
                      : RebalanceTaskState::kFailed;
        task_it->second.last_error = error_code;
        task_it->second.last_error_detail = std::move(error_detail);
        task_it->second.retry_after_ms = retry_after_ms;
        task_it->second.completed_at_unix_ms =
            retryable ? 0 : config_.now_unix_ms();
        impl_->last_error_detail = task_it->second.last_error_detail;

        RebalanceTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    RebalanceTaskOperationResult RebalanceManager::CancelTask(
        std::string_view task_id)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return MakeTaskMutationNotFound(task_id);
        }
        if (task_it->second.state != RebalanceTaskState::kQueued &&
            task_it->second.state != RebalanceTaskState::kRetryPending)
        {
            RebalanceTaskOperationResult result;
            result.code = RebalanceTaskOperationCode::kConflict;
            result.error_detail =
                "rebalance task cannot be cancelled from current state";
            result.task = task_it->second;
            return result;
        }

        task_it->second.state = RebalanceTaskState::kCancelled;
        task_it->second.last_error = StorageNodeStatusCode::kCancelled;
        task_it->second.last_error_detail = "rebalance task cancelled";
        task_it->second.completed_at_unix_ms = config_.now_unix_ms();
        task_it->second.retry_after_ms = 0;
        impl_->last_error_detail = task_it->second.last_error_detail;

        RebalanceTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    RebalanceTaskOperationResult RebalanceManager::RetryTask(
        std::string_view task_id)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return MakeTaskMutationNotFound(task_id);
        }
        if (task_it->second.state != RebalanceTaskState::kFailed)
        {
            RebalanceTaskOperationResult result;
            result.code = RebalanceTaskOperationCode::kConflict;
            result.error_detail = "rebalance task retry requires failed state";
            result.task = task_it->second;
            return result;
        }

        task_it->second.state = RebalanceTaskState::kRetryPending;
        task_it->second.progress_percent = 0;
        task_it->second.completed_at_unix_ms = 0;
        task_it->second.retry_after_ms = 0;

        RebalanceTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    std::optional<RebalanceTask> RebalanceManager::FindTask(
        std::string_view task_id) const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return std::nullopt;
        }
        return task_it->second;
    }

    std::vector<RebalanceTask> RebalanceManager::ListTasks() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        std::vector<RebalanceTask> tasks;
        tasks.reserve(impl_->tasks.size());
        for (const auto &[task_id, task] : impl_->tasks)
        {
            (void)task_id;
            tasks.push_back(task);
        }
        return tasks;
    }

    RebalanceManagerStats RebalanceManager::SnapshotStats() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        RebalanceManagerStats stats;
        stats.accepting_new_tasks = true;
        stats.max_active_tasks = config_.max_active_tasks;
        stats.max_tasks = config_.max_tasks;
        stats.total_tasks = impl_->tasks.size();
        stats.submitted_tasks = impl_->submitted_tasks;
        stats.rejected_tasks = impl_->rejected_tasks;
        stats.total_attempts = impl_->total_attempts;
        stats.last_error_detail = impl_->last_error_detail;

        for (const auto &[task_id, task] : impl_->tasks)
        {
            (void)task_id;
            switch (task.state)
            {
            case RebalanceTaskState::kQueued:
                ++stats.queued_tasks;
                break;
            case RebalanceTaskState::kRunning:
                ++stats.running_tasks;
                break;
            case RebalanceTaskState::kRetryPending:
                ++stats.retry_pending_tasks;
                break;
            case RebalanceTaskState::kCompleted:
                ++stats.completed_tasks;
                break;
            case RebalanceTaskState::kFailed:
                ++stats.failed_tasks;
                break;
            case RebalanceTaskState::kCancelled:
                ++stats.cancelled_tasks;
                break;
            }
        }

        return stats;
    }

    const RebalanceManagerConfig &RebalanceManager::config() const
    {
        return config_;
    }
}
