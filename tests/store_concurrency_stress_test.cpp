#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/runtime/storage_executor.h"
#include "support/store_test_utils.h"

namespace storedemo
{
    namespace
    {
        constexpr std::size_t kUniqueChunkCount = 128;
        constexpr std::size_t kDeleteChunkCount = 48;
        constexpr std::size_t kConcurrentListProbeCount = 8;
        constexpr std::size_t kSameChunkWriterCountPerPayload = 8;
        constexpr std::size_t kExecutorWorkerCount = 8;
        constexpr std::size_t kExecutorQueueCapacity = 16;
        constexpr std::size_t kFinalListPageSize = 13;
        constexpr std::chrono::seconds kSubmitDeadline = std::chrono::seconds(10);

        struct ListedChunksResult
        {
            StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
            std::string error_detail;
            std::vector<ChunkMetadata> chunks;
        };

        std::filesystem::path RepoRoot()
        {
            return std::filesystem::path(__FILE__).parent_path().parent_path().lexically_normal();
        }

        std::filesystem::path T026VisualizedDataDir()
        {
            return RepoRoot() / "node-data" / "t026-local-concurrency-stress";
        }

        void ResetT026VisualizedDataDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(T026VisualizedDataDir(), ec);
            ec.clear();
            std::filesystem::create_directories(T026VisualizedDataDir(), ec);
            if (ec)
            {
                throw std::runtime_error("failed to prepare T026 node-data root: " +
                                         ec.message());
            }
        }

        ChunkChecksum ComputeChecksumOrThrow(const std::string_view payload)
        {
            ChunkChecksum checksum;
            std::string error_detail;
            const auto status =
                ComputeChunkChecksum(payload, &checksum, &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to compute checksum: " + error_detail);
            }
            return checksum;
        }

        ChunkIdentity MakeIdentityOrThrow(const std::string_view object_id,
                                          const std::uint64_t version,
                                          const std::uint32_t chunk_index,
                                          const std::uint64_t offset = 0)
        {
            ChunkId chunk_id;
            std::string error_detail;
            const auto status = MakeChunkId(object_id,
                                            version,
                                            chunk_index,
                                            &chunk_id,
                                            &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to build chunk id: " + error_detail);
            }

            ChunkIdentity identity;
            identity.chunk_id = std::move(chunk_id);
            identity.object_id = std::string(object_id);
            identity.version = version;
            identity.chunk_index = chunk_index;
            identity.offset = offset;
            return identity;
        }

        WriteChunkRequest MakeWriteRequest(const ChunkIdentity &identity,
                                           const std::string &payload,
                                           const std::string &request_id)
        {
            return WriteChunkRequest{
                .request_id = request_id,
                .identity = identity,
                .expected_size = static_cast<std::uint64_t>(payload.size()),
                .expected_checksum = ComputeChecksumOrThrow(payload),
                .payload = payload};
        }

        ReadChunkRequest MakeReadRequest(const ChunkId &chunk_id,
                                         const std::string &request_id)
        {
            return ReadChunkRequest{
                .request_id = request_id,
                .chunk_id = chunk_id};
        }

        DeleteChunkRequest MakeDeleteRequest(const ChunkId &chunk_id,
                                             const std::string &request_id)
        {
            return DeleteChunkRequest{
                .request_id = request_id,
                .chunk_id = chunk_id};
        }

        StatChunkRequest MakeStatRequest(const ChunkId &chunk_id,
                                         const std::string &request_id,
                                         const bool verify_checksum = false)
        {
            return StatChunkRequest{
                .request_id = request_id,
                .chunk_id = chunk_id,
                .verify_checksum = verify_checksum};
        }

        ListChunksRequest MakeListRequest(const std::string &request_id,
                                          const ListChunksOptions &options)
        {
            return ListChunksRequest{
                .request_id = request_id,
                .options = options};
        }

        std::string MakeRequestId(const std::string_view prefix, const std::size_t index)
        {
            return std::string(prefix) + "-" + std::to_string(index);
        }

