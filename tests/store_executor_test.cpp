#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "store/runtime/storage_executor.h"

namespace storedemo
{
    namespace
    {
        using namespace std::chrono_literals;

        class StoreExecutorTest : public ::testing::Test
        {
        protected:
            static StorageExecutorConfig SingleWorkerConfig(const std::size_t queue_capacity = 4)
            {
                return StorageExecutorConfig{
                    .worker_count = 1,
                    .queue_capacity = queue_capacity};
            }
        };

        TEST_F(StoreExecutorTest, SanitizesZeroWorkerAndQueueCapacityToSafeMinimum)
        {
            BoundedStorageExecutor executor(StorageExecutorConfig{
                .worker_count = 0,
                .queue_capacity = 0});

            EXPECT_EQ(executor.config().worker_count, 1U);
            EXPECT_EQ(executor.config().queue_capacity, 1U);

            const auto stats = executor.SnapshotStats();
            EXPECT_TRUE(stats.accepting_new_tasks);
            EXPECT_EQ(stats.worker_count, 1U);
            EXPECT_EQ(stats.queue_capacity, 1U);
        }

        TEST_F(StoreExecutorTest, RejectsSubmitRequestWithoutCallableTask)
        {
            BoundedStorageExecutor executor(SingleWorkerConfig());

            StorageExecutorSubmitRequest request;
            request.task_name = "invalid-empty-task";

            const auto result = executor.Submit(std::move(request));
            EXPECT_EQ(result.code, StorageExecutorSubmitCode::kInvalidArgument);
            EXPECT_EQ(result.status_code(), StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(result.accepted());
            EXPECT_NE(result.error_detail.find("must not be empty"), std::string::npos);
        }

        TEST_F(StoreExecutorTest, QueueCapacityReturnsOverloadedWhenPendingWorkIsFull)
        {
            BoundedStorageExecutor executor(SingleWorkerConfig(1));
            std::promise<void> first_task_started_promise;
            std::future<void> first_task_started = first_task_started_promise.get_future();
            std::promise<void> release_first_task_promise;
            std::shared_future<void> release_first_task =
                release_first_task_promise.get_future().share();
            std::promise<void> queued_task_done_promise;
            std::future<void> queued_task_done = queued_task_done_promise.get_future();

            auto first_submit = executor.Submit(StorageExecutorSubmitRequest{
                .task_name = "blocking-task",
                .task =
                    [&]()
                    {
                        first_task_started_promise.set_value();
                        release_first_task.wait();
                    }});
            ASSERT_TRUE(first_submit.accepted());
            ASSERT_EQ(first_task_started.wait_for(1s), std::future_status::ready);

            auto queued_submit = executor.Submit(StorageExecutorSubmitRequest{
                .task_name = "queued-task",
                .task =
                    [&]()
                    {
                        queued_task_done_promise.set_value();
                    }});
            EXPECT_TRUE(queued_submit.accepted());
            EXPECT_EQ(queued_submit.queue_depth, 1U);

            const auto overloaded_submit = executor.Submit(StorageExecutorSubmitRequest{
                .task_name = "overloaded-task",
                .task = []() {}});
            EXPECT_EQ(overloaded_submit.code, StorageExecutorSubmitCode::kOverloaded);
            EXPECT_EQ(overloaded_submit.status_code(), StorageNodeStatusCode::kOverloaded);
            EXPECT_FALSE(overloaded_submit.accepted());
            EXPECT_EQ(overloaded_submit.queue_depth, 1U);
            EXPECT_NE(overloaded_submit.error_detail.find("queue is full"), std::string::npos);

            release_first_task_promise.set_value();
            EXPECT_EQ(queued_task_done.wait_for(1s), std::future_status::ready);

            const auto shutdown_result =
                executor.Shutdown(StorageExecutorShutdownRequest{
                    .mode = StorageExecutorStopMode::kDrain});
            EXPECT_TRUE(shutdown_result.stopped);
            EXPECT_TRUE(shutdown_result.drained);
        }

        TEST_F(StoreExecutorTest, ShutdownRejectsNewTasksAfterDrainCompletes)
        {
            BoundedStorageExecutor executor(SingleWorkerConfig());

            const auto shutdown_result =
                executor.Shutdown(StorageExecutorShutdownRequest{
                    .mode = StorageExecutorStopMode::kDrain});
            EXPECT_TRUE(shutdown_result.stopped);
            EXPECT_TRUE(shutdown_result.drained);
            EXPECT_EQ(shutdown_result.dropped_tasks, 0U);

            const auto stopped_submit = executor.Submit(StorageExecutorSubmitRequest{
                .task_name = "after-stop",
                .task = []() {}});
            EXPECT_EQ(stopped_submit.code, StorageExecutorSubmitCode::kStopped);
            EXPECT_EQ(stopped_submit.status_code(), StorageNodeStatusCode::kNodeUnavailable);
            EXPECT_FALSE(stopped_submit.accepted());

            const auto stats = executor.SnapshotStats();
            EXPECT_FALSE(stats.accepting_new_tasks);
            EXPECT_EQ(stats.rejected_tasks, 1U);
        }

        TEST_F(StoreExecutorTest, ShutdownDrainWaitsForQueuedTasksToFinish)
        {
            BoundedStorageExecutor executor(SingleWorkerConfig(4));
            std::promise<void> first_task_started_promise;
            std::future<void> first_task_started = first_task_started_promise.get_future();
            std::promise<void> release_first_task_promise;
            std::shared_future<void> release_first_task =
                release_first_task_promise.get_future().share();
            std::promise<void> second_task_done_promise;
            std::future<void> second_task_done = second_task_done_promise.get_future();
            std::atomic<int> run_count{0};

            ASSERT_TRUE(executor.Submit(StorageExecutorSubmitRequest{
                            .task_name = "drain-blocking",
                            .task =
                                [&]()
                                {
                                    run_count.fetch_add(1, std::memory_order_relaxed);
                                    first_task_started_promise.set_value();
                                    release_first_task.wait();
                                }})
                            .accepted());
            ASSERT_EQ(first_task_started.wait_for(1s), std::future_status::ready);

            ASSERT_TRUE(executor.Submit(StorageExecutorSubmitRequest{
                            .task_name = "drain-queued",
                            .task =
                                [&]()
                                {
                                    run_count.fetch_add(1, std::memory_order_relaxed);
                                    second_task_done_promise.set_value();
                                }})
                            .accepted());

            auto shutdown_future = std::async(std::launch::async,
                                              [&]()
                                              {
                                                  return executor.Shutdown(
                                                      StorageExecutorShutdownRequest{
                                                          .mode = StorageExecutorStopMode::kDrain});
                                              });
            EXPECT_EQ(shutdown_future.wait_for(100ms), std::future_status::timeout);

            release_first_task_promise.set_value();

            const auto shutdown_result = shutdown_future.get();
            EXPECT_TRUE(shutdown_result.stopped);
            EXPECT_TRUE(shutdown_result.drained);
            EXPECT_EQ(shutdown_result.dropped_tasks, 0U);
            EXPECT_EQ(second_task_done.wait_for(1s), std::future_status::ready);
            EXPECT_EQ(run_count.load(std::memory_order_relaxed), 2);
            EXPECT_EQ(shutdown_result.stats.completed_tasks, 2U);
            EXPECT_EQ(shutdown_result.stats.failed_tasks, 0U);
        }

        TEST_F(StoreExecutorTest, ShutdownCancelPendingDropsTasksThatDidNotStart)
        {
            BoundedStorageExecutor executor(SingleWorkerConfig(4));
            std::promise<void> first_task_started_promise;
            std::future<void> first_task_started = first_task_started_promise.get_future();
            std::promise<void> release_first_task_promise;
            std::shared_future<void> release_first_task =
                release_first_task_promise.get_future().share();
            std::atomic<int> run_count{0};
            std::atomic<int> cancelled_queue_runs{0};

            ASSERT_TRUE(executor.Submit(StorageExecutorSubmitRequest{
                            .task_name = "cancel-blocking",
                            .task =
                                [&]()
                                {
                                    run_count.fetch_add(1, std::memory_order_relaxed);
                                    first_task_started_promise.set_value();
                                    release_first_task.wait();
                                }})
                            .accepted());
            ASSERT_EQ(first_task_started.wait_for(1s), std::future_status::ready);

            ASSERT_TRUE(executor.Submit(StorageExecutorSubmitRequest{
                            .task_name = "cancel-pending-a",
                            .task =
                                [&]()
                                {
                                    cancelled_queue_runs.fetch_add(1, std::memory_order_relaxed);
                                    run_count.fetch_add(1, std::memory_order_relaxed);
                                }})
                            .accepted());
            ASSERT_TRUE(executor.Submit(StorageExecutorSubmitRequest{
                            .task_name = "cancel-pending-b",
                            .task =
                                [&]()
                                {
                                    cancelled_queue_runs.fetch_add(1, std::memory_order_relaxed);
                                    run_count.fetch_add(1, std::memory_order_relaxed);
                                }})
                            .accepted());

            auto shutdown_future = std::async(
                std::launch::async,
                [&]()
                {
                    return executor.Shutdown(StorageExecutorShutdownRequest{
                        .mode = StorageExecutorStopMode::kCancelPending});
                });
            EXPECT_EQ(shutdown_future.wait_for(100ms), std::future_status::timeout);

            release_first_task_promise.set_value();

            const auto shutdown_result = shutdown_future.get();
            EXPECT_TRUE(shutdown_result.stopped);
            EXPECT_FALSE(shutdown_result.drained);
            EXPECT_EQ(shutdown_result.dropped_tasks, 2U);
            EXPECT_EQ(run_count.load(std::memory_order_relaxed), 1);
            EXPECT_EQ(cancelled_queue_runs.load(std::memory_order_relaxed), 0);
            EXPECT_EQ(shutdown_result.stats.completed_tasks, 1U);
            EXPECT_EQ(shutdown_result.stats.dropped_tasks, 2U);
        }

        TEST_F(StoreExecutorTest, WorkerExceptionIsRecordedAndNextTaskStillRuns)
        {
            BoundedStorageExecutor executor(SingleWorkerConfig(4));
            std::promise<void> follow_up_done_promise;
            std::future<void> follow_up_done = follow_up_done_promise.get_future();

            ASSERT_TRUE(executor.Submit(StorageExecutorSubmitRequest{
                            .task_name = "throwing-task",
                            .task =
                                []()
                                {
                                    throw std::runtime_error("executor boom");
                                }})
                            .accepted());
            ASSERT_TRUE(executor.Submit(StorageExecutorSubmitRequest{
                            .task_name = "follow-up-task",
                            .task =
                                [&]()
                                {
                                    follow_up_done_promise.set_value();
                                }})
                            .accepted());

            const auto shutdown_result =
                executor.Shutdown(StorageExecutorShutdownRequest{
                    .mode = StorageExecutorStopMode::kDrain});
            EXPECT_TRUE(shutdown_result.stopped);
            EXPECT_TRUE(shutdown_result.drained);
            EXPECT_EQ(follow_up_done.wait_for(1s), std::future_status::ready);
            EXPECT_EQ(shutdown_result.stats.completed_tasks, 2U);
            EXPECT_EQ(shutdown_result.stats.failed_tasks, 1U);
            EXPECT_NE(shutdown_result.stats.last_error_detail.find("executor boom"),
                      std::string::npos);
        }

        TEST_F(StoreExecutorTest, TaskContextIsAcceptedButNotAutoCancelledByExecutor)
        {
            BoundedStorageExecutor executor(SingleWorkerConfig(2));
            std::promise<void> task_done_promise;
            std::future<void> task_done = task_done_promise.get_future();

            const auto submit_result = executor.Submit(StorageExecutorSubmitRequest{
                .task_name = "context-task",
                .context =
                    StorageTaskContext{
                        .timeout_ms = 1,
                        .best_effort_cancel = true,
                    },
                .task =
                    [&]()
                    {
                        task_done_promise.set_value();
                    }});
            ASSERT_TRUE(submit_result.accepted());

            const auto shutdown_result =
                executor.Shutdown(StorageExecutorShutdownRequest{
                    .mode = StorageExecutorStopMode::kDrain});
            EXPECT_TRUE(shutdown_result.stopped);
            EXPECT_TRUE(shutdown_result.drained);
            EXPECT_EQ(task_done.wait_for(1s), std::future_status::ready);
            EXPECT_EQ(shutdown_result.stats.failed_tasks, 0U);
            EXPECT_EQ(shutdown_result.stats.completed_tasks, 1U);
        }

        TEST_F(StoreExecutorTest, ShutdownCalledFromWorkerReturnsExplicitBoundaryError)
        {
            BoundedStorageExecutor executor(SingleWorkerConfig(2));
            std::promise<StorageExecutorShutdownResult> worker_shutdown_promise;
            std::future<StorageExecutorShutdownResult> worker_shutdown =
                worker_shutdown_promise.get_future();
            std::promise<void> follow_up_done_promise;
            std::future<void> follow_up_done = follow_up_done_promise.get_future();

            ASSERT_TRUE(executor.Submit(StorageExecutorSubmitRequest{
                            .task_name = "worker-shutdown",
                            .task =
                                [&]()
                                {
                                    worker_shutdown_promise.set_value(executor.Shutdown());
                                }})
                            .accepted());

            const auto worker_result = worker_shutdown.get();
            EXPECT_FALSE(worker_result.stopped);
            EXPECT_NE(worker_result.error_detail.find("must not be called from executor worker"),
                      std::string::npos);

            ASSERT_TRUE(executor.Submit(StorageExecutorSubmitRequest{
                            .task_name = "follow-up-after-worker-shutdown",
                            .task =
                                [&]()
                                {
                                    follow_up_done_promise.set_value();
                                }})
                            .accepted());

            const auto owner_shutdown_result =
                executor.Shutdown(StorageExecutorShutdownRequest{
                    .mode = StorageExecutorStopMode::kDrain});
            EXPECT_TRUE(owner_shutdown_result.stopped);
            EXPECT_TRUE(owner_shutdown_result.drained);
            EXPECT_EQ(follow_up_done.wait_for(1s), std::future_status::ready);
        }

        TEST_F(StoreExecutorTest, DestructorDrainsActiveTaskAndReclaimsWorkerThreads)
        {
            std::promise<void> task_started_promise;
            std::future<void> task_started = task_started_promise.get_future();
            std::promise<void> release_task_promise;
            std::shared_future<void> release_task =
                release_task_promise.get_future().share();

            auto destructor_future = std::async(
                std::launch::async,
                [&]()
                {
                    BoundedStorageExecutor executor(SingleWorkerConfig(2));
                    const auto submit_result = executor.Submit(StorageExecutorSubmitRequest{
                        .task_name = "destructor-blocking-task",
                        .task =
                            [&]()
                            {
                                task_started_promise.set_value();
                                release_task.wait();
                            }});
                    if (!submit_result.accepted())
                    {
                        return false;
                    }
                    return true;
                });

            ASSERT_EQ(task_started.wait_for(1s), std::future_status::ready);
            EXPECT_EQ(destructor_future.wait_for(100ms), std::future_status::timeout);

            release_task_promise.set_value();

            EXPECT_EQ(destructor_future.wait_for(1s), std::future_status::ready);
            EXPECT_TRUE(destructor_future.get());
        }
    } // namespace
} // namespace storedemo
