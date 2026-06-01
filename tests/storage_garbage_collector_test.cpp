#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <thread>

#include "store/common/store_types.h"
#include "store/maintenance/garbage_collector.h"

namespace storedemo
{
    namespace
    {
        using namespace std::chrono_literals;

        GarbageCollectorSafetyCheckResult AllowDeleteByMetadataSafety(
            const GarbageCollectorTask &task)
        {
            (void)task;
            return {};
        }

        class StorageGarbageCollectorTest : public ::testing::Test
        {
        protected:
            static GarbageCollectorConfig SingleWorkerConfig(
                const std::size_t queue_capacity = 4,
                const std::uint32_t default_max_attempts = 3)
            {
                return GarbageCollectorConfig{
                    .worker_count = 1,
                    .queue_capacity = queue_capacity,
                    .default_max_attempts = default_max_attempts};
            }

            static GarbageCollectorTask MakeTask(const std::string &task_id,
                                                 const std::string &chunk_id)
            {
                GarbageCollectorTask task;
                task.task_id = task_id;
                task.chunk_id = chunk_id;
                task.reason = GarbageCollectionReason::kDeletedObjectCleanup;
                task.metadata_boundary = "metadata-fact:deleted-object";
                return task;
            }
        };

        TEST_F(StorageGarbageCollectorTest, SubmitValidTaskSucceedsAndPreservesMetadataBoundary)
        {
            std::promise<void> task_started_promise;
            std::future<void> task_started = task_started_promise.get_future();
            std::promise<void> release_task_promise;
            std::shared_future<void> release_task =
                release_task_promise.get_future().share();
            std::string observed_metadata_boundary;
            std::string observed_chunk_id;

            GarbageCollector collector(
                [&](const GarbageCollectorTask &task)
                {
                    observed_metadata_boundary = task.metadata_boundary;
                    observed_chunk_id = task.chunk_id;
                    task_started_promise.set_value();
                    release_task.wait();

                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                SingleWorkerConfig());

            const auto submit_result =
                collector.SubmitTask(MakeTask("gc-task-submit", "obj-gc-submit~1~0"));
            ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;
            ASSERT_EQ(task_started.wait_for(1s), std::future_status::ready);

            auto running_task = collector.FindTask("gc-task-submit");
            ASSERT_TRUE(running_task.has_value());
            EXPECT_EQ(running_task->metadata_boundary, "metadata-fact:deleted-object");
            EXPECT_EQ(running_task->chunk_id, "obj-gc-submit~1~0");
            EXPECT_EQ(running_task->state, GarbageCollectorTaskState::kRunning);

            release_task_promise.set_value();
            const auto drain_result = collector.Drain();
            EXPECT_TRUE(drain_result.drained) << drain_result.error_detail;

            const auto completed_task = collector.FindTask("gc-task-submit");
            ASSERT_TRUE(completed_task.has_value());
            EXPECT_EQ(completed_task->metadata_boundary, "metadata-fact:deleted-object");
            EXPECT_EQ(completed_task->state, GarbageCollectorTaskState::kCompleted);
            EXPECT_EQ(completed_task->attempts, 1U);
            EXPECT_EQ(observed_metadata_boundary, "metadata-fact:deleted-object");
            EXPECT_EQ(observed_chunk_id, "obj-gc-submit~1~0");

            const auto stats = collector.SnapshotStats();
            EXPECT_EQ(stats.completed_tasks, 1U);
            EXPECT_EQ(stats.submitted_tasks, 1U);

            const auto stop_result = collector.Stop();
            EXPECT_TRUE(stop_result.stopped);
        }

        TEST_F(StorageGarbageCollectorTest, RejectsInvalidTask)
        {
            GarbageCollector collector(
                [](const GarbageCollectorTask &)
                {
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                SingleWorkerConfig());

            auto invalid_task = MakeTask("", "");
            invalid_task.reason = GarbageCollectionReason::kUnspecified;
            invalid_task.metadata_boundary.clear();

            const auto submit_result = collector.SubmitTask(std::move(invalid_task));
            EXPECT_EQ(submit_result.code, GarbageCollectorSubmitCode::kInvalidArgument);
            EXPECT_EQ(submit_result.status_code(), StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(submit_result.accepted());

            const auto stats = collector.SnapshotStats();
            EXPECT_EQ(stats.submitted_tasks, 0U);
            EXPECT_EQ(stats.rejected_tasks, 0U);
        }

        TEST_F(StorageGarbageCollectorTest, QueueCapacityFullReturnsOverloaded)
        {
            std::promise<void> first_started_promise;
            std::future<void> first_started = first_started_promise.get_future();
            std::promise<void> release_first_promise;
            std::shared_future<void> release_first =
                release_first_promise.get_future().share();

            GarbageCollector collector(
                [&](const GarbageCollectorTask &task)
                {
                    if (task.task_id == "gc-task-blocking")
                    {
                        first_started_promise.set_value();
                        release_first.wait();
                    }

                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                SingleWorkerConfig(1));

            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-blocking", "obj-gc-overloaded~1~0"))
                            .accepted());
            ASSERT_EQ(first_started.wait_for(1s), std::future_status::ready);

            const auto queued_result = collector.SubmitTask(
                MakeTask("gc-task-queued", "obj-gc-overloaded~1~1"));
            ASSERT_TRUE(queued_result.accepted()) << queued_result.error_detail;

            const auto overloaded_result = collector.SubmitTask(
                MakeTask("gc-task-overloaded", "obj-gc-overloaded~1~2"));
            EXPECT_EQ(overloaded_result.code, GarbageCollectorSubmitCode::kOverloaded);
            EXPECT_EQ(overloaded_result.status_code(), StorageNodeStatusCode::kOverloaded);
            EXPECT_FALSE(overloaded_result.accepted());

            release_first_promise.set_value();
            const auto stop_result =
                collector.Stop(GarbageCollectorStopRequest{
                    .mode = GarbageCollectorStopMode::kDrain});
            EXPECT_TRUE(stop_result.stopped);
            EXPECT_EQ(stop_result.stats.rejected_tasks, 1U);
        }

        TEST_F(StorageGarbageCollectorTest, RetryableFailureRetriesAndCompletesTask)
        {
            std::atomic<int> attempt_count{0};

            GarbageCollector collector(
                [&](const GarbageCollectorTask &task)
                {
                    EXPECT_EQ(task.metadata_boundary, "metadata-fact:deleted-object");
                    const int current_attempt =
                        attempt_count.fetch_add(1, std::memory_order_relaxed);

                    DeleteChunkResponse response;
                    if (current_attempt == 0)
                    {
                        response.status = StorageNodeStatusCode::kTimeout;
                        response.error_detail = "temporary timeout";
                        response.retry_after_ms = 25;
                        return response;
                    }

                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                SingleWorkerConfig(4, 3));

            const auto submit_result =
                collector.SubmitTask(MakeTask("gc-task-retry-success",
                                              "obj-gc-retry-success~1~0"));
            ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;

            const auto drain_result = collector.Drain();
            EXPECT_TRUE(drain_result.drained) << drain_result.error_detail;

            const auto task = collector.FindTask("gc-task-retry-success");
            ASSERT_TRUE(task.has_value());
            EXPECT_EQ(task->state, GarbageCollectorTaskState::kCompleted);
            EXPECT_EQ(task->attempts, 2U);
            EXPECT_EQ(task->last_error, StorageNodeStatusCode::kTimeout);
            EXPECT_EQ(task->last_error_detail, "temporary timeout");
            EXPECT_FALSE(task->retryable);
            EXPECT_EQ(task->next_retry_after_ms, 0U);
            EXPECT_EQ(attempt_count.load(std::memory_order_relaxed), 2);

            const auto stats = collector.SnapshotStats();
            EXPECT_EQ(stats.completed_tasks, 1U);
            EXPECT_EQ(stats.total_attempts, 2U);
        }

        TEST_F(StorageGarbageCollectorTest, NonRetryableFailureTransitionsToFailed)
        {
            GarbageCollector collector(
                [](const GarbageCollectorTask &)
                {
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kInvalidArgument;
                    response.error_detail = "identity mismatch";
                    return response;
                },
                AllowDeleteByMetadataSafety,
                SingleWorkerConfig());

            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-nonretry", "obj-gc-nonretry~1~0"))
                            .accepted());
            ASSERT_TRUE(collector.Drain().drained);

            const auto task = collector.FindTask("gc-task-nonretry");
            ASSERT_TRUE(task.has_value());
            EXPECT_EQ(task->state, GarbageCollectorTaskState::kFailed);
            EXPECT_EQ(task->attempts, 1U);
            EXPECT_EQ(task->last_error, StorageNodeStatusCode::kInvalidArgument);
            EXPECT_EQ(task->last_error_detail, "identity mismatch");
            EXPECT_FALSE(task->retryable);

            const auto stats = collector.SnapshotStats();
            EXPECT_EQ(stats.failed_tasks, 1U);
            EXPECT_EQ(stats.total_attempts, 1U);
        }

        TEST_F(StorageGarbageCollectorTest, RetryableFailureStopsAtMaxAttempts)
        {
            std::atomic<int> attempt_count{0};

            GarbageCollector collector(
                [&](const GarbageCollectorTask &)
                {
                    attempt_count.fetch_add(1, std::memory_order_relaxed);
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kTimeout;
                    response.error_detail = "still timing out";
                    response.retry_after_ms = 10;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                SingleWorkerConfig(4, 2));

            auto task = MakeTask("gc-task-max-attempts", "obj-gc-max-attempts~1~0");
            task.max_attempts = 2;
            ASSERT_TRUE(collector.SubmitTask(std::move(task)).accepted());
            ASSERT_TRUE(collector.Drain().drained);

            const auto snapshot = collector.FindTask("gc-task-max-attempts");
            ASSERT_TRUE(snapshot.has_value());
            EXPECT_EQ(snapshot->state, GarbageCollectorTaskState::kFailed);
            EXPECT_EQ(snapshot->attempts, 2U);
            EXPECT_EQ(snapshot->last_error, StorageNodeStatusCode::kTimeout);
            EXPECT_EQ(snapshot->last_error_detail, "still timing out");
            EXPECT_FALSE(snapshot->retryable);
            EXPECT_EQ(snapshot->next_retry_after_ms, 10U);
            EXPECT_EQ(attempt_count.load(std::memory_order_relaxed), 2);
        }

        TEST_F(StorageGarbageCollectorTest, DrainWaitsForSubmittedTasksWithoutStoppingCollector)
        {
            std::promise<void> first_started_promise;
            std::future<void> first_started = first_started_promise.get_future();
            std::promise<void> release_first_promise;
            std::shared_future<void> release_first =
                release_first_promise.get_future().share();
            std::promise<void> second_done_promise;
            std::future<void> second_done = second_done_promise.get_future();

            GarbageCollector collector(
                [&](const GarbageCollectorTask &task)
                {
                    if (task.task_id == "gc-task-drain-a")
                    {
                        first_started_promise.set_value();
                        release_first.wait();
                    }
                    if (task.task_id == "gc-task-drain-b")
                    {
                        second_done_promise.set_value();
                    }

                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                SingleWorkerConfig());

            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-drain-a", "obj-gc-drain~1~0"))
                            .accepted());
            ASSERT_EQ(first_started.wait_for(1s), std::future_status::ready);
            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-drain-b", "obj-gc-drain~1~1"))
                            .accepted());

            auto drain_future = std::async(std::launch::async,
                                           [&]()
                                           {
                                               return collector.Drain();
                                           });
            EXPECT_EQ(drain_future.wait_for(100ms), std::future_status::timeout);

            release_first_promise.set_value();

            const auto drain_result = drain_future.get();
            EXPECT_TRUE(drain_result.drained);
            EXPECT_EQ(second_done.wait_for(1s), std::future_status::ready);
            EXPECT_TRUE(collector.SnapshotStats().accepting_new_tasks);

            const auto follow_up_result = collector.SubmitTask(
                MakeTask("gc-task-drain-follow-up", "obj-gc-drain~1~2"));
            EXPECT_TRUE(follow_up_result.accepted()) << follow_up_result.error_detail;

            const auto stop_result =
                collector.Stop(GarbageCollectorStopRequest{
                    .mode = GarbageCollectorStopMode::kDrain});
            EXPECT_TRUE(stop_result.stopped);
        }

        TEST_F(StorageGarbageCollectorTest, StopDrainRejectsNewTasksAndWaitsForQueuedWork)
        {
            std::promise<void> first_started_promise;
            std::future<void> first_started = first_started_promise.get_future();
            std::promise<void> release_first_promise;
            std::shared_future<void> release_first =
                release_first_promise.get_future().share();
            std::atomic<int> completed_runs{0};

            GarbageCollector collector(
                [&](const GarbageCollectorTask &task)
                {
                    if (task.task_id == "gc-task-stop-a")
                    {
                        first_started_promise.set_value();
                        release_first.wait();
                    }

                    completed_runs.fetch_add(1, std::memory_order_relaxed);
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                SingleWorkerConfig());

            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-stop-a", "obj-gc-stop~1~0"))
                            .accepted());
            ASSERT_EQ(first_started.wait_for(1s), std::future_status::ready);
            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-stop-b", "obj-gc-stop~1~1"))
                            .accepted());

            auto stop_future = std::async(std::launch::async,
                                          [&]()
                                          {
                                              return collector.Stop(GarbageCollectorStopRequest{
                                                  .mode = GarbageCollectorStopMode::kDrain});
                                          });
            EXPECT_EQ(stop_future.wait_for(100ms), std::future_status::timeout);

            const auto rejected_submit = collector.SubmitTask(
                MakeTask("gc-task-stop-c", "obj-gc-stop~1~2"));
            EXPECT_EQ(rejected_submit.code, GarbageCollectorSubmitCode::kStopped);

            release_first_promise.set_value();
            const auto stop_result = stop_future.get();
            EXPECT_TRUE(stop_result.stopped);
            EXPECT_TRUE(stop_result.drained);
            EXPECT_EQ(completed_runs.load(std::memory_order_relaxed), 2);
        }

        TEST_F(StorageGarbageCollectorTest, StopCancelPendingCancelsQueuedTasks)
        {
            std::promise<void> first_started_promise;
            std::future<void> first_started = first_started_promise.get_future();
            std::promise<void> release_first_promise;
            std::shared_future<void> release_first =
                release_first_promise.get_future().share();
            std::atomic<int> handler_runs{0};

            GarbageCollector collector(
                [&](const GarbageCollectorTask &task)
                {
                    handler_runs.fetch_add(1, std::memory_order_relaxed);
                    if (task.task_id == "gc-task-cancel-a")
                    {
                        first_started_promise.set_value();
                        release_first.wait();
                    }

                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                SingleWorkerConfig());

            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-cancel-a", "obj-gc-cancel~1~0"))
                            .accepted());
            ASSERT_EQ(first_started.wait_for(1s), std::future_status::ready);
            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-cancel-b", "obj-gc-cancel~1~1"))
                            .accepted());

            auto stop_future = std::async(std::launch::async,
                                          [&]()
                                          {
                                              return collector.Stop(GarbageCollectorStopRequest{
                                                  .mode = GarbageCollectorStopMode::kCancelPending});
                                          });
            EXPECT_EQ(stop_future.wait_for(100ms), std::future_status::timeout);

            release_first_promise.set_value();
            const auto stop_result = stop_future.get();
            EXPECT_TRUE(stop_result.stopped);
            EXPECT_FALSE(stop_result.drained);
            EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 1);

            const auto cancelled_task = collector.FindTask("gc-task-cancel-b");
            ASSERT_TRUE(cancelled_task.has_value());
            EXPECT_EQ(cancelled_task->state, GarbageCollectorTaskState::kCancelled);
            EXPECT_EQ(cancelled_task->last_error, StorageNodeStatusCode::kCancelled);
            EXPECT_FALSE(cancelled_task->retryable);

            EXPECT_EQ(stop_result.stats.cancelled_tasks, 1U);
        }

