#include "store/maintenance/repair_manager.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include "store/node/storage_node_registry.h"
#include "store/placement/placement_manager.h"

namespace storedemo
{
    namespace
    {
        RepairManagerConfig SanitizeRepairManagerConfig(RepairManagerConfig config)
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

        bool IsTerminalTaskState(const RepairTaskState state)
        {
            return state == RepairTaskState::kCompleted ||
                   state == RepairTaskState::kFailed ||
                   state == RepairTaskState::kCancelled;
        }

        bool IsActiveTaskState(const RepairTaskState state)
        {
            return state == RepairTaskState::kQueued ||
                   state == RepairTaskState::kRunning ||
                   state == RepairTaskState::kRetryPending;
        }

        std::string BuildRepairTaskId(const ChunkId &chunk_id,
                                      const ChunkChecksum &expected_checksum,
                                      const std::uint64_t expected_size,
                                      const StorageNodeId &source_node,
                                      const StorageNodeId &target_node)
        {
            return chunk_id + "|repair|" + expected_checksum.value + "|" +
                   std::to_string(expected_size) + "|" + source_node + "|" +
                   target_node;
        }

        StorageNodeStatusCode ResolveManifestIdentity(ScrubManifest *manifest,
                                                      std::string *error_detail)
        {
            if (manifest == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "repair manifest must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (!manifest->identity.chunk_id.empty())
            {
                std::string validation_error;
                const auto status =
                    ValidateChunkId(manifest->identity.chunk_id, &validation_error);
                if (status != StorageNodeStatusCode::kOk)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = std::move(validation_error);
                    }
                    return status;
                }

                if (!manifest->identity.object_id.empty())
                {
                    ChunkId expected_chunk_id;
                    const auto derive_status = MakeChunkId(manifest->identity.object_id,
                                                           manifest->identity.version,
                                                           manifest->identity.chunk_index,
                                                           &expected_chunk_id,
                                                           error_detail);
                    if (derive_status != StorageNodeStatusCode::kOk)
                    {
                        return derive_status;
                    }
                    if (expected_chunk_id != manifest->identity.chunk_id)
                    {
                        if (error_detail != nullptr)
                        {
                            *error_detail =
                                "repair manifest chunk identity does not match chunk_id";
                        }
                        return StorageNodeStatusCode::kInvalidArgument;
                    }
                }

                return StorageNodeStatusCode::kOk;
            }

            if (manifest->identity.object_id.empty() || manifest->identity.version == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "repair manifest requires chunk_id or object identity";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            ChunkId chunk_id;
            const auto status = MakeChunkId(manifest->identity.object_id,
                                            manifest->identity.version,
                                            manifest->identity.chunk_index,
                                            &chunk_id,
                                            error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }
            manifest->identity.chunk_id = std::move(chunk_id);
            return StorageNodeStatusCode::kOk;
        }

        bool IsHealthyRepairSource(const StorageNodeRegistryNodeSnapshot &snapshot)
        {
            return snapshot.liveness == StorageNodeRegistryLiveness::kLive &&
                   snapshot.facts.health.health == StorageNodeHealth::kHealthy &&
                   snapshot.facts.health.disk_pressure !=
                       StorageNodeDiskPressure::kHigh &&
                   snapshot.facts.health.disk_pressure !=
                       StorageNodeDiskPressure::kFull;
        }

