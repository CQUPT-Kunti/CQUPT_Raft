#include "store/runtime/storage_executor.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace storedemo
{
    namespace
    {
        StorageExecutorConfig SanitizeStorageExecutorConfig(StorageExecutorConfig config)
        {
            if (config.worker_count == 0)
            {
                config.worker_count = 1;
            }
            if (config.queue_capacity == 0)
            {
                config.queue_capacity = 1;
            }
            return config;
        }
    }

    struct BoundedStorageExecutor::Impl
    {
        struct QueuedTask
        {
            std::string task_name;
            StorageTaskContext context;
            std::function<void()> task;
        };

        explicit Impl(const StorageExecutorConfig &config)
            : worker_count(config.worker_count)
            , queue_capacity(config.queue_capacity)
        {
        }

        mutable std::mutex mutex;
        std::condition_variable cv;
        bool accepting_new_tasks{true};
        bool stop_requested{false};
        StorageExecutorStopMode stop_mode{StorageExecutorStopMode::kDrain};
        std::size_t worker_count{0};
        std::size_t queue_capacity{0};
        std::size_t active_workers{0};
        std::uint64_t submitted_tasks{0};
        std::uint64_t completed_tasks{0};
        std::uint64_t rejected_tasks{0};
        std::uint64_t failed_tasks{0};
        std::uint64_t dropped_tasks{0};
        std::string last_error_detail;
        std::deque<QueuedTask> queue;
        std::vector<std::thread> workers;
    };

    const char *ToString(const StorageExecutorSubmitCode code)
    {
        switch (code)
        {
        case StorageExecutorSubmitCode::kAccepted:
            return "Accepted";
        case StorageExecutorSubmitCode::kOverloaded:
            return "Overloaded";
        case StorageExecutorSubmitCode::kStopped:
            return "Stopped";
        case StorageExecutorSubmitCode::kInvalidArgument:
            return "InvalidArgument";
        }
        return "UnknownStorageExecutorSubmitCode";
    }

    const char *ToString(const StorageExecutorStopMode mode)
    {
        switch (mode)
        {
        case StorageExecutorStopMode::kDrain:
            return "Drain";
        case StorageExecutorStopMode::kCancelPending:
            return "CancelPending";
        }
        return "UnknownStorageExecutorStopMode";
    }

    StorageNodeStatusCode StorageExecutorSubmitResult::status_code() const
    {
        switch (code)
        {
        case StorageExecutorSubmitCode::kAccepted:
            return StorageNodeStatusCode::kOk;
        case StorageExecutorSubmitCode::kOverloaded:
            return StorageNodeStatusCode::kOverloaded;
        case StorageExecutorSubmitCode::kStopped:
            return StorageNodeStatusCode::kNodeUnavailable;
        case StorageExecutorSubmitCode::kInvalidArgument:
            return StorageNodeStatusCode::kInvalidArgument;
        }
        return StorageNodeStatusCode::kIoError;
    }

    BoundedStorageExecutor::BoundedStorageExecutor(StorageExecutorConfig config)
        : impl_(std::make_unique<Impl>(SanitizeStorageExecutorConfig(config)))
        , config_(SanitizeStorageExecutorConfig(config))
    {
        impl_->workers.reserve(config_.worker_count);
        for (std::size_t index = 0; index < config_.worker_count; ++index)
        {
            impl_->workers.emplace_back(
                [this]()
                {
                    for (;;)
                    {
                        Impl::QueuedTask task;
                        {
                            std::unique_lock<std::mutex> lock(impl_->mutex);
                            impl_->cv.wait(lock,
                                           [this]()
                                           {
                                               return impl_->stop_requested ||
                                                      !impl_->queue.empty();
                                           });

                            if (impl_->queue.empty())
                            {
                                if (impl_->stop_requested)
                                {
                                    return;
                                }
                                continue;
                            }

                            task = std::move(impl_->queue.front());
                            impl_->queue.pop_front();
                            ++impl_->active_workers;
                        }

                        try
                        {
                            task.task();
                        }
                        catch (const std::exception &ex)
                        {
                            std::lock_guard<std::mutex> lock(impl_->mutex);
                            ++impl_->failed_tasks;
                            impl_->last_error_detail = ex.what();
                        }
                        catch (...)
                        {
                            std::lock_guard<std::mutex> lock(impl_->mutex);
                            ++impl_->failed_tasks;
                            impl_->last_error_detail = "unknown worker exception";
                        }

                        {
                            std::lock_guard<std::mutex> lock(impl_->mutex);
                            --impl_->active_workers;
                            ++impl_->completed_tasks;
                        }
                        impl_->cv.notify_all();
                    }
                });
        }
    }

    BoundedStorageExecutor::~BoundedStorageExecutor()
    {
        (void)Shutdown(StorageExecutorShutdownRequest{
            .mode = StorageExecutorStopMode::kDrain});
    }

    StorageExecutorSubmitResult BoundedStorageExecutor::Submit(
        StorageExecutorSubmitRequest request)
    {
        StorageExecutorSubmitResult result;
        if (!request.task)
        {
            result.code = StorageExecutorSubmitCode::kInvalidArgument;
            result.error_detail = "executor task must not be empty";
            return result;
        }

        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->accepting_new_tasks)
        {
            result.code = StorageExecutorSubmitCode::kStopped;
            result.error_detail = "executor is not accepting new tasks";
            ++impl_->rejected_tasks;
            result.queue_depth = impl_->queue.size();
            return result;
        }

        if (impl_->queue.size() >= impl_->queue_capacity)
        {
            result.code = StorageExecutorSubmitCode::kOverloaded;
            result.error_detail = "executor queue is full";
            result.retry_after_ms = 1;
            ++impl_->rejected_tasks;
            result.queue_depth = impl_->queue.size();
            return result;
        }

        impl_->queue.push_back(Impl::QueuedTask{
            .task_name = std::move(request.task_name),
            .context = request.context,
            .task = std::move(request.task)});
        ++impl_->submitted_tasks;
        result.queue_depth = impl_->queue.size();
        impl_->cv.notify_one();
        return result;
    }

    StorageExecutorShutdownResult BoundedStorageExecutor::Shutdown(
        StorageExecutorShutdownRequest request)
    {
        std::vector<std::thread> workers_to_join;
        StorageExecutorShutdownResult result;
        const std::thread::id current_thread_id = std::this_thread::get_id();
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            for (const auto &worker : impl_->workers)
            {
                if (worker.get_id() == current_thread_id)
                {
                    result.error_detail =
                        "shutdown must not be called from executor worker thread";
                    result.stats = StorageExecutorStats{
                        .accepting_new_tasks = impl_->accepting_new_tasks,
                        .stop_requested = impl_->stop_requested,
                        .worker_count = impl_->worker_count,
                        .queue_capacity = impl_->queue_capacity,
                        .queued_tasks = impl_->queue.size(),
                        .active_workers = impl_->active_workers,
                        .submitted_tasks = impl_->submitted_tasks,
                        .completed_tasks = impl_->completed_tasks,
                        .rejected_tasks = impl_->rejected_tasks,
                        .failed_tasks = impl_->failed_tasks,
                        .dropped_tasks = impl_->dropped_tasks,
                        .last_error_detail = impl_->last_error_detail,
                    };
                    return result;
                }
            }

            if (!impl_->workers.empty())
            {
                impl_->accepting_new_tasks = false;
                impl_->stop_requested = true;
                impl_->stop_mode = request.mode;
                if (request.mode == StorageExecutorStopMode::kCancelPending)
                {
                    result.dropped_tasks = impl_->queue.size();
                    impl_->dropped_tasks += result.dropped_tasks;
                    impl_->queue.clear();
                }
                workers_to_join.swap(impl_->workers);
            }
            else
            {
                result.dropped_tasks = 0;
            }
        }

        impl_->cv.notify_all();
        for (auto &worker : workers_to_join)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        result.stats = SnapshotStats();
        result.stopped = !result.stats.accepting_new_tasks &&
                         result.stats.active_workers == 0 &&
                         result.stats.queued_tasks == 0;
        result.drained =
            request.mode == StorageExecutorStopMode::kDrain || result.dropped_tasks == 0;
        return result;
    }

    StorageExecutorStats BoundedStorageExecutor::SnapshotStats() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return StorageExecutorStats{
            .accepting_new_tasks = impl_->accepting_new_tasks,
            .stop_requested = impl_->stop_requested,
            .worker_count = impl_->worker_count,
            .queue_capacity = impl_->queue_capacity,
            .queued_tasks = impl_->queue.size(),
            .active_workers = impl_->active_workers,
            .submitted_tasks = impl_->submitted_tasks,
            .completed_tasks = impl_->completed_tasks,
            .rejected_tasks = impl_->rejected_tasks,
            .failed_tasks = impl_->failed_tasks,
            .dropped_tasks = impl_->dropped_tasks,
            .last_error_detail = impl_->last_error_detail,
        };
    }

    const StorageExecutorConfig &BoundedStorageExecutor::config() const
    {
        return config_;
    }
}
