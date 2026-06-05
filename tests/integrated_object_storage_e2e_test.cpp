#include "raft/common/metadata_command.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "support/metadata_test_utils.h"

#include "metadata.pb.h"
#include "storage_node.pb.h"

#include <google/protobuf/descriptor.h>
#include <gtest/gtest.h>

#include <cstdint>
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

    std::vector<raftdemo::ChunkRef> MakeAuditChunks()
    {
        return {
            raftdemo::ChunkRef{
                "chunk-t022-0", 0, 4096, {"store-a", "store-b"}, "sha256:chunk-t022-0"},
            raftdemo::ChunkRef{
                "chunk-t022-1", 4096, 2048, {"store-b"}, "sha256:chunk-t022-1"}};
    }

    raftdemo::MetadataCommand MakeCreateObjectAuditCommand()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateObject;
        command.request_id = "create-object-t022";
        command.create_object = raftdemo::CreateObjectCommandPayload{
            raftdemo::ObjectRecord{"bucket-t022",
                                   "objects/boundary-audit.bin",
                                   "obj-t022",
                                   3,
                                   6144,
                                   "sha256:object-t022",
                                   raftdemo::ObjectState::PENDING,
                                   {},
                                   1717555200001ULL,
                                   std::nullopt,
                                   std::nullopt}};
        command.request_context = raftdemo::RequestRecord{
            "create-object-t022",
            raftdemo::MetadataRequestType::kCreateObject,
            "bucket-t022",
            "objects/boundary-audit.bin",
            "accepted",
            0,
            1717555200001ULL,
            std::nullopt};
        return command;
    }

    raftdemo::MetadataCommand MakeCommitObjectAuditCommand()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = "commit-object-t022";
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            "bucket-t022",
            "objects/boundary-audit.bin",
            "obj-t022",
            3,
            6144,
            "sha256:object-t022",
            MakeAuditChunks(),
            1717555200999ULL};
        command.request_context = raftdemo::RequestRecord{
            "commit-object-t022",
            raftdemo::MetadataRequestType::kCommitObject,
            "bucket-t022",
            "objects/boundary-audit.bin",
            "accepted",
            0,
            1717555200001ULL,
            1717555200999ULL};
        return command;
    }

    void ExpectChunkRefsEqual(const std::vector<raftdemo::ChunkRef> &actual,
                              const std::vector<raftdemo::ChunkRef> &expected)
    {
        ASSERT_EQ(actual.size(), expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index)
        {
            EXPECT_EQ(actual[index].chunk_id, expected[index].chunk_id);
            EXPECT_EQ(actual[index].offset, expected[index].offset);
            EXPECT_EQ(actual[index].size, expected[index].size);
            EXPECT_EQ(actual[index].replica_nodes, expected[index].replica_nodes);
            EXPECT_EQ(actual[index].checksum, expected[index].checksum);
        }
    }
} // namespace