        StorageNodeStatusCode SelectSourceNode(
            const std::vector<StorageNodeId> &candidate_sources,
            const std::map<StorageNodeId, StorageNodeRegistryNodeSnapshot, std::less<>> &snapshots,
            StorageNodeId *selected_source,
            std::string *error_detail)
        {
            if (selected_source == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "selected source output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            for (const auto &node_id : candidate_sources)
            {
                const auto snapshot_it = snapshots.find(node_id);
                if (snapshot_it == snapshots.end())
                {
                    continue;
                }
                if (!IsHealthyRepairSource(snapshot_it->second))
                {
                    continue;
                }
                *selected_source = node_id;
                return StorageNodeStatusCode::kOk;
            }

            if (error_detail != nullptr)
            {
                *error_detail = "no healthy repair source is available";
            }
            return StorageNodeStatusCode::kInvalidArgument;
        }

        StorageNodeStatusCode SelectTargetNode(const ScrubManifest &manifest,
                                               const StorageNodeRegistry &registry,
                                               const std::uint64_t now_unix_ms,
                                               StorageNodeId *selected_target,
                                               std::string *error_detail)
        {
            if (selected_target == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "selected target output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            PlacementManager placement_manager;
            PlacementRequest placement_request;
            placement_request.identity = manifest.identity;
            placement_request.chunk_size_bytes = manifest.expected_size;
            placement_request.policy.replica_count = 1;
            placement_request.policy.minimum_successful_writes = 1;
            placement_request.excluded_nodes = manifest.replica_nodes;
            placement_request.decision_epoch = now_unix_ms;

            const auto placement = placement_manager.SelectPlacement(
                placement_request, registry, now_unix_ms);
            if (!placement.ok() || placement.decision.replica_nodes.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = placement.error_detail.empty()
                                        ? "no healthy repair target is available"
                                        : placement.error_detail;
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            *selected_target = placement.decision.replica_nodes.front().node_id;
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ValidateSubmitRequest(RepairTaskRequest *request,
                                                    const StorageNodeRegistry *registry,
                                                    StorageNodeId *selected_source,
                                                    StorageNodeId *selected_target,
                                                    std::string *error_detail,
                                                    const std::uint64_t now_unix_ms)
        {
            if (request == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "repair task request must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
            if (registry == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "repair manager requires a registry";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            const auto identity_status =
                ResolveManifestIdentity(&request->manifest, error_detail);
            if (identity_status != StorageNodeStatusCode::kOk)
            {
                return identity_status;
            }

            if (request->manifest.expected_size == 0 ||
                request->repair_candidate.expected_size == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "repair task requires expected_size";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (!request->manifest.expected_checksum.IsSet() ||
                !request->repair_candidate.expected_checksum.IsSet())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "repair task requires expected_checksum";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request->repair_candidate.chunk_id.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "repair candidate requires chunk_id";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request->repair_candidate.chunk_id != request->manifest.identity.chunk_id)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "repair candidate chunk_id does not match repair manifest";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request->repair_candidate.expected_size != request->manifest.expected_size)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "repair candidate expected_size does not match repair manifest";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request->repair_candidate.expected_checksum.algorithm !=
                    request->manifest.expected_checksum.algorithm ||
                request->repair_candidate.expected_checksum.value !=
                    request->manifest.expected_checksum.value ||
                request->repair_candidate.expected_checksum.size_bytes !=
                    request->manifest.expected_checksum.size_bytes)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "repair candidate expected_checksum does not match repair manifest";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request->repair_candidate.healthy_source_replicas.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "repair candidate must include at least one healthy source replica";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            const auto snapshot = registry->Snapshot(now_unix_ms);
            if (!snapshot.ok())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = snapshot.error_detail;
                }
                return snapshot.status;
            }

            std::map<StorageNodeId, StorageNodeRegistryNodeSnapshot, std::less<>> snapshots;
            for (const auto &node : snapshot.nodes)
            {
                snapshots.emplace(node.node_id, node);
            }

            const auto source_status = SelectSourceNode(
                request->repair_candidate.healthy_source_replicas,
                snapshots,
                selected_source,
                error_detail);
            if (source_status != StorageNodeStatusCode::kOk)
            {
                return source_status;
            }

            return SelectTargetNode(request->manifest,
                                    *registry,
                                    now_unix_ms,
                                    selected_target,
                                    error_detail);
        }

        RepairTask MakeRepairTask(const RepairTaskRequest &request,
                                  const StorageNodeId &source_node,
                                  const StorageNodeId &target_node,
                                  const std::uint64_t now_unix_ms)
        {
            RepairTask task;
            task.identity = request.manifest.identity;
            task.chunk_id = request.repair_candidate.chunk_id;
            task.source_node = source_node;
            task.target_node = target_node;
            task.expected_checksum = request.repair_candidate.expected_checksum;
            task.expected_size = request.repair_candidate.expected_size;
            task.existing_replica_nodes = request.manifest.replica_nodes;
            task.bad_replicas = request.repair_candidate.bad_replicas;
            task.task_id = BuildRepairTaskId(task.chunk_id,
                                             task.expected_checksum,
                                             task.expected_size,
                                             task.source_node,
                                             task.target_node);
            task.submitted_at_unix_ms = now_unix_ms;
            return task;
        }

        RepairTaskOperationResult MakeTaskMutationNotFound(std::string_view task_id)
        {
            RepairTaskOperationResult result;
            result.code = RepairTaskOperationCode::kNotFound;
            result.error_detail = "repair task not found: " + std::string(task_id);
            return result;
        }
    }

    struct RepairManager::Impl
    {
        explicit Impl(const StorageNodeRegistry *registry_ptr,
                      RepairManagerConfig manager_config)
            : registry(registry_ptr)
            , config(std::move(manager_config))
        {
        }

        mutable std::mutex mutex;
        const StorageNodeRegistry *registry{nullptr};
        RepairManagerConfig config;
        std::map<std::string, RepairTask, std::less<>> tasks;
        std::uint64_t submitted_tasks{0};
        std::uint64_t rejected_tasks{0};
        std::uint64_t total_attempts{0};
        std::string last_error_detail;
    };

    const char *ToString(const RepairTaskState state)
    {
        switch (state)
        {
        case RepairTaskState::kQueued:
            return "Queued";
        case RepairTaskState::kRunning:
            return "Running";
        case RepairTaskState::kCompleted:
            return "Completed";
        case RepairTaskState::kFailed:
            return "Failed";
        case RepairTaskState::kCancelled:
            return "Cancelled";
        case RepairTaskState::kRetryPending:
            return "RetryPending";
        }
        return "UnknownRepairTaskState";
    }

    const char *ToString(const RepairManagerSubmitCode code)
    {
        switch (code)
        {
        case RepairManagerSubmitCode::kAccepted:
            return "Accepted";
        case RepairManagerSubmitCode::kOverloaded:
            return "Overloaded";
        case RepairManagerSubmitCode::kInvalidArgument:
            return "InvalidArgument";
        case RepairManagerSubmitCode::kAlreadyExists:
            return "AlreadyExists";
        }
        return "UnknownRepairManagerSubmitCode";
    }

    const char *ToString(const RepairTaskOperationCode code)
    {
        switch (code)
        {
        case RepairTaskOperationCode::kOk:
            return "Ok";
        case RepairTaskOperationCode::kNotFound:
            return "NotFound";
        case RepairTaskOperationCode::kConflict:
            return "Conflict";
        case RepairTaskOperationCode::kInvalidArgument:
            return "InvalidArgument";
        }
        return "UnknownRepairTaskOperationCode";
    }

    StorageNodeStatusCode RepairManagerSubmitResult::status_code() const
    {
        switch (code)
        {
        case RepairManagerSubmitCode::kAccepted:
            return StorageNodeStatusCode::kOk;
        case RepairManagerSubmitCode::kOverloaded:
            return StorageNodeStatusCode::kOverloaded;
        case RepairManagerSubmitCode::kInvalidArgument:
        case RepairManagerSubmitCode::kAlreadyExists:
            return StorageNodeStatusCode::kInvalidArgument;
        }
        return StorageNodeStatusCode::kIoError;
    }

    StorageNodeStatusCode RepairTaskOperationResult::status_code() const
    {
        switch (code)
        {
        case RepairTaskOperationCode::kOk:
            return StorageNodeStatusCode::kOk;
        case RepairTaskOperationCode::kNotFound:
            return StorageNodeStatusCode::kNotFound;
        case RepairTaskOperationCode::kConflict:
            return StorageNodeStatusCode::kConflict;
        case RepairTaskOperationCode::kInvalidArgument:
            return StorageNodeStatusCode::kInvalidArgument;
        }
        return StorageNodeStatusCode::kIoError;
    }

    RepairManager::RepairManager(const StorageNodeRegistry *registry,
                                 RepairManagerConfig config)
        : config_(SanitizeRepairManagerConfig(std::move(config)))
        , impl_(std::make_unique<Impl>(registry, config_))
    {
        if (registry == nullptr)
        {
            throw std::invalid_argument("RepairManager requires a non-null registry");
        }
    }

    RepairManager::~RepairManager() = default;

    RepairManagerSubmitResult RepairManager::SubmitTask(const RepairTaskRequest &request)
    {
        RepairManagerSubmitResult result;
        auto request_copy = request;
        StorageNodeId selected_source;
        StorageNodeId selected_target;
        std::string error_detail;
        const auto now_unix_ms = config_.now_unix_ms();
        const auto validation_status = ValidateSubmitRequest(&request_copy,
                                                             impl_->registry,
                                                             &selected_source,
                                                             &selected_target,
                                                             &error_detail,
                                                             now_unix_ms);
        if (validation_status != StorageNodeStatusCode::kOk)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            result.code = RepairManagerSubmitCode::kInvalidArgument;
            result.error_detail = std::move(error_detail);
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        const auto planned_task = MakeRepairTask(request_copy,
                                                 selected_source,
                                                 selected_target,
                                                 now_unix_ms);

        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->tasks.contains(planned_task.task_id))
        {
            result.code = RepairManagerSubmitCode::kAlreadyExists;
            result.error_detail = "repair task already exists";
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
            result.code = RepairManagerSubmitCode::kOverloaded;
            result.error_detail = "repair task registry is full";
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        if (active_task_count >= config_.max_active_tasks)
        {
            result.code = RepairManagerSubmitCode::kOverloaded;
            result.error_detail = "repair task queue is full";
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        auto [task_it, inserted] = impl_->tasks.emplace(planned_task.task_id, planned_task);
        (void)inserted;
        ++impl_->submitted_tasks;
        result.task = task_it->second;
        return result;
    }

    RepairTaskOperationResult RepairManager::MarkTaskRunning(std::string_view task_id)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return MakeTaskMutationNotFound(task_id);
        }
        if (task_it->second.state != RepairTaskState::kQueued &&
            task_it->second.state != RepairTaskState::kRetryPending)
        {
            RepairTaskOperationResult result;
            result.code = RepairTaskOperationCode::kConflict;
            result.error_detail = "repair task is not ready to run";
            result.task = task_it->second;
            return result;
        }

        task_it->second.state = RepairTaskState::kRunning;
        task_it->second.started_at_unix_ms = config_.now_unix_ms();
        task_it->second.progress_percent =
            std::max<std::uint32_t>(task_it->second.progress_percent, 1U);
        task_it->second.retry_after_ms = 0;
        task_it->second.last_error = StorageNodeStatusCode::kOk;
        task_it->second.last_error_detail.clear();
        ++task_it->second.attempts;
        ++impl_->total_attempts;

        RepairTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    RepairTaskOperationResult RepairManager::UpdateTaskProgress(
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
            RepairTaskOperationResult result;
            result.code = RepairTaskOperationCode::kInvalidArgument;
            result.error_detail = "repair task progress must be <= 100";
            result.task = task_it->second;
            return result;
        }
        if (task_it->second.state != RepairTaskState::kRunning)
        {
            RepairTaskOperationResult result;
            result.code = RepairTaskOperationCode::kConflict;
            result.error_detail = "repair task progress can only change while running";
            result.task = task_it->second;
            return result;
        }

        task_it->second.progress_percent = progress_percent;
        RepairTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    RepairTaskOperationResult RepairManager::CompleteTask(std::string_view task_id)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return MakeTaskMutationNotFound(task_id);
        }
        if (task_it->second.state != RepairTaskState::kRunning)
        {
            RepairTaskOperationResult result;
            result.code = RepairTaskOperationCode::kConflict;
            result.error_detail = "repair task can only complete from running";
            result.task = task_it->second;
            return result;
        }

