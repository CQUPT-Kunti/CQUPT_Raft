#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "store/chunk/chunk_store.h"
#include "store/maintenance/scrub_manager.h"
#include "store/runtime/storage_executor.h"

namespace storedemo
{
    class StorageNodeRegistry;

    enum class RepairTaskState : std::uint8_t
    {
        kQueued = 0,
        kRunning = 1,
        kCompleted = 2,
        kFailed = 3,
        kCancelled = 4,
        kRetryPending = 5,
    };

    enum class RepairManagerSubmitCode : std::uint8_t
    {
        kAccepted = 0,
        kOverloaded = 1,
        kInvalidArgument = 2,
        kAlreadyExists = 3,
    };

    enum class UnderReplicatedTaskSubmitCode : std::uint8_t
    {
        kAccepted = 0,
        kAlreadyExists = 1,
        kOverloaded = 2,
        kInvalidArgument = 3,
        kNotUnderReplicated = 4,
        kLostOrUnrecoverable = 5,
        kNoHealthySource = 6,
        kNoHealthyTarget = 7,
    };

    enum class RepairTaskOperationCode : std::uint8_t
    {
        kOk = 0,
        kNotFound = 1,
        kConflict = 2,
        kInvalidArgument = 3,
    };

    const char *ToString(RepairTaskState state);
    const char *ToString(RepairManagerSubmitCode code);
    const char *ToString(UnderReplicatedTaskSubmitCode code);
    const char *ToString(RepairTaskOperationCode code);

    struct RepairTaskRequest
    {
        ScrubManifest manifest;
        ScrubRepairCandidate repair_candidate;
        StorageTaskContext context;
    };

    struct RepairTask
    {
        std::string task_id;
        ChunkIdentity identity;
        ChunkId chunk_id;
        StorageNodeId source_node;
        StorageNodeId target_node;
        ChunkChecksum expected_checksum;
        std::uint64_t expected_size{0};
        std::vector<StorageNodeId> existing_replica_nodes;
        std::vector<StorageNodeId> bad_replicas;
        StorageTaskContext context;
        RepairTaskState state{RepairTaskState::kQueued};
        std::uint32_t progress_percent{0};
        std::uint32_t attempts{0};
        StorageNodeStatusCode last_error{StorageNodeStatusCode::kOk};
        std::string last_error_detail;
        std::uint64_t submitted_at_unix_ms{0};
        std::uint64_t started_at_unix_ms{0};
        std::uint64_t completed_at_unix_ms{0};
        std::uint64_t retry_after_ms{0};
    };

    struct RepairSourceReadResult : ChunkStoreResult
    {
        ChunkMetadata metadata;
        ChunkChecksum actual_checksum;
        std::string payload;
        bool verified{false};
    };

    struct RepairTargetWriteResult : ChunkStoreResult
    {
        ChunkMetadata metadata;
        StorageNodeId source_node_id;
        ChunkState source_state{ChunkState::kMissing};
        ChunkState target_state{ChunkState::kMissing};
        ChunkChecksum expected_checksum;
        ChunkChecksum observed_checksum;
        std::uint64_t expected_size{0};
        std::uint64_t observed_size{0};
        bool source_checksum_verified{false};
        bool source_unavailable{false};
        bool target_durable{false};
        bool already_exists{false};
        bool repaired{false};
        bool retryable{false};
    };

    using RepairTaskSourceReader =
        std::function<RepairSourceReadResult(const RepairTask &, const StorageTaskContext &)>;
    using RepairTaskTargetWriter =
        std::function<RepairTargetWriteResult(const RepairTask &,
                                              std::string_view,
                                              const StorageTaskContext &)>;

    using RepairManagerNowSource = std::function<std::uint64_t()>;

    struct RepairManagerConfig
    {
        std::size_t max_active_tasks{64};
        std::size_t max_tasks{256};
        std::uint64_t default_timeout_ms{0};
        RepairManagerNowSource now_unix_ms;
        RepairTaskSourceReader source_reader;
        RepairTaskTargetWriter target_writer;
    };

