#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
#include "store/node/storage_node_registry.h"
#include "store/io/durable_file.h"
#include "support/metadata_test_utils.h"
#include "support/storage_read_test_utils.h"
#include "support/store_test_utils.h"
#include "support/storage_upload_test_utils.h"

namespace
{
    using storedemo::test::CountingReplicaReader;
    using storedemo::test::ReadObjectByManifest;
    using storedemo::test::ReadObjectByManifestRequest;

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

    std::filesystem::path ResolveFinalPathOrThrow(const std::filesystem::path &data_root,
                                                  const storedemo::ChunkId &chunk_id)
    {
        storedemo::ChunkPathLayout layout;
        std::string error_detail;
        const auto layout_status =
            storedemo::BuildChunkPathLayout(chunk_id, "probe", &layout, &error_detail);
        if (layout_status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to build final path layout: " +
                                     error_detail);
        }

        std::filesystem::path final_path;
        const auto resolve_status =
            storedemo::ResolveDurablePathUnderRoot(data_root,
                                                   layout.final_relative_path,
                                                   &final_path,
                                                   &error_detail);
        if (resolve_status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to resolve final path: " + error_detail);
        }

        return final_path;
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

    raftdemo::ChunkRef MakeChunkRef(const storedemo::ChunkIdentity &identity,
                                    const std::string &payload,
                                    std::vector<storedemo::StorageNodeId> replica_nodes)
    {
        const auto checksum = storedemo::test::ComputeStoreChecksumOrThrow(payload);
        return raftdemo::ChunkRef{
            .chunk_id = identity.chunk_id,
            .offset = identity.offset,
            .size = static_cast<std::uint64_t>(payload.size()),
            .replica_nodes = std::move(replica_nodes),
            .checksum = checksum.value};
    }

    storedemo::StorageNodeRegistryFacts MakeRegistryFactsForRead(
        const storedemo::StorageNodeHealth health =
            storedemo::StorageNodeHealth::kHealthy,
        const storedemo::StorageNodeDiskPressure disk_pressure =
            storedemo::StorageNodeDiskPressure::kLow,
        const std::uint32_t active_reads = 0,
        const bool read_overloaded = false)
    {
        storedemo::StorageNodeRegistryFacts facts;
        facts.capacity.total_capacity_bytes = 64 * 1024;
        facts.capacity.used_capacity_bytes = 8 * 1024;
        facts.capacity.available_capacity_bytes = 56 * 1024;
        facts.capacity.chunk_count = 1;
        facts.health.health = health;
        facts.health.disk_pressure = disk_pressure;
        facts.load.load.active_reads = active_reads;
        facts.load.load.active_writes = active_reads / 2;
        facts.load.load.queued_ops = active_reads / 3;
        facts.load.read_admission_overloaded = read_overloaded;
        return facts;
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

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                EXPECT_EQ(node_id, store.config().node_id);
                return store.ReadChunk(request);
            });
        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t040-read",
                .object_key = object_key});

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

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                EXPECT_EQ(node_id, store.config().node_id);
                return store.ReadChunk(request);
            });
        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t040-pending",
                .object_key = "objects/pending.deb"});

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

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                EXPECT_EQ(node_id, store.config().node_id);
                return store.ReadChunk(request);
            });
        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t040-deleted",
                .object_key = "objects/deleted.deb"});

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_TRUE(result.payload.empty());
        EXPECT_EQ(reader.read_calls(), 0U);