        task_it->second.state = RepairTaskState::kCompleted;
        task_it->second.progress_percent = 100;
        task_it->second.completed_at_unix_ms = config_.now_unix_ms();
        task_it->second.last_error = StorageNodeStatusCode::kOk;
        task_it->second.last_error_detail.clear();
        task_it->second.retry_after_ms = 0;

        RepairTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    RepairTaskOperationResult RepairManager::FailTask(std::string_view task_id,
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
        if (task_it->second.state != RepairTaskState::kRunning &&
            task_it->second.state != RepairTaskState::kQueued)
        {
            RepairTaskOperationResult result;
            result.code = RepairTaskOperationCode::kConflict;
            result.error_detail = "repair task cannot fail from current state";
            result.task = task_it->second;
            return result;
        }
        if (error_code == StorageNodeStatusCode::kOk)
        {
            RepairTaskOperationResult result;
            result.code = RepairTaskOperationCode::kInvalidArgument;
            result.error_detail = "repair task failure requires non-ok error code";
            result.task = task_it->second;
            return result;
        }

        task_it->second.state =
            retryable ? RepairTaskState::kRetryPending : RepairTaskState::kFailed;
        task_it->second.last_error = error_code;
        task_it->second.last_error_detail = std::move(error_detail);
        task_it->second.retry_after_ms = retry_after_ms;
        task_it->second.completed_at_unix_ms =
            retryable ? 0 : config_.now_unix_ms();
        impl_->last_error_detail = task_it->second.last_error_detail;

        RepairTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    RepairTaskOperationResult RepairManager::CancelTask(std::string_view task_id)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return MakeTaskMutationNotFound(task_id);
        }
        if (task_it->second.state != RepairTaskState::kQueued &&
            task_it->second.state != RepairTaskState::kRetryPending)
        {
            RepairTaskOperationResult result;
            result.code = RepairTaskOperationCode::kConflict;
            result.error_detail = "repair task cannot be cancelled from current state";
            result.task = task_it->second;
            return result;
        }