TEST(IntegratedObjectStorageE2ETest,
     PayloadBoundaryAuditMetadataControlPlaneDescriptorsExcludeRawPayloadBytes)
{
    ASSERT_NE(raft::CreateObjectRequest::descriptor(), nullptr);
    ASSERT_NE(raft::CommitObjectRequest::descriptor(), nullptr);
    ASSERT_NE(raft::ChunkRef::descriptor(), nullptr);
    ASSERT_NE(raft::ObjectRecord::descriptor(), nullptr);
    ASSERT_NE(raft::HeadObjectResponse::descriptor(), nullptr);
    ASSERT_NE(raft::ListObjectsResponse::descriptor(), nullptr);

    EXPECT_EQ(raft::CreateObjectRequest::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::CommitObjectRequest::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::ChunkRef::descriptor()->FindFieldByName("payload"), nullptr);
    EXPECT_EQ(raft::ObjectRecord::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::HeadObjectResponse::descriptor()->FindFieldByName("payload"),
              nullptr);
    EXPECT_EQ(raft::ListObjectsResponse::descriptor()->FindFieldByName("payload"),
              nullptr);

    EXPECT_FALSE(DescriptorHasBytesField(*raft::CreateObjectRequest::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::CommitObjectRequest::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::ChunkRef::descriptor()));
    EXPECT_FALSE(DescriptorHasBytesField(*raft::ObjectRecord::descriptor()));
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

TEST(IntegratedObjectStorageE2ETest,
     PayloadBoundaryAuditMetadataCommandsSerializeOnlyManifestFacts)
{
    const raftdemo::MetadataCommand create_command = MakeCreateObjectAuditCommand();
    const raftdemo::MetadataCommand commit_command = MakeCommitObjectAuditCommand();

    std::string error;
    ASSERT_TRUE(raftdemo::ValidateMetadataCommand(create_command, &error)) << error;
    error.clear();
    ASSERT_TRUE(raftdemo::ValidateMetadataCommand(commit_command, &error)) << error;

    const std::string create_encoded =
        raftdemo::SerializeMetadataCommand(create_command);
    const std::string commit_encoded =
        raftdemo::SerializeMetadataCommand(commit_command);

    EXPECT_EQ(create_encoded.find("record_payload="), std::string::npos);
    EXPECT_EQ(commit_encoded.find("record_payload="), std::string::npos);
    EXPECT_EQ(create_encoded.find("payload"), std::string::npos);
    EXPECT_EQ(commit_encoded.find("payload"), std::string::npos);

    EXPECT_NE(create_encoded.find("target_bucket=bucket-t022"), std::string::npos);
    EXPECT_NE(create_encoded.find("target_object_id=obj-t022"), std::string::npos);
    EXPECT_NE(create_encoded.find("target_size=6144"), std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_count=2"), std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_0_id=chunk-t022-0"),
              std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_1_offset=4096"),
              std::string::npos);
    EXPECT_NE(commit_encoded.find("target_chunk_1_checksum=sha256:chunk-t022-1"),
              std::string::npos);

    raftdemo::MetadataCommand parsed_commit;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(commit_encoded, &parsed_commit));
    ASSERT_TRUE(parsed_commit.commit_object.has_value());
    EXPECT_EQ(parsed_commit.commit_object->bucket, "bucket-t022");
    EXPECT_EQ(parsed_commit.commit_object->object_key,
              "objects/boundary-audit.bin");
    EXPECT_EQ(parsed_commit.commit_object->object_id, "obj-t022");
    EXPECT_EQ(parsed_commit.commit_object->size, 6144U);
    ExpectChunkRefsEqual(parsed_commit.commit_object->chunks, MakeAuditChunks());
}

TEST(IntegratedObjectStorageE2ETest,
     PayloadBoundaryAuditMetadataSnapshotRoundTripKeepsManifestFactsOnly)
{
    raftdemo::MetadataStateMachine machine;
    const std::vector<raftdemo::ChunkRef> expected_chunks = MakeAuditChunks();

    std::uint64_t index = 1;
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateBucketCommand("bucket-t022",
                                                             "create-bucket-t022"))
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCreateObjectAuditCommand())
                    .Ok);
    ASSERT_TRUE(ApplyMetadataCommand(machine,
                                     index++,
                                     MakeCommitObjectAuditCommand())
                    .Ok);

    const std::filesystem::path snapshot_path =
        MakeSnapshotPath("t022-payload-boundary.snapshot");
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);

    const auto save = machine.SaveSnapshot(snapshot_path.string());
    ASSERT_EQ(save.status, raftdemo::SnapshotStatus::kOk) << save.message;

    const std::vector<char> snapshot_bytes = ReadBinaryFile(snapshot_path);
    const std::string snapshot_text(snapshot_bytes.begin(), snapshot_bytes.end());
    EXPECT_EQ(snapshot_text.find("record_payload="), std::string::npos);
    EXPECT_EQ(snapshot_text.find("payload"), std::string::npos);
    EXPECT_NE(snapshot_text.find("chunk-t022-0"), std::string::npos);
    EXPECT_NE(snapshot_text.find("chunk-t022-1"), std::string::npos);
    EXPECT_NE(snapshot_text.find("sha256:chunk-t022-1"), std::string::npos);

    raftdemo::MetadataStateMachine restored;
    const auto load = restored.LoadSnapshot(snapshot_path.string());
    ASSERT_EQ(load.status, raftdemo::SnapshotStatus::kOk) << load.message;

    const auto head = restored.HeadObject(
        {.bucket = "bucket-t022", .object_key = "objects/boundary-audit.bin"});
    ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(head.record.has_value());
    EXPECT_TRUE(head.record->IsCommitted());
    EXPECT_EQ(head.record->object_id, "obj-t022");
    EXPECT_EQ(head.record->version, 3U);
    EXPECT_EQ(head.record->size, 6144U);
    EXPECT_EQ(head.record->etag, "sha256:object-t022");
    ExpectChunkRefsEqual(head.record->chunks, expected_chunks);

    const auto chunk_refs = restored.FindChunkRefs("bucket-t022",
                                                   "objects/boundary-audit.bin");
    ASSERT_TRUE(chunk_refs.has_value());
    ExpectChunkRefsEqual(*chunk_refs, expected_chunks);
}