#endif
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectReadsFirstReadableReplicaWithoutFallback)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t045-read-first",
                            "create-bucket-t045-read-first"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t045-first";
        const std::string object_key = "objects/first-readable.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t045-read-first",
                            object_key,
                            object_id,
                            version,
                            "create-object-t045-read-first",
                            fixture.payload.size(),
                            "etag-t045-read-first"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t045-read-first",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t045-read-first",
                            fixture.payload.size(),
                            "etag-t045-read-first",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                if (node_id != "replica-a")
                {
                    storedemo::ReadChunkResponse response;
                    response.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                    response.error_detail = "unexpected fallback";
                    return response;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t045-read-first",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.read_calls(), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-a"), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-b"), 0U);
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectFallsBackAfterUnavailableReplica)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t045-read-unavailable",
                            "create-bucket-t045-read-unavailable"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t045-unavailable";
        const std::string object_key = "objects/fallback-unavailable.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t045-read-unavailable",
                            object_key,
                            object_id,
                            version,
                            "create-object-t045-read-unavailable",
                            fixture.payload.size(),
                            "etag-t045-read-unavailable"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t045-read-unavailable",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t045-read-unavailable",
                            fixture.payload.size(),
                            "etag-t045-read-unavailable",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                if (node_id == "replica-a")
                {
                    storedemo::ReadChunkResponse response;
                    response.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                    response.error_detail = "replica-a unavailable";
                    return response;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t045-read-unavailable",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-b"), payload_parts.size());
        const std::vector<std::string> expected_order{
            "replica-a", "replica-b",
            "replica-a", "replica-b",
            "replica-a", "replica-b"};
        EXPECT_EQ(reader.read_node_ids(), expected_order);
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectFallsBackAfterNotFoundReplica)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t047-read-not-found",
                            "create-bucket-t047-read-not-found"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t047-not-found";
        const std::string object_key = "objects/fallback-not-found.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t047-read-not-found",
                            object_key,
                            object_id,
                            version,
                            "create-object-t047-read-not-found",
                            fixture.payload.size(),
                            "etag-t047-read-not-found"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t047-read-not-found",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t047-read-not-found",
                            fixture.payload.size(),
                            "etag-t047-read-not-found",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                if (node_id == "replica-a")
                {
                    storedemo::ReadChunkResponse response;
                    response.status = storedemo::StorageNodeStatusCode::kNotFound;
                    response.error_detail = "replica-a missing chunk";
                    response.metadata.identity.chunk_id = request.chunk_id;
                    response.metadata.node_id = node_id;
                    return response;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t047-read-not-found",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-b"), payload_parts.size());
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectFallsBackAfterTimeoutReplica)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t047-read-timeout",
                            "create-bucket-t047-read-timeout"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t047-timeout";
        const std::string object_key = "objects/fallback-timeout.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t047-read-timeout",
                            object_key,
                            object_id,
                            version,
                            "create-object-t047-read-timeout",
                            fixture.payload.size(),
                            "etag-t047-read-timeout"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t047-read-timeout",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t047-read-timeout",
                            fixture.payload.size(),
                            "etag-t047-read-timeout",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                if (node_id == "replica-a")
                {
                    storedemo::ReadChunkResponse response;
                    response.status = storedemo::StorageNodeStatusCode::kTimeout;
                    response.error_detail = "replica-a timeout";
                    response.metadata.identity.chunk_id = request.chunk_id;
                    response.metadata.node_id = node_id;
                    return response;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t047-read-timeout",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-b"), payload_parts.size());
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectFallsBackAfterChecksumMismatchReplica)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t045-read-checksum",
                            "create-bucket-t045-read-checksum"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t045-checksum";
        const std::string object_key = "objects/fallback-checksum.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t045-read-checksum",
                            object_key,
                            object_id,
                            version,
                            "create-object-t045-read-checksum",
                            fixture.payload.size(),
                            "etag-t045-read-checksum"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t045-read-checksum",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t045-read-checksum",
                            fixture.payload.size(),
                            "etag-t045-read-checksum",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                if (node_id == "replica-a")
                {
                    storedemo::ReadChunkResponse failure;
                    failure.status = storedemo::StorageNodeStatusCode::kChecksumMismatch;
                    failure.error_detail = "replica-a checksum mismatch";
                    failure.payload = "corrupted-payload-must-not-surface";
                    failure.metadata.identity.chunk_id = request.chunk_id;
                    failure.metadata.node_id = node_id;
                    failure.metadata.size = request.expected_checksum.size_bytes;
                    failure.metadata.checksum = request.expected_checksum;
                    failure.metadata.state = storedemo::ChunkState::kCorrupted;
                    return failure;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t045-read-checksum",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-b"), payload_parts.size());
        ASSERT_EQ(result.chunk_results.size(), payload_parts.size());
        for (const auto &chunk_result : result.chunk_results)
        {
            ASSERT_EQ(chunk_result.attempts.size(), 2U);
            EXPECT_EQ(chunk_result.attempts[0].node_id, "replica-a");
            EXPECT_EQ(chunk_result.attempts[0].status,
                      storedemo::StorageNodeStatusCode::kChecksumMismatch);
            EXPECT_EQ(chunk_result.selected_node_id, "replica-b");
        }
    }

    TEST_F(StorageReadIntegrationTest, AllReplicaFailuresReturnExplicitError)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t045-read-all-fail",
                            "create-bucket-t045-read-all-fail"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t045-all-fail";
        const std::string object_key = "objects/all-fail.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t045-read-all-fail",
                            object_key,
                            object_id,
                            version,
                            "create-object-t045-read-all-fail",
                            fixture.payload.size(),
                            "etag-t045-read-all-fail"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t045-read-all-fail",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t045-read-all-fail",
                            fixture.payload.size(),
                            "etag-t045-read-all-fail",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                storedemo::ReadChunkResponse response;
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                if (node_id == "replica-a")
                {
                    response.status = storedemo::StorageNodeStatusCode::kTimeout;
                    response.error_detail = "replica-a timeout";
                    return response;
                }

                response.status = storedemo::StorageNodeStatusCode::kChecksumMismatch;
                response.error_detail = "replica-b checksum mismatch";
                response.metadata.state = storedemo::ChunkState::kCorrupted;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t045-read-all-fail",
                .object_key = object_key});

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_TRUE(result.payload.empty());
        EXPECT_NE(result.error_detail.find("all replicas failed after"),
                  std::string::npos);
        EXPECT_EQ(reader.calls_for_node("replica-a"), 1U);
        EXPECT_EQ(reader.calls_for_node("replica-b"), 1U);
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectSkipsKnownCorruptedReplicaFacts)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t047-read-skip-corrupted",
                            "create-bucket-t047-read-skip-corrupted"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t047-skip-corrupted";
        const std::string object_key = "objects/skip-corrupted.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t047-read-skip-corrupted",
                            object_key,
                            object_id,
                            version,
                            "create-object-t047-read-skip-corrupted",
                            fixture.payload.size(),
                            "etag-t047-read-skip-corrupted"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t047-read-skip-corrupted",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t047-read-skip-corrupted",
                            fixture.payload.size(),
                            "etag-t047-read-skip-corrupted",
                            manifest))
                        .Ok);

        storedemo::StorageNodeRegistry registry(
            storedemo::StorageNodeRegistryConfig{
                .stale_timeout_ms = 30,
                .dead_timeout_ms = 90});
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-a",
                         .endpoint = "127.0.0.1:7401",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead( storedemo::StorageNodeHealth::kHealthy,
                                                            storedemo::StorageNodeDiskPressure::kLow,
                                                            0)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-b",
                         .endpoint = "127.0.0.1:7402",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead( storedemo::StorageNodeHealth::kHealthy,
                                                            storedemo::StorageNodeDiskPressure::kLow,
                                                            1)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                if (node_id == "replica-a")
                {
                    ADD_FAILURE() << "known corrupted replica should be filtered before read";
                    storedemo::ReadChunkResponse response;
                    response.status = storedemo::StorageNodeStatusCode::kCorrupted;
                    response.error_detail = "unexpected read against replica-a";
                    return response;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t047-read-skip-corrupted",
                .object_key = object_key,
                .candidate_resolver =
                    [](const raftdemo::ChunkRef &chunk_ref)
                    {
                        return std::vector<storedemo::ReadReplicaCandidate>{
                            storedemo::ReadReplicaCandidate{
                                .node_id = chunk_ref.replica_nodes.at(0),
                                .known_corrupted = true,
                                .has_observed_facts = true},
                            storedemo::ReadReplicaCandidate{
                                .node_id = chunk_ref.replica_nodes.at(1),
                                .has_observed_facts = true}};
                    },
                .registry_snapshot_resolver =
                    [&registry]()
                    { return registry.Snapshot(110); }});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), 0U);
        EXPECT_EQ(reader.calls_for_node("replica-b"), payload_parts.size());
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectFallsBackAfterLocalStoreQuarantinesCorruptedReplica)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "real local store quarantine fallback is only verified on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t072_quarantine_fallback");
        storedemo::LocalDiskChunkStore store_a(
            storedemo::LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("stores") / "replica_a",
                .node_id = "replica-a"});
        storedemo::LocalDiskChunkStore store_b(
            storedemo::LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("stores") / "replica_b",
                .node_id = "replica-b"});
        ASSERT_EQ(store_a.Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store_b.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t072-read-quarantine",
                            "create-bucket-t072-read-quarantine"))
                        .Ok);

        const std::string object_id = "obj-t072-read-quarantine";
        const std::string object_key = "objects/t072-quarantine.bin";
        const std::uint64_t version = 1;
        const std::string payload = storedemo::test::MakeChunkPayload(96, "t072-read");
        const auto identity = MakeStoreIdentityOrThrow(object_id, version, 0, 0);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t072-read-quarantine",
                            object_key,
                            object_id,
                            version,
                            "create-object-t072-read-quarantine",
                            payload.size(),
                            "etag-t072-read-quarantine"))
                        .Ok);

        ASSERT_EQ(store_a.WriteChunk(MakeWriteRequest(identity, payload, "write-replica-a")).status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store_b.WriteChunk(MakeWriteRequest(identity, payload, "write-replica-b")).status,
                  storedemo::StorageNodeStatusCode::kOk);

        const auto tampered_path =
            ResolveFinalPathOrThrow(temp_dir.Path("stores") / "replica_a",
                                    identity.chunk_id);
        {
            std::ofstream output(tampered_path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(output.is_open());
            output << storedemo::test::MakeChunkPayload(payload.size(), "tampered-a");
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t072-read-quarantine",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t072-read-quarantine",
                            payload.size(),
                            "etag-t072-read-quarantine",
                            {MakeChunkRef(identity, payload, {"replica-a", "replica-b"})}))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                if (node_id == "replica-a")
                {
                    return store_a.ReadChunk(request);
                }
                if (node_id == "replica-b")
                {
                    return store_b.ReadChunk(request);
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kNotFound;
                response.error_detail = "unknown replica";
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t072-read-quarantine",
                .object_key = object_key,
                .candidate_resolver =
                    [](const raftdemo::ChunkRef &chunk_ref)
                    {
                        return std::vector<storedemo::ReadReplicaCandidate>{
                            storedemo::ReadReplicaCandidate{
                                .node_id = chunk_ref.replica_nodes.at(0),
                                .has_observed_facts = true},
                            storedemo::ReadReplicaCandidate{
                                .node_id = chunk_ref.replica_nodes.at(1),
                                .has_observed_facts = true}};
                    }});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, payload);
        ASSERT_EQ(result.chunk_results.size(), 1U);
        ASSERT_EQ(result.chunk_results[0].attempts.size(), 2U);
        EXPECT_EQ(result.chunk_results[0].attempts[0].node_id, "replica-a");
        EXPECT_EQ(result.chunk_results[0].attempts[0].status,
                  storedemo::StorageNodeStatusCode::kCorrupted);
        EXPECT_EQ(result.chunk_results[0].selected_node_id, "replica-b");

        const auto replica_a_stat =
            store_a.StatChunk({.request_id = "stat-replica-a", .chunk_id = identity.chunk_id});
        ASSERT_EQ(replica_a_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(replica_a_stat.metadata.state, storedemo::ChunkState::kQuarantined);
#endif
    }

    TEST_F(StorageReadIntegrationTest,
           CommittedObjectPrefersFreshHealthyLowLoadReplicaFromRegistryFacts)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t066-read-priority",
                            "create-bucket-t066-read-priority"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t066-priority";
        const std::string object_key = "objects/priority.deb";
        const std::uint64_t version = 1;
        const auto identity = MakeStoreIdentityOrThrow(object_id, version, 0, 0);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t066-read-priority",
                            object_key,
                            object_id,
                            version,
                            "create-object-t066-read-priority",
                            fixture.payload.size(),
                            "etag-t066-read-priority"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t066-read-priority",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t066-read-priority",
                            fixture.payload.size(),
                            "etag-t066-read-priority",
                            {MakeChunkRef(identity,
                                          fixture.payload,
                                          {"replica-a", "replica-b", "replica-c"})}))
                        .Ok);

        storedemo::StorageNodeRegistry registry(
            storedemo::StorageNodeRegistryConfig{
                .stale_timeout_ms = 20,
                .dead_timeout_ms = 60});
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-a",
                         .endpoint = "127.0.0.1:7501",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kHealthy,
                             storedemo::StorageNodeDiskPressure::kLow,
                             8)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-b",
                         .endpoint = "127.0.0.1:7502",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kHealthy,
                             storedemo::StorageNodeDiskPressure::kLow,
                             1)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-c",
                         .endpoint = "127.0.0.1:7503",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kReadOnly,
                             storedemo::StorageNodeDiskPressure::kHigh,
                             0)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = fixture.payload;
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t066-read-priority",
                .object_key = object_key,
                .registry_snapshot_resolver =
                    [&registry]()
                    { return registry.Snapshot(110); }});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.read_calls(), 1U);
        ASSERT_EQ(reader.read_node_ids().size(), 1U);
        EXPECT_EQ(reader.read_node_ids().front(), "replica-b");
    }

    TEST_F(StorageReadIntegrationTest,
           RegistryFactsSkipStaleReplicaAndFallbackToNextHealthyReplicaAfterReadFailure)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t066-read-fallback",
                            "create-bucket-t066-read-fallback"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t066-fallback";
        const std::string object_key = "objects/fallback.deb";
        const std::uint64_t version = 1;
        const auto identity = MakeStoreIdentityOrThrow(object_id, version, 0, 0);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t066-read-fallback",
                            object_key,
                            object_id,
                            version,
                            "create-object-t066-read-fallback",
                            fixture.payload.size(),
                            "etag-t066-read-fallback"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t066-read-fallback",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t066-read-fallback",
                            fixture.payload.size(),
                            "etag-t066-read-fallback",
                            {MakeChunkRef(identity,
                                          fixture.payload,
                                          {"replica-a", "replica-b", "replica-c"})}))
                        .Ok);

        storedemo::StorageNodeRegistry registry(
            storedemo::StorageNodeRegistryConfig{
                .stale_timeout_ms = 20,
                .dead_timeout_ms = 60});
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-a",
                         .endpoint = "127.0.0.1:7601",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kHealthy,
                             storedemo::StorageNodeDiskPressure::kLow,
                             0)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-b",
                         .endpoint = "127.0.0.1:7602",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kHealthy,
                             storedemo::StorageNodeDiskPressure::kLow,
                             1)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-c",
                         .endpoint = "127.0.0.1:7603",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kHealthy,
                             storedemo::StorageNodeDiskPressure::kLow,
                             2)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.ReportLoad(
                        {.node_id = "replica-b",
                         .endpoint = "127.0.0.1:7602",
                         .sequence = 1,
                         .observed_at_unix_ms = 110,
                         .load = storedemo::StorageNodeRegistryLoadFacts{
                             .load = storedemo::StorageNodeLoadSnapshot{
                                 .active_reads = 0,
                                 .active_writes = 0,
                                 .queued_ops = 0},
                             .read_admission_overloaded = false}})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.ReportLoad(
                        {.node_id = "replica-c",
                         .endpoint = "127.0.0.1:7603",
                         .sequence = 1,
                         .observed_at_unix_ms = 111,
                         .load = storedemo::StorageNodeRegistryLoadFacts{
                             .load = storedemo::StorageNodeLoadSnapshot{
                                 .active_reads = 2,
                                 .active_writes = 1,
                                 .queued_ops = 0},
                             .read_admission_overloaded = false}})
                .status,
            storedemo::StorageNodeStatusCode::kOk);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                storedemo::ReadChunkResponse response;
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                if (node_id == "replica-b")
                {
                    response.status = storedemo::StorageNodeStatusCode::kTimeout;
                    response.error_detail = "replica-b timed out";
                    return response;
                }

                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = fixture.payload;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t066-read-fallback",
                .object_key = object_key,
                .registry_snapshot_resolver =
                    [&registry]()
                    { return registry.Snapshot(125); }});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), 0U);
        EXPECT_EQ(reader.calls_for_node("replica-b"), 1U);
        EXPECT_EQ(reader.calls_for_node("replica-c"), 1U);
    }

    TEST_F(StorageReadIntegrationTest, EmptyReplicaNodesFailBeforeDataPlaneRead)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t045-read-empty",
                            "create-bucket-t045-read-empty"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t045-empty";
        const std::string object_key = "objects/empty-replicas.deb";
        const std::uint64_t version = 1;
        const auto identity = MakeStoreIdentityOrThrow(object_id, version, 0, 0);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t045-read-empty",
                            object_key,
                            object_id,
                            version,
                            "create-object-t045-read-empty",
                            fixture.payload.size(),
                            "etag-t045-read-empty"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t045-read-empty",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t045-read-empty",
                            fixture.payload.size(),
                            "etag-t045-read-empty",
                            {MakeChunkRef(identity, fixture.payload, {})}))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &,
                const storedemo::ReadChunkRequest &) -> storedemo::ReadChunkResponse
            {
                ADD_FAILURE() << "empty replica_nodes should fail before data-plane read";
                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kIoError;
                response.error_detail = "unexpected read";
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t045-read-empty",
                .object_key = object_key});

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kInvalidArgument);
        EXPECT_TRUE(result.payload.empty());
        EXPECT_EQ(reader.read_calls(), 0U);
    }
} // namespace
