#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/maintenance/garbage_collector.h"
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
    struct TestCleanupCandidate
    {
        std::string bucket;
        std::string object_key;
        raftdemo::ChunkRef chunk;
        std::string reason;
    };

    struct TestCleanupAttempt
    {
        bool eligible{false};
        bool protected_by_live_manifest{false};
        bool delete_attempted{false};
        std::string protecting_object_key;
        std::string error_detail;
        storedemo::DeleteChunkResponse delete_response;
    };

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
            .expected_checksum = storedemo::test::ComputeStoreChecksumOrThrow(payload),
            .payload = payload};
    }

    raftdemo::ChunkRef MakeChunkRefFromMetadata(const storedemo::ChunkMetadata &metadata)
    {
        return raftdemo::ChunkRef{
            .chunk_id = metadata.identity.chunk_id,
            .offset = metadata.identity.offset,
            .size = metadata.size,
            .replica_nodes = {metadata.node_id},
            .checksum = metadata.checksum.value};
    }

    storedemo::CleanupChunkFact MakeCleanupChunkFactFromChunkRef(
        const raftdemo::ChunkRef &chunk_ref)
    {
        storedemo::ChunkIdentity identity;
        std::string error_detail;
        const auto status =
            storedemo::ParseChunkId(chunk_ref.chunk_id, &identity, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to parse cleanup chunk id: " + error_detail);
        }

        storedemo::CleanupChunkFact fact;
        fact.identity = std::move(identity);
        fact.identity.offset = chunk_ref.offset;
        fact.size = chunk_ref.size;
        fact.checksum.algorithm = storedemo::ChunkChecksumAlgorithm::kSha256;
        fact.checksum.value = chunk_ref.checksum;
        fact.checksum.size_bytes = chunk_ref.size;
        fact.replica_nodes = chunk_ref.replica_nodes;
        return fact;
    }

    std::vector<std::string> CandidateChunkIds(
        const std::vector<TestCleanupCandidate> &candidates)
    {
        std::vector<std::string> chunk_ids;
        chunk_ids.reserve(candidates.size());
        for (const auto &candidate : candidates)
        {
            chunk_ids.push_back(candidate.chunk.chunk_id);
        }
        return chunk_ids;
    }

    std::vector<TestCleanupCandidate> BuildCleanupCandidatesFromManifest(
        const std::string &bucket,
        const std::string &object_key,
        std::vector<raftdemo::ChunkRef> manifest,
        std::string reason)
    {
        std::stable_sort(manifest.begin(),
                         manifest.end(),
                         [](const raftdemo::ChunkRef &lhs, const raftdemo::ChunkRef &rhs)
                         {
                             if (lhs.offset != rhs.offset)
                             {
                                 return lhs.offset < rhs.offset;
                             }
                             return lhs.chunk_id < rhs.chunk_id;
                         });

        std::unordered_set<std::string> seen_chunk_ids;
        std::vector<TestCleanupCandidate> candidates;
        candidates.reserve(manifest.size());
        for (auto &chunk_ref : manifest)
        {
            if (!seen_chunk_ids.insert(chunk_ref.chunk_id).second)
            {
                continue;
            }

            candidates.push_back(TestCleanupCandidate{
                .bucket = bucket,
                .object_key = object_key,
                .chunk = std::move(chunk_ref),
                .reason = reason});
        }

        return candidates;
    }

    std::vector<TestCleanupCandidate> BuildCleanupCandidatesFromUploadResult(
        const std::string &bucket,
        const std::string &object_key,
        const std::vector<storedemo::UploadCleanupCandidate> &cleanup_candidates)
    {
        std::vector<TestCleanupCandidate> candidates;
        candidates.reserve(cleanup_candidates.size());
        for (const auto &candidate : cleanup_candidates)
        {
            candidates.push_back(TestCleanupCandidate{
                .bucket = bucket,
                .object_key = object_key,
                .chunk = raftdemo::ChunkRef{
                    .chunk_id = candidate.chunk.identity.chunk_id,
                    .offset = candidate.chunk.offset,
                    .size = candidate.chunk.size,
                    .replica_nodes = candidate.chunk.replica_nodes,
                    .checksum = candidate.chunk.checksum.value},
                .reason = candidate.reason});
        }
        return candidates;
    }

    std::optional<std::string> FindProtectingCommittedObject(
        const raftdemo::MetadataStateMachine &machine,
        const std::vector<std::string> &buckets,
        const std::string &chunk_id)
    {
        for (const auto &bucket : buckets)
        {
            const auto listed = machine.ListObjects({.bucket = bucket, .prefix = ""});
            if (listed.result.code == raftdemo::MetadataStatusCode::kNotFound)
            {
                continue;
            }
            if (listed.result.code != raftdemo::MetadataStatusCode::kOk)
            {
                continue;
            }

            for (const auto &record : listed.records)
            {
                const auto manifest = machine.FindChunkRefs(bucket, record.object_key);
                if (!manifest.has_value())
                {
                    continue;
                }

                const auto protecting_it = std::find_if(
                    manifest->begin(),
                    manifest->end(),
                    [&](const raftdemo::ChunkRef &chunk_ref)
                    {
                        return chunk_ref.chunk_id == chunk_id;
                    });
                if (protecting_it != manifest->end())
                {
                    return record.object_key;
                }
            }
        }

        return std::nullopt;
    }

    TestCleanupAttempt ApplyTestOnlyCleanupCandidate(
        storedemo::LocalDiskChunkStore &store,
        const raftdemo::MetadataStateMachine &machine,
        const std::vector<std::string> &buckets,
        const TestCleanupCandidate &candidate,
        const std::string &request_id)
    {
        TestCleanupAttempt attempt;

        const auto protecting_object =
            FindProtectingCommittedObject(machine, buckets, candidate.chunk.chunk_id);
        if (protecting_object.has_value())
        {
            attempt.protected_by_live_manifest = true;
            attempt.protecting_object_key = *protecting_object;
            attempt.error_detail =
                "cleanup blocked by committed live manifest: " + *protecting_object;
            return attempt;
        }

        const auto node_it = std::find(candidate.chunk.replica_nodes.begin(),
                                       candidate.chunk.replica_nodes.end(),
                                       store.config().node_id);
        if (node_it == candidate.chunk.replica_nodes.end())
        {
            attempt.error_detail =
                "cleanup candidate does not target this LocalDiskChunkStore node";
            return attempt;
        }

        attempt.eligible = true;
        attempt.delete_attempted = true;

        storedemo::DeleteChunkRequest request;
        request.request_id = request_id;
        request.chunk_id = candidate.chunk.chunk_id;
        request.reason = candidate.reason;
        request.metadata_boundary = "test-only-gc-safety-checked";
        request.expected_checksum.algorithm = storedemo::ChunkChecksumAlgorithm::kSha256;
        request.expected_checksum.value = candidate.chunk.checksum;
        request.expected_checksum.size_bytes = candidate.chunk.size;

        attempt.delete_response = store.DeleteChunk(request);
        if (!attempt.delete_response.ok())
        {
            attempt.error_detail = attempt.delete_response.error_detail;
        }
        return attempt;
    }

    storedemo::GarbageCollectorSafetyCheckResult EvaluateMetadataDrivenSafety(
        const raftdemo::MetadataStateMachine &machine,
        const std::vector<std::string> &buckets,
        const storedemo::GarbageCollectorTask &task)
    {
        const auto protecting_object =
            FindProtectingCommittedObject(machine, buckets, task.chunk_id);
        if (protecting_object.has_value())
        {
            storedemo::GarbageCollectorSafetyCheckResult result;
            result.status = storedemo::StorageNodeStatusCode::kConflict;
            result.error_detail =
                "chunk still referenced by committed live manifest: " + *protecting_object;
            return result;
        }

        return {};
    }

    class StorageDeleteGcTest : public ::testing::Test
    {
    protected:
        static storedemo::LocalDiskChunkStoreConfig MakeStoreConfig(
            const std::filesystem::path &root,
            const std::size_t node_index)
        {
            return storedemo::LocalDiskChunkStoreConfig{
                .data_dir = root / ("node_" + std::to_string(node_index)),
                .node_id = storedemo::test::MakeStorageNodeIdFixture(node_index)};
        }
    };

    TEST_F(StorageDeleteGcTest,
           DeleteObjectMakesObjectInvisibleAndTestOnlyCleanupDeletesChunk)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T049 delete/GC safety tests are currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_gc_delete_cleanup");
        storedemo::LocalDiskChunkStore store(MakeStoreConfig(temp_dir.root(), 49));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t049-delete",
                            "create-bucket-t049-delete"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const auto identity = MakeStoreIdentityOrThrow("obj-t049-delete", 1, 0, 0);
        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t049-delete"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t049-delete",
                            "objects/test_file.deb",
                            identity.object_id,
                            identity.version,
                            "create-object-t049-delete",
                            fixture.payload.size(),
                            "etag-t049-delete"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t049-delete",
                            "objects/test_file.deb",
                            identity.object_id,
                            identity.version,
                            "commit-object-t049-delete",
                            fixture.payload.size(),
                            "etag-t049-delete",
                            {MakeChunkRefFromMetadata(write.metadata)}))
                        .Ok);

        const auto committed_manifest =
            machine.FindChunkRefs("bucket-t049-delete", "objects/test_file.deb");
        ASSERT_TRUE(committed_manifest.has_value());
        ASSERT_EQ(committed_manifest->size(), 1U);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeDeleteObjectCommand(
                            "bucket-t049-delete",
                            "objects/test_file.deb",
                            identity.object_id,
                            "delete-object-t049-delete"))
                        .Ok);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t049-delete", .object_key = "objects/test_file.deb"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list =
            machine.ListObjects({.bucket = "bucket-t049-delete", .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());
        EXPECT_FALSE(
            machine.FindChunkRefs("bucket-t049-delete", "objects/test_file.deb").has_value());

        const auto stored_object =
            machine.FindObject("bucket-t049-delete", "objects/test_file.deb");
        ASSERT_TRUE(stored_object.has_value());
        EXPECT_TRUE(stored_object->IsDeleted());

        const auto first_candidates = BuildCleanupCandidatesFromManifest(
            "bucket-t049-delete",
            "objects/test_file.deb",
            *committed_manifest,
            "deleted object cleanup candidate");
        const auto second_candidates = BuildCleanupCandidatesFromManifest(
            "bucket-t049-delete",
            "objects/test_file.deb",
            *committed_manifest,
            "deleted object cleanup candidate");
        ASSERT_EQ(first_candidates.size(), 1U);
        EXPECT_EQ(CandidateChunkIds(first_candidates), CandidateChunkIds(second_candidates));

        const auto cleanup = ApplyTestOnlyCleanupCandidate(store,
                                                           machine,
                                                           {"bucket-t049-delete"},
                                                           first_candidates.front(),
                                                           "cleanup-t049-delete");
        EXPECT_TRUE(cleanup.eligible);
        EXPECT_TRUE(cleanup.delete_attempted);
        EXPECT_FALSE(cleanup.protected_by_live_manifest);
        ASSERT_EQ(cleanup.delete_response.status, storedemo::StorageNodeStatusCode::kOk)
            << cleanup.delete_response.error_detail;
        EXPECT_TRUE(cleanup.delete_response.deleted);

        const auto read_after_cleanup = store.ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-after-cleanup-t049-delete",
            .chunk_id = identity.chunk_id});
        EXPECT_EQ(read_after_cleanup.status, storedemo::StorageNodeStatusCode::kNotFound);
