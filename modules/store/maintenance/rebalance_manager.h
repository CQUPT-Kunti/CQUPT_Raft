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
        bool source_payload_verified{false};
        bool target_durable{false};
        bool target_already_exists{false};
        bool target_verified{false};
        bool manifest_coordinated{false};
        bool manifest_already_applied{false};
        bool source_cleanup_completed{false};
        bool source_cleanup_already_missing{false};
        bool orphan_candidate_recorded{false};
    };

    struct RebalanceSourceReadResult : ChunkStoreResult
    {
        ChunkMetadata metadata;
        ChunkChecksum actual_checksum;
        std::string payload;
        bool verified{false};
    };

    struct RebalanceTargetWriteResult : ChunkStoreResult
    {
        ChunkMetadata metadata;
        ChunkState target_state{ChunkState::kMissing};
        ChunkChecksum expected_checksum;
        ChunkChecksum observed_checksum;
        std::uint64_t expected_size{0};
        std::uint64_t observed_size{0};
        bool target_durable{false};
        bool already_exists{false};
        bool repaired{false};
        bool retryable{false};
    };

    struct RebalanceTargetVerifyResult : ChunkStoreResult
    {
        ChunkMetadata metadata;
        ChunkChecksum actual_checksum;
        std::uint64_t actual_size{0};
        bool verified{false};
        bool retryable{false};
    };

    struct RebalanceManifestCoordinationResult : ChunkStoreResult
    {
        bool updated{false};
        bool already_applied{false};
        bool retryable{false};
    };

    struct RebalanceSourceCleanupResult : ChunkStoreResult
    {
        bool completed{false};
        bool already_missing{false};
        bool retryable{false};
    };

    struct RebalanceCleanupCandidateResult : ChunkStoreResult
    {
        bool recorded{false};
        bool already_exists{false};
        bool retryable{false};
    };

    using RebalanceTaskSourceReader =
        std::function<RebalanceSourceReadResult(const RebalanceTask &,
                                                const StorageTaskContext &)>;
    using RebalanceTaskTargetWriter =
        std::function<RebalanceTargetWriteResult(const RebalanceTask &,
                                                 std::string_view,
                                                 const StorageTaskContext &)>;
    using RebalanceTaskTargetVerifier =
        std::function<RebalanceTargetVerifyResult(const RebalanceTask &,
                                                  const StorageTaskContext &)>;
    using RebalanceTaskManifestCoordinator =
        std::function<RebalanceManifestCoordinationResult(
            const RebalanceTask &,
            const StorageTaskContext &)>;
    using RebalanceTaskSourceCleanupHandler =
        std::function<RebalanceSourceCleanupResult(const RebalanceTask &,
                                                   const StorageTaskContext &)>;
    using RebalanceTaskCleanupCandidateRecorder =
        std::function<RebalanceCleanupCandidateResult(const RebalanceTask &,
                                                      std::string_view,
                                                      const StorageTaskContext &)>;

    using RebalanceManagerNowSource = std::function<std::uint64_t()>;

    struct RebalanceManagerConfig
    {
        std::size_t max_active_tasks{64};
        std::size_t max_tasks{256};
        std::uint64_t default_timeout_ms{0};
        RebalanceManagerNowSource now_unix_ms;
        RebalanceTaskSourceReader source_reader;
        RebalanceTaskTargetWriter target_writer;
        RebalanceTaskTargetVerifier target_verifier;
        RebalanceTaskManifestCoordinator manifest_coordinator;
        RebalanceTaskSourceCleanupHandler source_cleanup_handler;
        RebalanceTaskCleanupCandidateRecorder cleanup_candidate_recorder;
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

    struct RebalanceTaskRunResult : ChunkStoreResult
    {
        std::optional<RebalanceTask> task;
        StorageNodeId source_node;
        StorageNodeId target_node;
        ChunkChecksum source_checksum;
        ChunkChecksum target_checksum;
        std::uint64_t source_size{0};
        std::uint64_t target_size{0};
        bool source_verified{false};
        bool target_durable{false};
        bool target_already_exists{false};
        bool target_verified{false};
        bool manifest_coordination_attempted{false};
        bool manifest_updated{false};
        bool manifest_idempotent{false};
        bool source_cleanup_attempted{false};
        bool source_cleanup_completed{false};
        bool orphan_candidate_created{false};
        bool idempotent_success{false};
        bool retryable{false};
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
        RebalanceTaskRunResult RunTask(std::string_view task_id);

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
