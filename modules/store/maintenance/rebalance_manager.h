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
#include "store/runtime/storage_executor.h"

namespace storedemo
{
    class StorageNodeRegistry;

    enum class RebalanceTaskReason : std::uint8_t
    {
        kCapacityImbalance = 0,
        kHotspot = 1,
        kNewNodeJoin = 2,
        kDraining = 3,
        kMaintenance = 4,
    };

    enum class RebalanceTaskState : std::uint8_t
    {
        kQueued = 0,
        kRunning = 1,
        kCompleted = 2,
        kFailed = 3,
        kCancelled = 4,
        kRetryPending = 5,
    };

    enum class RebalanceManagerSubmitCode : std::uint8_t
    {
        kAccepted = 0,
        kOverloaded = 1,
        kInvalidArgument = 2,
        kAlreadyExists = 3,
    };

    enum class RebalanceTaskOperationCode : std::uint8_t
    {
        kOk = 0,
        kNotFound = 1,
        kConflict = 2,
        kInvalidArgument = 3,
    };

    const char *ToString(RebalanceTaskReason reason);
    const char *ToString(RebalanceTaskState state);
    const char *ToString(RebalanceManagerSubmitCode code);
    const char *ToString(RebalanceTaskOperationCode code);

    struct RebalanceTaskRequest
    {
        ChunkIdentity identity;
        StorageNodeId source_node;
        StorageNodeId target_node;
        RebalanceTaskReason reason{RebalanceTaskReason::kCapacityImbalance};
        ChunkChecksum expected_checksum;
        std::uint64_t expected_size{0};
        ChunkState source_state{ChunkState::kLive};
        StorageTaskContext context;
    };

    struct RebalanceTask
    {
        std::string task_id;
        ChunkIdentity identity;
        ChunkId chunk_id;
        StorageNodeId source_node;
        StorageNodeId target_node;
        RebalanceTaskReason reason{RebalanceTaskReason::kCapacityImbalance};
        ChunkChecksum expected_checksum;
        std::uint64_t expected_size{0};
        ChunkState source_state{ChunkState::kLive};
        StorageTaskContext context;
        RebalanceTaskState state{RebalanceTaskState::kQueued};
        std::uint32_t progress_percent{0};
        std::uint32_t attempts{0};
        StorageNodeStatusCode last_error{StorageNodeStatusCode::kOk};
        std::string last_error_detail;
        std::uint64_t submitted_at_unix_ms{0};
        std::uint64_t started_at_unix_ms{0};
        std::uint64_t completed_at_unix_ms{0};
        std::uint64_t retry_after_ms{0};
    };

    using RebalanceManagerNowSource = std::function<std::uint64_t()>;

    struct RebalanceManagerConfig
    {
        std::size_t max_active_tasks{64};
        std::size_t max_tasks{256};
        RebalanceManagerNowSource now_unix_ms;
    };

    struct RebalanceManagerSubmitResult
    {
        RebalanceManagerSubmitCode code{RebalanceManagerSubmitCode::kAccepted};
        std::string error_detail;
        std::optional<RebalanceTask> task;

        [[nodiscard]] bool accepted() const
        {
            return code == RebalanceManagerSubmitCode::kAccepted;
        }

        [[nodiscard]] StorageNodeStatusCode status_code() const;
    };

    struct RebalanceTaskOperationResult
    {
        RebalanceTaskOperationCode code{RebalanceTaskOperationCode::kOk};
        std::string error_detail;
        std::optional<RebalanceTask> task;

        [[nodiscard]] bool ok() const
        {
            return code == RebalanceTaskOperationCode::kOk;
        }

        [[nodiscard]] StorageNodeStatusCode status_code() const;
    };

    struct RebalanceManagerStats
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

    class RebalanceManager
    {
    public:
        explicit RebalanceManager(const StorageNodeRegistry *registry,
                                  RebalanceManagerConfig config = {});
        ~RebalanceManager();

        RebalanceManager(const RebalanceManager &) = delete;
        RebalanceManager &operator=(const RebalanceManager &) = delete;

        RebalanceManagerSubmitResult SubmitTask(const RebalanceTaskRequest &request);

        RebalanceTaskOperationResult MarkTaskRunning(std::string_view task_id);
        RebalanceTaskOperationResult UpdateTaskProgress(std::string_view task_id,
                                                        std::uint32_t progress_percent);
        RebalanceTaskOperationResult CompleteTask(std::string_view task_id);
        RebalanceTaskOperationResult FailTask(std::string_view task_id,
                                              StorageNodeStatusCode error_code,
                                              std::string error_detail,
                                              bool retryable = false,
                                              std::uint64_t retry_after_ms = 0);
        RebalanceTaskOperationResult CancelTask(std::string_view task_id);
        RebalanceTaskOperationResult RetryTask(std::string_view task_id);

        [[nodiscard]] std::optional<RebalanceTask> FindTask(
            std::string_view task_id) const;
        [[nodiscard]] std::vector<RebalanceTask> ListTasks() const;
        [[nodiscard]] RebalanceManagerStats SnapshotStats() const;
        [[nodiscard]] const RebalanceManagerConfig &config() const;

    private:
        struct Impl;

        RebalanceManagerConfig config_;
        std::unique_ptr<Impl> impl_;
    };
}