#endif
    }

    TEST_F(StorageDeleteGcTest,
           SharedCommittedManifestReferenceProtectsChunkFromCleanupAfterDelete)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T049 delete/GC safety tests are currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_gc_shared_chunk");
        storedemo::LocalDiskChunkStore store(MakeStoreConfig(temp_dir.root(), 50));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 10;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t049-shared",
                            "create-bucket-t049-shared"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        const auto shared_identity = MakeStoreIdentityOrThrow("obj-t049-shared-a", 1, 0, 0);
        const auto write = store.WriteChunk(
            MakeWriteRequest(shared_identity, fixture.payload, "write-t049-shared"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        const auto shared_chunk_ref = MakeChunkRefFromMetadata(write.metadata);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t049-shared",
                            "objects/shared-a",
                            "obj-t049-shared-a",
                            1,
                            "create-object-t049-shared-a",
                            fixture.payload.size(),
                            "etag-t049-shared-a"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t049-shared",
                            "objects/shared-a",
                            "obj-t049-shared-a",
                            1,
                            "commit-object-t049-shared-a",
                            fixture.payload.size(),
                            "etag-t049-shared-a",
                            {shared_chunk_ref}))
                        .Ok);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t049-shared",
                            "objects/shared-b",
                            "obj-t049-shared-b",
                            1,
                            "create-object-t049-shared-b",
                            fixture.payload.size(),
                            "etag-t049-shared-b"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t049-shared",
                            "objects/shared-b",
                            "obj-t049-shared-b",
                            1,
                            "commit-object-t049-shared-b",
                            fixture.payload.size(),
                            "etag-t049-shared-b",
                            {shared_chunk_ref}))
                        .Ok);

        const auto deleted_manifest =
            machine.FindChunkRefs("bucket-t049-shared", "objects/shared-a");
        ASSERT_TRUE(deleted_manifest.has_value());

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeDeleteObjectCommand(
                            "bucket-t049-shared",
                            "objects/shared-a",
                            "obj-t049-shared-a",
                            "delete-object-t049-shared-a"))
                        .Ok);

        const auto cleanup_candidates = BuildCleanupCandidatesFromManifest(
            "bucket-t049-shared",
            "objects/shared-a",
            *deleted_manifest,
            "deleted object cleanup candidate");
        ASSERT_EQ(cleanup_candidates.size(), 1U);

        const auto cleanup = ApplyTestOnlyCleanupCandidate(store,
                                                           machine,
                                                           {"bucket-t049-shared"},
                                                           cleanup_candidates.front(),
                                                           "cleanup-t049-shared");
        EXPECT_FALSE(cleanup.eligible);
        EXPECT_FALSE(cleanup.delete_attempted);
        EXPECT_TRUE(cleanup.protected_by_live_manifest);
        EXPECT_EQ(cleanup.protecting_object_key, "objects/shared-b");

        const auto surviving_head = machine.HeadObject(
            {.bucket = "bucket-t049-shared", .object_key = "objects/shared-b"});
        ASSERT_EQ(surviving_head.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(surviving_head.record.has_value());
        EXPECT_TRUE(surviving_head.record->IsCommitted());

        const auto read = store.ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-t049-shared-survives",
            .chunk_id = shared_identity.chunk_id});
        ASSERT_EQ(read.status, storedemo::StorageNodeStatusCode::kOk)
            << read.error_detail;
        EXPECT_EQ(read.payload, fixture.payload);
