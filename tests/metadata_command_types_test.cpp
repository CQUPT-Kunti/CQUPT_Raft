#include "raft/common/metadata_command.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
    raftdemo::ChunkRef MakeChunk(std::string id, std::uint64_t offset)
    {
        raftdemo::ChunkRef chunk;
        chunk.chunk_id = std::move(id);
        chunk.offset = offset;
        chunk.size = 4096;
        chunk.replica_nodes = {"node-a", "node-b"};
        chunk.checksum = "sha256:chunk";
        return chunk;
    }
}

TEST(MetadataCommandTypesTest, EnumValuesCoverAllWriteCommandKinds)
{
    EXPECT_EQ(static_cast<std::uint8_t>(raftdemo::MetadataCommandType::kCreateBucket), 1U);
    EXPECT_EQ(static_cast<std::uint8_t>(raftdemo::MetadataCommandType::kDeleteBucket), 2U);
    EXPECT_EQ(static_cast<std::uint8_t>(raftdemo::MetadataCommandType::kCreateObject), 3U);
    EXPECT_EQ(static_cast<std::uint8_t>(raftdemo::MetadataCommandType::kCommitObject), 4U);
    EXPECT_EQ(static_cast<std::uint8_t>(raftdemo::MetadataCommandType::kAbortObject), 5U);
    EXPECT_EQ(static_cast<std::uint8_t>(raftdemo::MetadataCommandType::kDeleteObject), 6U);
}

TEST(MetadataCommandTypesTest, CreateBucketCommandKeepsRequestIdAndBucketRecord)
{
    raftdemo::MetadataCommand command;
    command.command_type = raftdemo::MetadataCommandType::kCreateBucket;
    command.request_id = "req-create-bucket";
    command.create_bucket = raftdemo::CreateBucketCommandPayload{
        raftdemo::BucketRecord{"photos", 1710000000, false, std::nullopt}};
    command.request_context = raftdemo::RequestRecord{
        "req-create-bucket",
        raftdemo::MetadataRequestType::kCreateBucket,
        "photos",
        "",
        "accepted",
        0,
        1710000000,
        std::nullopt};

    EXPECT_TRUE(command.IsWriteCommand());
    EXPECT_TRUE(command.IsCreateBucketCommand());
    ASSERT_TRUE(command.create_bucket.has_value());
    EXPECT_EQ(command.request_id, "req-create-bucket");
    EXPECT_EQ(command.create_bucket->bucket_record.bucket, "photos");
    ASSERT_TRUE(command.request_context.has_value());
    EXPECT_EQ(command.request_context->request_id, command.request_id);
}

TEST(MetadataCommandTypesTest, CreateAndDeleteObjectCommandsUseObjectIdentityPayloads)
{
    raftdemo::MetadataCommand create_command;
    create_command.command_type = raftdemo::MetadataCommandType::kCreateObject;
    create_command.request_id = "req-create-object";
    create_command.create_object = raftdemo::CreateObjectCommandPayload{
        raftdemo::ObjectRecord{"photos",
                               "2026/cover.jpg",
                               "obj-9",
                               1,
                               8192,
                               "etag-1",
                               raftdemo::ObjectState::PENDING,
                               {},
                               1710000001,
                               std::nullopt,
                               std::nullopt}};

    raftdemo::MetadataCommand delete_command;
    delete_command.command_type = raftdemo::MetadataCommandType::kDeleteObject;
    delete_command.request_id = "req-delete-object";
    delete_command.delete_object = raftdemo::DeleteObjectCommandPayload{
        "photos", "2026/cover.jpg", "obj-9", 1, 1710000099};

    EXPECT_TRUE(create_command.IsCreateObjectCommand());
    ASSERT_TRUE(create_command.create_object.has_value());
    EXPECT_EQ(create_command.create_object->object_record.object_id, "obj-9");
    EXPECT_TRUE(create_command.create_object->object_record.IsPending());

    EXPECT_TRUE(delete_command.IsDeleteObjectCommand());
    ASSERT_TRUE(delete_command.delete_object.has_value());
    EXPECT_EQ(delete_command.delete_object->bucket, "photos");
    EXPECT_EQ(delete_command.delete_object->object_key, "2026/cover.jpg");
    ASSERT_TRUE(delete_command.delete_object->delete_time.has_value());
}

TEST(MetadataCommandTypesTest, CommitObjectCommandCarriesChunkRefs)
{
    raftdemo::MetadataCommand command;
    command.command_type = raftdemo::MetadataCommandType::kCommitObject;
    command.request_id = "req-commit-object";
    command.commit_object = raftdemo::CommitObjectCommandPayload{
        "photos",
        "2026/cover.jpg",
        "obj-9",
        2,
        8192,
        "etag-commit",
        {MakeChunk("chunk-1", 0), MakeChunk("chunk-2", 4096)},
        1710000010};

    EXPECT_TRUE(command.IsCommitObjectCommand());
    EXPECT_TRUE(command.CarriesChunkRefs());
    ASSERT_TRUE(command.commit_object.has_value());
    EXPECT_EQ(command.commit_object->version, 2U);
    EXPECT_EQ(command.commit_object->etag, "etag-commit");
    ASSERT_EQ(command.commit_object->chunks.size(), 2U);
    EXPECT_EQ(command.commit_object->chunks[1].offset, 4096U);
}

TEST(MetadataCommandTypesTest, AbortObjectCommandPreservesRequestIdForIdempotency)
{
    raftdemo::MetadataCommand command;
    command.command_type = raftdemo::MetadataCommandType::kAbortObject;
    command.request_id = "req-abort-object";
    command.abort_object = raftdemo::AbortObjectCommandPayload{
        "photos", "2026/cover.jpg", "obj-9", 2};

    EXPECT_TRUE(command.IsAbortObjectCommand());
    ASSERT_TRUE(command.abort_object.has_value());
    EXPECT_EQ(command.request_id, "req-abort-object");
    EXPECT_EQ(command.abort_object->object_id, "obj-9");
    EXPECT_EQ(command.abort_object->version, 2U);
}

TEST(MetadataCommandTypesTest, HeadAndListQueriesRemainReadOnlyModels)
{
    raftdemo::HeadObjectQuery head_query;
    head_query.bucket = "photos";
    head_query.object_key = "2026/cover.jpg";
    head_query.object_id = "obj-9";
    head_query.version = 2;

    raftdemo::ListObjectsQuery list_query;
    list_query.bucket = "photos";
    list_query.prefix = "2026/";
    list_query.limit = static_cast<std::size_t>(50);
    list_query.continuation_token = "page-2";
    list_query.include_deleted = false;

    raftdemo::MetadataCommand command;

    EXPECT_FALSE(command.IsWriteCommand());
    EXPECT_EQ(head_query.bucket, "photos");
    EXPECT_EQ(head_query.object_key, "2026/cover.jpg");
    ASSERT_TRUE(head_query.version.has_value());
    EXPECT_EQ(head_query.version.value(), 2U);
    ASSERT_TRUE(list_query.limit.has_value());
    EXPECT_EQ(list_query.limit.value(), 50U);
    EXPECT_FALSE(list_query.include_deleted);
}
