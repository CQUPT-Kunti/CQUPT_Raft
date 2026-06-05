#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "store/common/store_types.h"

namespace storedemo
{
    enum class StorageExecutorSubmitCode : std::uint8_t
    {
        kAccepted = 0,
        kOverloaded = 1,
        kStopped = 2,
        kInvalidArgument = 3,
    };

    enum class StorageExecutorStopMode : std::uint8_t
    {
        kDrain = 0,
        kCancelPending = 1,
    };

    const char *ToString(StorageExecutorSubmitCode code);
    const char *ToString(StorageExecutorStopMode mode);

    struct StorageTaskContext
    {
        std::uint64_t timeout_ms{0};
        bool best_effort_cancel{false};
    };

    struct StorageExecutorConfig
    {
        std::size_t worker_count{4};
        std::size_t queue_capacity{256};
    };

    struct StorageExecutorSubmitRequest
    {
        std::string task_name;
        StorageTaskContext context;
        std::function<void()> task;
    };

    struct StorageExecutorSubmitResult
    {
        StorageExecutorSubmitCode code{StorageExecutorSubmitCode::kAccepted};
        std::string error_detail;
        std::uint64_t retry_after_ms{0};
        std::size_t queue_depth{0};

        [[nodiscard]] bool accepted() const
        {
            return code == StorageExecutorSubmitCode::kAccepted;
        }

        [[nodiscard]] StorageNodeStatusCode status_code() const;
    };

    struct StorageExecutorShutdownRequest
    {
        StorageExecutorStopMode mode{StorageExecutorStopMode::kDrain};
    };

    struct StorageExecutorStats
    {
        bool accepting_new_tasks{false};
        bool stop_requested{false};
        std::size_t worker_count{0};
        std::size_t queue_capacity{0};
        std::size_t queued_tasks{0};
        std::size_t active_workers{0};
        std::uint64_t submitted_tasks{0};
        std::uint64_t completed_tasks{0};
        std::uint64_t rejected_tasks{0};
        std::uint64_t failed_tasks{0};
        std::uint64_t dropped_tasks{0};
        std::string last_error_detail;
    };

    struct StorageExecutorShutdownResult
    {
        bool stopped{false};
        bool drained{false};
        std::size_t dropped_tasks{0};
        std::string error_detail;
        StorageExecutorStats stats;
    };

    class BoundedStorageExecutor
    {
    public:
        explicit BoundedStorageExecutor(StorageExecutorConfig config = {});
        ~BoundedStorageExecutor();

        BoundedStorageExecutor(const BoundedStorageExecutor &) = delete;
        BoundedStorageExecutor &operator=(const BoundedStorageExecutor &) = delete;

        StorageExecutorSubmitResult Submit(StorageExecutorSubmitRequest request);
        StorageExecutorShutdownResult Shutdown(
            StorageExecutorShutdownRequest request = {});

        [[nodiscard]] StorageExecutorStats SnapshotStats() const;
        [[nodiscard]] const StorageExecutorConfig &config() const;

    private:
        struct Impl;

        std::unique_ptr<Impl> impl_;
        StorageExecutorConfig config_;
    };
}