#endif
    }

    TEST_F(StorageDeleteGcTest,
           GarbageCollectorMetadataSafetyBlocksLiveManifestReferencedChunk)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T055 metadata-driven GC safety tests are currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_gc_gc_safety_blocked");
        storedemo::LocalDiskChunkStore store(MakeStoreConfig(temp_dir.root(), 55));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 100;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t055-blocked",
                            "create-bucket-t055-blocked"))
                        .Ok);

        const auto payload = storedemo::test::MakeChunkPayload(768, "t055-safety-blocked");
        const auto shared_identity = MakeStoreIdentityOrThrow("obj-t055-shared-a", 1, 0, 0);
        const auto write = store.WriteChunk(
            MakeWriteRequest(shared_identity, payload, "write-t055-shared"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        const auto shared_chunk_ref = MakeChunkRefFromMetadata(write.metadata);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t055-blocked",
                            "objects/shared-a",
                            "obj-t055-shared-a",
                            1,
                            "create-object-t055-shared-a",
                            payload.size(),
                            "etag-t055-shared-a"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t055-blocked",
                            "objects/shared-a",
                            "obj-t055-shared-a",
                            1,
                            "commit-object-t055-shared-a",
                            payload.size(),
                            "etag-t055-shared-a",
                            {shared_chunk_ref}))
                        .Ok);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t055-blocked",
                            "objects/shared-b",
                            "obj-t055-shared-b",
                            1,
                            "create-object-t055-shared-b",
                            payload.size(),
                            "etag-t055-shared-b"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t055-blocked",
                            "objects/shared-b",
                            "obj-t055-shared-b",
                            1,
                            "commit-object-t055-shared-b",
                            payload.size(),
                            "etag-t055-shared-b",
                            {shared_chunk_ref}))
                        .Ok);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeDeleteObjectCommand(
                            "bucket-t055-blocked",
                            "objects/shared-a",
                            "obj-t055-shared-a",
                            "delete-object-t055-shared-a"))
                        .Ok);

        std::atomic<int> handler_runs{0};
        std::string observed_metadata_boundary;
        storedemo::GarbageCollector collector(
            [&](const storedemo::GarbageCollectorTask &task)
            {
                handler_runs.fetch_add(1, std::memory_order_relaxed);
                storedemo::DeleteChunkRequest request;
                request.request_id = "delete-handler-" + task.task_id;
                request.chunk_id = task.chunk_id;
                request.reason = "gc deleted object cleanup";
                request.metadata_boundary = task.metadata_boundary;
                return store.DeleteChunk(request);
            },
            [&](const storedemo::GarbageCollectorTask &task)
            {
                observed_metadata_boundary = task.metadata_boundary;
                return EvaluateMetadataDrivenSafety(machine,
                                                    {"bucket-t055-blocked"},
                                                    task);
            },
            {.worker_count = 1, .queue_capacity = 4, .default_max_attempts = 2});

        storedemo::GarbageCollectorTask task;
        task.task_id = "gc-t055-live-manifest-blocked";
        task.chunk_id = shared_identity.chunk_id;
        task.reason = storedemo::GarbageCollectionReason::kDeletedObjectCleanup;
        task.metadata_boundary = "metadata-fact:deleted-object";

        ASSERT_TRUE(collector.SubmitTask(std::move(task)).accepted());
        ASSERT_TRUE(collector.Drain().drained);

        const auto snapshot = collector.FindTask("gc-t055-live-manifest-blocked");
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_EQ(snapshot->state, storedemo::GarbageCollectorTaskState::kFailed);
        EXPECT_EQ(snapshot->attempts, 1U);
        EXPECT_EQ(snapshot->last_error, storedemo::StorageNodeStatusCode::kConflict);
        EXPECT_NE(snapshot->last_error_detail.find("committed live manifest"),
                  std::string::npos);
        EXPECT_EQ(observed_metadata_boundary, "metadata-fact:deleted-object");
        EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 0);

        const auto read = store.ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-t055-shared-survives",
            .chunk_id = shared_identity.chunk_id});
        ASSERT_EQ(read.status, storedemo::StorageNodeStatusCode::kOk)
            << read.error_detail;
        EXPECT_EQ(read.payload, payload);
