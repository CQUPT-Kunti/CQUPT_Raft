#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/upload/upload_coordinator.h"
#include "support/metadata_test_utils.h"
#include "support/store_test_utils.h"
#include "support/storage_upload_test_utils.h"

namespace raftdemo
{
    std::string SerializeMetadataCommand(const MetadataCommand &command);
} // namespace raftdemo

namespace
{
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

    std::filesystem::path T027VisualizedDataDir()
    {
        return RepoRoot() / "node-data" / "t027-upload-close-loop";
    }

    void ResetT027VisualizedDataDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(T027VisualizedDataDir(), ec);
        ec.clear();
        std::filesystem::create_directories(T027VisualizedDataDir(), ec);
        if (ec)
        {
            throw std::runtime_error("failed to prepare T027 node-data root: " +
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

    std::string ReadBinaryFileToString(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open binary file: " + path.string());
        }

        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
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
        const std::uint64_t create_time = 1711000001)
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

    raftdemo::MetadataCommand MakeCommitObjectCommandWithChunks(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id,
        const std::uint64_t size,
        const std::string &etag,
        std::vector<raftdemo::ChunkRef> chunks,
        const std::uint64_t commit_time = 1711000002)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = request_id;
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            bucket,
            object_key,
            object_id,
            1,
            size,
            etag,
            std::move(chunks),
            commit_time};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kCommitObject,
            bucket,
            object_key,
            "accepted",
            0,
            commit_time,
            std::nullopt};
        return command;
    }

    storedemo::StorageNodePlacementCandidate MakeUploadCandidate(
        const std::size_t index,
        const std::uint64_t available_capacity_bytes,
        const std::uint32_t queued_ops = 0)
    {
        storedemo::StorageNodePlacementCandidate candidate;
        candidate.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        candidate.endpoint = "127.0.0.1:" + std::to_string(7100 + index);
        candidate.health = storedemo::StorageNodeHealth::kHealthy;
        candidate.disk_pressure = storedemo::StorageNodeDiskPressure::kLow;
        candidate.total_capacity_bytes = available_capacity_bytes + 8192;
        candidate.used_capacity_bytes = 8192;
        candidate.available_capacity_bytes = available_capacity_bytes;
        candidate.load.queued_ops = queued_ops;
        return candidate;
    }

    storedemo::ReplicaPolicy MakeUploadReplicaPolicy(const std::size_t replica_count,
                                                     const std::size_t minimum_successful_writes)
    {
        storedemo::ReplicaPolicy policy;
        policy.replica_count = replica_count;
        policy.minimum_successful_writes = minimum_successful_writes;
        policy.avoid_same_node = true;
        return policy;
    }

    class StorageUploadIntegrationTest : public ::testing::Test
    {
    protected:
        static storedemo::LocalDiskChunkStoreConfig MakeStoreConfig()
        {
            return storedemo::LocalDiskChunkStoreConfig{
                .data_dir = T027VisualizedDataDir(),
                .node_id = storedemo::test::MakeStorageNodeIdFixture(27)};
        }
    };

    TEST_F(StorageUploadIntegrationTest, WriteFailureKeepsPendingObjectInvisibleWithoutCommit)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T027 real local upload skeleton is Linux-primary in this environment";
#else
        ASSERT_NO_THROW(ResetT027VisualizedDataDir());

        raftdemo::MetadataStateMachine machine;
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t027-failed-write",
                            "create-bucket-t027-failed-write"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t027-failed-write", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(128, "t027-failed-write");

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t027-failed-write",
                                                        "uploads/failed-write",
                                                        identity.object_id,
                                                        "create-object-t027-failed-write",
                                                        payload.size(),
                                                        "etag-t027-failed-write"))
                        .Ok);

        auto write_request = MakeWriteRequest(identity,
                                              payload,
                                              "write-t027-failed-write");
        write_request.expected_checksum = ComputeStoreChecksumOrThrow("different-payload");
        const auto write_response = store.WriteChunk(write_request);
        EXPECT_EQ(write_response.status,
                  storedemo::StorageNodeStatusCode::kChecksumMismatch);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t027-failed-write",
             .object_key = "uploads/failed-write"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list = machine.ListObjects(
            {.bucket = "bucket-t027-failed-write", .prefix = "uploads/"});
        EXPECT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        const auto read_response = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-t027-failed-write"));
        EXPECT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().live_root), 0U);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().staging_root), 0U);