        TEST_F(StorageGarbageCollectorTest,
               LiveManifestSafetyViolationBlocksDeleteHandlerAndFailsTask)
        {
            std::atomic<int> handler_runs{0};
            std::string observed_metadata_boundary;

            GarbageCollector collector(
                [&](const GarbageCollectorTask &)
                {
                    handler_runs.fetch_add(1, std::memory_order_relaxed);
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                [&](const GarbageCollectorTask &task)
                {
                    observed_metadata_boundary = task.metadata_boundary;
                    GarbageCollectorSafetyCheckResult result;
                    result.status = StorageNodeStatusCode::kConflict;
                    result.error_detail =
                        "chunk still referenced by committed live manifest";
                    return result;
                },
                SingleWorkerConfig());

            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-safety-blocked", "obj-gc-safety~1~0"))
                            .accepted());
            ASSERT_TRUE(collector.Drain().drained);

            const auto task = collector.FindTask("gc-task-safety-blocked");
            ASSERT_TRUE(task.has_value());
            EXPECT_EQ(task->state, GarbageCollectorTaskState::kFailed);
            EXPECT_EQ(task->attempts, 1U);
            EXPECT_EQ(task->last_error, StorageNodeStatusCode::kConflict);
            EXPECT_EQ(task->last_error_detail,
                      "chunk still referenced by committed live manifest");
            EXPECT_FALSE(task->retryable);
            EXPECT_EQ(task->next_retry_after_ms, 0U);
            EXPECT_EQ(observed_metadata_boundary, "metadata-fact:deleted-object");
            EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 0);
        }