#endif
    }

    TEST_F(StorageDeleteGcTest,
           GarbageCollectorMetadataSafetyAllowsDeletedObjectChunkCleanup)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T055 metadata-driven GC safety tests are currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_gc_gc_safety_allowed");
        storedemo::LocalDiskChunkStore store(MakeStoreConfig(temp_dir.root(), 56));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 120;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t055-allowed",
                            "create-bucket-t055-allowed"))
                        .Ok);

        const auto payload = storedemo::test::MakeChunkPayload(640, "t055-safety-allowed");
        const auto identity = MakeStoreIdentityOrThrow("obj-t055-allowed", 1, 0, 0);
        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, payload, "write-t055-allowed"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t055-allowed",
                            "objects/deleted-object",
                            identity.object_id,
                            identity.version,
                            "create-object-t055-allowed",
                            payload.size(),
                            "etag-t055-allowed"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t055-allowed",
                            "objects/deleted-object",
                            identity.object_id,
                            identity.version,
                            "commit-object-t055-allowed",
                            payload.size(),
                            "etag-t055-allowed",
                            {MakeChunkRefFromMetadata(write.metadata)}))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeDeleteObjectCommand(
                            "bucket-t055-allowed",
                            "objects/deleted-object",
                            identity.object_id,
                            "delete-object-t055-allowed"))
                        .Ok);

        std::atomic<int> handler_runs{0};
        std::string observed_metadata_boundary;
        storedemo::GarbageCollector collector(
            [&](const storedemo::GarbageCollectorTask &task)
            {
                handler_runs.fetch_add(1, std::memory_order_relaxed);
                observed_metadata_boundary = task.metadata_boundary;
                storedemo::DeleteChunkRequest request;
                request.request_id = "delete-handler-" + task.task_id;
                request.chunk_id = task.chunk_id;
                request.reason = "gc deleted object cleanup";
                request.metadata_boundary = task.metadata_boundary;
                return store.DeleteChunk(request);
            },
            [&](const storedemo::GarbageCollectorTask &task)
            {
                observed_metadata_boundary = task.metadata_boundary;
                return EvaluateMetadataDrivenSafety(machine,
                                                    {"bucket-t055-allowed"},
                                                    task);
            },
            {.worker_count = 1, .queue_capacity = 4, .default_max_attempts = 2});

        storedemo::GarbageCollectorTask task;
        task.task_id = "gc-t055-deleted-object-allowed";
        task.chunk_id = identity.chunk_id;
        task.reason = storedemo::GarbageCollectionReason::kDeletedObjectCleanup;
        task.metadata_boundary = "metadata-fact:deleted-object";

        ASSERT_TRUE(collector.SubmitTask(std::move(task)).accepted());
        ASSERT_TRUE(collector.Drain().drained);

        const auto snapshot = collector.FindTask("gc-t055-deleted-object-allowed");
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_EQ(snapshot->state, storedemo::GarbageCollectorTaskState::kCompleted);
        EXPECT_EQ(snapshot->attempts, 1U);
        EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 1);
        EXPECT_EQ(observed_metadata_boundary, "metadata-fact:deleted-object");

        const auto read_after_cleanup = store.ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-after-cleanup-t055-allowed",
            .chunk_id = identity.chunk_id});
        EXPECT_EQ(read_after_cleanup.status, storedemo::StorageNodeStatusCode::kNotFound);
