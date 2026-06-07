#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/maintenance/garbage_collector.h"
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

    storedemo::UploadCoordinatorRequest MakeChunkedRequest(
        const std::vector<std::string> &payloads)
    {
        storedemo::UploadCoordinatorRequest request;
        request.request_id = "upload-t025";
        request.bucket = "bucket-t025";
        request.object_key = "objects/streaming-boundary.bin";
        request.object_id = "obj-t025";
        request.version = 2;
        request.replica_policy = MakeReplicaPolicy(2, 2);
        request.candidates = MakeCandidates();
        request.client_time_unix_ms = 1712002500;

        std::uint64_t offset = 0;
        for (std::size_t index = 0; index < payloads.size(); ++index)
        {
            request.chunks.push_back(storedemo::UploadChunkInput{
                .chunk_index = static_cast<std::uint32_t>(index),
                .offset = offset,
                .payload = payloads[index]});
            offset += static_cast<std::uint64_t>(payloads[index].size());
        }

        return request;
    }

    std::string JoinPayloads(const std::vector<std::string> &payloads)
    {
        std::string joined;
        for (const auto &payload : payloads)
        {
            joined += payload;
        }
        return joined;
    }

    storedemo::UploadCoordinatorRequest MakeRequestWithPolicy(
        const std::string &payload,
        const std::size_t replica_count,
        const std::size_t minimum_successful_writes)
    {
        auto request = MakeRequest(payload);
        request.replica_policy =
            MakeReplicaPolicy(replica_count, minimum_successful_writes);
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
        EXPECT_FALSE(metadata_client_->last_commit_request().has_value());

        const auto head = machine_.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);

        const auto list = machine_.ListObjects(
            {.bucket = request.bucket, .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        ASSERT_EQ(result.committed_chunks.size(), 0U);
        ASSERT_EQ(result.cleanup_candidates.size(), 1U);
        ASSERT_EQ(result.chunk_executions.size(), 1U);
        const auto &execution = result.chunk_executions.front();
        EXPECT_EQ(execution.durable_success_count, 1U);
        ASSERT_EQ(execution.replica_results.size(), 2U);
        EXPECT_EQ(execution.replica_results[1].status,
                  storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_EQ(result.cleanup_candidates.front().chunk.identity.chunk_id,
                  execution.identity.chunk_id);
        EXPECT_EQ(result.cleanup_candidates.front().chunk.replica_nodes.size(), 1U);
        EXPECT_EQ(result.cleanup_candidates.front().chunk.replica_nodes.front(),
                  store1->config().node_id);

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
        ASSERT_EQ(result.cleanup_candidates.size(), 1U);
        EXPECT_EQ(result.cleanup_candidates.front().chunk.identity.chunk_id,
                  result.committed_chunks.front().identity.chunk_id);
        EXPECT_EQ(result.cleanup_candidates.front().chunk.replica_nodes,
                  result.committed_chunks.front().replica_nodes);
    }

    TEST_F(StorageUploadCoordinatorTest,
           UploadStreamingChecksumFactsPropagateWithoutPayloadEnteringMetadata)
    {
        const std::vector<std::string> payloads = {
            "streaming-boundary-alpha-0123456789",
            "streaming-boundary-beta-abcdefghijklmnopqrstuvwxyz"};
        const auto full_payload = JoinPayloads(payloads);
        const auto object_checksum =
            storedemo::test::ComputeStoreChecksumOrThrow(full_payload);

        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_upload_coordinator_streaming_checksum");
        auto store1 = MakeStore(temp_dir.Path("stores"), 1);
        auto store2 = MakeStore(temp_dir.Path("stores"), 2);

        storedemo::UploadCoordinator coordinator(metadata_client_, chunk_writer_);
        auto request = MakeChunkedRequest(payloads);
        request.request_id = "upload-t025-streaming";
        request.bucket = "bucket-t025-streaming";
        request.object_key = "objects/streaming-boundary.bin";
        request.object_id = "obj-t025-streaming";
        request.object_checksum = storedemo::UploadObjectChecksumFacts{
            .size = static_cast<std::uint64_t>(full_payload.size()),
            .checksum = object_checksum,
            .etag = "etag-from-object-checksum"};
        request.etag = "legacy-etag-should-not-win";

        const auto result = coordinator.UploadObject(request);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_TRUE(metadata_client_->last_create_request().has_value());
        ASSERT_TRUE(metadata_client_->last_commit_request().has_value());

        const auto &create_request = *metadata_client_->last_create_request();
        const auto &commit_request = *metadata_client_->last_commit_request();
        EXPECT_EQ(create_request.size,
                  static_cast<std::uint64_t>(full_payload.size()));
        EXPECT_EQ(commit_request.size,
                  static_cast<std::uint64_t>(full_payload.size()));
        EXPECT_EQ(create_request.etag, "etag-from-object-checksum");
        EXPECT_EQ(commit_request.etag, "etag-from-object-checksum");
        ASSERT_EQ(commit_request.chunks.size(), payloads.size());

        ASSERT_EQ(chunk_writer_->history_for(store1->config().node_id).size(), payloads.size());
        ASSERT_EQ(chunk_writer_->history_for(store2->config().node_id).size(), payloads.size());
        EXPECT_EQ(chunk_writer_->history_for(store1->config().node_id)[0].payload,
                  payloads[0]);
        EXPECT_EQ(chunk_writer_->history_for(store1->config().node_id)[1].payload,
                  payloads[1]);
        EXPECT_NE(chunk_writer_->history_for(store1->config().node_id)[0].payload,
                  full_payload);
        EXPECT_NE(chunk_writer_->history_for(store1->config().node_id)[1].payload,
                  full_payload);

        const auto serialized_create = raftdemo::SerializeMetadataCommand(
            storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                create_request.bucket,
                create_request.object_key,
                create_request.object_id,
                create_request.version,
                create_request.request_id,
                create_request.size,
                create_request.etag,
                create_request.client_time_unix_ms));
        const auto serialized_commit = raftdemo::SerializeMetadataCommand(
            storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                commit_request.bucket,
                commit_request.object_key,
                commit_request.object_id,
                commit_request.version,
                commit_request.request_id,
                commit_request.size,
                commit_request.etag,
                {
                    raftdemo::ChunkRef{
                        .chunk_id = commit_request.chunks[0].identity.chunk_id,
                        .offset = commit_request.chunks[0].offset,
                        .size = commit_request.chunks[0].size,
                        .replica_nodes = commit_request.chunks[0].replica_nodes,
                        .checksum = commit_request.chunks[0].checksum.value},
                    raftdemo::ChunkRef{
                        .chunk_id = commit_request.chunks[1].identity.chunk_id,
                        .offset = commit_request.chunks[1].offset,
                        .size = commit_request.chunks[1].size,
                        .replica_nodes = commit_request.chunks[1].replica_nodes,
                        .checksum = commit_request.chunks[1].checksum.value}},
                commit_request.client_time_unix_ms));
        EXPECT_EQ(serialized_create.find(payloads[0]), std::string::npos);
        EXPECT_EQ(serialized_create.find(payloads[1]), std::string::npos);
        EXPECT_EQ(serialized_commit.find(payloads[0]), std::string::npos);
        EXPECT_EQ(serialized_commit.find(payloads[1]), std::string::npos);
        EXPECT_EQ(serialized_commit.find(full_payload), std::string::npos);
    }

    TEST_F(StorageUploadCoordinatorTest,
           UploadWithoutExplicitEtagUsesStreamingObjectChecksumAsMetadataFact)
    {
        const std::vector<std::string> payloads = {
            "chunk-a-bounded-checksum",
            "chunk-b-bounded-checksum",
            "chunk-c-bounded-checksum"};
        const auto full_payload = JoinPayloads(payloads);
        const auto expected_checksum =
            storedemo::test::ComputeStoreChecksumOrThrow(full_payload);

        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_upload_coordinator_computed_etag");
        auto store1 = MakeStore(temp_dir.Path("stores"), 1);
        auto store2 = MakeStore(temp_dir.Path("stores"), 2);
        (void)store1;
        (void)store2;

        storedemo::UploadCoordinator coordinator(metadata_client_, chunk_writer_);
        auto request = MakeChunkedRequest(payloads);
        request.request_id = "upload-t025-computed-etag";
        request.bucket = "bucket-t025-computed-etag";
        request.object_key = "objects/computed-etag.bin";
        request.object_id = "obj-t025-computed-etag";

        const auto result = coordinator.UploadObject(request);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_TRUE(metadata_client_->last_create_request().has_value());
        ASSERT_TRUE(metadata_client_->last_commit_request().has_value());
        EXPECT_EQ(metadata_client_->last_create_request()->etag,
                  expected_checksum.value);
        EXPECT_EQ(metadata_client_->last_commit_request()->etag,
                  expected_checksum.value);
        EXPECT_EQ(metadata_client_->last_create_request()->size,
                  static_cast<std::uint64_t>(full_payload.size()));
        EXPECT_EQ(metadata_client_->last_commit_request()->size,
                  static_cast<std::uint64_t>(full_payload.size()));

        const auto head = machine_.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(head.record.has_value());
        EXPECT_EQ(head.record->etag, expected_checksum.value);
        EXPECT_EQ(head.record->size, full_payload.size());
        ASSERT_EQ(head.record->chunks.size(), payloads.size());
    }

    TEST_F(StorageUploadCoordinatorTest,
           ObjectChecksumMismatchFailsBeforeMetadataCreateOrChunkWrite)
    {
        const std::vector<std::string> payloads = {
            "checksum-mismatch-alpha",
            "checksum-mismatch-beta"};
        const auto full_payload = JoinPayloads(payloads);
        auto wrong_checksum =
            storedemo::test::ComputeStoreChecksumOrThrow(full_payload);
        wrong_checksum.value.front() =
            wrong_checksum.value.front() == '0' ? '1' : '0';

        storedemo::UploadCoordinator coordinator(metadata_client_, chunk_writer_);
        auto request = MakeChunkedRequest(payloads);
        request.request_id = "upload-t025-mismatch";
        request.bucket = "bucket-t025-mismatch";
        request.object_key = "objects/checksum-mismatch.bin";
        request.object_id = "obj-t025-mismatch";
        request.object_checksum = storedemo::UploadObjectChecksumFacts{
            .size = static_cast<std::uint64_t>(full_payload.size()),
            .checksum = wrong_checksum,
            .etag = "etag-ignored-on-mismatch"};

        const auto result = coordinator.UploadObject(request);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_EQ(result.error_detail, "object checksum mismatch");
        EXPECT_EQ(metadata_client_->create_calls(), 0U);
        EXPECT_EQ(metadata_client_->commit_calls(), 0U);
        EXPECT_EQ(chunk_writer_->write_calls(), 0U);
        EXPECT_FALSE(metadata_client_->last_create_request().has_value());
        EXPECT_FALSE(metadata_client_->last_commit_request().has_value());
    }

    TEST_F(StorageUploadCoordinatorTest,
           ObjectChecksumSizeMismatchFailsBeforeMetadataCreateOrChunkWrite)
    {
        const std::vector<std::string> payloads = {
            "checksum-size-alpha",
            "checksum-size-beta"};
        const auto full_payload = JoinPayloads(payloads);
        auto object_checksum =
            storedemo::test::ComputeStoreChecksumOrThrow(full_payload);

        storedemo::UploadCoordinator coordinator(metadata_client_, chunk_writer_);
        auto request = MakeChunkedRequest(payloads);
        request.request_id = "upload-t025-size-mismatch";
        request.bucket = "bucket-t025-size-mismatch";
        request.object_key = "objects/checksum-size-mismatch.bin";
        request.object_id = "obj-t025-size-mismatch";
        request.object_checksum = storedemo::UploadObjectChecksumFacts{
            .size = static_cast<std::uint64_t>(full_payload.size() + 1U),
            .checksum = object_checksum,
            .etag = "etag-ignored-on-size-mismatch"};

        const auto result = coordinator.UploadObject(request);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kInvalidArgument);
        EXPECT_EQ(result.error_detail,
                  "object_checksum.size must match summed chunk payload size");
        EXPECT_EQ(metadata_client_->create_calls(), 0U);
        EXPECT_EQ(metadata_client_->commit_calls(), 0U);
        EXPECT_EQ(chunk_writer_->write_calls(), 0U);
        EXPECT_FALSE(metadata_client_->last_create_request().has_value());
        EXPECT_FALSE(metadata_client_->last_commit_request().has_value());
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

    TEST_F(StorageUploadCoordinatorTest,
           PartialReplicaFailureDoesNotCommitAndMarksCleanupCandidate)
    {
        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_upload_coordinator_partial_replica_failure");
        auto store1 = MakeStore(temp_dir.Path("stores"), 1);

        storedemo::WriteChunkResponse forced_overloaded;
        forced_overloaded.status = storedemo::StorageNodeStatusCode::kOverloaded;
        forced_overloaded.error_detail = "forced overloaded replica";
        forced_overloaded.retry_after_ms = 15;
        chunk_writer_->ForceResponse(storedemo::test::MakeStorageNodeIdFixture(2),
                                     forced_overloaded);

        storedemo::WriteChunkResponse forced_unavailable;
        forced_unavailable.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
        forced_unavailable.error_detail = "forced unavailable replica";
        chunk_writer_->ForceResponse(storedemo::test::MakeStorageNodeIdFixture(3),
                                     forced_unavailable);

        storedemo::UploadCoordinator coordinator(metadata_client_, chunk_writer_);
        auto request = MakeRequestWithPolicy(fixture.payload, 3, 2);
        request.request_id = "upload-t037";
        request.bucket = "bucket-t037";
        request.object_key = "objects/test_file.deb";
        request.object_id = "obj-t037";

        const auto result = coordinator.UploadObject(request);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kOverloaded);
        EXPECT_TRUE(result.create_succeeded);
        EXPECT_FALSE(result.committed);
        EXPECT_TRUE(result.pending_object_possible);
        EXPECT_TRUE(result.orphan_chunk_possible);
        EXPECT_EQ(metadata_client_->commit_calls(), 0U);
        EXPECT_FALSE(metadata_client_->last_commit_request().has_value());
        EXPECT_TRUE(result.committed_chunks.empty());
        ASSERT_EQ(result.cleanup_candidates.size(), 1U);

        const auto head = machine_.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);

        const auto list = machine_.ListObjects(
            {.bucket = request.bucket, .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        ASSERT_EQ(result.chunk_executions.size(), 1U);
        const auto &execution = result.chunk_executions.front();
        EXPECT_EQ(execution.durable_success_count, 1U);
        EXPECT_FALSE(execution.commit_eligible);
        ASSERT_EQ(execution.replica_results.size(), 3U);
        EXPECT_EQ(execution.replica_results[0].status,
                  storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(execution.replica_results[1].status,
                  storedemo::StorageNodeStatusCode::kOverloaded);
        EXPECT_EQ(execution.replica_results[2].status,
                  storedemo::StorageNodeStatusCode::kNodeUnavailable);

        const auto &cleanup_candidate = result.cleanup_candidates.front();
        EXPECT_EQ(cleanup_candidate.chunk.identity.chunk_id,
                  execution.identity.chunk_id);
        EXPECT_EQ(cleanup_candidate.chunk.identity.object_id, request.object_id);
        EXPECT_EQ(cleanup_candidate.chunk.identity.version, request.version);
        EXPECT_EQ(cleanup_candidate.chunk.identity.chunk_index, 0U);
        EXPECT_EQ(cleanup_candidate.chunk.offset, 0U);
        EXPECT_EQ(cleanup_candidate.chunk.size,
                  static_cast<std::uint64_t>(fixture.payload.size()));
        ASSERT_EQ(cleanup_candidate.chunk.replica_nodes.size(), 1U);
        EXPECT_EQ(cleanup_candidate.chunk.replica_nodes.front(),
                  store1->config().node_id);
        EXPECT_NE(cleanup_candidate.reason.find("cleanup candidate"),
                  std::string::npos);

        const auto read = store1->ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-store-partial-replica-failure",
            .chunk_id = execution.identity.chunk_id});
        ASSERT_EQ(read.status, storedemo::StorageNodeStatusCode::kOk)
            << read.error_detail;
        EXPECT_EQ(read.payload, fixture.payload);
        EXPECT_EQ(cleanup_candidate.chunk.checksum.value,
                  read.metadata.checksum.value);
    }

    TEST_F(StorageUploadCoordinatorTest,
           FailedUploadCleanupFactsCanGenerateGenericCleanupCandidates)
    {
        const auto payload = storedemo::test::MakeChunkPayload(1024, "t056-upload-failed");

        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_upload_coordinator_t056_failed_cleanup");
        auto store1 = MakeStore(temp_dir.Path("stores"), 1);

        storedemo::WriteChunkResponse forced_overloaded;
        forced_overloaded.status = storedemo::StorageNodeStatusCode::kOverloaded;
        forced_overloaded.error_detail = "forced overloaded replica";
        forced_overloaded.retry_after_ms = 25;
        chunk_writer_->ForceResponse(storedemo::test::MakeStorageNodeIdFixture(2),
                                     forced_overloaded);

        storedemo::WriteChunkResponse forced_unavailable;
        forced_unavailable.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
        forced_unavailable.error_detail = "forced unavailable replica";
        chunk_writer_->ForceResponse(storedemo::test::MakeStorageNodeIdFixture(3),
                                     forced_unavailable);

        storedemo::UploadCoordinator coordinator(metadata_client_, chunk_writer_);
        auto request = MakeRequestWithPolicy(payload, 3, 2);
        request.request_id = "upload-t056";
        request.bucket = "bucket-t056-upload";
        request.object_key = "objects/test_file.zip";
        request.object_id = "obj-t056-upload";
        request.version = 4;
        request.client_time_unix_ms = 1712005600;

        const auto result = coordinator.UploadObject(request);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOverloaded);
        ASSERT_TRUE(result.create_succeeded);
        ASSERT_FALSE(result.committed);
        ASSERT_TRUE(result.pending_object_possible);
        ASSERT_TRUE(result.orphan_chunk_possible);
        ASSERT_EQ(result.cleanup_candidates.size(), 1U);

        std::vector<storedemo::CleanupChunkFact> durable_chunks;
        durable_chunks.reserve(result.cleanup_candidates.size());
        for (const auto &cleanup_candidate : result.cleanup_candidates)
        {
            durable_chunks.push_back(storedemo::CleanupChunkFact{
                .identity = cleanup_candidate.chunk.identity,
                .size = cleanup_candidate.chunk.size,
                .checksum = cleanup_candidate.chunk.checksum,
                .replica_nodes = cleanup_candidate.chunk.replica_nodes});
        }

        storedemo::FailedUploadCleanupRequest cleanup_request;
        cleanup_request.bucket = request.bucket;
        cleanup_request.object_key = request.object_key;
        cleanup_request.object_id = request.object_id;
        cleanup_request.version = request.version;
        cleanup_request.object_state = storedemo::CleanupObjectState::kPending;
        cleanup_request.created_at_unix_ms = request.client_time_unix_ms;
        cleanup_request.durable_chunks = std::move(durable_chunks);

        const auto candidates =
            storedemo::BuildFailedUploadCleanupCandidates(cleanup_request);
        ASSERT_EQ(candidates.size(), 1U);
        EXPECT_EQ(candidates.front().source, storedemo::CleanupCandidateSource::kFailedUpload);
        EXPECT_EQ(candidates.front().reason,
                  storedemo::GarbageCollectionReason::kFailedUploadCleanup);
        EXPECT_EQ(candidates.front().identity.chunk_id,
                  result.cleanup_candidates.front().chunk.identity.chunk_id);
        EXPECT_EQ(candidates.front().identity.object_id, request.object_id);
        EXPECT_EQ(candidates.front().identity.version, request.version);
        EXPECT_EQ(candidates.front().replica_nodes,
                  result.cleanup_candidates.front().chunk.replica_nodes);
        EXPECT_NE(candidates.front().metadata_boundary.find("metadata-fact:failed-upload"),
                  std::string::npos);
        EXPECT_NE(candidates.front().metadata_boundary.find("bucket=bucket-t056-upload"),
                  std::string::npos);
        EXPECT_NE(candidates.front().metadata_boundary.find("object=objects/test_file.zip"),
                  std::string::npos);

        const auto task = storedemo::CleanupCandidateToGarbageCollectorTask(
            candidates.front());
        EXPECT_EQ(task.chunk_id, candidates.front().identity.chunk_id);
        EXPECT_EQ(task.object_id, request.object_id);
        EXPECT_EQ(task.version, request.version);
        EXPECT_EQ(task.reason, storedemo::GarbageCollectionReason::kFailedUploadCleanup);
        EXPECT_EQ(task.metadata_boundary, candidates.front().metadata_boundary);

        const auto read = store1->ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-store-t056-failed-upload",
            .chunk_id = task.chunk_id});
        ASSERT_EQ(read.status, storedemo::StorageNodeStatusCode::kOk)
            << read.error_detail;
        EXPECT_EQ(read.payload, payload);
    }
}
