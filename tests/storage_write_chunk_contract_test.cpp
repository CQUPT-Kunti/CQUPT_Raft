#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "raft/common/metadata_command.h"
#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/runtime/storage_executor.h"
#include "support/metadata_test_utils.h"
#include "support/store_test_utils.h"

namespace
{
    using namespace std::chrono_literals;

    struct FixtureBinaryPayload
    {
        std::string payload;
        std::filesystem::path source_path;
        bool used_repo_fixture{false};
    };

    std::filesystem::path RepoRoot()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path().lexically_normal();
    }

    std::filesystem::path T028VisualizedDataDir()
    {
        return RepoRoot() / "node-data" / "t028-write-chunk-contract";
    }

    void ResetT028VisualizedDataDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(T028VisualizedDataDir(), ec);
        ec.clear();
        std::filesystem::create_directories(T028VisualizedDataDir(), ec);
        if (ec)
        {
            throw std::runtime_error("failed to prepare T028 node-data root: " +
                                     ec.message());
        }
    }

    std::size_t CountRegularFilesRecursively(const std::filesystem::path &root)
    {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec))
        {
            return 0;
        }

        std::size_t count = 0;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (entry.is_regular_file())
            {
                ++count;
            }
        }
        return count;
    }

    FixtureBinaryPayload LoadFixtureBinaryPayload()
    {
        const std::filesystem::path repo_root = RepoRoot();
        const std::filesystem::path primary_path =
            repo_root / "tests" / "test_file" / "test_file.deb";
        const std::filesystem::path fallback_path =
            repo_root / "test" / "test_file" / "test_file.deb";

        for (const auto &candidate : {primary_path, fallback_path})
        {
            if (!std::filesystem::exists(candidate))
            {
                continue;
            }

            std::ifstream input(candidate, std::ios::binary);
            if (!input.is_open())
            {
                throw std::runtime_error("failed to open binary fixture: " +
                                         candidate.string());
            }

            return FixtureBinaryPayload{
                .payload = std::string(std::istreambuf_iterator<char>(input),
                                       std::istreambuf_iterator<char>()),
                .source_path = candidate,
                .used_repo_fixture = true};
        }

        std::string payload;
        payload.reserve(4096);
        for (std::size_t index = 0; index < 4096; ++index)
        {
            payload.push_back(static_cast<char>(index % 251));
        }

        return FixtureBinaryPayload{
            .payload = std::move(payload),
            .source_path = {},
            .used_repo_fixture = false};
    }

    storedemo::ChunkChecksum ComputeStoreChecksumOrThrow(const std::string_view payload)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute store checksum: " + error_detail);
        }
        return checksum;
    }

    storedemo::ChunkIdentity MakeStoreIdentityOrThrow(const std::string_view object_id,
                                                      const std::uint64_t version,
                                                      const std::uint32_t chunk_index,
                                                      const std::uint64_t offset = 0)
    {
        storedemo::ChunkId chunk_id;
        std::string error_detail;
        const auto status = storedemo::MakeChunkId(object_id,
                                                   version,
                                                   chunk_index,
                                                   &chunk_id,
                                                   &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to build chunk id: " + error_detail);
        }

        storedemo::ChunkIdentity identity;
        identity.chunk_id = std::move(chunk_id);
        identity.object_id = std::string(object_id);
        identity.version = version;
        identity.chunk_index = chunk_index;
        identity.offset = offset;
        return identity;
    }

    storedemo::WriteChunkRequest MakeWriteRequest(const storedemo::ChunkIdentity &identity,
                                                  const std::string &payload,
                                                  const std::string &request_id)
    {
        return storedemo::WriteChunkRequest{
            .request_id = request_id,
            .identity = identity,
            .expected_size = static_cast<std::uint64_t>(payload.size()),
            .expected_checksum = ComputeStoreChecksumOrThrow(payload),
            .payload = payload};
    }

    storedemo::ReadChunkRequest MakeReadRequest(const storedemo::ChunkId &chunk_id,
                                                const std::string &request_id)
    {
        return storedemo::ReadChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id};
    }

    raftdemo::MetadataCommand MakeCreateObjectCommandWithSize(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id,
        const std::uint64_t size,
        const std::string &etag,
        const std::uint64_t create_time = 1712000001)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateObject;
        command.request_id = request_id;
        command.create_object = raftdemo::CreateObjectCommandPayload{
            raftdemo::ObjectRecord{bucket,
                                   object_key,
                                   object_id,
                                   1,
                                   size,
                                   etag,
                                   raftdemo::ObjectState::PENDING,
                                   {},
                                   create_time,
                                   std::nullopt,
                                   std::nullopt}};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kCreateObject,
            bucket,
            object_key,
            "accepted",
            0,
            create_time,
            std::nullopt};
        return command;
    }

    bool IsConflictLikeStatus(const storedemo::StorageNodeStatusCode status)
    {
        return status == storedemo::StorageNodeStatusCode::kConflict ||
               status == storedemo::StorageNodeStatusCode::kChecksumMismatch;
    }

    bool IsAlreadyExistsLikeResponse(const storedemo::WriteChunkResponse &response)
    {
        return response.already_exists &&
               (response.status == storedemo::StorageNodeStatusCode::kOk ||
                response.status == storedemo::StorageNodeStatusCode::kAlreadyExists);
    }

    class WriteChunkContractAdapter
    {
    public:
        explicit WriteChunkContractAdapter(storedemo::ChunkStore &store,
                                           storedemo::BoundedStorageExecutor *executor = nullptr)
            : store_(store)
            , executor_(executor)
        {
        }

        storedemo::WriteChunkResponse WriteChunk(
            storedemo::WriteChunkRequest request,
            storedemo::StorageTaskContext context = {}) const
        {
            if (executor_ == nullptr)
            {
                return store_.WriteChunk(request);
            }

            auto response_promise =
                std::make_shared<std::promise<storedemo::WriteChunkResponse>>();
            auto response_future = response_promise->get_future();
            auto request_ptr =
                std::make_shared<storedemo::WriteChunkRequest>(std::move(request));
            const auto submit_result = executor_->Submit(
                storedemo::StorageExecutorSubmitRequest{
                    .task_name = "write-chunk-contract",
                    .context = context,
                    .task =
                        [this, request_ptr, response_promise]()
                        {
                            response_promise->set_value(store_.WriteChunk(*request_ptr));
                        }});

            if (!submit_result.accepted())
            {
                storedemo::WriteChunkResponse response;
                response.status = submit_result.status_code();
                response.error_detail = submit_result.error_detail;
                response.retry_after_ms = submit_result.retry_after_ms;
                return response;
            }

            return response_future.get();
        }

    private:
        storedemo::ChunkStore &store_;
        storedemo::BoundedStorageExecutor *executor_;
    };

    class StorageWriteChunkContractTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