#endif
    }

    TEST_F(StorageUploadIntegrationTest, PendingObjectStaysInvisibleUntilDurableChunkCommitThenBecomesVisible)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T027 real local upload skeleton is Linux-primary in this environment";
#else
        ASSERT_NO_THROW(ResetT027VisualizedDataDir());

        raftdemo::MetadataStateMachine machine;
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto fixture = LoadFixtureBinaryPayload();
        ASSERT_FALSE(fixture.payload.empty());

        std::uint64_t index = 10;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t027-success",
                            "create-bucket-t027-success"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t027-success", 1, 0, 0);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t027-success",
                                                        "uploads/success",
                                                        identity.object_id,
                                                        "create-object-t027-success",
                                                        fixture.payload.size(),
                                                        "etag-t027-success"))
                        .Ok);

        const auto head_before_write = machine.HeadObject(
            {.bucket = "bucket-t027-success", .object_key = "uploads/success"});
        EXPECT_EQ(head_before_write.result.code, raftdemo::MetadataStatusCode::kNotFound);

        const auto write_response = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t027-success"));
        ASSERT_EQ(write_response.status, storedemo::StorageNodeStatusCode::kOk)
            << write_response.error_detail;
        ASSERT_TRUE(write_response.durable);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().live_root), 1U);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().staging_root), 0U);

        const auto head_before_commit = machine.HeadObject(
            {.bucket = "bucket-t027-success", .object_key = "uploads/success"});
        EXPECT_EQ(head_before_commit.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head_before_commit.record.has_value());

        const auto list_before_commit = machine.ListObjects(
            {.bucket = "bucket-t027-success", .prefix = "uploads/"});
        EXPECT_EQ(list_before_commit.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list_before_commit.records.empty());

        const auto local_read_response = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-t027-success"));
        ASSERT_EQ(local_read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << local_read_response.error_detail;
        EXPECT_EQ(local_read_response.payload, fixture.payload);

        std::vector<raftdemo::ChunkRef> manifest_chunks{
            raftdemo::ChunkRef{
                identity.chunk_id,
                0,
                static_cast<std::uint64_t>(fixture.payload.size()),
                {write_response.metadata.node_id},
                write_response.metadata.checksum.value}};
        const auto commit_apply = raftdemo::test::ApplyMetadataCommand(
            machine,
            index++,
            MakeCommitObjectCommandWithChunks("bucket-t027-success",
                                              "uploads/success",
                                              identity.object_id,
                                              "commit-object-t027-success",
                                              fixture.payload.size(),
                                              "etag-t027-success",
                                              std::move(manifest_chunks)));
        ASSERT_TRUE(commit_apply.Ok) << commit_apply.message;

        const auto head_after_commit = machine.HeadObject(
            {.bucket = "bucket-t027-success", .object_key = "uploads/success"});
        ASSERT_EQ(head_after_commit.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(head_after_commit.record.has_value());
        EXPECT_TRUE(head_after_commit.record->IsCommitted());
        EXPECT_EQ(head_after_commit.record->size,
                  static_cast<std::uint64_t>(fixture.payload.size()));
        ASSERT_EQ(head_after_commit.record->chunks.size(), 1U);
        EXPECT_EQ(head_after_commit.record->chunks.front().chunk_id, identity.chunk_id);
        EXPECT_EQ(head_after_commit.record->chunks.front().size,
                  static_cast<std::uint64_t>(fixture.payload.size()));
        ASSERT_EQ(head_after_commit.record->chunks.front().replica_nodes.size(), 1U);
        EXPECT_EQ(head_after_commit.record->chunks.front().replica_nodes.front(),
                  write_response.metadata.node_id);
        EXPECT_EQ(head_after_commit.record->chunks.front().checksum,
                  write_response.metadata.checksum.value);

        const auto list_after_commit = machine.ListObjects(
            {.bucket = "bucket-t027-success", .prefix = "uploads/"});
        ASSERT_EQ(list_after_commit.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_EQ(list_after_commit.records.size(), 1U);
        EXPECT_EQ(list_after_commit.records.front().object_key, "uploads/success");
        EXPECT_TRUE(list_after_commit.records.front().IsCommitted());

        const auto indexed_chunks = machine.FindChunkRefs("bucket-t027-success",
                                                          "uploads/success");
        ASSERT_TRUE(indexed_chunks.has_value());
        ASSERT_EQ(indexed_chunks->size(), 1U);
        EXPECT_EQ(indexed_chunks->front().chunk_id, identity.chunk_id);
        EXPECT_EQ(indexed_chunks->front().checksum,
                  write_response.metadata.checksum.value);
        EXPECT_TRUE(std::filesystem::exists(T027VisualizedDataDir() / "chunks" / "live"));
        EXPECT_TRUE(std::filesystem::exists(T027VisualizedDataDir() / "chunks" / "staging"));
#endif
    }

    TEST_F(StorageUploadIntegrationTest, CommitFailureAfterDurableWriteLeavesInvisibleOrphanCandidateBoundary)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T027 real local upload skeleton is Linux-primary in this environment";
#else
        ASSERT_NO_THROW(ResetT027VisualizedDataDir());

        raftdemo::MetadataStateMachine machine;
        storedemo::LocalDiskChunkStore store(MakeStoreConfig());
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        std::uint64_t index = 20;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t027-commit-fail",
                            "create-bucket-t027-commit-fail"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t027-commit-fail", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(192, "t027-commit-fail");
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t027-commit-fail",
                                                        "uploads/commit-fail",
                                                        identity.object_id,
                                                        "create-object-t027-commit-fail",
                                                        payload.size(),
                                                        "etag-t027-commit-fail"))
                        .Ok);

        const auto write_response = store.WriteChunk(
            MakeWriteRequest(identity, payload, "write-t027-commit-fail"));
        ASSERT_EQ(write_response.status, storedemo::StorageNodeStatusCode::kOk)
            << write_response.error_detail;
        ASSERT_TRUE(write_response.durable);

        const auto commit_apply = raftdemo::test::ApplyMetadataCommand(
            machine,
            index++,
            MakeCommitObjectCommandWithChunks("bucket-t027-commit-fail",
                                              "uploads/commit-fail",
                                              "wrong-object-id",
                                              "commit-object-t027-commit-fail",
                                              payload.size(),
                                              "etag-t027-commit-fail",
                                              {raftdemo::ChunkRef{
                                                  identity.chunk_id,
                                                  0,
                                                  static_cast<std::uint64_t>(payload.size()),
                                                  {write_response.metadata.node_id},
                                                  write_response.metadata.checksum.value}}));
        EXPECT_FALSE(commit_apply.Ok);

        const auto head_after_failed_commit = machine.HeadObject(
            {.bucket = "bucket-t027-commit-fail",
             .object_key = "uploads/commit-fail"});
        EXPECT_EQ(head_after_failed_commit.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head_after_failed_commit.record.has_value());

        const auto list_after_failed_commit = machine.ListObjects(
            {.bucket = "bucket-t027-commit-fail", .prefix = "uploads/"});
        EXPECT_EQ(list_after_failed_commit.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list_after_failed_commit.records.empty());

        const auto stored_object =
            machine.FindObject("bucket-t027-commit-fail", "uploads/commit-fail");
        ASSERT_TRUE(stored_object.has_value());
        EXPECT_TRUE(stored_object->IsPending());
        EXPECT_TRUE(stored_object->chunks.empty());

        const auto indexed_chunks =
            machine.FindChunkRefs("bucket-t027-commit-fail", "uploads/commit-fail");
        EXPECT_FALSE(indexed_chunks.has_value());

        const auto local_read_response = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-t027-commit-fail"));
        ASSERT_EQ(local_read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << local_read_response.error_detail;
        EXPECT_EQ(local_read_response.payload, payload);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().live_root), 1U);
        EXPECT_EQ(CountRegularFilesRecursively(store.paths().staging_root), 0U);