#endif
    }

    TEST_F(StorageDeleteGcTest,
           DeletedCleanupCandidateTaskStillRespectsLiveManifestSafetyGate)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T056 cleanup candidate tests are currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_gc_t056_deleted_candidate");
        storedemo::LocalDiskChunkStore store(MakeStoreConfig(temp_dir.root(), 57));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 140;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t056-deleted",
                            "create-bucket-t056-deleted"))
                        .Ok);

        const auto payload = storedemo::test::MakeChunkPayload(896, "t056-deleted-candidate");
        const auto shared_identity = MakeStoreIdentityOrThrow("obj-t056-shared-a", 1, 0, 0);
        const auto write = store.WriteChunk(
            MakeWriteRequest(shared_identity, payload, "write-t056-shared"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        const auto shared_chunk_ref = MakeChunkRefFromMetadata(write.metadata);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t056-deleted",
                            "objects/shared-a",
                            "obj-t056-shared-a",
                            1,
                            "create-object-t056-shared-a",
                            payload.size(),
                            "etag-t056-shared-a"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t056-deleted",
                            "objects/shared-a",
                            "obj-t056-shared-a",
                            1,
                            "commit-object-t056-shared-a",
                            payload.size(),
                            "etag-t056-shared-a",
                            {shared_chunk_ref}))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t056-deleted",
                            "objects/shared-b",
                            "obj-t056-shared-b",
                            1,
                            "create-object-t056-shared-b",
                            payload.size(),
                            "etag-t056-shared-b"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t056-deleted",
                            "objects/shared-b",
                            "obj-t056-shared-b",
                            1,
                            "commit-object-t056-shared-b",
                            payload.size(),
                            "etag-t056-shared-b",
                            {shared_chunk_ref}))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeDeleteObjectCommand(
                            "bucket-t056-deleted",
                            "objects/shared-a",
                            "obj-t056-shared-a",
                            "delete-object-t056-shared-a"))
                        .Ok);

        storedemo::DeletedObjectCleanupRequest cleanup_request;
        cleanup_request.bucket = "bucket-t056-deleted";
        cleanup_request.object_key = "objects/shared-a";
        cleanup_request.object_id = "obj-t056-shared-a";
        cleanup_request.version = 1;
        cleanup_request.object_state = storedemo::CleanupObjectState::kDeleted;
        cleanup_request.created_at_unix_ms = 1712005600;
        cleanup_request.durable_chunks = {
            MakeCleanupChunkFactFromChunkRef(shared_chunk_ref)};

        const auto candidates =
            storedemo::BuildDeletedObjectCleanupCandidates(cleanup_request);
        ASSERT_EQ(candidates.size(), 1U);
        EXPECT_EQ(candidates.front().source, storedemo::CleanupCandidateSource::kDeletedObject);
        EXPECT_EQ(candidates.front().reason,
                  storedemo::GarbageCollectionReason::kDeletedObjectCleanup);
        EXPECT_NE(candidates.front().metadata_boundary.find("metadata-fact:deleted-object"),
                  std::string::npos);

        auto task = storedemo::CleanupCandidateToGarbageCollectorTask(candidates.front());
        EXPECT_EQ(task.chunk_id, shared_identity.chunk_id);
        EXPECT_EQ(task.object_id, "obj-t056-shared-a");
        EXPECT_EQ(task.chunk_index, 0U);
        EXPECT_EQ(task.metadata_boundary, candidates.front().metadata_boundary);

        std::atomic<int> handler_runs{0};
        storedemo::GarbageCollector collector(
            [&](const storedemo::GarbageCollectorTask &collector_task)
            {
                handler_runs.fetch_add(1, std::memory_order_relaxed);
                storedemo::DeleteChunkRequest request;
                request.request_id = "delete-handler-" + collector_task.task_id;
                request.chunk_id = collector_task.chunk_id;
                request.reason = "gc deleted object cleanup";
                request.metadata_boundary = collector_task.metadata_boundary;
                return store.DeleteChunk(request);
            },
            [&](const storedemo::GarbageCollectorTask &collector_task)
            {
                return EvaluateMetadataDrivenSafety(machine,
                                                    {"bucket-t056-deleted"},
                                                    collector_task);
            },
            {.worker_count = 1, .queue_capacity = 4, .default_max_attempts = 2});

        ASSERT_TRUE(collector.SubmitTask(std::move(task)).accepted());
        ASSERT_TRUE(collector.Drain().drained);

        const auto snapshot =
            collector.FindTask("gc-candidate/DeletedObject/" + shared_identity.chunk_id);
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_EQ(snapshot->state, storedemo::GarbageCollectorTaskState::kFailed);
        EXPECT_EQ(snapshot->attempts, 1U);
        EXPECT_EQ(snapshot->last_error, storedemo::StorageNodeStatusCode::kConflict);
        EXPECT_NE(snapshot->last_error_detail.find("committed live manifest"),
                  std::string::npos);
        EXPECT_EQ(snapshot->metadata_boundary, candidates.front().metadata_boundary);
        EXPECT_EQ(handler_runs.load(std::memory_order_relaxed), 0);

        const auto read = store.ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-t056-live-manifest-protected",
            .chunk_id = shared_identity.chunk_id});
        ASSERT_EQ(read.status, storedemo::StorageNodeStatusCode::kOk)
            << read.error_detail;
        EXPECT_EQ(read.payload, payload);
