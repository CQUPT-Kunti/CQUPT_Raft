#include "raft/metadata/metadata_records.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

TEST(MetadataRecordTypesTest, ObjectStateDefaultsToPendingAndKeepsStableValues)
{
    const raftdemo::ObjectRecord record;

    EXPECT_EQ(static_cast<std::uint8_t>(raftdemo::ObjectState::PENDING), 1U);
    EXPECT_EQ(static_cast<std::uint8_t>(raftdemo::ObjectState::COMMITTED), 2U);
    EXPECT_EQ(static_cast<std::uint8_t>(raftdemo::ObjectState::DELETED), 3U);
    EXPECT_TRUE(record.IsPending());
    EXPECT_FALSE(record.IsCommitted());
    EXPECT_FALSE(record.IsDeleted());
    EXPECT_TRUE(record.chunks.empty());
    EXPECT_FALSE(record.commit_time.has_value());
    EXPECT_FALSE(record.delete_time.has_value());
}

TEST(MetadataRecordTypesTest, ChunkRefOnlyCarriesReferenceMetadata)
{
    raftdemo::ChunkRef chunk;
    chunk.chunk_id = "chunk-0001";
    chunk.offset = 4096;
    chunk.size = 8192;
    chunk.replica_nodes = {"node-a", "node-b", "node-c"};
    chunk.checksum = "sha256:chunk-0001";

    EXPECT_EQ(chunk.chunk_id, "chunk-0001");
    EXPECT_EQ(chunk.offset, 4096U);
    EXPECT_EQ(chunk.size, 8192U);
    ASSERT_EQ(chunk.replica_nodes.size(), 3U);
    EXPECT_EQ(chunk.replica_nodes[0], "node-a");
    EXPECT_EQ(chunk.replica_nodes[2], "node-c");
    EXPECT_EQ(chunk.checksum, "sha256:chunk-0001");
    EXPECT_TRUE(chunk.HasReplicaNodes());
}

TEST(MetadataRecordTypesTest, BucketAndObjectRecordsCaptureLifecycleFields)
{
    raftdemo::BucketRecord bucket;
    bucket.bucket = "photos";
    bucket.create_time = 1710000000;

    raftdemo::ObjectRecord object;
    object.bucket = bucket.bucket;
    object.object_key = "2026/cover.jpg";
    object.object_id = "obj-0007";
    object.version = 3;
    object.size = 65536;
    object.etag = "etag-v3";
    object.state = raftdemo::ObjectState::COMMITTED;
    object.create_time = 1710000001;
    object.commit_time = 1710000010;
    object.chunks = {
        raftdemo::ChunkRef{"chunk-1", 0, 32768, {"node-a", "node-b"}, "sha256:c1"},
        raftdemo::ChunkRef{"chunk-2", 32768, 32768, {"node-b", "node-c"}, "sha256:c2"}};

    EXPECT_TRUE(bucket.IsActive());
    EXPECT_FALSE(bucket.delete_time.has_value());
    EXPECT_EQ(object.bucket, "photos");
    EXPECT_EQ(object.object_key, "2026/cover.jpg");
    EXPECT_EQ(object.object_id, "obj-0007");
    EXPECT_EQ(object.version, 3U);
    EXPECT_EQ(object.size, 65536U);
    EXPECT_EQ(object.etag, "etag-v3");
    EXPECT_TRUE(object.IsCommitted());
    ASSERT_EQ(object.chunks.size(), 2U);
    EXPECT_EQ(object.chunks[1].offset, 32768U);
    EXPECT_EQ(object.commit_time.value(), 1710000010U);
}

TEST(MetadataRecordTypesTest, RequestRecordTracksIdempotencyAndApplyFacts)
{
    raftdemo::RequestRecord request;
    request.request_id = "req-42";
    request.command_type = raftdemo::MetadataRequestType::kCreateObject;
    request.bucket = "photos";
    request.object_key = "2026/cover.jpg";
    request.result_status = "committed";
    request.applied_index = 128;
    request.create_time = 1710000001;

    EXPECT_EQ(request.request_id, "req-42");
    EXPECT_EQ(request.command_type, raftdemo::MetadataRequestType::kCreateObject);
    EXPECT_EQ(request.bucket, "photos");
    EXPECT_EQ(request.object_key, "2026/cover.jpg");
    EXPECT_EQ(request.result_status, "committed");
    EXPECT_EQ(request.applied_index, 128U);
    EXPECT_FALSE(request.Finished());

    request.finish_time = 1710000011;
    EXPECT_TRUE(request.Finished());
}
