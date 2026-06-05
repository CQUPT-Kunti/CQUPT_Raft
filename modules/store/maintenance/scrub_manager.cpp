#include "store/maintenance/scrub_manager.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "store/node/storage_node_registry.h"
#include "store/placement/replica_policy.h"

namespace storedemo
{
    namespace
    {
        ScrubManagerConfig SanitizeScrubManagerConfig(ScrubManagerConfig config)
        {
            if (config.worker_count == 0)
            {
                config.worker_count = 1;
            }
            if (config.queue_capacity == 0)
            {
                config.queue_capacity = 1;
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

        bool IsTerminalTaskState(const ScrubTaskState state)
        {
            return state == ScrubTaskState::kCompleted ||
                   state == ScrubTaskState::kFailed ||
                   state == ScrubTaskState::kCancelled;
        }

        bool HasPendingTasks(const std::map<std::string, ScrubTask, std::less<>> &tasks)
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

        StorageExecutorStopMode ToExecutorStopMode(const ScrubManagerStopMode mode)
        {
            return mode == ScrubManagerStopMode::kCancelPending
                       ? StorageExecutorStopMode::kCancelPending
                       : StorageExecutorStopMode::kDrain;
        }

        StorageNodeStatusCode ResolveManifestIdentity(ScrubManifest *manifest,
                                                      std::string *error_detail)
        {
            if (manifest == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "scrub manifest must not be null";
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
                                "scrub manifest chunk identity does not match chunk_id";
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
                        "scrub manifest requires chunk_id or object identity";
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

        StorageNodeStatusCode ValidateTaskForSubmission(
            ScrubTask *task,
            const ScrubManagerConfig &config,
            std::string *error_detail)
        {
            if (task == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "scrub task must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (task->task_id.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "scrub task_id must not be empty";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            const auto identity_status =
                ResolveManifestIdentity(&task->manifest, error_detail);
            if (identity_status != StorageNodeStatusCode::kOk)
            {
                return identity_status;
            }

            if (task->manifest.replica_nodes.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "scrub manifest replica_nodes must not be empty";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (task->manifest.desired_replica_count == 0)
            {
                task->manifest.desired_replica_count = task->manifest.replica_nodes.size();
            }

            if (task->context.timeout_ms == 0)
            {
                task->context.timeout_ms = config.default_timeout_ms;
            }

            task->attempts = 0;
            task->last_error = StorageNodeStatusCode::kOk;
            task->last_error_detail.clear();
            task->state = ScrubTaskState::kQueued;
            task->result.reset();
            task->started_at_unix_ms = 0;
            task->completed_at_unix_ms = 0;
            return StorageNodeStatusCode::kOk;
        }

        StatChunkRequest MakeScrubStatRequest(const ChunkId &chunk_id,
                                              const std::string &request_id,
                                              const bool verify_checksum)
        {
            return StatChunkRequest{
                .request_id = request_id,
                .chunk_id = chunk_id,
                .include_quarantine = true,
                .verify_checksum = verify_checksum};
        }

        bool ChecksumEquals(const ChunkChecksum &lhs, const ChunkChecksum &rhs)
        {
            return lhs.algorithm == rhs.algorithm && lhs.value == rhs.value &&
                   lhs.size_bytes == rhs.size_bytes;
        }

        bool IsHealthyReplicaForRepairSource(
            const ScrubReplicaFact &fact,
            const StorageNodeRegistryNodeSnapshot *snapshot)
        {
            if (snapshot == nullptr)
            {
                return false;
            }
            if (!fact.checksum_verified || fact.known_corrupted || fact.known_missing)
            {
                return false;
            }
            if (snapshot->liveness != StorageNodeRegistryLiveness::kLive)
            {
                return false;
            }
            if (snapshot->facts.health.health != StorageNodeHealth::kHealthy)
            {
                return false;
            }
            if (snapshot->facts.health.disk_pressure ==
                    StorageNodeDiskPressure::kHigh ||
                snapshot->facts.health.disk_pressure ==
                    StorageNodeDiskPressure::kFull)
            {
                return false;
            }
            return true;
        }

        ScrubReplicaFact InspectReplica(ChunkStore *store,
                                        const StorageNodeId &node_id,
                                        const ScrubManifest &manifest,
                                        const std::size_t index)
        {
            ScrubReplicaFact fact;
            fact.node_id = node_id;

            if (store == nullptr)
            {
                fact.status = StorageNodeStatusCode::kNodeUnavailable;
                fact.known_missing = true;
                return fact;
            }

            const auto stat_request =
                MakeScrubStatRequest(manifest.identity.chunk_id,
                                     manifest.identity.chunk_id + "/scrub-stat/" +
                                         std::to_string(index),
                                     false);
            const auto initial = store->StatChunk(stat_request);
            fact.state_before = initial.metadata.state;
            fact.state_after = initial.metadata.state;
            fact.observed_size = initial.metadata.size;
            fact.observed_checksum = initial.metadata.checksum;

            if (initial.status == StorageNodeStatusCode::kNotFound)
            {
                fact.status = initial.status;
                fact.state_before = ChunkState::kMissing;
                fact.state_after = ChunkState::kMissing;
                fact.known_missing = true;
                fact.observed_size = 0;
                fact.observed_checksum = {};
                return fact;
            }

            if (!initial.ok())
            {
                fact.status = initial.status;
                fact.known_corrupted =
                    initial.status == StorageNodeStatusCode::kCorrupted;
                fact.quarantined =
                    initial.metadata.state == ChunkState::kQuarantined;
                return fact;
            }

            if (initial.metadata.state == ChunkState::kQuarantined ||
                initial.metadata.state == ChunkState::kCorrupted)
            {
                fact.status = StorageNodeStatusCode::kCorrupted;
                fact.known_corrupted = true;
                fact.quarantined =
                    initial.metadata.state == ChunkState::kQuarantined;
                return fact;
            }

            if (initial.metadata.state == ChunkState::kMissing ||
                initial.metadata.state == ChunkState::kDeleted)
            {
                fact.status = StorageNodeStatusCode::kNotFound;
                fact.known_missing = true;
                fact.state_after = ChunkState::kMissing;
                return fact;
            }

            if (initial.metadata.state != ChunkState::kLive)
            {
                fact.status = StorageNodeStatusCode::kConflict;
                return fact;
            }

            const auto verify_request =
                MakeScrubStatRequest(manifest.identity.chunk_id,
                                     manifest.identity.chunk_id + "/scrub-verify/" +
                                         std::to_string(index),
                                     true);
            const auto verify = store->StatChunk(verify_request);
            const auto post_verify = store->StatChunk(stat_request);
            if (post_verify.ok())
            {
                fact.state_after = post_verify.metadata.state;
                fact.observed_size = post_verify.metadata.size;
                fact.observed_checksum = post_verify.metadata.checksum;
            }
            else if (post_verify.status == StorageNodeStatusCode::kNotFound)
            {
                fact.state_after = ChunkState::kMissing;
                fact.observed_size = 0;
                fact.observed_checksum = {};
            }

            if (!verify.ok())
            {
                fact.status = verify.status;
                fact.known_corrupted =
                    verify.status == StorageNodeStatusCode::kCorrupted ||
                    fact.state_after == ChunkState::kQuarantined ||
                    fact.state_after == ChunkState::kCorrupted;
                fact.known_missing = verify.status == StorageNodeStatusCode::kNotFound;
                fact.quarantined = fact.state_after == ChunkState::kQuarantined;
                return fact;
            }

            fact.checksum_verified =
                verify.metadata.size == manifest.expected_size &&
                (!manifest.expected_checksum.IsSet() ||
                 ChecksumEquals(verify.metadata.checksum,
                                manifest.expected_checksum));
            fact.observed_size = verify.metadata.size;
            fact.observed_checksum = verify.metadata.checksum;
            fact.status = fact.checksum_verified
                              ? StorageNodeStatusCode::kOk
                              : StorageNodeStatusCode::kChecksumMismatch;
            fact.known_corrupted = !fact.checksum_verified;
            fact.quarantined = fact.state_after == ChunkState::kQuarantined;
            return fact;
        }

        ScrubTaskResult RunDefaultScrubTask(const ScrubTask &task,
                                            const std::map<StorageNodeId, ChunkStore *> &stores,
                                            const StorageNodeRegistry *registry,
                                            const std::uint64_t now_unix_ms)
        {
            ScrubTaskResult result;
            if (registry == nullptr)
            {
                result.status = StorageNodeStatusCode::kInvalidArgument;
                result.error_detail = "scrub manager requires a registry";
                return result;
            }

            const auto snapshot = registry->Snapshot(now_unix_ms);
            if (!snapshot.ok())
            {
                result.status = snapshot.status;
                result.error_detail = snapshot.error_detail;
                return result;
            }

            std::map<StorageNodeId, StorageNodeRegistryNodeSnapshot> snapshot_by_node;
            for (const auto &node : snapshot.nodes)
            {
                snapshot_by_node.emplace(node.node_id, node);
            }

            std::map<StorageNodeId, ScrubReplicaFact> fact_by_node;
            std::vector<ReadReplicaCandidate> supplemental_candidates;
            supplemental_candidates.reserve(task.manifest.replica_nodes.size());

            for (std::size_t index = 0; index < task.manifest.replica_nodes.size(); ++index)
            {
                const auto &node_id = task.manifest.replica_nodes[index];
                auto store_it = stores.find(node_id);
                ScrubReplicaFact fact = InspectReplica(store_it == stores.end()
                                                           ? nullptr
                                                           : store_it->second,
                                                       node_id,
                                                       task.manifest,
                                                       index);
                fact_by_node.emplace(node_id, fact);
                result.replica_facts.push_back(fact);

                if (fact.known_corrupted || fact.known_missing)
                {
                    supplemental_candidates.push_back(ReadReplicaCandidate{
                        .node_id = node_id,
                        .known_corrupted = fact.known_corrupted,
                        .known_missing = fact.known_missing,
                        .has_observed_facts = true});
                }
            }

            ReplicaPolicySelector selector;
            const auto selection = selector.SelectReadReplicas(
                ReadReplicaSelectionRequest{
                    .chunk_id = task.manifest.identity.chunk_id,
                    .replica_nodes = task.manifest.replica_nodes},
                snapshot,
                supplemental_candidates);
            if (!selection.ok() &&
                selection.status != StorageNodeStatusCode::kNodeUnavailable)
            {
                result.status = selection.status;
                result.error_detail = selection.error_detail;
                return result;
            }

            std::vector<StorageNodeId> bad_replicas;
            std::vector<StorageNodeId> healthy_sources;
            for (const auto &fact : result.replica_facts)
            {
                if (fact.known_corrupted || fact.known_missing)
                {
                    bad_replicas.push_back(fact.node_id);
                }
            }

            for (const auto &candidate : selection.decision.ordered_replicas)
            {
                const auto snapshot_it = snapshot_by_node.find(candidate.node_id);
                if (snapshot_it == snapshot_by_node.end())
                {
                    continue;
                }

                const auto fact_it = fact_by_node.find(candidate.node_id);
                if (fact_it == fact_by_node.end())
                {
                    continue;
                }

                const auto &node_snapshot = snapshot_it->second;
                if (!IsHealthyReplicaForRepairSource(fact_it->second, &node_snapshot))
                {
                    continue;
                }

                healthy_sources.push_back(candidate.node_id);
            }

            const auto healthy_replica_count = healthy_sources.size();
            const auto required_replica_count = task.manifest.desired_replica_count;
            const auto missing_replica_count =
                healthy_replica_count >= required_replica_count
                    ? 0U
                    : required_replica_count - healthy_replica_count;
            const bool under_replicated =
                healthy_replica_count < required_replica_count;
            if (!bad_replicas.empty() || under_replicated)
            {
                result.repair_candidate = ScrubRepairCandidate{
                    .chunk_id = task.manifest.identity.chunk_id,
                    .expected_size = task.manifest.expected_size,
                    .expected_checksum = task.manifest.expected_checksum,
                    .bad_replicas = std::move(bad_replicas),
                    .healthy_source_replicas = std::move(healthy_sources),
                    .healthy_replica_count = healthy_replica_count,
                    .required_replica_count = required_replica_count,
                    .missing_replica_count = missing_replica_count,
                    .under_replicated = under_replicated,
                    .lost_or_unrecoverable = false};
                result.repair_candidate->lost_or_unrecoverable =
                    result.repair_candidate->healthy_source_replicas.empty();
            }

            result.status = StorageNodeStatusCode::kOk;
            return result;
        }
    }

    struct ScrubManager::Impl
    {
        explicit Impl(std::map<StorageNodeId, ChunkStore *> store_map,
                      const StorageNodeRegistry *registry_ptr,
                      ScrubManagerConfig scrub_config,
                      ScrubTaskRunner scrub_runner)
            : stores(std::move(store_map))
            , registry(registry_ptr)
            , config(std::move(scrub_config))
            , executor(StorageExecutorConfig{
                  .worker_count = config.worker_count,
                  .queue_capacity = config.queue_capacity})
            , runner(std::move(scrub_runner))
        {
        }

        mutable std::mutex mutex;
        std::condition_variable cv;
        bool accepting_new_tasks{true};
        bool stop_requested{false};
        std::map<StorageNodeId, ChunkStore *> stores;
        const StorageNodeRegistry *registry{nullptr};
        ScrubManagerConfig config;
        BoundedStorageExecutor executor;
        ScrubTaskRunner runner;
        std::map<std::string, ScrubTask, std::less<>> tasks;
        std::uint64_t submitted_tasks{0};
        std::uint64_t rejected_tasks{0};
        std::uint64_t total_attempts{0};
        std::string last_error_detail;
    };

    const char *ToString(const ScrubTaskState state)
    {
        switch (state)
        {
        case ScrubTaskState::kQueued:
            return "Queued";
        case ScrubTaskState::kRunning:
            return "Running";
        case ScrubTaskState::kCompleted:
            return "Completed";
        case ScrubTaskState::kFailed:
            return "Failed";
        case ScrubTaskState::kCancelled:
            return "Cancelled";
        }
        return "UnknownScrubTaskState";
    }

    const char *ToString(const ScrubManagerSubmitCode code)
    {
        switch (code)
        {
        case ScrubManagerSubmitCode::kAccepted:
            return "Accepted";
        case ScrubManagerSubmitCode::kOverloaded:
            return "Overloaded";
        case ScrubManagerSubmitCode::kStopped:
            return "Stopped";
        case ScrubManagerSubmitCode::kInvalidArgument:
            return "InvalidArgument";
        case ScrubManagerSubmitCode::kAlreadyExists:
            return "AlreadyExists";
        }
        return "UnknownScrubManagerSubmitCode";
    }

    const char *ToString(const ScrubManagerStopMode mode)
    {
        switch (mode)
        {
        case ScrubManagerStopMode::kDrain:
            return "Drain";
        case ScrubManagerStopMode::kCancelPending:
            return "CancelPending";
        }
        return "UnknownScrubManagerStopMode";
    }

    StorageNodeStatusCode ScrubManagerSubmitResult::status_code() const
    {
        switch (code)
        {
        case ScrubManagerSubmitCode::kAccepted:
            return StorageNodeStatusCode::kOk;
        case ScrubManagerSubmitCode::kOverloaded:
            return StorageNodeStatusCode::kOverloaded;
        case ScrubManagerSubmitCode::kStopped:
            return StorageNodeStatusCode::kNodeUnavailable;
        case ScrubManagerSubmitCode::kInvalidArgument:
        case ScrubManagerSubmitCode::kAlreadyExists:
            return StorageNodeStatusCode::kInvalidArgument;
        }
        return StorageNodeStatusCode::kIoError;
    }

    ScrubManager::ScrubManager(std::map<StorageNodeId, ChunkStore *> stores,
                               const StorageNodeRegistry *registry,
                               ScrubManagerConfig config,
                               ScrubTaskRunner task_runner)
        : config_(SanitizeScrubManagerConfig(std::move(config)))
        , impl_(std::make_unique<Impl>(std::move(stores),
                                       registry,
                                       config_,
                                       std::move(task_runner)))
    {
        if (registry == nullptr)
        {
            throw std::invalid_argument("ScrubManager requires a non-null registry");
        }
    }

    ScrubManager::~ScrubManager()
    {
        (void)Stop(ScrubManagerStopRequest{.mode = ScrubManagerStopMode::kDrain});
    }

    ScrubManagerSubmitResult ScrubManager::SubmitTask(ScrubTask task)
    {
        ScrubManagerSubmitResult result;

        std::string error_detail;
        const auto validation_status =
            ValidateTaskForSubmission(&task, config_, &error_detail);
        if (validation_status != StorageNodeStatusCode::kOk)
        {
            result.code = ScrubManagerSubmitCode::kInvalidArgument;
            result.error_detail = std::move(error_detail);
            std::lock_guard<std::mutex> lock(impl_->mutex);
            ++impl_->rejected_tasks;
            impl_->last_error_detail = result.error_detail;
            return result;
        }

        const auto now_unix_ms = config_.now_unix_ms();
        task.submitted_at_unix_ms = now_unix_ms;

        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (!impl_->accepting_new_tasks)
            {
                result.code = ScrubManagerSubmitCode::kStopped;
                result.error_detail = "scrub manager is not accepting new tasks";
                ++impl_->rejected_tasks;
                impl_->last_error_detail = result.error_detail;
                return result;
            }
            if (impl_->tasks.contains(task.task_id))
            {
                result.code = ScrubManagerSubmitCode::kAlreadyExists;
                result.error_detail = "scrub task_id already exists";
                ++impl_->rejected_tasks;
                impl_->last_error_detail = result.error_detail;
                return result;
            }
            impl_->tasks.emplace(task.task_id, task);
        }

        const auto submit_result = impl_->executor.Submit(
            StorageExecutorSubmitRequest{
                .task_name = task.task_id,
                .context = task.context,
                .task = [this, task_id = task.task_id]()
                {
                    ScrubTask task_snapshot;
                    {
                        std::lock_guard<std::mutex> lock(impl_->mutex);
                        const auto task_it = impl_->tasks.find(task_id);
                        if (task_it == impl_->tasks.end())
                        {
                            return;
                        }
                        if (task_it->second.state == ScrubTaskState::kCancelled)
                        {
                            impl_->cv.notify_all();
                            return;
                        }

                        task_it->second.state = ScrubTaskState::kRunning;
                        task_it->second.started_at_unix_ms = config_.now_unix_ms();
                        ++task_it->second.attempts;
                        ++impl_->total_attempts;
                        task_snapshot = task_it->second;
                    }

                    ScrubTaskResult task_result;
                    try
                    {
                        if (impl_->runner)
                        {
                            task_result = impl_->runner(task_snapshot);
                        }
                        else
                        {
                            task_result = RunDefaultScrubTask(task_snapshot,
                                                              impl_->stores,
                                                              impl_->registry,
                                                              config_.now_unix_ms());
                        }
                    }
                    catch (const std::exception &ex)
                    {
                        task_result.status = StorageNodeStatusCode::kIoError;
                        task_result.error_detail = ex.what();
                    }
                    catch (...)
                    {
                        task_result.status = StorageNodeStatusCode::kIoError;
                        task_result.error_detail = "unknown scrub task exception";
                    }

                    {
                        std::lock_guard<std::mutex> lock(impl_->mutex);
                        const auto task_it = impl_->tasks.find(task_id);
                        if (task_it == impl_->tasks.end())
                        {
                            impl_->cv.notify_all();
                            return;
                        }

                        if (task_it->second.state == ScrubTaskState::kCancelled)
                        {
                            task_it->second.completed_at_unix_ms = config_.now_unix_ms();
                            impl_->cv.notify_all();
                            return;
                        }

                        task_it->second.result = task_result;
                        task_it->second.last_error = task_result.status;
                        task_it->second.last_error_detail = task_result.error_detail;
                        task_it->second.completed_at_unix_ms = config_.now_unix_ms();
                        task_it->second.state =
                            task_result.status == StorageNodeStatusCode::kOk
                                ? ScrubTaskState::kCompleted
                                : ScrubTaskState::kFailed;
                        if (task_result.status != StorageNodeStatusCode::kOk)
                        {
                            impl_->last_error_detail = task_result.error_detail;
                        }
                    }

                    impl_->cv.notify_all();
                }});

        if (!submit_result.accepted())
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->tasks.erase(task.task_id);
            ++impl_->rejected_tasks;
            impl_->last_error_detail = submit_result.error_detail;
            result.queue_depth = submit_result.queue_depth;
            result.retry_after_ms = submit_result.retry_after_ms;
            switch (submit_result.code)
            {
            case StorageExecutorSubmitCode::kOverloaded:
                result.code = ScrubManagerSubmitCode::kOverloaded;
                break;
            case StorageExecutorSubmitCode::kStopped:
                result.code = ScrubManagerSubmitCode::kStopped;
                break;
            case StorageExecutorSubmitCode::kInvalidArgument:
            case StorageExecutorSubmitCode::kAccepted:
                result.code = ScrubManagerSubmitCode::kInvalidArgument;
                break;
            }
            result.error_detail = submit_result.error_detail;
            return result;
        }

        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            ++impl_->submitted_tasks;
        }
        result.queue_depth = submit_result.queue_depth;
        return result;
    }

    ScrubManagerDrainResult ScrubManager::Drain()
    {
        ScrubManagerDrainResult result;
        std::unique_lock<std::mutex> lock(impl_->mutex);
        impl_->cv.wait(lock,
                       [this]()
                       {
                           return !HasPendingTasks(impl_->tasks);
                       });
        result.drained = true;
        return result;
    }

    ScrubManagerStopResult ScrubManager::Stop(ScrubManagerStopRequest request)
    {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->accepting_new_tasks = false;
            impl_->stop_requested = true;

            if (request.mode == ScrubManagerStopMode::kCancelPending)
            {
                for (auto &[task_id, task] : impl_->tasks)
                {
                    (void)task_id;
                    if (task.state != ScrubTaskState::kQueued)
                    {
                        continue;
                    }

                    task.state = ScrubTaskState::kCancelled;
                    task.last_error = StorageNodeStatusCode::kCancelled;
                    task.last_error_detail =
                        "scrub task cancelled before execution";
                    task.completed_at_unix_ms = config_.now_unix_ms();
                    ScrubTaskResult cancelled_result;
                    cancelled_result.status = StorageNodeStatusCode::kCancelled;
                    cancelled_result.error_detail =
                        "scrub task cancelled before execution";
                    task.result = std::move(cancelled_result);
                }
            }
        }

        const auto shutdown_result = impl_->executor.Shutdown(
            StorageExecutorShutdownRequest{
                .mode = ToExecutorStopMode(request.mode)});
        impl_->cv.notify_all();

        ScrubManagerStopResult result;
        result.error_detail = shutdown_result.error_detail;
        result.stats = SnapshotStats();
        result.stopped = !result.stats.accepting_new_tasks &&
                         result.stats.queued_tasks == 0 &&
                         result.stats.running_tasks == 0;
        result.drained = result.stats.queued_tasks == 0 &&
                         result.stats.running_tasks == 0;
        return result;
    }

    std::optional<ScrubTask> ScrubManager::FindTask(std::string_view task_id) const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto task_it = impl_->tasks.find(std::string(task_id));
        if (task_it == impl_->tasks.end())
        {
            return std::nullopt;
        }
        return task_it->second;
    }

