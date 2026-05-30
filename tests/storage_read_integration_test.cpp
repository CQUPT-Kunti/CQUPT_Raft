#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "raft/common/metadata_result.h"
#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "support/metadata_test_utils.h"
#include "support/store_test_utils.h"
#include "support/storage_upload_test_utils.h"

namespace
{
    storedemo::ChunkIdentity MakeStoreIdentityOrThrow(const std::string_view object_id,
                                                      const std::uint64_t version,
                                                      const std::uint32_t chunk_index,
                                                      const std::uint64_t offset)
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

    struct ReadCommittedManifestResult
    {
        storedemo::StorageNodeStatusCode status{storedemo::StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::string payload;
    };

    class CountingChunkReader
    {
    public:
        explicit CountingChunkReader(storedemo::ChunkStore &store)
            : store_(store)
        {
        }

        storedemo::ReadChunkResponse ReadChunk(const raftdemo::ChunkRef &chunk_ref,
                                               const std::string &request_id)
        {
            ++read_calls_;
            read_chunk_ids_.push_back(chunk_ref.chunk_id);

            storedemo::ReadChunkRequest request;
            request.request_id = request_id;
            request.chunk_id = chunk_ref.chunk_id;
            request.expected_checksum.algorithm = storedemo::ChunkChecksumAlgorithm::kSha256;
            request.expected_checksum.value = chunk_ref.checksum;
            request.expected_checksum.size_bytes = chunk_ref.size;
            request.verify_checksum = true;
            return store_.ReadChunk(request);
        }

        [[nodiscard]] std::size_t read_calls() const
        {
            return read_calls_;
        }

        [[nodiscard]] const std::vector<std::string> &read_chunk_ids() const
        {
            return read_chunk_ids_;
        }

    private:
        storedemo::ChunkStore &store_;
        std::size_t read_calls_{0};
        std::vector<std::string> read_chunk_ids_;
    };

    ReadCommittedManifestResult ReadCommittedObjectByManifest(
        const raftdemo::MetadataStateMachine &machine,
        CountingChunkReader &reader,
        const std::string &bucket,
        const std::string &object_key)
    {
        const auto head = machine.HeadObject(
            {.bucket = bucket, .object_key = object_key});
        if (head.result.code != raftdemo::MetadataStatusCode::kOk ||
            !head.record.has_value() ||
            !head.record->IsCommitted())
        {
            return ReadCommittedManifestResult{
                .status = storedemo::test::MapMetadataStatusCode(head.result.code),
                .error_detail = head.result.summary.message};
        }

        const auto manifest = machine.FindChunkRefs(bucket, object_key);
        if (!manifest.has_value())
        {
            return ReadCommittedManifestResult{
                .status = storedemo::StorageNodeStatusCode::kNotFound,
                .error_detail = "committed object manifest not found"};
        }

        std::vector<raftdemo::ChunkRef> ordered_manifest = *manifest;
        std::stable_sort(ordered_manifest.begin(),
                         ordered_manifest.end(),
                         [](const raftdemo::ChunkRef &lhs, const raftdemo::ChunkRef &rhs)
                         {
                             if (lhs.offset != rhs.offset)
                             {
                                 return lhs.offset < rhs.offset;
                             }
                             return lhs.chunk_id < rhs.chunk_id;
                         });

        ReadCommittedManifestResult result;
        for (std::size_t index = 0; index < ordered_manifest.size(); ++index)
        {
            const auto &chunk_ref = ordered_manifest.at(index);
            if (chunk_ref.replica_nodes.empty())
            {
                return ReadCommittedManifestResult{
                    .status = storedemo::StorageNodeStatusCode::kInvalidArgument,
                    .error_detail = "manifest chunk is missing replica_nodes"};
            }

            const auto read = reader.ReadChunk(
                chunk_ref,
                "storage-read-" + std::to_string(index));
            if (read.status != storedemo::StorageNodeStatusCode::kOk)
            {
                return ReadCommittedManifestResult{
                    .status = read.status,
                    .error_detail = read.error_detail};
            }

            if (read.metadata.size != chunk_ref.size)
            {
                return ReadCommittedManifestResult{
                    .status = storedemo::StorageNodeStatusCode::kCorrupted,
                    .error_detail = "manifest size does not match local chunk facts"};
            }
            if (read.metadata.checksum.value != chunk_ref.checksum)
            {
                return ReadCommittedManifestResult{
                    .status = storedemo::StorageNodeStatusCode::kChecksumMismatch,
                    .error_detail = "manifest checksum does not match local chunk facts"};
            }

            result.payload.append(read.payload);
        }

        return result;
    }