#endif
    }

    TEST_F(StorageUploadIntegrationTest,
           DurableChunkPayloadStaysInChunkStoreAndNeverEntersMetadataSerialization)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T095 manifest boundary integration is validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_upload_t095_manifest_boundary");
        raftdemo::MetadataStateMachine machine;
        storedemo::LocalDiskChunkStore store(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("stores"), 95));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const std::string payload_marker =
            "T095_PAYLOAD_MUST_STAY_OUT_OF_METADATA_AND_RAFT";
        const std::string payload =
            payload_marker + storedemo::test::MakeChunkPayload(160, "t095-manifest");

        std::uint64_t index = 30;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t095-boundary",
                            "create-bucket-t095-boundary"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t095-boundary", 1, 0, 0);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t095-boundary",
                                                        "uploads/manifest-boundary",
                                                        identity.object_id,
                                                        "create-object-t095-boundary",
                                                        payload.size(),
                                                        "etag-t095-boundary"))
                        .Ok);

        const auto write_response = store.WriteChunk(
            MakeWriteRequest(identity, payload, "write-t095-boundary"));
        ASSERT_EQ(write_response.status, storedemo::StorageNodeStatusCode::kOk)
            << write_response.error_detail;
        ASSERT_TRUE(write_response.durable);

        std::vector<raftdemo::ChunkRef> manifest_chunks{
            raftdemo::ChunkRef{
                identity.chunk_id,
                0,
                static_cast<std::uint64_t>(payload.size()),
                {write_response.metadata.node_id},
                write_response.metadata.checksum.value}};
        const auto commit_command = MakeCommitObjectCommandWithChunks(
            "bucket-t095-boundary",
            "uploads/manifest-boundary",
            identity.object_id,
            "commit-object-t095-boundary",
            payload.size(),
            "etag-t095-boundary",
            manifest_chunks);
        const std::string serialized_commit =
            raftdemo::SerializeMetadataCommand(commit_command);
        EXPECT_EQ(serialized_commit.find(payload_marker), std::string::npos);

        const auto commit_apply =
            raftdemo::test::ApplyMetadataCommand(machine, index++, commit_command);
        ASSERT_TRUE(commit_apply.Ok) << commit_apply.message;

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t095-boundary",
             .object_key = "uploads/manifest-boundary"});
        ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(head.record.has_value());
        EXPECT_TRUE(head.record->IsCommitted());
        ASSERT_EQ(head.record->chunks.size(), 1U);
        EXPECT_EQ(head.record->chunks.front().chunk_id, identity.chunk_id);
        EXPECT_EQ(head.record->chunks.front().offset, 0U);
        EXPECT_EQ(head.record->chunks.front().size,
                  static_cast<std::uint64_t>(payload.size()));
        EXPECT_EQ(head.record->chunks.front().replica_nodes,
                  std::vector<std::string>{write_response.metadata.node_id});
        EXPECT_EQ(head.record->chunks.front().checksum,
                  write_response.metadata.checksum.value);

        const auto list = machine.ListObjects(
            {.bucket = "bucket-t095-boundary", .prefix = "uploads/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_EQ(list.records.size(), 1U);
        ASSERT_EQ(list.records.front().chunks.size(), 1U);
        EXPECT_EQ(list.records.front().chunks.front().chunk_id, identity.chunk_id);

        const std::filesystem::path snapshot_path =
            temp_dir.Path("metadata") / "t095-manifest-boundary.snapshot";
        const auto save = machine.SaveSnapshot(snapshot_path.string());
        ASSERT_EQ(save.status, raftdemo::SnapshotStatus::kOk) << save.message;

        const std::string snapshot_bytes = ReadBinaryFileToString(snapshot_path);
        EXPECT_EQ(snapshot_bytes.find(payload_marker), std::string::npos);

        const auto local_read = store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-t095-boundary"));
        ASSERT_EQ(local_read.status, storedemo::StorageNodeStatusCode::kOk)
            << local_read.error_detail;
        EXPECT_EQ(local_read.payload, payload);
#endif
    }

    TEST_F(StorageUploadIntegrationTest, UploadCoordinatorCommitManifestMatchesLocalDurableFacts)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T036 upload manifest integration currently validated on Linux";
#else
        raftdemo::MetadataStateMachine machine;
        auto metadata_client =
            std::make_shared<storedemo::test::InMemoryUploadMetadataClient>(machine);
        auto chunk_writer =
            std::make_shared<storedemo::test::LocalStoreUploadChunkWriter>();

        storedemo::test::ScopedStoreTestDir temp_dir("storage_upload_t036_manifest");
        auto store1 = std::make_shared<storedemo::LocalDiskChunkStore>(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("stores"), 1));
        auto store2 = std::make_shared<storedemo::LocalDiskChunkStore>(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("stores"), 2));
        ASSERT_EQ(store1->Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store2->Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        chunk_writer->RegisterStore(store1->config().node_id, store1);
        chunk_writer->RegisterStore(store2->config().node_id, store2);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");
        ASSERT_GT(fixture.payload.size(), 1U);

        const auto split_point = fixture.payload.size() / 2;
        ASSERT_GT(split_point, 0U);
        ASSERT_LT(split_point, fixture.payload.size());

        const std::string first_payload = fixture.payload.substr(0, split_point);
        const std::string second_payload = fixture.payload.substr(split_point);

        storedemo::WriteChunkResponse forced_failure;
        forced_failure.status = storedemo::StorageNodeStatusCode::kOverloaded;
        forced_failure.error_detail = "forced non-durable replica rejection";
        forced_failure.retry_after_ms = 25;
        chunk_writer->ForceResponse(storedemo::test::MakeStorageNodeIdFixture(3),
                                    forced_failure);

        storedemo::UploadCoordinatorRequest request;
        request.request_id = "upload-t036";
        request.bucket = "bucket-t036";
        request.object_key = "objects/test_file.deb";
        request.object_id = "obj-t036";
        request.version = 1;
        request.replica_policy = MakeUploadReplicaPolicy(3, 2);
        request.candidates = {
            MakeUploadCandidate(1, 128ULL * 1024ULL * 1024ULL, 1),
            MakeUploadCandidate(2, 96ULL * 1024ULL * 1024ULL, 2),
            MakeUploadCandidate(3, 64ULL * 1024ULL * 1024ULL, 3)};
        request.client_time_unix_ms = 1712002000;
        request.chunks.push_back(storedemo::UploadChunkInput{
            .chunk_index = 0,
            .offset = 0,
            .payload = first_payload});
        request.chunks.push_back(storedemo::UploadChunkInput{
            .chunk_index = 1,
            .offset = static_cast<std::uint64_t>(first_payload.size()),
            .payload = second_payload});

        storedemo::UploadCoordinator coordinator(metadata_client, chunk_writer);
        const auto result = coordinator.UploadObject(request);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_TRUE(result.committed);
        ASSERT_FALSE(result.pending_object_possible);
        ASSERT_FALSE(result.orphan_chunk_possible);
        ASSERT_EQ(result.chunk_executions.size(), 2U);
        ASSERT_EQ(metadata_client->commit_calls(), 1U);
        ASSERT_TRUE(metadata_client->last_commit_request().has_value());

        const auto &commit_request = *metadata_client->last_commit_request();
        ASSERT_EQ(commit_request.request_id, "upload-t036/commit");
        ASSERT_EQ(commit_request.chunks.size(), 2U);

        const std::vector<storedemo::StorageNodeId> expected_replicas{
            store1->config().node_id,
            store2->config().node_id};

        auto assert_manifest_matches_durable_facts =
            [&](const std::size_t manifest_index,
                const std::uint32_t chunk_index,
                const std::uint64_t offset,
                const std::string &payload)
        {
            const auto identity =
                MakeStoreIdentityOrThrow(request.object_id,
                                         request.version,
                                         chunk_index,
                                         offset);
            const auto &manifest_chunk = commit_request.chunks.at(manifest_index);
            EXPECT_EQ(manifest_chunk.identity.chunk_id, identity.chunk_id);
            EXPECT_EQ(manifest_chunk.identity.object_id, request.object_id);
            EXPECT_EQ(manifest_chunk.identity.version, request.version);
            EXPECT_EQ(manifest_chunk.identity.chunk_index, chunk_index);
            EXPECT_EQ(manifest_chunk.identity.offset, offset);
            EXPECT_EQ(manifest_chunk.offset, offset);
            EXPECT_EQ(manifest_chunk.size,
                      static_cast<std::uint64_t>(payload.size()));
            EXPECT_EQ(manifest_chunk.replica_nodes, expected_replicas);

            const auto read1 = store1->ReadChunk(
                MakeReadRequest(identity.chunk_id,
                                "read-t036-store1-" + std::to_string(chunk_index)));
            const auto read2 = store2->ReadChunk(
                MakeReadRequest(identity.chunk_id,
                                "read-t036-store2-" + std::to_string(chunk_index)));
            ASSERT_EQ(read1.status, storedemo::StorageNodeStatusCode::kOk)
                << read1.error_detail;
            ASSERT_EQ(read2.status, storedemo::StorageNodeStatusCode::kOk)
                << read2.error_detail;
            EXPECT_EQ(read1.payload, payload);
            EXPECT_EQ(read2.payload, payload);
            EXPECT_EQ(manifest_chunk.size, read1.metadata.size);
            EXPECT_EQ(manifest_chunk.size, read2.metadata.size);
            EXPECT_EQ(manifest_chunk.checksum.value, read1.metadata.checksum.value);
            EXPECT_EQ(manifest_chunk.checksum.value, read2.metadata.checksum.value);
        };

        assert_manifest_matches_durable_facts(0, 0, 0, first_payload);
        assert_manifest_matches_durable_facts(1,
                                              1,
                                              static_cast<std::uint64_t>(first_payload.size()),
                                              second_payload);

        for (const auto &execution : result.chunk_executions)
        {
            EXPECT_EQ(execution.durable_success_count, 2U);
            EXPECT_TRUE(execution.commit_eligible);
            ASSERT_EQ(execution.replica_results.size(), 3U);
            EXPECT_EQ(execution.replica_results.back().node_id,
                      storedemo::test::MakeStorageNodeIdFixture(3));
            EXPECT_EQ(execution.replica_results.back().status,
                      storedemo::StorageNodeStatusCode::kOverloaded);
        }

        const auto head = machine.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(head.record.has_value());
        ASSERT_EQ(head.record->chunks.size(), commit_request.chunks.size());

        const auto manifest = machine.FindChunkRefs(request.bucket, request.object_key);
        ASSERT_TRUE(manifest.has_value());
        ASSERT_EQ(manifest->size(), commit_request.chunks.size());
        for (std::size_t index = 0; index < manifest->size(); ++index)
        {
            EXPECT_EQ(manifest->at(index).chunk_id,
                      commit_request.chunks.at(index).identity.chunk_id);
            EXPECT_EQ(manifest->at(index).offset,
                      commit_request.chunks.at(index).offset);
            EXPECT_EQ(manifest->at(index).size,
                      commit_request.chunks.at(index).size);
            EXPECT_EQ(manifest->at(index).checksum,
                      commit_request.chunks.at(index).checksum.value);
            EXPECT_EQ(manifest->at(index).replica_nodes,
                      commit_request.chunks.at(index).replica_nodes);
        }
#endif
    }

    TEST_F(StorageUploadIntegrationTest,
           UploadCoordinatorPartialReplicaFailureLeavesCleanupCandidateInvisible)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T037 upload partial replica failure integration currently validated on Linux";