        task_it->second.state = RepairTaskState::kCancelled;
        task_it->second.last_error = StorageNodeStatusCode::kCancelled;
        task_it->second.last_error_detail = "repair task cancelled";
        task_it->second.completed_at_unix_ms = config_.now_unix_ms();
        task_it->second.retry_after_ms = 0;
        impl_->last_error_detail = task_it->second.last_error_detail;

        RepairTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    RepairTaskOperationResult RepairManager::RetryTask(std::string_view task_id)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return MakeTaskMutationNotFound(task_id);
        }
        if (task_it->second.state != RepairTaskState::kFailed)
        {
            RepairTaskOperationResult result;
            result.code = RepairTaskOperationCode::kConflict;
            result.error_detail = "repair task retry requires failed state";
            result.task = task_it->second;
            return result;
        }

        task_it->second.state = RepairTaskState::kRetryPending;
        task_it->second.progress_percent = 0;
        task_it->second.completed_at_unix_ms = 0;
        task_it->second.retry_after_ms = 0;

        RepairTaskOperationResult result;
        result.task = task_it->second;
        return result;
    }

    std::optional<RepairTask> RepairManager::FindTask(std::string_view task_id) const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return std::nullopt;
        }
        return task_it->second;
    }

    std::vector<RepairTask> RepairManager::ListTasks() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        std::vector<RepairTask> tasks;
        tasks.reserve(impl_->tasks.size());
        for (const auto &[task_id, task] : impl_->tasks)
        {
            (void)task_id;
            tasks.push_back(task);
        }
        return tasks;
    }

    RepairManagerStats RepairManager::SnapshotStats() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        RepairManagerStats stats;
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
            case RepairTaskState::kQueued:
                ++stats.queued_tasks;
                break;
            case RepairTaskState::kRunning:
                ++stats.running_tasks;
                break;
            case RepairTaskState::kRetryPending:
                ++stats.retry_pending_tasks;
                break;
            case RepairTaskState::kCompleted:
                ++stats.completed_tasks;
                break;
            case RepairTaskState::kFailed:
                ++stats.failed_tasks;
                break;
            case RepairTaskState::kCancelled:
                ++stats.cancelled_tasks;
                break;
            }
        }

        return stats;
    }

    const RepairManagerConfig &RepairManager::config() const
    {
        return config_;
    }
}