#endif
    }

    TEST_F(StorageDeleteGcTest,
           RepeatedDeleteReplayAndRepeatedCleanupAttemptRemainIdempotent)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T049 delete/GC safety tests are currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_gc_idempotent");
        storedemo::LocalDiskChunkStore store(MakeStoreConfig(temp_dir.root(), 51));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 20;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t049-idem",
                            "create-bucket-t049-idem"))
                        .Ok);

        const auto payload = storedemo::test::MakeChunkPayload(512, "t049-idempotent");
        const auto identity = MakeStoreIdentityOrThrow("obj-t049-idem", 1, 0, 0);
        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, payload, "write-t049-idem"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t049-idem",
                            "objects/idempotent",
                            identity.object_id,
                            identity.version,
                            "create-object-t049-idem",
                            payload.size(),
                            "etag-t049-idem"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t049-idem",
                            "objects/idempotent",
                            identity.object_id,
                            identity.version,
                            "commit-object-t049-idem",
                            payload.size(),
                            "etag-t049-idem",
                            {MakeChunkRefFromMetadata(write.metadata)}))
                        .Ok);

        const auto manifest = machine.FindChunkRefs("bucket-t049-idem", "objects/idempotent");
        ASSERT_TRUE(manifest.has_value());

        const auto delete_command = raftdemo::test::MakeDeleteObjectCommand(
            "bucket-t049-idem",
            "objects/idempotent",
            identity.object_id,
            "delete-object-t049-idem");
        const auto first_delete = raftdemo::test::ApplyMetadataCommand(
            machine,
            index++,
            delete_command);
        ASSERT_TRUE(first_delete.Ok);
        const auto replay_delete = raftdemo::test::ApplyMetadataCommand(
            machine,
            index++,
            delete_command);
        ASSERT_TRUE(replay_delete.Ok);
        EXPECT_EQ(replay_delete.message, "idempotent replay");
        EXPECT_EQ(machine.TombstoneCount(), 1U);

        const auto candidates = BuildCleanupCandidatesFromManifest(
            "bucket-t049-idem",
            "objects/idempotent",
            *manifest,
            "deleted object cleanup candidate");
        ASSERT_EQ(candidates.size(), 1U);

        const auto first_cleanup = ApplyTestOnlyCleanupCandidate(store,
                                                                 machine,
                                                                 {"bucket-t049-idem"},
                                                                 candidates.front(),
                                                                 "cleanup-t049-idem-first");
        EXPECT_TRUE(first_cleanup.eligible);
        EXPECT_TRUE(first_cleanup.delete_attempted);
        ASSERT_EQ(first_cleanup.delete_response.status, storedemo::StorageNodeStatusCode::kOk)
            << first_cleanup.delete_response.error_detail;
        EXPECT_TRUE(first_cleanup.delete_response.deleted);

        const auto second_cleanup = ApplyTestOnlyCleanupCandidate(store,
                                                                  machine,
                                                                  {"bucket-t049-idem"},
                                                                  candidates.front(),
                                                                  "cleanup-t049-idem-second");
        EXPECT_TRUE(second_cleanup.eligible);
        EXPECT_TRUE(second_cleanup.delete_attempted);
        ASSERT_EQ(second_cleanup.delete_response.status, storedemo::StorageNodeStatusCode::kOk)
            << second_cleanup.delete_response.error_detail;
        EXPECT_TRUE(second_cleanup.delete_response.already_missing);