#else
        raftdemo::MetadataStateMachine machine;
        auto metadata_client =
            std::make_shared<storedemo::test::InMemoryUploadMetadataClient>(machine);
        auto chunk_writer =
            std::make_shared<storedemo::test::LocalStoreUploadChunkWriter>();

        storedemo::test::ScopedStoreTestDir temp_dir("storage_upload_t037_partial_failure");
        auto store1 = std::make_shared<storedemo::LocalDiskChunkStore>(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("stores"), 1));
        ASSERT_EQ(store1->Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        chunk_writer->RegisterStore(store1->config().node_id, store1);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        storedemo::WriteChunkResponse forced_overloaded;
        forced_overloaded.status = storedemo::StorageNodeStatusCode::kOverloaded;
        forced_overloaded.error_detail = "forced overloaded replica";
        forced_overloaded.retry_after_ms = 20;
        chunk_writer->ForceResponse(storedemo::test::MakeStorageNodeIdFixture(2),
                                    forced_overloaded);

        storedemo::WriteChunkResponse forced_unavailable;
        forced_unavailable.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
        forced_unavailable.error_detail = "forced unavailable replica";
        chunk_writer->ForceResponse(storedemo::test::MakeStorageNodeIdFixture(3),
                                    forced_unavailable);

        storedemo::UploadCoordinatorRequest request;
        request.request_id = "upload-t037";
        request.bucket = "bucket-t037";
        request.object_key = "objects/test_file.deb";
        request.object_id = "obj-t037";
        request.version = 1;
        request.replica_policy = MakeUploadReplicaPolicy(3, 2);
        request.candidates = {
            MakeUploadCandidate(1, 128ULL * 1024ULL * 1024ULL, 1),
            MakeUploadCandidate(2, 96ULL * 1024ULL * 1024ULL, 2),
            MakeUploadCandidate(3, 64ULL * 1024ULL * 1024ULL, 3)};
        request.client_time_unix_ms = 1712003000;
        request.chunks.push_back(storedemo::UploadChunkInput{
            .chunk_index = 0,
            .offset = 0,
            .payload = fixture.payload});

        storedemo::UploadCoordinator coordinator(metadata_client, chunk_writer);
        const auto result = coordinator.UploadObject(request);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOverloaded);
        ASSERT_TRUE(result.create_succeeded);
        ASSERT_FALSE(result.committed);
        ASSERT_TRUE(result.pending_object_possible);
        ASSERT_TRUE(result.orphan_chunk_possible);
        ASSERT_EQ(metadata_client->commit_calls(), 0U);
        ASSERT_FALSE(metadata_client->last_commit_request().has_value());
        ASSERT_TRUE(result.committed_chunks.empty());
        ASSERT_EQ(result.cleanup_candidates.size(), 1U);

        const auto head = machine.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list = machine.ListObjects(
            {.bucket = request.bucket, .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        const auto stored_object =
            machine.FindObject(request.bucket, request.object_key);
        ASSERT_TRUE(stored_object.has_value());
        EXPECT_TRUE(stored_object->IsPending());
        EXPECT_TRUE(stored_object->chunks.empty());

        const auto &cleanup_candidate = result.cleanup_candidates.front();
        ASSERT_EQ(cleanup_candidate.chunk.replica_nodes.size(), 1U);
        EXPECT_EQ(cleanup_candidate.chunk.replica_nodes.front(),
                  store1->config().node_id);

        const auto read = store1->ReadChunk(
            MakeReadRequest(cleanup_candidate.chunk.identity.chunk_id,
                            "read-t037-cleanup-candidate"));
        ASSERT_EQ(read.status, storedemo::StorageNodeStatusCode::kOk)
            << read.error_detail;
        EXPECT_EQ(read.payload, fixture.payload);
        EXPECT_EQ(cleanup_candidate.chunk.size, read.metadata.size);
        EXPECT_EQ(cleanup_candidate.chunk.checksum.value,
                  read.metadata.checksum.value);
        EXPECT_NE(cleanup_candidate.reason.find("cleanup candidate"),
                  std::string::npos);
#endif
    }
} // namespace