    std::vector<ScrubTask> ScrubManager::ListTasks() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        std::vector<ScrubTask> tasks;
        tasks.reserve(impl_->tasks.size());
        for (const auto &[task_id, task] : impl_->tasks)
        {
            (void)task_id;
            tasks.push_back(task);
        }
        return tasks;
    }

    ScrubManagerStats ScrubManager::SnapshotStats() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);

        ScrubManagerStats stats;
        stats.accepting_new_tasks = impl_->accepting_new_tasks;
        stats.stop_requested = impl_->stop_requested;
        stats.worker_count = config_.worker_count;
        stats.queue_capacity = config_.queue_capacity;
        stats.submitted_tasks = impl_->submitted_tasks;
        stats.rejected_tasks = impl_->rejected_tasks;
        stats.total_attempts = impl_->total_attempts;
        stats.last_error_detail = impl_->last_error_detail;

        for (const auto &[task_id, task] : impl_->tasks)
        {
            (void)task_id;
            switch (task.state)
            {
            case ScrubTaskState::kQueued:
                ++stats.queued_tasks;
                break;
            case ScrubTaskState::kRunning:
                ++stats.running_tasks;
                break;
            case ScrubTaskState::kCompleted:
                ++stats.completed_tasks;
                break;
            case ScrubTaskState::kFailed:
                ++stats.failed_tasks;
                break;
            case ScrubTaskState::kCancelled:
                ++stats.cancelled_tasks;
                break;
            }
        }

        return stats;
    }

    const ScrubManagerConfig &ScrubManager::config() const
    {
        return config_;
    }
}
