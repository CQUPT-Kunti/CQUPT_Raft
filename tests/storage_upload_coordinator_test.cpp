#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/placement/replica_policy.h"
#include "store/upload/upload_coordinator.h"
#include "support/store_test_utils.h"
#include "support/storage_upload_test_utils.h"

namespace
{
    storedemo::StorageNodePlacementCandidate MakeCandidate(
        const std::size_t index,
        const std::uint64_t available_capacity_bytes,
        const std::uint32_t queued_ops = 0,
        const storedemo::StorageNodeHealth health =
            storedemo::StorageNodeHealth::kHealthy,
        const storedemo::StorageNodeDiskPressure disk_pressure =
            storedemo::StorageNodeDiskPressure::kLow)
    {
        storedemo::StorageNodePlacementCandidate candidate;
        candidate.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        candidate.endpoint = "127.0.0.1:" + std::to_string(7000 + index);
        candidate.health = health;
        candidate.disk_pressure = disk_pressure;
        candidate.total_capacity_bytes = available_capacity_bytes + 8192;
        candidate.used_capacity_bytes = 8192;
        candidate.available_capacity_bytes = available_capacity_bytes;
        candidate.load.queued_ops = queued_ops;
        return candidate;
    }

    storedemo::ReplicaPolicy MakeReplicaPolicy(const std::size_t replica_count,
                                               const std::size_t minimum_successful_writes)
    {
        storedemo::ReplicaPolicy policy;
        policy.replica_count = replica_count;
        policy.minimum_successful_writes = minimum_successful_writes;
        policy.avoid_same_node = true;
        return policy;
    }

    std::vector<storedemo::StorageNodePlacementCandidate> MakeCandidates()
    {
        return {
            MakeCandidate(1, 128ULL * 1024ULL * 1024ULL, 1),
            MakeCandidate(2, 96ULL * 1024ULL * 1024ULL, 2),
            MakeCandidate(3, 64ULL * 1024ULL * 1024ULL, 3)};
    }

    storedemo::UploadCoordinatorRequest MakeRequest(const std::string &payload)
    {
        storedemo::UploadCoordinatorRequest request;
        request.request_id = "upload-t035";
        request.bucket = "bucket-t035";
        request.object_key = "objects/test_file.deb";
        request.object_id = "obj-t035";
        request.version = 1;
        request.replica_policy = MakeReplicaPolicy(2, 2);
        request.candidates = MakeCandidates();
        request.client_time_unix_ms = 1712001000;
        request.chunks.push_back(storedemo::UploadChunkInput{
            .chunk_index = 0,
            .offset = 0,
            .payload = payload});
        return request;
    }

    class StorageUploadCoordinatorTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            metadata_client_ =
                std::make_shared<storedemo::test::InMemoryUploadMetadataClient>(machine_);
            chunk_writer_ =
                std::make_shared<storedemo::test::LocalStoreUploadChunkWriter>();
        }

        std::shared_ptr<storedemo::LocalDiskChunkStore> MakeStore(
            const std::filesystem::path &root,
            const std::size_t node_index)
        {
            auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
                storedemo::test::MakeUploadStoreConfig(root, node_index));
            EXPECT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);
            chunk_writer_->RegisterStore(store->config().node_id, store);
            return store;
        }

        raftdemo::MetadataStateMachine machine_;
        std::shared_ptr<storedemo::test::InMemoryUploadMetadataClient> metadata_client_;
        std::shared_ptr<storedemo::test::LocalStoreUploadChunkWriter> chunk_writer_;
    };

    TEST_F(StorageUploadCoordinatorTest, UploadSuccessCommitsVisibleObject)
    {
        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        storedemo::test::ScopedStoreTestDir temp_dir("storage_upload_coordinator_success");
        auto store1 = MakeStore(temp_dir.Path("stores"), 1);
        auto store2 = MakeStore(temp_dir.Path("stores"), 2);

        storedemo::UploadCoordinator coordinator(metadata_client_, chunk_writer_);
        auto request = MakeRequest(fixture.payload);

        const auto result = coordinator.UploadObject(request);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_TRUE(result.create_succeeded);
        EXPECT_TRUE(result.committed);
        EXPECT_FALSE(result.pending_object_possible);
        EXPECT_FALSE(result.orphan_chunk_possible);
        ASSERT_EQ(result.committed_chunks.size(), 1U);
        EXPECT_EQ(result.committed_chunks.front().replica_nodes.size(), 2U);

        const auto head = machine_.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(head.record.has_value());
        EXPECT_EQ(head.record->object_id, request.object_id);
        EXPECT_EQ(head.record->size, fixture.payload.size());
        ASSERT_EQ(head.record->chunks.size(), 1U);
        EXPECT_EQ(head.record->chunks.front().chunk_id,
                  result.committed_chunks.front().identity.chunk_id);
        EXPECT_EQ(head.record->chunks.front().replica_nodes.size(), 2U);

        const auto list = machine_.ListObjects(
            {.bucket = request.bucket, .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_EQ(list.records.size(), 1U);
        EXPECT_EQ(list.records.front().object_key, request.object_key);

        const auto read1 = store1->ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-store-1",
            .chunk_id = result.committed_chunks.front().identity.chunk_id});
        const auto read2 = store2->ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-store-2",
            .chunk_id = result.committed_chunks.front().identity.chunk_id});
        ASSERT_EQ(read1.status, storedemo::StorageNodeStatusCode::kOk)
            << read1.error_detail;
        ASSERT_EQ(read2.status, storedemo::StorageNodeStatusCode::kOk)
            << read2.error_detail;
        EXPECT_EQ(read1.payload, fixture.payload);
        EXPECT_EQ(read2.payload, fixture.payload);
    }

    TEST_F(StorageUploadCoordinatorTest, WriteFailureDoesNotCommitAndExposesOrphanRisk)
    {
        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();

        storedemo::test::ScopedStoreTestDir temp_dir("storage_upload_coordinator_write_failure");
        auto store1 = MakeStore(temp_dir.Path("stores"), 1);
        auto store2 = MakeStore(temp_dir.Path("stores"), 2);
        (void)store2;

        storedemo::WriteChunkResponse forced_failure;
        forced_failure.status = storedemo::StorageNodeStatusCode::kChecksumMismatch;
        forced_failure.error_detail = "checksum mismatch from forced writer";
        chunk_writer_->ForceResponse(storedemo::test::MakeStorageNodeIdFixture(2),
                                     forced_failure);

        storedemo::UploadCoordinator coordinator(metadata_client_, chunk_writer_);
        auto request = MakeRequest(fixture.payload);

        const auto result = coordinator.UploadObject(request);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_TRUE(result.create_succeeded);
        EXPECT_FALSE(result.committed);
        EXPECT_TRUE(result.pending_object_possible);
        EXPECT_TRUE(result.orphan_chunk_possible);
        EXPECT_EQ(metadata_client_->commit_calls(), 0U);

        const auto head = machine_.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);

        const auto list = machine_.ListObjects(
            {.bucket = request.bucket, .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        ASSERT_EQ(result.committed_chunks.size(), 0U);
        ASSERT_EQ(result.chunk_executions.size(), 1U);
        const auto &execution = result.chunk_executions.front();
        EXPECT_EQ(execution.durable_success_count, 1U);
        ASSERT_EQ(execution.replica_results.size(), 2U);
        EXPECT_EQ(execution.replica_results[1].status,
                  storedemo::StorageNodeStatusCode::kChecksumMismatch);

        const auto read = store1->ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-store-write-failure",
            .chunk_id = execution.identity.chunk_id});
        ASSERT_EQ(read.status, storedemo::StorageNodeStatusCode::kOk)
            << read.error_detail;
        EXPECT_EQ(read.payload, fixture.payload);
    }

    TEST_F(StorageUploadCoordinatorTest, CommitFailureReturnsErrorAndLeavesDurableChunk)
    {
        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();

        storedemo::test::ScopedStoreTestDir temp_dir("storage_upload_coordinator_commit_failure");
        auto store1 = MakeStore(temp_dir.Path("stores"), 1);
        auto store2 = MakeStore(temp_dir.Path("stores"), 2);

        metadata_client_->ForceCommitFailure(storedemo::UploadMetadataResult{
            .status = storedemo::StorageNodeStatusCode::kConflict,
            .error_detail = "forced commit failure"});

        storedemo::UploadCoordinator coordinator(metadata_client_, chunk_writer_);
        auto request = MakeRequest(fixture.payload);

        const auto result = coordinator.UploadObject(request);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kConflict);
        EXPECT_TRUE(result.create_succeeded);
        EXPECT_FALSE(result.committed);
        EXPECT_TRUE(result.pending_object_possible);
        EXPECT_TRUE(result.orphan_chunk_possible);
        EXPECT_EQ(metadata_client_->commit_calls(), 1U);
        ASSERT_EQ(result.committed_chunks.size(), 1U);

        const auto head = machine_.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);

        const auto read1 = store1->ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-store-commit-failure-1",
            .chunk_id = result.committed_chunks.front().identity.chunk_id});
        const auto read2 = store2->ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-store-commit-failure-2",
            .chunk_id = result.committed_chunks.front().identity.chunk_id});
        ASSERT_EQ(read1.status, storedemo::StorageNodeStatusCode::kOk)
            << read1.error_detail;
        ASSERT_EQ(read2.status, storedemo::StorageNodeStatusCode::kOk)
            << read2.error_detail;
        EXPECT_EQ(read1.payload, fixture.payload);
        EXPECT_EQ(read2.payload, fixture.payload);
    }

    TEST_F(StorageUploadCoordinatorTest, PlacementFailureSkipsWritesAndCommit)
    {
        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();

        storedemo::test::ScopedStoreTestDir temp_dir("storage_upload_coordinator_placement_failure");
        auto store1 = MakeStore(temp_dir.Path("stores"), 1);
        (void)store1;

        storedemo::UploadCoordinator coordinator(metadata_client_, chunk_writer_);
        auto request = MakeRequest(fixture.payload);
        request.excluded_nodes = {
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(2)};
        request.candidates[2].health = storedemo::StorageNodeHealth::kUnavailable;

        const auto result = coordinator.UploadObject(request);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_TRUE(result.create_succeeded);
        EXPECT_FALSE(result.committed);
        EXPECT_TRUE(result.pending_object_possible);
        EXPECT_FALSE(result.orphan_chunk_possible);
        EXPECT_EQ(chunk_writer_->write_calls(), 0U);
        EXPECT_EQ(metadata_client_->commit_calls(), 0U);

        const auto head = machine_.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);

        const auto list = machine_.ListObjects(
            {.bucket = request.bucket, .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());
    }
}
