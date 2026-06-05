#include "raft/state_machine/metadata_state_machine.h"
#include "support/metadata_test_utils.h"

#include "common.pb.h"
#include "metadata.pb.h"
#include "storage_node.pb.h"

#include <google/protobuf/descriptor.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using raftdemo::test::ApplyMetadataCommand;
    using raftdemo::test::MakeCreateBucketCommand;
    using raftdemo::test::MakeSnapshotPath;

    bool DescriptorHasBytesField(const google::protobuf::Descriptor &descriptor)
    {
        for (int index = 0; index < descriptor.field_count(); ++index)
        {
            if (descriptor.field(index)->type() ==
                google::protobuf::FieldDescriptor::TYPE_BYTES)
            {
                return true;
            }
        }
        return false;
    }

    std::vector<char> ReadBinaryFile(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open snapshot file: " + path.string());
        }

        return std::vector<char>(std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>());
    }

    void ExpectChunkRefEquals(const raftdemo::ChunkRef &actual,
                              const raftdemo::ChunkRef &expected)
    {
        EXPECT_EQ(actual.chunk_id, expected.chunk_id);
        EXPECT_EQ(actual.offset, expected.offset);
        EXPECT_EQ(actual.size, expected.size);
        EXPECT_EQ(actual.replica_nodes, expected.replica_nodes);
        EXPECT_EQ(actual.checksum, expected.checksum);
    }

    void ExpectChunkRefVectorEquals(
        const std::vector<raftdemo::ChunkRef> &actual,
        const std::vector<raftdemo::ChunkRef> &expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t index = 0; index < actual.size(); ++index)
        {
            ExpectChunkRefEquals(actual.at(index), expected.at(index));
        }
    }

    raftdemo::MetadataCommand MakeCreateObjectCommandWithSize(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::uint64_t version,
        const std::string &request_id,
        const std::uint64_t size,
        const std::string &etag,
        const std::uint64_t create_time = 1713000001)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateObject;
        command.request_id = request_id;
        command.create_object = raftdemo::CreateObjectCommandPayload{
            raftdemo::ObjectRecord{bucket,
                                   object_key,
                                   object_id,
                                   version,
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
        const std::uint64_t version,
        const std::string &request_id,
        const std::uint64_t size,
        const std::string &etag,
        std::vector<raftdemo::ChunkRef> chunks,
        const std::uint64_t commit_time = 1713000002)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = request_id;
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            bucket,
            object_key,
            object_id,
            version,
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
} // namespace

TEST(MetadataManifestTest, MetadataProtoCarriesChunkRefsWithoutPayloadBytes)
{
    ASSERT_NE(raft::ChunkRef::descriptor(), nullptr);
    ASSERT_NE(raft::ObjectRecord::descriptor(), nullptr);
    ASSERT_NE(raft::CommitObjectRequest::descriptor(), nullptr);
    ASSERT_NE(raft::HeadObjectResponse::descriptor(), nullptr);
    ASSERT_NE(raft::ListObjectsResponse::descriptor(), nullptr);

    EXPECT_EQ(raft::ChunkRef::descriptor()->field_count(), 5);
    EXPECT_EQ(raft::ObjectRecord::descriptor()->field_count(), 11);
    EXPECT_EQ(raft::CommitObjectRequest::descriptor()->field_count(), 9);

    EXPECT_EQ(raft::ChunkRef::descriptor()->FindFieldByName("payload"), nullptr);
    EXPECT_EQ(raft::ObjectRecord::descriptor()->FindFieldByName("payload"), nullptr);
    EXPECT_EQ(raft::CommitObjectRequest::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::HeadObjectResponse::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::ListObjectsResponse::descriptor()->FindFieldByName("payload"),
              nullptr);

    EXPECT_FALSE(DescriptorHasBytesField(*raft::ChunkRef::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::ObjectRecord::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::CommitObjectRequest::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::HeadObjectResponse::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::ListObjectsResponse::descriptor()));

    ASSERT_NE(storage::WriteChunkRequest::descriptor(), nullptr);
    ASSERT_NE(storage::ReadChunkResponse::descriptor(), nullptr);
    EXPECT_NE(storage::WriteChunkRequest::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_NE(storage::ReadChunkResponse::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_TRUE(DescriptorHasBytesField(*storage::WriteChunkRequest::descriptor()));
    EXPECT_TRUE(DescriptorHasBytesField(*storage::ReadChunkResponse::descriptor()));
}

TEST(MetadataManifestTest,
     MetadataStateMachineSnapshotRoundTripPreservesChunkRefsWithoutPayloadMarker)
{
    raftdemo::MetadataStateMachine machine;

    const std::string payload_marker =
        "T095_PAYLOAD_MUST_STAY_OUT_OF_METADATA_SNAPSHOT";
    const std::vector<raftdemo::ChunkRef> expected_chunks{
        raftdemo::ChunkRef{
            "chunk-t095-0", 0, 128, {"node-a", "node-b"}, "sha256:chunk-a"},
        raftdemo::ChunkRef{
            "chunk-t095-1", 128, 96, {"node-c"}, "sha256:chunk-b"}};

    std::uint64_t index = 1;
    ASSERT_TRUE(ApplyMetadataCommand(
                    machine,
                    index++,
                    MakeCreateBucketCommand("bucket-t095-manifest",
                                            "create-bucket-t095-manifest"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(
                    machine,
                    index++,
                    MakeCreateObjectCommandWithSize("bucket-t095-manifest",
                                                    "objects/manifest-only.bin",
                                                    "obj-t095-manifest",
                                                    7,
                                                    "create-object-t095-manifest",
                                                    224,
                                                    "etag-t095-manifest"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(
                    machine,
                    index++,
                    MakeCommitObjectCommandWithChunks("bucket-t095-manifest",
                                                      "objects/manifest-only.bin",
                                                      "obj-t095-manifest",
                                                      7,
                                                      "commit-object-t095-manifest",
                                                      224,
                                                      "etag-t095-manifest",
                                                      expected_chunks))
                    .Ok);

    const std::filesystem::path snapshot_path =
        MakeSnapshotPath("t095-manifest-chunkref.snapshot");
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);

    const auto save = machine.SaveSnapshot(snapshot_path.string());
    ASSERT_EQ(save.status, raftdemo::SnapshotStatus::kOk) << save.message;

    const std::vector<char> snapshot_bytes = ReadBinaryFile(snapshot_path);
    const std::string snapshot_text(snapshot_bytes.begin(), snapshot_bytes.end());
    EXPECT_EQ(snapshot_text.find(payload_marker), std::string::npos);

    raftdemo::MetadataStateMachine restored;
    const auto load = restored.LoadSnapshot(snapshot_path.string());
    ASSERT_EQ(load.status, raftdemo::SnapshotStatus::kOk) << load.message;

    const auto head = restored.HeadObject(
        {.bucket = "bucket-t095-manifest", .object_key = "objects/manifest-only.bin"});
    ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(head.record.has_value());
    EXPECT_TRUE(head.record->IsCommitted());
    EXPECT_EQ(head.record->object_id, "obj-t095-manifest");
    EXPECT_EQ(head.record->version, 7U);
    EXPECT_EQ(head.record->size, 224U);
    ExpectChunkRefVectorEquals(head.record->chunks, expected_chunks);

    const auto indexed_chunks = restored.FindChunkRefs("bucket-t095-manifest",
                                                       "objects/manifest-only.bin");
    ASSERT_TRUE(indexed_chunks.has_value());
    ExpectChunkRefVectorEquals(*indexed_chunks, expected_chunks);
}