    class StorageReadIntegrationTest : public ::testing::Test
    {
    protected:
        static std::vector<std::string> SplitPayloadIntoChunks(const std::string &payload)
        {
            const std::size_t total = payload.size();
            const std::size_t first = total / 3;
            const std::size_t second = total / 3;
            const std::size_t third = total - first - second;

            return {
                payload.substr(0, first),
                payload.substr(first, second),
                payload.substr(first + second, third)};
        }
    };

    TEST_F(StorageReadIntegrationTest, CommittedObjectReadsManifestChunksInOffsetOrder)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T040 committed manifest read integration currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t040_committed");
        storedemo::LocalDiskChunkStore store(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("store"), 40));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t040-read",
                            "create-bucket-t040-read"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");
        ASSERT_FALSE(fixture.payload.empty());

        const std::string object_id = "obj-t040-read";
        const std::string object_key = "objects/test_file.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);
        ASSERT_EQ(payload_parts.size(), 3U);
        ASSERT_FALSE(payload_parts.at(0).empty());
        ASSERT_FALSE(payload_parts.at(1).empty());
        ASSERT_FALSE(payload_parts.at(2).empty());

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t040-read",
                            object_key,
                            object_id,
                            version,
                            "create-object-t040-read",
                            fixture.payload.size(),
                            "etag-t040-read"))
                        .Ok);

        std::vector<storedemo::ChunkIdentity> identities;
        identities.reserve(payload_parts.size());
        std::vector<raftdemo::ChunkRef> ordered_manifest;
        ordered_manifest.reserve(payload_parts.size());
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            const auto identity = MakeStoreIdentityOrThrow(object_id,
                                                           version,
                                                           static_cast<std::uint32_t>(chunk_index),
                                                           next_offset);
            identities.push_back(identity);
            next_offset += payload_parts.at(chunk_index).size();

            const auto write = store.WriteChunk(
                MakeWriteRequest(identity,
                                 payload_parts.at(chunk_index),
                                 "write-t040-read-" + std::to_string(chunk_index)));
            ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
                << write.error_detail;
            ASSERT_TRUE(write.durable);
            ordered_manifest.push_back(MakeChunkRefFromMetadata(write.metadata));
        }

        std::vector<raftdemo::ChunkRef> shuffled_manifest{
            ordered_manifest.at(2),
            ordered_manifest.at(0),
            ordered_manifest.at(1)};
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t040-read",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t040-read",
                            fixture.payload.size(),
                            "etag-t040-read",
                            std::move(shuffled_manifest)))
                        .Ok);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t040-read", .object_key = object_key});
        ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(head.record.has_value());
        EXPECT_TRUE(head.record->IsCommitted());

        const auto manifest = machine.FindChunkRefs("bucket-t040-read", object_key);
        ASSERT_TRUE(manifest.has_value());
        ASSERT_EQ(manifest->size(), payload_parts.size());

        for (std::size_t chunk_index = 0; chunk_index < manifest->size(); ++chunk_index)
        {
            const auto &chunk_ref = manifest->at(chunk_index);
            EXPECT_EQ(chunk_ref.replica_nodes.size(), 1U);
            EXPECT_EQ(chunk_ref.replica_nodes.front(), store.config().node_id);

            storedemo::ReadChunkRequest read_request;
            read_request.request_id =
                "verify-manifest-t040-" + std::to_string(chunk_index);
            read_request.chunk_id = chunk_ref.chunk_id;
            read_request.expected_checksum.algorithm =
                storedemo::ChunkChecksumAlgorithm::kSha256;
            read_request.expected_checksum.value = chunk_ref.checksum;
            read_request.expected_checksum.size_bytes = chunk_ref.size;
            read_request.verify_checksum = true;

            const auto read = store.ReadChunk(read_request);
            ASSERT_EQ(read.status, storedemo::StorageNodeStatusCode::kOk)
                << read.error_detail;
            EXPECT_EQ(read.metadata.size, chunk_ref.size);
            EXPECT_EQ(read.metadata.checksum.value, chunk_ref.checksum);
        }

        CountingChunkReader reader(store);
        const auto result =
            ReadCommittedObjectByManifest(machine, reader, "bucket-t040-read", object_key);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.read_calls(), payload_parts.size());

        const std::vector<std::string> expected_read_order{
            identities.at(0).chunk_id,
            identities.at(1).chunk_id,
            identities.at(2).chunk_id};
        EXPECT_EQ(reader.read_chunk_ids(), expected_read_order);