#endif
    }

    TEST_F(StorageDeleteGcTest,
           FailedUploadCleanupCandidateIsDeletableWithoutPretendingProductionGc)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T049 delete/GC safety tests are currently validated on Linux";
#else
        raftdemo::MetadataStateMachine machine;
        auto metadata_client =
            std::make_shared<storedemo::test::InMemoryUploadMetadataClient>(machine);
        auto chunk_writer =
            std::make_shared<storedemo::test::LocalStoreUploadChunkWriter>();

        storedemo::test::ScopedStoreTestDir temp_dir("storage_delete_gc_orphan_candidate");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("stores"), 1));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        chunk_writer->RegisterStore(store->config().node_id, store);

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
        request.request_id = "upload-t049";
        request.bucket = "bucket-t049-upload-fail";
        request.object_key = "objects/test_file.deb";
        request.object_id = "obj-t049-upload-fail";
        request.version = 1;
        request.replica_policy = storedemo::ReplicaPolicy{
            .replica_count = 3,
            .minimum_successful_writes = 2,
            .avoid_same_node = true};
        request.candidates = {
            storedemo::StorageNodePlacementCandidate{
                .node_id = storedemo::test::MakeStorageNodeIdFixture(1),
                .endpoint = "127.0.0.1:7101",
                .health = storedemo::StorageNodeHealth::kHealthy,
                .disk_pressure = storedemo::StorageNodeDiskPressure::kLow,
                .total_capacity_bytes = 128ULL * 1024ULL * 1024ULL,
                .used_capacity_bytes = 1024,
                .available_capacity_bytes = 128ULL * 1024ULL * 1024ULL - 1024,
                .load = {.queued_ops = 1}},
            storedemo::StorageNodePlacementCandidate{
                .node_id = storedemo::test::MakeStorageNodeIdFixture(2),
                .endpoint = "127.0.0.1:7102",
                .health = storedemo::StorageNodeHealth::kHealthy,
                .disk_pressure = storedemo::StorageNodeDiskPressure::kLow,
                .total_capacity_bytes = 96ULL * 1024ULL * 1024ULL,
                .used_capacity_bytes = 1024,
                .available_capacity_bytes = 96ULL * 1024ULL * 1024ULL - 1024,
                .load = {.queued_ops = 2}},
            storedemo::StorageNodePlacementCandidate{
                .node_id = storedemo::test::MakeStorageNodeIdFixture(3),
                .endpoint = "127.0.0.1:7103",
                .health = storedemo::StorageNodeHealth::kHealthy,
                .disk_pressure = storedemo::StorageNodeDiskPressure::kLow,
                .total_capacity_bytes = 64ULL * 1024ULL * 1024ULL,
                .used_capacity_bytes = 1024,
                .available_capacity_bytes = 64ULL * 1024ULL * 1024ULL - 1024,
                .load = {.queued_ops = 3}}};
        request.client_time_unix_ms = 1712004900;
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
        ASSERT_EQ(result.cleanup_candidates.size(), 1U);

        const auto head = machine.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list =
            machine.ListObjects({.bucket = request.bucket, .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        const auto stored_object = machine.FindObject(request.bucket, request.object_key);
        ASSERT_TRUE(stored_object.has_value());
        EXPECT_TRUE(stored_object->IsPending());

        const auto candidates = BuildCleanupCandidatesFromUploadResult(
            request.bucket,
            request.object_key,
            result.cleanup_candidates);
        ASSERT_EQ(candidates.size(), 1U);
        EXPECT_NE(candidates.front().reason.find("cleanup candidate"), std::string::npos);

        const auto cleanup = ApplyTestOnlyCleanupCandidate(*store,
                                                           machine,
                                                           {request.bucket},
                                                           candidates.front(),
                                                           "cleanup-t049-upload-fail");
        EXPECT_TRUE(cleanup.eligible);
        EXPECT_TRUE(cleanup.delete_attempted);
        EXPECT_FALSE(cleanup.protected_by_live_manifest);
        ASSERT_EQ(cleanup.delete_response.status, storedemo::StorageNodeStatusCode::kOk)
            << cleanup.delete_response.error_detail;
        EXPECT_TRUE(cleanup.delete_response.deleted);

        const auto read_after_cleanup = store->ReadChunk(storedemo::ReadChunkRequest{
            .request_id = "read-after-cleanup-t049-upload-fail",
            .chunk_id = candidates.front().chunk.chunk_id});
        EXPECT_EQ(read_after_cleanup.status, storedemo::StorageNodeStatusCode::kNotFound);
#endif
    }
} // namespace