        std::string MakeUniquePayload(const std::size_t index)
        {
            return test::MakeChunkPayload(256 + (index % 17) * 23,
                                          "unique-payload-" + std::to_string(index));
        }

        ListedChunksResult CollectAllPages(LocalDiskChunkStore &store,
                                           ListChunksOptions options,
                                           const std::string_view request_prefix)
        {
            ListedChunksResult result;
            std::string page_token;

            for (std::size_t page_index = 0; page_index < 1024; ++page_index)
            {
                options.page_token = page_token;
                const auto response = store.ListChunks(MakeListRequest(
                    MakeRequestId(std::string(request_prefix), page_index),
                    options));
                if (!response.ok())
                {
                    result.status = response.status;
                    result.error_detail = response.error_detail;
                    return result;
                }

                result.chunks.insert(result.chunks.end(),
                                     response.chunks.begin(),
                                     response.chunks.end());
                if (response.next_page_token.empty())
                {
                    return result;
                }

                if (response.next_page_token == page_token)
                {
                    result.status = StorageNodeStatusCode::kConflict;
                    result.error_detail =
                        "ListChunks returned a repeated page token during T026 pagination";
                    return result;
                }

                page_token = response.next_page_token;
            }

            result.status = StorageNodeStatusCode::kConflict;
            result.error_detail = "ListChunks pagination exceeded the T026 safety bound";
            return result;
        }

        std::size_t CountRegularFilesRecursively(const std::filesystem::path &root)
        {
            std::error_code ec;
            if (!std::filesystem::exists(root, ec))
            {
                return 0;
            }

            std::size_t count = 0;
            for (const auto &entry :
                 std::filesystem::recursive_directory_iterator(root))
            {
                if (entry.is_regular_file())
                {
                    ++count;
                }
            }
            return count;
        }

        bool IsOneOf(const StorageNodeStatusCode status,
                     const std::initializer_list<StorageNodeStatusCode> expected)
        {
            return std::find(expected.begin(), expected.end(), status) != expected.end();
        }

        bool SubmitWithBackpressure(BoundedStorageExecutor &executor,
                                    const std::string &task_name,
                                    std::function<void()> task,
                                    std::atomic<std::size_t> &overloaded_submissions,
                                    std::atomic<std::size_t> &max_queue_depth,
                                    std::string *error_detail)
        {
            const auto deadline = std::chrono::steady_clock::now() + kSubmitDeadline;
            for (;;)
            {
                const auto result = executor.Submit(StorageExecutorSubmitRequest{
                    .task_name = task_name,
                    .context = {.timeout_ms = 3000, .best_effort_cancel = false},
                    .task = task});
                max_queue_depth.store(
                    std::max(max_queue_depth.load(std::memory_order_relaxed),
                             result.queue_depth),
                    std::memory_order_relaxed);

                if (result.accepted())
                {
                    return true;
                }

                if (result.code != StorageExecutorSubmitCode::kOverloaded)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = "executor submit failed with " +
                                        std::string(ToString(result.code)) + ": " +
                                        result.error_detail;
                    }
                    return false;
                }

                overloaded_submissions.fetch_add(1, std::memory_order_relaxed);
                if (std::chrono::steady_clock::now() >= deadline)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail =
                            "executor remained overloaded past the retry deadline";
                    }
                    return false;
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        class StoreConcurrencyStressTest : public ::testing::Test
        {
        protected:
            static LocalDiskChunkStoreConfig MakeConfig()
            {
                return LocalDiskChunkStoreConfig{
                    .data_dir = T026VisualizedDataDir(),
                    .node_id = test::MakeStorageNodeIdFixture(26)};
            }
        };

        TEST_F(StoreConcurrencyStressTest,
               LocalDiskChunkStoreMixedConcurrencyStressLeavesInspectableNodeDataLayout)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "T026 real local chunk IO stress is Linux-primary in this environment";