    struct RepairManagerSubmitResult
    {
        RepairManagerSubmitCode code{RepairManagerSubmitCode::kAccepted};
        std::string error_detail;
        std::optional<RepairTask> task;

        [[nodiscard]] bool accepted() const
        {
            return code == RepairManagerSubmitCode::kAccepted;
        }

        [[nodiscard]] StorageNodeStatusCode status_code() const;
    };

    struct UnderReplicatedTaskSubmitResult
    {
        UnderReplicatedTaskSubmitCode code{UnderReplicatedTaskSubmitCode::kAccepted};
        std::string error_detail;
        std::optional<RepairTask> task;

        [[nodiscard]] bool accepted() const
        {
            return code == UnderReplicatedTaskSubmitCode::kAccepted;
        }

        [[nodiscard]] StorageNodeStatusCode status_code() const;
    };

    struct RepairTaskOperationResult
    {
        RepairTaskOperationCode code{RepairTaskOperationCode::kOk};
        std::string error_detail;
        std::optional<RepairTask> task;

        [[nodiscard]] bool ok() const
        {
            return code == RepairTaskOperationCode::kOk;
        }

        [[nodiscard]] StorageNodeStatusCode status_code() const;
    };

    struct RepairTaskRunResult : ChunkStoreResult
    {
        std::optional<RepairTask> task;
        StorageNodeId source_node;
        StorageNodeId target_node;
        ChunkChecksum source_checksum;
        ChunkChecksum target_checksum;
        std::uint64_t source_size{0};
        std::uint64_t target_size{0};
        bool source_verified{false};
        bool target_durable{false};
        bool already_exists{false};
        bool repaired{false};
        bool retryable{false};
    };

    struct RepairManagerStats
    {
        bool accepting_new_tasks{true};
        std::size_t max_active_tasks{0};
        std::size_t max_tasks{0};
        std::size_t queued_tasks{0};
        std::size_t running_tasks{0};
        std::size_t retry_pending_tasks{0};
        std::size_t completed_tasks{0};
        std::size_t failed_tasks{0};
        std::size_t cancelled_tasks{0};
        std::size_t total_tasks{0};
        std::uint64_t submitted_tasks{0};
        std::uint64_t rejected_tasks{0};
        std::uint64_t total_attempts{0};
        std::string last_error_detail;
    };

    class RepairManager
    {
    public:
        explicit RepairManager(const StorageNodeRegistry *registry,
                               RepairManagerConfig config = {});
        ~RepairManager();

        RepairManager(const RepairManager &) = delete;
        RepairManager &operator=(const RepairManager &) = delete;

        RepairManagerSubmitResult SubmitTask(const RepairTaskRequest &request);
        UnderReplicatedTaskSubmitResult SubmitUnderReplicatedTask(
            const ScrubTask &scrub_task);

        RepairTaskOperationResult MarkTaskRunning(std::string_view task_id);
        RepairTaskOperationResult UpdateTaskProgress(std::string_view task_id,
                                                     std::uint32_t progress_percent);
        RepairTaskOperationResult CompleteTask(std::string_view task_id);
        RepairTaskOperationResult FailTask(std::string_view task_id,
                                           StorageNodeStatusCode error_code,
                                           std::string error_detail,
                                           bool retryable = false,
                                           std::uint64_t retry_after_ms = 0);
        RepairTaskOperationResult CancelTask(std::string_view task_id);
        RepairTaskOperationResult RetryTask(std::string_view task_id);
        RepairTaskRunResult RunTask(std::string_view task_id);

        [[nodiscard]] std::optional<RepairTask> FindTask(
            std::string_view task_id) const;
        [[nodiscard]] std::vector<RepairTask> ListTasks() const;
        [[nodiscard]] RepairManagerStats SnapshotStats() const;
        [[nodiscard]] const RepairManagerConfig &config() const;

    private:
        struct Impl;

        RepairManagerConfig config_;
        std::unique_ptr<Impl> impl_;
    };
}
