#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "store/common/store_types.h"
#include "store/maintenance/garbage_collector.h"
#include "store/maintenance/gc_task_store.h"
#include "support/store_test_utils.h"

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

        CleanupChunkFact MakeCleanupChunkFact(const std::string &object_id,
                                              const std::uint64_t version,
                                              const std::uint32_t chunk_index,
                                              const std::uint64_t offset,
                                              const std::uint64_t size)
        {
            ChunkId chunk_id;
            std::string error_detail;
            if (MakeChunkId(object_id, version, chunk_index, &chunk_id, &error_detail) !=
                StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to build cleanup chunk id: " + error_detail);
            }

            CleanupChunkFact fact;
            fact.identity.chunk_id = std::move(chunk_id);
            fact.identity.object_id = object_id;
            fact.identity.version = version;
            fact.identity.chunk_index = chunk_index;
            fact.identity.offset = offset;
            fact.size = size;
            fact.checksum.algorithm = ChunkChecksumAlgorithm::kSha256;
            fact.checksum.value = "checksum-" + object_id + "-" + std::to_string(chunk_index);
            fact.checksum.size_bytes = size;
            fact.replica_nodes = {"node-a", "node-b"};
            return fact;
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

            static GarbageCollectorConfig PersistentSingleWorkerConfig(
                const std::filesystem::path &root_path,
                const std::size_t queue_capacity = 4,
                const std::uint32_t default_max_attempts = 3)
            {
                return GarbageCollectorConfig{
                    .worker_count = 1,
                    .queue_capacity = queue_capacity,
                    .default_max_attempts = default_max_attempts,
                    .persistence_root = root_path};
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

            static GarbageCollectorTaskStore MakeTaskStore(
                const std::filesystem::path &root_path)
            {
                return GarbageCollectorTaskStore(GarbageCollectorTaskStoreConfig{
                    .root_path = root_path});
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
               SubmitTaskPersistsSnapshotFileAndRetainsCriticalFields)
        {
            storedemo::test::ScopedStoreTestDir temp_dir("gc_task_snapshot_submit");
            std::promise<void> task_started_promise;
            std::future<void> task_started = task_started_promise.get_future();
            std::promise<void> release_task_promise;
            std::shared_future<void> release_task =
                release_task_promise.get_future().share();

            GarbageCollector collector(
                [&](const GarbageCollectorTask &)
                {
                    task_started_promise.set_value();
                    release_task.wait();
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                PersistentSingleWorkerConfig(temp_dir.root()));

            auto task = MakeTask("gc-task-persisted-submit", "obj-gc-persisted~1~0");
            task.object_id = "obj-gc-persisted";
            task.version = 1;
            task.chunk_index = 0;
            task.max_attempts = 5;
            ASSERT_TRUE(collector.SubmitTask(task).accepted());
            ASSERT_EQ(task_started.wait_for(1s), std::future_status::ready);

            const auto task_store = MakeTaskStore(temp_dir.root());
            EXPECT_TRUE(std::filesystem::exists(task_store.snapshot_path()));

            const auto load_result = task_store.LoadSnapshot();
            ASSERT_TRUE(load_result.ok()) << load_result.error_detail;
            ASSERT_TRUE(load_result.snapshot_found);
            ASSERT_EQ(load_result.tasks.size(), 1U);
            EXPECT_EQ(load_result.tasks.front().task_id, task.task_id);
            EXPECT_EQ(load_result.tasks.front().chunk_id, task.chunk_id);
            EXPECT_EQ(load_result.tasks.front().object_id, task.object_id);
            EXPECT_EQ(load_result.tasks.front().version, task.version);
            EXPECT_EQ(load_result.tasks.front().chunk_index, task.chunk_index);
            EXPECT_EQ(load_result.tasks.front().reason, task.reason);
            EXPECT_EQ(load_result.tasks.front().metadata_boundary, task.metadata_boundary);
            EXPECT_EQ(load_result.tasks.front().state, GarbageCollectorTaskState::kRunning);
            EXPECT_EQ(load_result.tasks.front().attempts, 0U);
            EXPECT_EQ(load_result.tasks.front().max_attempts, 5U);

            release_task_promise.set_value();
            EXPECT_TRUE(collector.Drain().drained);
        }

        TEST_F(StorageGarbageCollectorTest,
               SaveSnapshotPreservesV1FormatSortedOrderAndLargeTaskRecovery)
        {
            storedemo::test::ScopedStoreTestDir temp_dir("gc_streaming_snapshot_save");
            auto task_store = MakeTaskStore(temp_dir.root());

            std::vector<GarbageCollectorTask> tasks;
            tasks.reserve(128);
            std::vector<std::string> expected_task_ids;
            expected_task_ids.reserve(128);
            for (int index = 127; index >= 0; --index)
            {
                const std::string task_id = "gc-task-stream-" + std::to_string(index);
                const std::string object_id =
                    "obj-gc-stream-" + std::to_string(index % 11);
                auto task = MakeTask(
                    task_id,
                    object_id + "~" + std::to_string((index % 5) + 1) + "~" +
                        std::to_string(index % 7));
                task.object_id = object_id;
                task.version = static_cast<std::uint64_t>((index % 5) + 1);
                task.chunk_index = static_cast<std::uint32_t>(index % 7);
                task.reason =
                    (index % 2 == 0)
                        ? GarbageCollectionReason::kDeletedObjectCleanup
                        : GarbageCollectionReason::kFailedUploadCleanup;
                task.metadata_boundary =
                    "metadata-fact:streaming-save-" + std::to_string(index);
                task.attempts = static_cast<std::uint32_t>(index % 3);
                task.max_attempts = 5;
                task.last_error = (index % 4 == 0)
                                      ? StorageNodeStatusCode::kTimeout
                                      : StorageNodeStatusCode::kOk;
                task.last_error_detail =
                    (index % 4 == 0) ? "timeout-" + std::to_string(index) : "";
                task.state = (index % 3 == 0) ? GarbageCollectorTaskState::kRetryPending
                                              : GarbageCollectorTaskState::kQueued;
                task.retryable = (index % 3) == 0;
                task.next_retry_after_ms =
                    task.retryable ? static_cast<std::uint64_t>(index + 10) : 0U;
                expected_task_ids.push_back(task_id);
                tasks.push_back(std::move(task));
            }
            std::sort(expected_task_ids.begin(), expected_task_ids.end());

            const auto save_result = task_store.SaveSnapshot(tasks);
            ASSERT_TRUE(save_result.ok()) << save_result.error_detail;
            EXPECT_TRUE(std::filesystem::exists(task_store.snapshot_path()));

            std::ifstream input(task_store.snapshot_path(), std::ios::binary);
            ASSERT_TRUE(input.is_open());
            std::string header;
            std::string count_line;
            ASSERT_TRUE(std::getline(input, header));
            ASSERT_TRUE(std::getline(input, count_line));
            EXPECT_EQ(header, "GC_TASK_STORE_V1");
            EXPECT_EQ(count_line, "count 128");

            std::vector<std::string> raw_task_lines;
            std::string line;
            while (std::getline(input, line))
            {
                if (!line.empty())
                {
                    raw_task_lines.push_back(line);
                }
            }
            ASSERT_EQ(raw_task_lines.size(), 128U);
            EXPECT_EQ(raw_task_lines.front().rfind("task ", 0), 0U);
            EXPECT_EQ(raw_task_lines.back().rfind("task ", 0), 0U);

            const auto load_result = task_store.LoadSnapshot();
            ASSERT_TRUE(load_result.ok()) << load_result.error_detail;
            ASSERT_TRUE(load_result.snapshot_found);
            ASSERT_EQ(load_result.tasks.size(), 128U);

            for (std::size_t index = 0; index < load_result.tasks.size(); ++index)
            {
                const auto &task = load_result.tasks[index];
                EXPECT_EQ(task.task_id, expected_task_ids[index]);
                EXPECT_FALSE(task.chunk_id.empty());
                EXPECT_FALSE(task.metadata_boundary.empty());
                EXPECT_LE(task.attempts, task.max_attempts);
                if ((index % 2) == 0)
                {
                    EXPECT_TRUE(task.reason == GarbageCollectionReason::kDeletedObjectCleanup ||
                                task.reason == GarbageCollectionReason::kFailedUploadCleanup);
                }
            }

            const auto &sample_task = load_result.tasks.at(37);
            EXPECT_FALSE(sample_task.object_id.empty());
            EXPECT_FALSE(sample_task.chunk_id.empty());
            EXPECT_FALSE(sample_task.metadata_boundary.empty());
            EXPECT_LE(sample_task.attempts, sample_task.max_attempts);
            EXPECT_TRUE(sample_task.state == GarbageCollectorTaskState::kQueued ||
                        sample_task.state == GarbageCollectorTaskState::kRetryPending);
        }

        TEST_F(StorageGarbageCollectorTest, RestartResumesQueuedAndRunningTasks)
        {
            storedemo::test::ScopedStoreTestDir temp_dir("gc_restart_resume_basic");
            auto task_store = MakeTaskStore(temp_dir.root());

            auto queued_task = MakeTask("gc-task-resume-queued", "obj-gc-resume~1~0");
            queued_task.state = GarbageCollectorTaskState::kQueued;
            queued_task.max_attempts = 3;

            auto running_task = MakeTask("gc-task-resume-running", "obj-gc-resume~1~1");
            running_task.state = GarbageCollectorTaskState::kRunning;
            running_task.max_attempts = 3;

            const auto save_result =
                task_store.SaveSnapshot({queued_task, running_task});
            ASSERT_TRUE(save_result.ok()) << save_result.error_detail;

            std::atomic<int> handler_runs{0};
            GarbageCollector collector(
                [&](const GarbageCollectorTask &task)
                {
                    handler_runs.fetch_add(1, std::memory_order_relaxed);
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                PersistentSingleWorkerConfig(temp_dir.root()));

            ASSERT_TRUE(collector.Drain().drained);

            const auto queued_snapshot = collector.FindTask("gc-task-resume-queued");
            const auto running_snapshot = collector.FindTask("gc-task-resume-running");
            ASSERT_TRUE(queued_snapshot.has_value());
            ASSERT_TRUE(running_snapshot.has_value());
            EXPECT_EQ(queued_snapshot->state, GarbageCollectorTaskState::kCompleted);
            EXPECT_EQ(running_snapshot->state, GarbageCollectorTaskState::kCompleted);
            EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 2);
        }

        TEST_F(StorageGarbageCollectorTest,
               RestartResumesRetryPendingTaskWithoutLosingErrorFacts)
        {
            storedemo::test::ScopedStoreTestDir temp_dir("gc_restart_resume_retry_pending");
            auto task_store = MakeTaskStore(temp_dir.root());

            auto retry_pending_task = MakeTask("gc-task-retry-persisted",
                                               "obj-gc-retry-persisted~1~0");
            retry_pending_task.state = GarbageCollectorTaskState::kRetryPending;
            retry_pending_task.attempts = 1;
            retry_pending_task.max_attempts = 3;
            retry_pending_task.last_error = StorageNodeStatusCode::kTimeout;
            retry_pending_task.last_error_detail = "persisted timeout";
            retry_pending_task.retryable = true;
            retry_pending_task.next_retry_after_ms = 25;

            const auto save_result = task_store.SaveSnapshot({retry_pending_task});
            ASSERT_TRUE(save_result.ok()) << save_result.error_detail;

            std::atomic<int> handler_runs{0};
            GarbageCollector collector(
                [&](const GarbageCollectorTask &task)
                {
                    EXPECT_EQ(task.metadata_boundary, "metadata-fact:deleted-object");
                    handler_runs.fetch_add(1, std::memory_order_relaxed);
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                PersistentSingleWorkerConfig(temp_dir.root()));

            ASSERT_TRUE(collector.Drain().drained);

            const auto snapshot = collector.FindTask("gc-task-retry-persisted");
            ASSERT_TRUE(snapshot.has_value());
            EXPECT_EQ(snapshot->state, GarbageCollectorTaskState::kCompleted);
            EXPECT_EQ(snapshot->attempts, 2U);
            EXPECT_EQ(snapshot->last_error, StorageNodeStatusCode::kTimeout);
            EXPECT_EQ(snapshot->last_error_detail, "persisted timeout");
            EXPECT_EQ(snapshot->reason, GarbageCollectionReason::kDeletedObjectCleanup);
            EXPECT_EQ(snapshot->chunk_id, "obj-gc-retry-persisted~1~0");
            EXPECT_EQ(snapshot->metadata_boundary, "metadata-fact:deleted-object");
            EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 1);
        }

        TEST_F(StorageGarbageCollectorTest,
               RestartDoesNotReexecuteCompletedOrFailedTasks)
        {
            storedemo::test::ScopedStoreTestDir temp_dir("gc_restart_terminal_states");
            auto task_store = MakeTaskStore(temp_dir.root());

            auto completed_task = MakeTask("gc-task-completed-persisted",
                                           "obj-gc-terminal~1~0");
            completed_task.state = GarbageCollectorTaskState::kCompleted;
            completed_task.attempts = 1;
            completed_task.max_attempts = 3;

            auto failed_task = MakeTask("gc-task-failed-persisted",
                                        "obj-gc-terminal~1~1");
            failed_task.state = GarbageCollectorTaskState::kFailed;
            failed_task.attempts = 3;
            failed_task.max_attempts = 3;
            failed_task.last_error = StorageNodeStatusCode::kConflict;
            failed_task.last_error_detail = "live manifest blocked";
            failed_task.retryable = false;

            auto cancelled_task = MakeTask("gc-task-cancelled-persisted",
                                           "obj-gc-terminal~1~2");
            cancelled_task.state = GarbageCollectorTaskState::kCancelled;
            cancelled_task.max_attempts = 3;
            cancelled_task.last_error = StorageNodeStatusCode::kCancelled;
            cancelled_task.last_error_detail = "cancelled before restart";

            const auto save_result =
                task_store.SaveSnapshot({completed_task, failed_task, cancelled_task});
            ASSERT_TRUE(save_result.ok()) << save_result.error_detail;

            std::atomic<int> handler_runs{0};
            GarbageCollector collector(
                [&](const GarbageCollectorTask &)
                {
                    handler_runs.fetch_add(1, std::memory_order_relaxed);
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                PersistentSingleWorkerConfig(temp_dir.root()));

            std::this_thread::sleep_for(100ms);
            const auto stop_result = collector.Stop();
            EXPECT_TRUE(stop_result.stopped);
            EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 0);

            const auto completed_snapshot =
                collector.FindTask("gc-task-completed-persisted");
            const auto failed_snapshot =
                collector.FindTask("gc-task-failed-persisted");
            const auto cancelled_snapshot =
                collector.FindTask("gc-task-cancelled-persisted");
            ASSERT_TRUE(completed_snapshot.has_value());
            ASSERT_TRUE(failed_snapshot.has_value());
            ASSERT_TRUE(cancelled_snapshot.has_value());
            EXPECT_EQ(completed_snapshot->state, GarbageCollectorTaskState::kCompleted);
            EXPECT_EQ(failed_snapshot->state, GarbageCollectorTaskState::kFailed);
            EXPECT_EQ(cancelled_snapshot->state, GarbageCollectorTaskState::kCancelled);
        }

        TEST_F(StorageGarbageCollectorTest,
               CorruptedPersistenceSnapshotDoesNotCrashCollector)
        {
            storedemo::test::ScopedStoreTestDir temp_dir("gc_restart_corrupted_snapshot");
            auto task_store = MakeTaskStore(temp_dir.root());

            std::filesystem::create_directories(task_store.snapshot_path().parent_path());
            {
                std::ofstream output(task_store.snapshot_path(), std::ios::binary);
                ASSERT_TRUE(output.is_open());
                output << "not-a-valid-gc-snapshot\n";
            }

            std::atomic<int> handler_runs{0};
            GarbageCollector collector(
                [&](const GarbageCollectorTask &)
                {
                    handler_runs.fetch_add(1, std::memory_order_relaxed);
                    DeleteChunkResponse response;
                    response.status = StorageNodeStatusCode::kOk;
                    response.deleted = true;
                    return response;
                },
                AllowDeleteByMetadataSafety,
                PersistentSingleWorkerConfig(temp_dir.root()));

            EXPECT_NE(collector.SnapshotStats().last_error_detail.find(
                          "failed to load persisted garbage collector tasks"),
                      std::string::npos);

            ASSERT_TRUE(collector.SubmitTask(
                            MakeTask("gc-task-after-corruption", "obj-gc-after~1~0"))
                            .accepted());
            ASSERT_TRUE(collector.Drain().drained);
            EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 1);
        }

        TEST_F(StorageGarbageCollectorTest,
               RestoredTasksStillPassThroughMetadataSafetyChecker)
        {
            storedemo::test::ScopedStoreTestDir temp_dir("gc_restart_safety_checker");
            auto task_store = MakeTaskStore(temp_dir.root());
            auto blocked_task = MakeTask("gc-task-persisted-blocked",
                                         "obj-gc-persisted-blocked~1~0");
            blocked_task.state = GarbageCollectorTaskState::kQueued;
            blocked_task.max_attempts = 3;
            const auto save_result = task_store.SaveSnapshot({blocked_task});
            ASSERT_TRUE(save_result.ok()) << save_result.error_detail;

            std::atomic<int> handler_runs{0};
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
                    EXPECT_EQ(task.metadata_boundary, "metadata-fact:deleted-object");
                    GarbageCollectorSafetyCheckResult result;
                    result.status = StorageNodeStatusCode::kConflict;
                    result.error_detail = "persisted live manifest block";
                    return result;
                },
                PersistentSingleWorkerConfig(temp_dir.root()));

            ASSERT_TRUE(collector.Drain().drained);
            const auto snapshot = collector.FindTask("gc-task-persisted-blocked");
            ASSERT_TRUE(snapshot.has_value());
            EXPECT_EQ(snapshot->state, GarbageCollectorTaskState::kFailed);
            EXPECT_EQ(snapshot->last_error, StorageNodeStatusCode::kConflict);
            EXPECT_EQ(snapshot->last_error_detail, "persisted live manifest block");
            EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 0);
        }

        TEST_F(StorageGarbageCollectorTest,
               PendingTimeoutGeneratesSortedDeduplicatedCleanupCandidates)
        {
            PendingTimeoutCleanupRequest request;
            request.bucket = "bucket-t056";
            request.object_key = "objects/pending-timeout";
            request.object_id = "obj-t056-pending";
            request.version = 7;
            request.object_state = CleanupObjectState::kPending;
            request.created_at_unix_ms = 1000;
            request.now_unix_ms = 2500;
            request.timeout_ms = 1000;
            request.durable_chunks = {
                MakeCleanupChunkFact("obj-t056-pending", 7, 2, 1024, 256),
                MakeCleanupChunkFact("obj-t056-pending", 7, 0, 0, 512),
                MakeCleanupChunkFact("obj-t056-pending", 7, 2, 1024, 256)};

            const auto candidates = BuildPendingTimeoutCleanupCandidates(request);

            ASSERT_EQ(candidates.size(), 2U);
            EXPECT_EQ(candidates[0].identity.chunk_index, 0U);
            EXPECT_EQ(candidates[1].identity.chunk_index, 2U);
            EXPECT_EQ(candidates[0].source, CleanupCandidateSource::kPendingTimeout);
            EXPECT_EQ(candidates[0].reason, GarbageCollectionReason::kOrphanChunkCleanup);
            EXPECT_EQ(candidates[0].object_state, CleanupObjectState::kPending);
            EXPECT_EQ(candidates[0].deadline_unix_ms, 2000U);
            EXPECT_NE(candidates[0].metadata_boundary.find("metadata-fact:pending-timeout"),
                      std::string::npos);
            EXPECT_NE(candidates[0].metadata_boundary.find("deadline_ms=2000"),
                      std::string::npos);
        }

        TEST_F(StorageGarbageCollectorTest,
               PendingTimeoutBeforeDeadlineDoesNotGenerateCleanupCandidates)
        {
            PendingTimeoutCleanupRequest request;
            request.bucket = "bucket-t056";
            request.object_key = "objects/pending-live";
            request.object_id = "obj-t056-pending-live";
            request.version = 3;
            request.object_state = CleanupObjectState::kPending;
            request.created_at_unix_ms = 1000;
            request.now_unix_ms = 1500;
            request.timeout_ms = 1000;
            request.durable_chunks = {
                MakeCleanupChunkFact("obj-t056-pending-live", 3, 0, 0, 128)};

            EXPECT_TRUE(BuildPendingTimeoutCleanupCandidates(request).empty());
        }

        TEST_F(StorageGarbageCollectorTest,
               FailedUploadCleanupCandidateConvertsToGarbageCollectorTask)
        {
            FailedUploadCleanupRequest request;
            request.bucket = "bucket-t056";
            request.object_key = "objects/failed-upload";
            request.object_id = "obj-t056-failed";
            request.version = 9;
            request.object_state = CleanupObjectState::kPending;
            request.created_at_unix_ms = 777;
            request.durable_chunks = {
                MakeCleanupChunkFact("obj-t056-failed", 9, 1, 256, 256)};

            const auto candidates = BuildFailedUploadCleanupCandidates(request);

            ASSERT_EQ(candidates.size(), 1U);
            EXPECT_EQ(candidates.front().source, CleanupCandidateSource::kFailedUpload);
            EXPECT_EQ(candidates.front().reason, GarbageCollectionReason::kFailedUploadCleanup);
            EXPECT_NE(candidates.front().metadata_boundary.find("metadata-fact:failed-upload"),
                      std::string::npos);

            const auto task = CleanupCandidateToGarbageCollectorTask(candidates.front());
            EXPECT_EQ(task.chunk_id, candidates.front().identity.chunk_id);
            EXPECT_EQ(task.object_id, candidates.front().identity.object_id);
            EXPECT_EQ(task.version, candidates.front().identity.version);
            EXPECT_EQ(task.chunk_index, candidates.front().identity.chunk_index);
            EXPECT_EQ(task.reason, GarbageCollectionReason::kFailedUploadCleanup);
            EXPECT_EQ(task.metadata_boundary, candidates.front().metadata_boundary);
            EXPECT_NE(task.task_id.find(candidates.front().identity.chunk_id), std::string::npos);
        }

        TEST_F(StorageGarbageCollectorTest,
               AbortAndDeletedCleanupCandidatesRespectObjectStateBoundaries)
        {
            AbortCleanupRequest abort_request;
            abort_request.bucket = "bucket-t056";
            abort_request.object_key = "objects/abort";
            abort_request.object_id = "obj-t056-abort";
            abort_request.version = 5;
            abort_request.object_state = CleanupObjectState::kAborted;
            abort_request.durable_chunks = {
                MakeCleanupChunkFact("obj-t056-abort", 5, 0, 0, 64)};

            DeletedObjectCleanupRequest deleted_request;
            deleted_request.bucket = "bucket-t056";
            deleted_request.object_key = "objects/deleted";
            deleted_request.object_id = "obj-t056-deleted";
            deleted_request.version = 6;
            deleted_request.object_state = CleanupObjectState::kDeleted;
            deleted_request.durable_chunks = {
                MakeCleanupChunkFact("obj-t056-deleted", 6, 0, 0, 96)};

            const auto abort_candidates = BuildAbortCleanupCandidates(abort_request);
            const auto deleted_candidates = BuildDeletedObjectCleanupCandidates(deleted_request);

            ASSERT_EQ(abort_candidates.size(), 1U);
            ASSERT_EQ(deleted_candidates.size(), 1U);
            EXPECT_EQ(abort_candidates.front().reason, GarbageCollectionReason::kAbortCleanup);
            EXPECT_EQ(deleted_candidates.front().reason,
                      GarbageCollectionReason::kDeletedObjectCleanup);
            EXPECT_NE(abort_candidates.front().metadata_boundary.find("metadata-fact:abort-cleanup"),
                      std::string::npos);
            EXPECT_NE(
                deleted_candidates.front().metadata_boundary.find("metadata-fact:deleted-object"),
                std::string::npos);

            abort_request.object_state = CleanupObjectState::kCommitted;
            deleted_request.object_state = CleanupObjectState::kCommitted;
            EXPECT_TRUE(BuildAbortCleanupCandidates(abort_request).empty());
            EXPECT_TRUE(BuildDeletedObjectCleanupCandidates(deleted_request).empty());
        }

        TEST_F(StorageGarbageCollectorTest,
               CommittedObjectDoesNotGenerateFailedUploadCleanupCandidates)
        {
            FailedUploadCleanupRequest request;
            request.bucket = "bucket-t056";
            request.object_key = "objects/committed";
            request.object_id = "obj-t056-committed";
            request.version = 11;
            request.object_state = CleanupObjectState::kCommitted;
            request.durable_chunks = {
                MakeCleanupChunkFact("obj-t056-committed", 11, 0, 0, 128)};

            EXPECT_TRUE(BuildFailedUploadCleanupCandidates(request).empty());
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