#else
            ASSERT_NO_THROW(ResetT026VisualizedDataDir());
            LocalDiskChunkStore store(MakeConfig());
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            std::vector<ChunkIdentity> unique_identities;
            std::vector<std::string> unique_payloads;
            unique_identities.reserve(kUniqueChunkCount);
            unique_payloads.reserve(kUniqueChunkCount);
            for (std::size_t index = 0; index < kUniqueChunkCount; ++index)
            {
                unique_identities.push_back(MakeIdentityOrThrow(
                    "t026-unique-object-" + std::to_string(index),
                    1,
                    static_cast<std::uint32_t>(index),
                    static_cast<std::uint64_t>(index * 4096)));
                unique_payloads.push_back(MakeUniquePayload(index));
            }

            std::vector<WriteChunkResponse> unique_write_responses(kUniqueChunkCount);
            {
                std::barrier start_barrier(static_cast<std::ptrdiff_t>(kUniqueChunkCount));
                std::vector<std::thread> threads;
                threads.reserve(kUniqueChunkCount);
                for (std::size_t index = 0; index < kUniqueChunkCount; ++index)
                {
                    threads.emplace_back(
                        [&store, &start_barrier, &unique_identities, &unique_payloads,
                         &unique_write_responses, index]()
                        {
                            start_barrier.arrive_and_wait();
                            unique_write_responses[index] = store.WriteChunk(
                                MakeWriteRequest(unique_identities[index],
                                                 unique_payloads[index],
                                                 MakeRequestId("t026-unique-write", index)));
                        });
                }
                for (auto &thread : threads)
                {
                    thread.join();
                }
            }

            for (std::size_t index = 0; index < kUniqueChunkCount; ++index)
            {
                SCOPED_TRACE("unique-write-" + std::to_string(index));
                ASSERT_EQ(unique_write_responses[index].status, StorageNodeStatusCode::kOk)
                    << unique_write_responses[index].error_detail;
                EXPECT_TRUE(unique_write_responses[index].durable);
                EXPECT_FALSE(unique_write_responses[index].already_exists);
                EXPECT_EQ(unique_write_responses[index].metadata.state, ChunkState::kLive);
                EXPECT_EQ(unique_write_responses[index].metadata.size,
                          unique_payloads[index].size());
            }

            EXPECT_EQ(CountRegularFilesRecursively(store.paths().live_root), kUniqueChunkCount);
            EXPECT_EQ(CountRegularFilesRecursively(store.paths().staging_root), 0U);

            constexpr std::size_t kReadThreadCount = kUniqueChunkCount / 2;
            constexpr std::size_t kStatThreadCount = kUniqueChunkCount / 2;
            std::vector<ReadChunkResponse> read_phase_responses(kReadThreadCount);
            std::vector<StatChunkResponse> stat_phase_responses(kStatThreadCount);
            std::vector<ListedChunksResult> list_phase_results(kConcurrentListProbeCount);

            {
                const std::size_t total_threads =
                    kReadThreadCount + kStatThreadCount + kConcurrentListProbeCount;
                std::barrier start_barrier(static_cast<std::ptrdiff_t>(total_threads));
                std::vector<std::thread> threads;
                threads.reserve(total_threads);

                for (std::size_t index = 0; index < kUniqueChunkCount; ++index)
                {
                    threads.emplace_back(
                        [&store, &start_barrier, &unique_identities, &read_phase_responses,
                         &stat_phase_responses, index]()
                        {
                            start_barrier.arrive_and_wait();
                            if ((index % 2) == 0)
                            {
                                read_phase_responses[index / 2] = store.ReadChunk(
                                    MakeReadRequest(unique_identities[index].chunk_id,
                                                    MakeRequestId("t026-read-phase", index)));
                                return;
                            }

                            stat_phase_responses[index / 2] = store.StatChunk(
                                MakeStatRequest(unique_identities[index].chunk_id,
                                                MakeRequestId("t026-stat-phase", index),
                                                true));
                        });
                }

                for (std::size_t index = 0; index < kConcurrentListProbeCount; ++index)
                {
                    threads.emplace_back(
                        [&store, &start_barrier, &list_phase_results, index]()
                        {
                            start_barrier.arrive_and_wait();
                            list_phase_results[index] = CollectAllPages(
                                store,
                                ListChunksOptions{
                                    .state_filter = ChunkState::kLive,
                                    .page_size = 17,
                                },
                                "t026-list-phase");
                        });
                }

                for (auto &thread : threads)
                {
                    thread.join();
                }
            }

            for (std::size_t index = 0; index < kReadThreadCount; ++index)
            {
                const std::size_t chunk_index = index * 2;
                SCOPED_TRACE("read-phase-" + std::to_string(chunk_index));
                ASSERT_EQ(read_phase_responses[index].status, StorageNodeStatusCode::kOk)
                    << read_phase_responses[index].error_detail;
                EXPECT_EQ(read_phase_responses[index].payload, unique_payloads[chunk_index]);
                EXPECT_TRUE(read_phase_responses[index].verified);
            }

            for (std::size_t index = 0; index < kStatThreadCount; ++index)
            {
                const std::size_t chunk_index = index * 2 + 1;
                SCOPED_TRACE("stat-phase-" + std::to_string(chunk_index));
                ASSERT_EQ(stat_phase_responses[index].status, StorageNodeStatusCode::kOk)
                    << stat_phase_responses[index].error_detail;
                EXPECT_EQ(stat_phase_responses[index].metadata.state, ChunkState::kLive);
                EXPECT_TRUE(stat_phase_responses[index].verified);
                EXPECT_EQ(stat_phase_responses[index].metadata.size,
                          unique_payloads[chunk_index].size());
            }

            for (std::size_t index = 0; index < kConcurrentListProbeCount; ++index)
            {
                SCOPED_TRACE("list-phase-" + std::to_string(index));
                ASSERT_EQ(list_phase_results[index].status, StorageNodeStatusCode::kOk)
                    << list_phase_results[index].error_detail;
                EXPECT_EQ(list_phase_results[index].chunks.size(), kUniqueChunkCount);
                for (const auto &metadata : list_phase_results[index].chunks)
                {
                    EXPECT_EQ(metadata.state, ChunkState::kLive);
                }
            }

            const auto same_chunk_identity =
                MakeIdentityOrThrow("t026-conflict-object", 1, 0, 0);
            const std::string same_chunk_payload_a =
                test::MakeChunkPayload(1024, "same-chunk-a");
            const std::string same_chunk_payload_b =
                test::MakeChunkPayload(1024, "same-chunk-b");
            const auto checksum_a = ComputeChecksumOrThrow(same_chunk_payload_a);
            const auto checksum_b = ComputeChecksumOrThrow(same_chunk_payload_b);

            std::vector<WriteChunkResponse> same_chunk_responses(
                kSameChunkWriterCountPerPayload * 2);
            {
                const std::size_t total_writers = same_chunk_responses.size();
                std::barrier start_barrier(static_cast<std::ptrdiff_t>(total_writers));
                std::vector<std::thread> threads;
                threads.reserve(total_writers);

                for (std::size_t index = 0; index < total_writers; ++index)
                {
                    threads.emplace_back(
                        [&store, &start_barrier, &same_chunk_identity, &same_chunk_payload_a,
                         &same_chunk_payload_b, &same_chunk_responses, index]()
                        {
                            start_barrier.arrive_and_wait();
                            const bool use_payload_a =
                                index < kSameChunkWriterCountPerPayload;
                            same_chunk_responses[index] = store.WriteChunk(
                                MakeWriteRequest(same_chunk_identity,
                                                 use_payload_a ? same_chunk_payload_a
                                                               : same_chunk_payload_b,
                                                 MakeRequestId("t026-same-chunk", index)));
                        });
                }

                for (auto &thread : threads)
                {
                    thread.join();
                }
            }

            const auto same_chunk_read =
                store.ReadChunk(MakeReadRequest(same_chunk_identity.chunk_id,
                                               "t026-same-chunk-final-read"));
            ASSERT_EQ(same_chunk_read.status, StorageNodeStatusCode::kOk)
                << same_chunk_read.error_detail;
            const bool payload_a_won = same_chunk_read.payload == same_chunk_payload_a;
            ASSERT_TRUE(payload_a_won || same_chunk_read.payload == same_chunk_payload_b);

            std::size_t accepted_writers = 0;
            std::size_t conflict_writers = 0;
            std::size_t non_already_exists_successes = 0;
            for (std::size_t index = 0; index < same_chunk_responses.size(); ++index)
            {
                const bool attempted_payload_a =
                    index < kSameChunkWriterCountPerPayload;
                const bool winner_attempt = attempted_payload_a == payload_a_won;
                SCOPED_TRACE("same-chunk-write-" + std::to_string(index));

                if (winner_attempt)
                {
                    ASSERT_EQ(same_chunk_responses[index].status, StorageNodeStatusCode::kOk)
                        << same_chunk_responses[index].error_detail;
                    ++accepted_writers;
                    if (!same_chunk_responses[index].already_exists)
                    {
                        ++non_already_exists_successes;
                    }
                    EXPECT_EQ(same_chunk_responses[index].metadata.state, ChunkState::kLive);
                    EXPECT_EQ(same_chunk_responses[index].metadata.checksum.value,
                              payload_a_won ? checksum_a.value : checksum_b.value);
                }
                else
                {
                    EXPECT_EQ(same_chunk_responses[index].status, StorageNodeStatusCode::kConflict)
                        << same_chunk_responses[index].error_detail;
                    ++conflict_writers;
                }
            }

            EXPECT_EQ(accepted_writers, kSameChunkWriterCountPerPayload);
            EXPECT_EQ(conflict_writers, kSameChunkWriterCountPerPayload);
            EXPECT_EQ(non_already_exists_successes, 1U);
            EXPECT_EQ(same_chunk_read.actual_checksum.value,
                      payload_a_won ? checksum_a.value : checksum_b.value);

            BoundedStorageExecutor executor(StorageExecutorConfig{
                .worker_count = kExecutorWorkerCount,
                .queue_capacity = kExecutorQueueCapacity});
            std::atomic<std::size_t> overloaded_submissions{0};
            std::atomic<std::size_t> max_queue_depth{0};

            std::vector<DeleteChunkResponse> delete_responses(kDeleteChunkCount);
            std::vector<ReadChunkResponse> mixed_read_responses(kUniqueChunkCount);
            std::vector<StatChunkResponse> mixed_stat_responses(kDeleteChunkCount);
            std::vector<ListChunksResponse> mixed_list_responses(kConcurrentListProbeCount);

            const std::size_t total_mixed_tasks =
                kDeleteChunkCount + kUniqueChunkCount + kDeleteChunkCount +
                kConcurrentListProbeCount;
            std::latch done(static_cast<std::ptrdiff_t>(total_mixed_tasks));
            std::string submit_error_detail;

            for (std::size_t index = 0; index < kDeleteChunkCount; ++index)
            {
                ASSERT_TRUE(SubmitWithBackpressure(
                    executor,
                    MakeRequestId("t026-delete-task", index),
                    [&store, &unique_identities, &delete_responses, &done, index]()
                    {
                        delete_responses[index] = store.DeleteChunk(
                            MakeDeleteRequest(unique_identities[index].chunk_id,
                                              MakeRequestId("t026-delete", index)));
                        done.count_down();
                    },
                    overloaded_submissions,
                    max_queue_depth,
                    &submit_error_detail))
                    << submit_error_detail;
            }

            for (std::size_t index = 0; index < kUniqueChunkCount; ++index)
            {
                ASSERT_TRUE(SubmitWithBackpressure(
                    executor,
                    MakeRequestId("t026-read-delete-race-task", index),
                    [&store, &unique_identities, &mixed_read_responses, &done, index]()
                    {
                        mixed_read_responses[index] = store.ReadChunk(
                            MakeReadRequest(unique_identities[index].chunk_id,
                                            MakeRequestId("t026-read-delete-race", index)));
                        done.count_down();
                    },
                    overloaded_submissions,
                    max_queue_depth,
                    &submit_error_detail))
                    << submit_error_detail;
            }

            for (std::size_t index = 0; index < kDeleteChunkCount; ++index)
            {
                ASSERT_TRUE(SubmitWithBackpressure(
                    executor,
                    MakeRequestId("t026-stat-delete-race-task", index),
                    [&store, &unique_identities, &mixed_stat_responses, &done, index]()
                    {
                        mixed_stat_responses[index] = store.StatChunk(
                            MakeStatRequest(unique_identities[index].chunk_id,
                                            MakeRequestId("t026-stat-delete-race", index),
                                            false));
                        done.count_down();
                    },
                    overloaded_submissions,
                    max_queue_depth,
                    &submit_error_detail))
                    << submit_error_detail;
            }

            for (std::size_t index = 0; index < kConcurrentListProbeCount; ++index)
            {
                ASSERT_TRUE(SubmitWithBackpressure(
                    executor,
                    MakeRequestId("t026-list-delete-race-task", index),
                    [&store, &mixed_list_responses, &done, index]()
                    {
                        mixed_list_responses[index] = store.ListChunks(
                            MakeListRequest(
                                MakeRequestId("t026-list-delete-race", index),
                                ListChunksOptions{
                                    .page_size = kUniqueChunkCount + 16,
                                }));
                        done.count_down();
                    },
                    overloaded_submissions,
                    max_queue_depth,
                    &submit_error_detail))
                    << submit_error_detail;
            }

            done.wait();
            const auto shutdown_result = executor.Shutdown(StorageExecutorShutdownRequest{
                .mode = StorageExecutorStopMode::kDrain});
            EXPECT_TRUE(shutdown_result.stopped);
            EXPECT_TRUE(shutdown_result.drained);
            EXPECT_LE(max_queue_depth.load(std::memory_order_relaxed),
                      kExecutorQueueCapacity);
            EXPECT_GT(overloaded_submissions.load(std::memory_order_relaxed), 0U);
            EXPECT_GT(shutdown_result.stats.rejected_tasks, 0U);

            for (std::size_t index = 0; index < kDeleteChunkCount; ++index)
            {
                SCOPED_TRACE("delete-race-delete-" + std::to_string(index));
                EXPECT_EQ(delete_responses[index].status, StorageNodeStatusCode::kOk)
                    << delete_responses[index].error_detail;
                EXPECT_TRUE(delete_responses[index].deleted);
                EXPECT_FALSE(delete_responses[index].already_missing);
                EXPECT_EQ(delete_responses[index].metadata.state, ChunkState::kDeleted);
            }

            for (std::size_t index = 0; index < kUniqueChunkCount; ++index)
            {
                SCOPED_TRACE("delete-race-read-" + std::to_string(index));
                if (index < kDeleteChunkCount)
                {
                    EXPECT_TRUE(IsOneOf(mixed_read_responses[index].status,
                                        {StorageNodeStatusCode::kOk,
                                         StorageNodeStatusCode::kNotFound,
                                         StorageNodeStatusCode::kIoError}))
                        << mixed_read_responses[index].error_detail;
                    if (mixed_read_responses[index].ok())
                    {
                        EXPECT_EQ(mixed_read_responses[index].payload,
                                  unique_payloads[index]);
                    }
                }
                else
                {
                    ASSERT_EQ(mixed_read_responses[index].status, StorageNodeStatusCode::kOk)
                        << mixed_read_responses[index].error_detail;
                    EXPECT_EQ(mixed_read_responses[index].payload,
                              unique_payloads[index]);
                }
            }

            for (std::size_t index = 0; index < kDeleteChunkCount; ++index)
            {
                SCOPED_TRACE("delete-race-stat-" + std::to_string(index));
                ASSERT_EQ(mixed_stat_responses[index].status, StorageNodeStatusCode::kOk)
                    << mixed_stat_responses[index].error_detail;
                EXPECT_TRUE(mixed_stat_responses[index].metadata.state == ChunkState::kLive ||
                            mixed_stat_responses[index].metadata.state == ChunkState::kDeleted);
            }

            const std::size_t expected_total_entries = kUniqueChunkCount + 1;
            for (std::size_t index = 0; index < kConcurrentListProbeCount; ++index)
            {
                SCOPED_TRACE("delete-race-list-" + std::to_string(index));
                ASSERT_EQ(mixed_list_responses[index].status, StorageNodeStatusCode::kOk)
                    << mixed_list_responses[index].error_detail;
                EXPECT_EQ(mixed_list_responses[index].chunks.size(), expected_total_entries);
                EXPECT_TRUE(mixed_list_responses[index].next_page_token.empty());
                for (const auto &metadata : mixed_list_responses[index].chunks)
                {
                    EXPECT_TRUE(metadata.state == ChunkState::kLive ||
                                metadata.state == ChunkState::kDeleted);
                }
            }

            for (std::size_t index = 0; index < kDeleteChunkCount; ++index)
            {
                const auto final_read = store.ReadChunk(
                    MakeReadRequest(unique_identities[index].chunk_id,
                                    MakeRequestId("t026-final-deleted-read", index)));
                EXPECT_EQ(final_read.status, StorageNodeStatusCode::kNotFound)
                    << final_read.error_detail;

                const auto final_stat = store.StatChunk(
                    MakeStatRequest(unique_identities[index].chunk_id,
                                    MakeRequestId("t026-final-deleted-stat", index),
                                    false));
                ASSERT_EQ(final_stat.status, StorageNodeStatusCode::kOk)
                    << final_stat.error_detail;
                EXPECT_EQ(final_stat.metadata.state, ChunkState::kDeleted);
            }

            for (std::size_t index = kDeleteChunkCount; index < kUniqueChunkCount; ++index)
            {
                const auto final_read = store.ReadChunk(
                    MakeReadRequest(unique_identities[index].chunk_id,
                                    MakeRequestId("t026-final-live-read", index)));
                ASSERT_EQ(final_read.status, StorageNodeStatusCode::kOk)
                    << final_read.error_detail;
                EXPECT_EQ(final_read.payload, unique_payloads[index]);
            }

            const auto final_live_chunks = CollectAllPages(
                store,
                ListChunksOptions{
                    .state_filter = ChunkState::kLive,
                    .page_size = kFinalListPageSize,
                },
                "t026-final-live-list");
            ASSERT_EQ(final_live_chunks.status, StorageNodeStatusCode::kOk)
                << final_live_chunks.error_detail;

            const auto final_deleted_chunks = CollectAllPages(
                store,
                ListChunksOptions{
                    .state_filter = ChunkState::kDeleted,
                    .page_size = kFinalListPageSize,
                },
                "t026-final-deleted-list");
            ASSERT_EQ(final_deleted_chunks.status, StorageNodeStatusCode::kOk)
                << final_deleted_chunks.error_detail;

            const auto final_all_chunks = CollectAllPages(
                store,
                ListChunksOptions{
                    .page_size = kFinalListPageSize,
                },
                "t026-final-all-list");
            ASSERT_EQ(final_all_chunks.status, StorageNodeStatusCode::kOk)
                << final_all_chunks.error_detail;

            EXPECT_EQ(final_live_chunks.chunks.size(),
                      kUniqueChunkCount - kDeleteChunkCount + 1);
            EXPECT_EQ(final_deleted_chunks.chunks.size(), kDeleteChunkCount);
            EXPECT_EQ(final_all_chunks.chunks.size(), expected_total_entries);

            for (const auto &metadata : final_live_chunks.chunks)
            {
                EXPECT_EQ(metadata.state, ChunkState::kLive);
            }
            for (const auto &metadata : final_deleted_chunks.chunks)
            {
                EXPECT_EQ(metadata.state, ChunkState::kDeleted);
            }

            EXPECT_EQ(CountRegularFilesRecursively(store.paths().live_root),
                      kUniqueChunkCount - kDeleteChunkCount + 1);
            EXPECT_EQ(CountRegularFilesRecursively(store.paths().staging_root), 0U);
            EXPECT_TRUE(std::filesystem::exists(T026VisualizedDataDir() / "chunks" / "live"));
            EXPECT_TRUE(std::filesystem::exists(T026VisualizedDataDir() / "chunks" / "staging"));
#endif
        }
    } // namespace
} // namespace storedemo