        TEST_F(StorageGarbageCollectorTest,
               DeletedObjectOrphanAndFailedUploadTasksPassSafetyCheck)
        {
            std::atomic<int> handler_runs{0};
            std::atomic<int> safety_runs{0};

            GarbageCollector collector(
                [&](const GarbageCollectorTask &)
                {
                    handler_runs.fetch_add(1, std::memory_order_relaxed);
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                [&](const GarbageCollectorTask &task)
                {
                    safety_runs.fetch_add(1, std::memory_order_relaxed);
                    EXPECT_FALSE(task.metadata_boundary.empty());
                    switch (task.reason)
                    {
                    case GarbageCollectionReason::kDeletedObjectCleanup:
                    case GarbageCollectionReason::kOrphanChunkCleanup:
                    case GarbageCollectionReason::kFailedUploadCleanup:
                        return GarbageCollectorSafetyCheckResult{};
                    case GarbageCollectionReason::kAbortCleanup:
                    case GarbageCollectionReason::kUnspecified:
                    default:
                        break;
                    }

                    GarbageCollectorSafetyCheckResult result;
                    result.status = StorageNodeStatusCode::kInvalidArgument;
                    result.error_detail = "unexpected cleanup reason";
                    return result;
                },
                SingleWorkerConfig());

            auto deleted_task =
                MakeTask("gc-task-deleted-safe", "obj-gc-safe-deleted~1~0");

            auto orphan_task =
                MakeTask("gc-task-orphan-safe", "obj-gc-safe-orphan~1~0");
            orphan_task.reason = GarbageCollectionReason::kOrphanChunkCleanup;
            orphan_task.metadata_boundary = "metadata-fact:orphan-chunk";

            auto failed_upload_task =
                MakeTask("gc-task-failed-upload-safe", "obj-gc-safe-failed~1~0");
            failed_upload_task.reason = GarbageCollectionReason::kFailedUploadCleanup;
            failed_upload_task.metadata_boundary = "metadata-fact:failed-upload";

            ASSERT_TRUE(collector.SubmitTask(std::move(deleted_task)).accepted());
            ASSERT_TRUE(collector.SubmitTask(std::move(orphan_task)).accepted());
            ASSERT_TRUE(collector.SubmitTask(std::move(failed_upload_task)).accepted());
            ASSERT_TRUE(collector.Drain().drained);

            for (const std::string task_id : {"gc-task-deleted-safe",
                                              "gc-task-orphan-safe",
                                              "gc-task-failed-upload-safe"})
            {
                const auto task = collector.FindTask(task_id);
                ASSERT_TRUE(task.has_value());
                EXPECT_EQ(task->state, GarbageCollectorTaskState::kCompleted);
                EXPECT_EQ(task->attempts, 1U);
            }
            EXPECT_EQ(safety_runs.load(std::memory_order_relaxed), 3);
            EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 3);
        }

