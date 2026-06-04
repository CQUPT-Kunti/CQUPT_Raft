#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
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

    enum class ScrubTaskState : std::uint8_t
    {
        kQueued = 0,
        kRunning = 1,
        kCompleted = 2,
        kFailed = 3,
        kCancelled = 4,
    };

    enum class ScrubManagerSubmitCode : std::uint8_t
    {
        kAccepted = 0,
        kOverloaded = 1,
        kStopped = 2,
        kInvalidArgument = 3,
        kAlreadyExists = 4,
    };

    enum class ScrubManagerStopMode : std::uint8_t
    {
        kDrain = 0,
        kCancelPending = 1,
    };

    const char *ToString(ScrubTaskState state);
    const char *ToString(ScrubManagerSubmitCode code);
    const char *ToString(ScrubManagerStopMode mode);

    struct ScrubManifest
    {
        ChunkIdentity identity;
        std::uint64_t expected_size{0};
        ChunkChecksum expected_checksum;
        std::vector<StorageNodeId> replica_nodes;
        std::size_t desired_replica_count{0};
    };

    struct ScrubReplicaFact
    {
        StorageNodeId node_id;
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        ChunkState state_before{ChunkState::kMissing};
        ChunkState state_after{ChunkState::kMissing};
        std::uint64_t observed_size{0};
        ChunkChecksum observed_checksum;
        bool checksum_verified{false};
        bool known_corrupted{false};
        bool known_missing{false};
        bool quarantined{false};
    };

    struct ScrubRepairCandidate
    {
        ChunkId chunk_id;
        std::uint64_t expected_size{0};
        ChunkChecksum expected_checksum;
        std::vector<StorageNodeId> bad_replicas;
        std::vector<StorageNodeId> healthy_source_replicas;
        bool under_replicated{false};
        bool lost_or_unrecoverable{false};
    };

    struct ScrubTaskResult : ChunkStoreResult
    {
        std::vector<ScrubReplicaFact> replica_facts;
        std::optional<ScrubRepairCandidate> repair_candidate;
    };

    struct ScrubTask
    {
        std::string task_id;
        ScrubManifest manifest;
        StorageTaskContext context;
        std::uint32_t attempts{0};
        StorageNodeStatusCode last_error{StorageNodeStatusCode::kOk};
        std::string last_error_detail;
        ScrubTaskState state{ScrubTaskState::kQueued};
        std::optional<ScrubTaskResult> result;
        std::uint64_t submitted_at_unix_ms{0};
        std::uint64_t started_at_unix_ms{0};
        std::uint64_t completed_at_unix_ms{0};
    };

    using ScrubManagerNowSource = std::function<std::uint64_t()>;
    using ScrubTaskRunner = std::function<ScrubTaskResult(const ScrubTask &)>;

    struct ScrubManagerConfig
    {
        std::size_t worker_count{1};
        std::size_t queue_capacity{64};
        std::uint64_t default_timeout_ms{0};
        ScrubManagerNowSource now_unix_ms;
    };

    struct ScrubManagerSubmitResult
    {
        ScrubManagerSubmitCode code{ScrubManagerSubmitCode::kAccepted};
        std::string error_detail;
        std::uint64_t retry_after_ms{0};
        std::size_t queue_depth{0};

        [[nodiscard]] bool accepted() const
        {
            return code == ScrubManagerSubmitCode::kAccepted;
        }

        [[nodiscard]] StorageNodeStatusCode status_code() const;
    };

    struct ScrubManagerDrainResult
    {
        bool drained{false};
        std::string error_detail;
    };

    struct ScrubManagerStats
    {
        bool accepting_new_tasks{false};
        bool stop_requested{false};
        std::size_t worker_count{0};
        std::size_t queue_capacity{0};
        std::size_t queued_tasks{0};
        std::size_t running_tasks{0};
        std::size_t completed_tasks{0};
        std::size_t failed_tasks{0};
        std::size_t cancelled_tasks{0};
        std::uint64_t submitted_tasks{0};
        std::uint64_t rejected_tasks{0};
        std::uint64_t total_attempts{0};
        std::string last_error_detail;
    };

    struct ScrubManagerStopRequest
    {
        ScrubManagerStopMode mode{ScrubManagerStopMode::kDrain};
    };

    struct ScrubManagerStopResult
    {
        bool stopped{false};
        bool drained{false};
        std::string error_detail;
        ScrubManagerStats stats;
    };

    class ScrubManager
    {
    public:
        explicit ScrubManager(std::map<StorageNodeId, ChunkStore *> stores,
                              const StorageNodeRegistry *registry,
                              ScrubManagerConfig config = {},
                              ScrubTaskRunner task_runner = {});
        ~ScrubManager();

        ScrubManager(const ScrubManager &) = delete;
        ScrubManager &operator=(const ScrubManager &) = delete;

        ScrubManagerSubmitResult SubmitTask(ScrubTask task);
        ScrubManagerDrainResult Drain();
        ScrubManagerStopResult Stop(ScrubManagerStopRequest request = {});

        [[nodiscard]] std::optional<ScrubTask> FindTask(
            std::string_view task_id) const;
        [[nodiscard]] std::vector<ScrubTask> ListTasks() const;
        [[nodiscard]] ScrubManagerStats SnapshotStats() const;
        [[nodiscard]] const ScrubManagerConfig &config() const;

    private:
        struct Impl;

        std::unique_ptr<Impl> impl_;
        ScrubManagerConfig config_;
    };
}