#endif
    }

    TEST_F(StorageReadIntegrationTest, PendingObjectDoesNotReadDataPlaneEvenIfChunkExists)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T040 pending-object read gate currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t040_pending");
        storedemo::LocalDiskChunkStore store(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("store"), 41));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t040-pending",
                            "create-bucket-t040-pending"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const auto identity =
            MakeStoreIdentityOrThrow("obj-t040-pending", 1, 0, 0);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t040-pending",
                            "objects/pending.deb",
                            identity.object_id,
                            identity.version,
                            "create-object-t040-pending",
                            fixture.payload.size(),
                            "etag-t040-pending"))
                        .Ok);

        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t040-pending"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t040-pending", .object_key = "objects/pending.deb"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list = machine.ListObjects(
            {.bucket = "bucket-t040-pending", .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        CountingChunkReader reader(store);
        const auto result = ReadCommittedObjectByManifest(machine,
                                                          reader,
                                                          "bucket-t040-pending",
                                                          "objects/pending.deb");

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_TRUE(result.payload.empty());
        EXPECT_EQ(reader.read_calls(), 0U);
#endif
    }

    TEST_F(StorageReadIntegrationTest, DeletedObjectDoesNotReadDataPlaneAfterMetadataTombstone)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T040 deleted-object read gate currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t040_deleted");
        storedemo::LocalDiskChunkStore store(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("store"), 42));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t040-deleted",
                            "create-bucket-t040-deleted"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const auto identity =
            MakeStoreIdentityOrThrow("obj-t040-deleted", 1, 0, 0);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t040-deleted",
                            "objects/deleted.deb",
                            identity.object_id,
                            identity.version,
                            "create-object-t040-deleted",
                            fixture.payload.size(),
                            "etag-t040-deleted"))
                        .Ok);

        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t040-deleted"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t040-deleted",
                            "objects/deleted.deb",
                            identity.object_id,
                            identity.version,
                            "commit-object-t040-deleted",
                            fixture.payload.size(),
                            "etag-t040-deleted",
                            {MakeChunkRefFromMetadata(write.metadata)}))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeDeleteObjectCommand(
                            "bucket-t040-deleted",
                            "objects/deleted.deb",
                            identity.object_id,
                            "delete-object-t040-deleted"))
                        .Ok);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t040-deleted", .object_key = "objects/deleted.deb"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list = machine.ListObjects(
            {.bucket = "bucket-t040-deleted", .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        const auto stored_object =
            machine.FindObject("bucket-t040-deleted", "objects/deleted.deb");
        ASSERT_TRUE(stored_object.has_value());
        EXPECT_TRUE(stored_object->IsDeleted());

        CountingChunkReader reader(store);
        const auto result = ReadCommittedObjectByManifest(machine,
                                                          reader,
                                                          "bucket-t040-deleted",
                                                          "objects/deleted.deb");

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_TRUE(result.payload.empty());
        EXPECT_EQ(reader.read_calls(), 0U);
#endif
    }
} // namespace