        TEST_F(StorageGarbageCollectorTest,
               RetryableSafetyCheckerFailureRetriesBeforeCallingDeleteHandler)
        {
            std::atomic<int> safety_attempts{0};
            std::atomic<int> handler_runs{0};
            std::string observed_metadata_boundary;

            GarbageCollector collector(
                [&](const GarbageCollectorTask &task)
                {
                    handler_runs.fetch_add(1, std::memory_order_relaxed);
                    observed_metadata_boundary = task.metadata_boundary;
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                [&](const GarbageCollectorTask &task)
                {
                    observed_metadata_boundary = task.metadata_boundary;
                    const int current_attempt =
                        safety_attempts.fetch_add(1, std::memory_order_relaxed);
                    if (current_attempt == 0)
                    {
                        GarbageCollectorSafetyCheckResult result;
                        result.status = StorageNodeStatusCode::kNodeUnavailable;
                        result.error_detail = "metadata safety checker unavailable";
                        result.retry_after_ms = 15;
                        return result;
                    }

                    return GarbageCollectorSafetyCheckResult{};
                },
                SingleWorkerConfig(4, 3));

            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-safety-retry", "obj-gc-safety-retry~1~0"))
                            .accepted());
            ASSERT_TRUE(collector.Drain().drained);

            const auto task = collector.FindTask("gc-task-safety-retry");
            ASSERT_TRUE(task.has_value());
            EXPECT_EQ(task->state, GarbageCollectorTaskState::kCompleted);
            EXPECT_EQ(task->attempts, 2U);
            EXPECT_EQ(task->last_error, StorageNodeStatusCode::kNodeUnavailable);
            EXPECT_EQ(task->last_error_detail, "metadata safety checker unavailable");
            EXPECT_FALSE(task->retryable);
            EXPECT_EQ(task->next_retry_after_ms, 0U);
            EXPECT_EQ(safety_attempts.load(std::memory_order_relaxed), 2);
            EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 1);
            EXPECT_EQ(observed_metadata_boundary, "metadata-fact:deleted-object");
        }

        TEST_F(StorageGarbageCollectorTest,
               RepeatedSafetyBlockedTasksNeverCallDeleteHandler)
        {
            std::atomic<int> handler_runs{0};
            std::atomic<int> safety_runs{0};

            GarbageCollector collector(
                [&](const GarbageCollectorTask &)
                {
                    handler_runs.fetch_add(1, std::memory_order_relaxed);
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                [&](const GarbageCollectorTask &)
                {
                    safety_runs.fetch_add(1, std::memory_order_relaxed);
                    GarbageCollectorSafetyCheckResult result;
                    result.status = StorageNodeStatusCode::kConflict;
                    result.error_detail =
                        "chunk still referenced by committed live manifest";
                    return result;
                },
                SingleWorkerConfig());

            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-safety-blocked-a", "obj-gc-safety-repeat~1~0"))
                            .accepted());
            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-safety-blocked-b", "obj-gc-safety-repeat~1~0"))
                            .accepted());
            ASSERT_TRUE(collector.Drain().drained);

            for (const std::string task_id : {"gc-task-safety-blocked-a",
                                              "gc-task-safety-blocked-b"})
            {
                const auto task = collector.FindTask(task_id);
                ASSERT_TRUE(task.has_value());
                EXPECT_EQ(task->state, GarbageCollectorTaskState::kFailed);
                EXPECT_EQ(task->last_error, StorageNodeStatusCode::kConflict);
            }
            EXPECT_EQ(safety_runs.load(std::memory_order_relaxed), 2);
            EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 0);
        }
    }
}