#if !defined(__linux__)
            GTEST_SKIP() << "T028 real local WriteChunk contract validation is Linux-primary in this environment";
#else
            ASSERT_NO_THROW(ResetT028VisualizedDataDir());
#endif
        }

        static storedemo::LocalDiskChunkStoreConfig MakeStoreConfig()
        {
            return storedemo::LocalDiskChunkStoreConfig{
                .data_dir = T028VisualizedDataDir(),
                .node_id = storedemo::test::MakeStorageNodeIdFixture(28)};
        }
    };

    TEST_F(StorageWriteChunkContractTest, DurableSuccessKeepsMetadataUncommitted)
    {
        raftdemo::MetadataStateMachine machine;
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t028-durable",
                            "create-bucket-t028-durable"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t028-durable", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(256, "t028-durable");
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t028-durable",
                                                        "uploads/durable",
                                                        identity.object_id,
                                                        "create-object-t028-durable",
                                                        payload.size(),
                                                        "etag-t028-durable"))
                        .Ok);

        WriteChunkContractAdapter adapter(store);
        const auto response = adapter.WriteChunk(
            MakeWriteRequest(identity, payload, "write-t028-durable"));

        ASSERT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk)
            << response.error_detail;
        EXPECT_TRUE(response.durable);
        EXPECT_FALSE(response.already_exists);
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kLive);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t028-durable", .object_key = "uploads/durable"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list = machine.ListObjects(
            {.bucket = "bucket-t028-durable", .prefix = "uploads/"});
        EXPECT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        const auto read_response = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-t028-durable"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, payload);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().live_root), 1U);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().staging_root), 0U);
    }

    TEST_F(StorageWriteChunkContractTest, RequestIdRetrySamePayloadIsSafe)
    {
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t028-request-retry", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(192, "t028-request-retry");
        WriteChunkContractAdapter adapter(store);

        const auto first = adapter.WriteChunk(
            MakeWriteRequest(identity, payload, "request-retry-t028"));
        ASSERT_EQ(first.status, storedemo::StorageNodeStatusCode::kOk)
            << first.error_detail;
        ASSERT_TRUE(first.durable);

        const auto retry = adapter.WriteChunk(
            MakeWriteRequest(identity, payload, "request-retry-t028"));
        EXPECT_TRUE(IsAlreadyExistsLikeResponse(retry))
            << static_cast<int>(retry.status) << " " << retry.error_detail;
        EXPECT_TRUE(retry.durable);

        const auto read_response = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-request-retry-t028"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, payload);
    }

    TEST_F(StorageWriteChunkContractTest, RequestIdRetryDifferentPayloadConflicts)
    {
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity =
            MakeStoreIdentityOrThrow("obj-t028-request-conflict", 1, 0, 0);
        const auto original_payload =
            storedemo::test::MakeChunkPayload(160, "t028-request-conflict-original");
        const auto conflicting_payload =
            storedemo::test::MakeChunkPayload(160, "t028-request-conflict-different");
        WriteChunkContractAdapter adapter(store);

        ASSERT_EQ(adapter.WriteChunk(
                      MakeWriteRequest(identity,
                                       original_payload,
                                       "request-conflict-t028"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        const auto retry = adapter.WriteChunk(
            MakeWriteRequest(identity,
                             conflicting_payload,
                             "request-conflict-t028"));
        EXPECT_TRUE(IsConflictLikeStatus(retry.status))
            << static_cast<int>(retry.status) << " " << retry.error_detail;
        EXPECT_FALSE(retry.durable);

        const auto read_response = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-request-conflict-t028"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, original_payload);
    }

    TEST_F(StorageWriteChunkContractTest, ChecksumMismatchDoesNotWriteLiveChunk)
    {
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t028-checksum", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(128, "t028-checksum");
        auto request = MakeWriteRequest(identity, payload, "checksum-mismatch-t028");
        request.expected_checksum =
            ComputeStoreChecksumOrThrow("t028-checksum-different-payload");

        WriteChunkContractAdapter adapter(store);
        const auto response = adapter.WriteChunk(std::move(request));
        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_FALSE(response.durable);
        EXPECT_FALSE(response.already_exists);

        const auto read_response = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-checksum-mismatch-t028"));
        EXPECT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().live_root), 0U);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().staging_root), 0U);
    }

    TEST_F(StorageWriteChunkContractTest, AlreadyExistsSameContentIsIdempotent)
    {
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t028-already-exists", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(224, "t028-already-exists");
        WriteChunkContractAdapter adapter(store);

        ASSERT_EQ(adapter.WriteChunk(
                      MakeWriteRequest(identity,
                                       payload,
                                       "already-exists-first-t028"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        const auto duplicate = adapter.WriteChunk(
            MakeWriteRequest(identity, payload, "already-exists-second-t028"));
        EXPECT_TRUE(IsAlreadyExistsLikeResponse(duplicate))
            << static_cast<int>(duplicate.status) << " " << duplicate.error_detail;
        EXPECT_TRUE(duplicate.durable);
    }

    TEST_F(StorageWriteChunkContractTest, ConflictDifferentContentDoesNotOverwrite)
    {
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t028-conflict", 1, 0, 0);
        const auto original_payload =
            storedemo::test::MakeChunkPayload(200, "t028-conflict-original");
        const auto conflicting_payload =
            storedemo::test::MakeChunkPayload(200, "t028-conflict-different");
        const auto original_checksum = ComputeStoreChecksumOrThrow(original_payload);
        WriteChunkContractAdapter adapter(store);

        ASSERT_EQ(adapter.WriteChunk(
                      MakeWriteRequest(identity, original_payload, "conflict-first-t028"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        const auto conflict = adapter.WriteChunk(
            MakeWriteRequest(identity, conflicting_payload, "conflict-second-t028"));
        EXPECT_TRUE(IsConflictLikeStatus(conflict.status))
            << static_cast<int>(conflict.status) << " " << conflict.error_detail;
        EXPECT_FALSE(conflict.durable);

        const auto read_response = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-conflict-second-t028"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, original_payload);
        EXPECT_EQ(read_response.metadata.checksum.algorithm, original_checksum.algorithm);
        EXPECT_EQ(read_response.metadata.checksum.value, original_checksum.value);
    }

    TEST_F(StorageWriteChunkContractTest, OverloadedMapsToExplicitStatus)
    {
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        storedemo::BoundedStorageExecutor executor(
            storedemo::StorageExecutorConfig{
                .worker_count = 1,
                .queue_capacity = 1});
        WriteChunkContractAdapter adapter(store, &executor);

        std::promise<void> first_task_started_promise;
        std::future<void> first_task_started = first_task_started_promise.get_future();
        std::promise<void> release_first_task_promise;
        std::shared_future<void> release_first_task =
            release_first_task_promise.get_future().share();

        ASSERT_TRUE(executor.Submit(storedemo::StorageExecutorSubmitRequest{
                        .task_name = "blocking-admission-task",
                        .task =
                            [&]()
                            {
                                first_task_started_promise.set_value();
                                release_first_task.wait();
                            }})
                        .accepted());
        ASSERT_EQ(first_task_started.wait_for(1s), std::future_status::ready);

        ASSERT_TRUE(executor.Submit(storedemo::StorageExecutorSubmitRequest{
                        .task_name = "queued-admission-task",
                        .task = []() {} })
                        .accepted());

        const auto identity = MakeStoreIdentityOrThrow("obj-t028-overloaded", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(96, "t028-overloaded");
        const auto response = adapter.WriteChunk(
            MakeWriteRequest(identity, payload, "overloaded-t028"),
            storedemo::StorageTaskContext{
                .timeout_ms = 3000,
                .best_effort_cancel = false,
            });

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOverloaded);
        EXPECT_FALSE(response.durable);
        EXPECT_EQ(response.retry_after_ms, 1U);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().live_root), 0U);

        release_first_task_promise.set_value();
        const auto shutdown_result = executor.Shutdown(
            storedemo::StorageExecutorShutdownRequest{
                .mode = storedemo::StorageExecutorStopMode::kDrain});
        EXPECT_TRUE(shutdown_result.stopped);
    }

    TEST_F(StorageWriteChunkContractTest, TimeoutOrCancellationCurrentlyIsExplicitBoundary)
    {
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        storedemo::BoundedStorageExecutor executor(
            storedemo::StorageExecutorConfig{
                .worker_count = 1,
                .queue_capacity = 2});
        WriteChunkContractAdapter adapter(store, &executor);

        const auto identity = MakeStoreIdentityOrThrow("obj-t028-timeout-boundary", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(144, "t028-timeout-boundary");
        const auto response = adapter.WriteChunk(
            MakeWriteRequest(identity, payload, "timeout-boundary-t028"),
            storedemo::StorageTaskContext{
                .timeout_ms = 1,
                .best_effort_cancel = true,
            });

        ASSERT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk)
            << response.error_detail;
        EXPECT_TRUE(response.durable);

        const auto read_response = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-timeout-boundary-t028"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, payload);

        const auto shutdown_result = executor.Shutdown(
            storedemo::StorageExecutorShutdownRequest{
                .mode = storedemo::StorageExecutorStopMode::kDrain});
        EXPECT_TRUE(shutdown_result.stopped);
        EXPECT_TRUE(shutdown_result.drained);
    }

    TEST_F(StorageWriteChunkContractTest, BinaryPayloadUsesTestsFixture)
    {
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto fixture = LoadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_FALSE(fixture.payload.empty());

        const auto identity = MakeStoreIdentityOrThrow("obj-t028-binary-fixture", 1, 0, 0);
        WriteChunkContractAdapter adapter(store);
        const auto response = adapter.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "binary-fixture-t028"));

        ASSERT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk)
            << response.error_detail;
        EXPECT_TRUE(response.durable);

        const auto read_response = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-binary-fixture-t028"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, fixture.payload);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().live_root), 1U);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().staging_root), 0U);
    }
} // namespace
