#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace raftdemo
{
    std::string SerializeMetadataCommand(const MetadataCommand &command);
    bool ParseMetadataCommand(const std::string &input, MetadataCommand *out);
    bool ValidateMetadataCommand(const MetadataCommand &command, std::string *error);
    std::string ComputeMetadataCommandFingerprint(const MetadataCommand &command);
} // namespace raftdemo

namespace
{
    raftdemo::MetadataRecord MakeValidRecord()
    {
        raftdemo::MetadataRecord record;
        record.object_key = "object/demo";
        record.object_size = 16;
        record.chunk_size = 8;
        record.chunk_count = 2;
        record.checksum = "checksum-1";
        record.mock_locations = {"node=a", "rack,1", "line\nbreak"};
        record.payload = "metadata-only-payload";
        record.create_request_id = "create-req-1";
        return record;
    }

    raftdemo::MetadataCommand MakeValidCommitCommand()
    {
        raftdemo::MetadataCommand command;
        command.operation = raftdemo::MetadataOperation::kCommit;
        command.request_id = "commit-req-1";
        command.object_key = "object/demo";
        command.commit_info = "commit-note";
        return command;
    }

    raftdemo::MetadataCommand MakeValidDeleteCommand()
    {
        raftdemo::MetadataCommand command;
        command.operation = raftdemo::MetadataOperation::kDelete;
        command.request_id = "delete-req-1";
        command.object_key = "object/demo";
        command.delete_info = "delete-note";
        return command;
    }

    raftdemo::MetadataCommand MakeCreateBucketCommand()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateBucket;
        command.request_id = "bucket-create-req-1";
        command.create_bucket = raftdemo::CreateBucketCommandPayload{
            raftdemo::BucketRecord{"bucket-a", 1710000000, false, std::nullopt}};
        command.request_context = raftdemo::RequestRecord{
            "bucket-create-req-1",
            raftdemo::MetadataRequestType::kCreateBucket,
            "bucket-a",
            "",
            "accepted",
            11,
            1710000000,
            std::nullopt};
        return command;
    }

    raftdemo::MetadataCommand MakeDeleteBucketCommand()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kDeleteBucket;
        command.request_id = "bucket-delete-req-1";
        command.delete_bucket = raftdemo::DeleteBucketCommandPayload{"bucket-a", true};
        return command;
    }

    raftdemo::MetadataCommand MakeCreateObjectV2Command()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateObject;
        command.request_id = "object-create-req-1";
        command.create_object = raftdemo::CreateObjectCommandPayload{
            raftdemo::ObjectRecord{"bucket-a",
                                   "object/demo",
                                   "obj-001",
                                   1,
                                   8192,
                                   "etag-create",
                                   raftdemo::ObjectState::PENDING,
                                   {},
                                   1710000001,
                                   std::nullopt,
                                   std::nullopt}};
        command.request_context = raftdemo::RequestRecord{
            "object-create-req-1",
            raftdemo::MetadataRequestType::kCreateObject,
            "bucket-a",
            "object/demo",
            "accepted",
            12,
            1710000001,
            std::nullopt};
        return command;
    }

    raftdemo::MetadataCommand MakeCommitObjectV2Command()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = "object-commit-req-1";
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            "bucket-a",
            "object/demo",
            "obj-001",
            2,
            8192,
            "etag-commit",
            {
                raftdemo::ChunkRef{"chunk-1", 0, 4096, {"node-a", "node-b"}, "sha256:c1"},
                raftdemo::ChunkRef{"chunk-2", 4096, 4096, {"node-b", "node-c"}, "sha256:c2"},
            },
            1710000020};
        command.request_context = raftdemo::RequestRecord{
            "object-commit-req-1",
            raftdemo::MetadataRequestType::kCommitObject,
            "bucket-a",
            "object/demo",
            "committed",
            13,
            1710000010,
            1710000020};
        return command;
    }

    raftdemo::MetadataCommand MakeAbortObjectV2Command()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kAbortObject;
        command.request_id = "object-abort-req-1";
        command.abort_object = raftdemo::AbortObjectCommandPayload{
            "bucket-a", "object/demo", "obj-001", 2};
        return command;
    }

    raftdemo::MetadataCommand MakeDeleteObjectV2Command()
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kDeleteObject;
        command.request_id = "object-delete-req-1";
        command.delete_object = raftdemo::DeleteObjectCommandPayload{
            "bucket-a", "object/demo", "obj-001", 2, 1710000030};
        return command;
    }
} // namespace

TEST(MetadataCommandTest, CreateCommandSerializeParseValidateSucceeds)
{
    const raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(MakeValidRecord());

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "");

    const std::string encoded = raftdemo::SerializeMetadataCommand(command);
    EXPECT_TRUE(encoded.rfind("META1\n", 0) == 0);

    raftdemo::MetadataCommand parsed;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(encoded, &parsed));
    error.clear();
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(parsed, &error));
    EXPECT_EQ(error, "");
    ASSERT_TRUE(parsed.record.has_value());
    EXPECT_TRUE(parsed.IsCreate());
    EXPECT_EQ(parsed.request_id, "create-req-1");
    EXPECT_EQ(parsed.object_key, "object/demo");
    EXPECT_EQ(parsed.record->state, raftdemo::MetadataRecordState::kPending);
    EXPECT_EQ(parsed.record->mock_locations.size(), 3U);
    EXPECT_EQ(parsed.record->mock_locations[0], "node=a");
    EXPECT_EQ(parsed.record->mock_locations[1], "rack,1");
    EXPECT_EQ(parsed.record->mock_locations[2], "line\nbreak");
    EXPECT_FALSE(parsed.record->IsVisibleToClients());
}

TEST(MetadataCommandTest, CommitCommandSerializeParseValidateSucceeds)
{
    const raftdemo::MetadataCommand command = MakeValidCommitCommand();

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "");

    const std::string encoded = raftdemo::SerializeMetadataCommand(command);
    raftdemo::MetadataCommand parsed;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(encoded, &parsed));
    EXPECT_TRUE(parsed.IsCommit());
    EXPECT_EQ(parsed.request_id, command.request_id);
    EXPECT_EQ(parsed.object_key, command.object_key);
    EXPECT_EQ(parsed.commit_info, command.commit_info);
    EXPECT_FALSE(parsed.record.has_value());
    error.clear();
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(parsed, &error));
    EXPECT_EQ(error, "");
}

TEST(MetadataCommandTest, DeleteCommandSerializeParseValidateSucceeds)
{
    const raftdemo::MetadataCommand command = MakeValidDeleteCommand();

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "");

    const std::string encoded = raftdemo::SerializeMetadataCommand(command);
    raftdemo::MetadataCommand parsed;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(encoded, &parsed));
    EXPECT_TRUE(parsed.IsDelete());
    EXPECT_EQ(parsed.request_id, command.request_id);
    EXPECT_EQ(parsed.object_key, command.object_key);
    EXPECT_EQ(parsed.delete_info, command.delete_info);
    EXPECT_FALSE(parsed.record.has_value());
    error.clear();
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(parsed, &error));
    EXPECT_EQ(error, "");
}

TEST(MetadataCommandTest, MissingRequestIdIsRejected)
{
    raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(MakeValidRecord());
    command.request_id.clear();

    std::string error;
    EXPECT_FALSE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "missing request_id");
}

TEST(MetadataCommandTest, EmptyObjectKeyIsRejected)
{
    raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(MakeValidRecord());
    command.object_key.clear();
    command.record->object_key.clear();

    std::string error;
    EXPECT_FALSE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "missing object_key");
}

TEST(MetadataCommandTest, PayloadOverLimitIsRejected)
{
    raftdemo::MetadataRecord record = MakeValidRecord();
    record.payload = std::string(4097, 'x');

    const raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(std::move(record));

    std::string error;
    EXPECT_FALSE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "create record payload exceeds limit");
}

TEST(MetadataCommandTest, FingerprintChangesWhenSameRequestIdCarriesDifferentContent)
{
    raftdemo::MetadataRecord left_record = MakeValidRecord();
    raftdemo::MetadataRecord right_record = MakeValidRecord();
    left_record.create_request_id = "same-request";
    right_record.create_request_id = "same-request";
    right_record.payload = "different-payload";

    const raftdemo::MetadataCommand left =
        raftdemo::MakeCreateMetadataCommand(std::move(left_record));
    const raftdemo::MetadataCommand right =
        raftdemo::MakeCreateMetadataCommand(std::move(right_record));

    EXPECT_EQ(left.request_id, right.request_id);
    EXPECT_NE(raftdemo::ComputeMetadataCommandFingerprint(left),
              raftdemo::ComputeMetadataCommandFingerprint(right));
}

TEST(MetadataCommandTest, MockLocationsParseAndValidateSucceeds)
{
    raftdemo::MetadataRecord record = MakeValidRecord();
    record.mock_locations = {"node-1", "zone=a,1", "k=v"};

    const raftdemo::MetadataCommand command =
        raftdemo::MakeCreateMetadataCommand(record);
    const std::string encoded = raftdemo::SerializeMetadataCommand(command);

    raftdemo::MetadataCommand parsed;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(encoded, &parsed));
    ASSERT_TRUE(parsed.record.has_value());
    EXPECT_EQ(parsed.record->mock_locations, record.mock_locations);

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(parsed, &error));
    EXPECT_EQ(error, "");
}

TEST(MetadataCommandTest, MetadataResultHelpersRemainLightweight)
{
    raftdemo::MetadataResponseSummary summary;
    summary.request_id = "req";
    summary.object_key = "object/demo";
    summary.term = 3;
    summary.log_index = 9;
    summary.leader_hint.leader_id = "node-1";

    const raftdemo::MetadataResult result =
        raftdemo::MakeMetadataResult(raftdemo::MetadataStatusCode::kNotLeader,
                                     summary);

    EXPECT_FALSE(result.Ok());
    EXPECT_TRUE(result.NeedsLeaderRetry());
    EXPECT_TRUE(result.summary.leader_hint.HasLeader());
    EXPECT_TRUE(result.summary.HasLogPosition());
}

TEST(MetadataCommandTest, CreateBucketCommandSerializeParseValidateSucceeds)
{
    const raftdemo::MetadataCommand command = MakeCreateBucketCommand();

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));

    const std::string encoded = raftdemo::SerializeMetadataCommand(command);
    raftdemo::MetadataCommand parsed;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(encoded, &parsed));
    ASSERT_TRUE(parsed.create_bucket.has_value());
    EXPECT_TRUE(parsed.IsCreateBucketCommand());
    EXPECT_EQ(parsed.request_id, "bucket-create-req-1");
    EXPECT_EQ(parsed.create_bucket->bucket_record.bucket, "bucket-a");
    ASSERT_TRUE(parsed.request_context.has_value());
    EXPECT_EQ(parsed.request_context->bucket, "bucket-a");
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(parsed, &error));
}

TEST(MetadataCommandTest, DeleteBucketCommandSerializeParseValidateSucceeds)
{
    const raftdemo::MetadataCommand command = MakeDeleteBucketCommand();

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));

    const std::string encoded = raftdemo::SerializeMetadataCommand(command);
    raftdemo::MetadataCommand parsed;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(encoded, &parsed));
    ASSERT_TRUE(parsed.delete_bucket.has_value());
    EXPECT_TRUE(parsed.IsDeleteBucketCommand());
    EXPECT_EQ(parsed.request_id, "bucket-delete-req-1");
    EXPECT_EQ(parsed.delete_bucket->bucket, "bucket-a");
    EXPECT_TRUE(parsed.delete_bucket->if_empty);
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(parsed, &error));
}

TEST(MetadataCommandTest, CreateObjectV2CommandSerializeParseValidateSucceeds)
{
    const raftdemo::MetadataCommand command = MakeCreateObjectV2Command();

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));

    const std::string encoded = raftdemo::SerializeMetadataCommand(command);
    raftdemo::MetadataCommand parsed;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(encoded, &parsed));
    ASSERT_TRUE(parsed.create_object.has_value());
    EXPECT_TRUE(parsed.IsCreateObjectCommand());
    EXPECT_EQ(parsed.create_object->object_record.bucket, "bucket-a");
    EXPECT_EQ(parsed.create_object->object_record.object_key, "object/demo");
    EXPECT_EQ(parsed.create_object->object_record.object_id, "obj-001");
    EXPECT_TRUE(parsed.create_object->object_record.IsPending());
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(parsed, &error));
}

TEST(MetadataCommandTest, CommitObjectV2CommandSerializeParseValidateSucceeds)
{
    const raftdemo::MetadataCommand command = MakeCommitObjectV2Command();

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));

    const std::string encoded = raftdemo::SerializeMetadataCommand(command);
    raftdemo::MetadataCommand parsed;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(encoded, &parsed));
    ASSERT_TRUE(parsed.commit_object.has_value());
    EXPECT_TRUE(parsed.IsCommitObjectCommand());
    EXPECT_EQ(parsed.request_id, "object-commit-req-1");
    EXPECT_EQ(parsed.commit_object->bucket, "bucket-a");
    EXPECT_EQ(parsed.commit_object->object_key, "object/demo");
    EXPECT_EQ(parsed.commit_object->object_id, "obj-001");
    ASSERT_EQ(parsed.commit_object->chunks.size(), 2U);
    EXPECT_EQ(parsed.commit_object->chunks[0].chunk_id, "chunk-1");
    EXPECT_EQ(parsed.commit_object->chunks[1].offset, 4096U);
    EXPECT_EQ(parsed.commit_object->chunks[1].replica_nodes[1], "node-c");
    EXPECT_TRUE(parsed.CarriesChunkRefs());
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(parsed, &error));
}

TEST(MetadataCommandTest, AbortObjectV2CommandSerializeParseValidateSucceeds)
{
    const raftdemo::MetadataCommand command = MakeAbortObjectV2Command();

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));

    const std::string encoded = raftdemo::SerializeMetadataCommand(command);
    raftdemo::MetadataCommand parsed;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(encoded, &parsed));
    ASSERT_TRUE(parsed.abort_object.has_value());
    EXPECT_TRUE(parsed.IsAbortObjectCommand());
    EXPECT_EQ(parsed.abort_object->bucket, "bucket-a");
    EXPECT_EQ(parsed.abort_object->object_key, "object/demo");
    EXPECT_EQ(parsed.abort_object->object_id, "obj-001");
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(parsed, &error));
}

TEST(MetadataCommandTest, DeleteObjectV2CommandSerializeParseValidateSucceeds)
{
    const raftdemo::MetadataCommand command = MakeDeleteObjectV2Command();

    std::string error;
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(command, &error));

    const std::string encoded = raftdemo::SerializeMetadataCommand(command);
    raftdemo::MetadataCommand parsed;
    ASSERT_TRUE(raftdemo::ParseMetadataCommand(encoded, &parsed));
    ASSERT_TRUE(parsed.delete_object.has_value());
    EXPECT_TRUE(parsed.IsDeleteObjectCommand());
    EXPECT_EQ(parsed.delete_object->bucket, "bucket-a");
    EXPECT_EQ(parsed.delete_object->object_key, "object/demo");
    EXPECT_EQ(parsed.delete_object->object_id, "obj-001");
    ASSERT_TRUE(parsed.delete_object->delete_time.has_value());
    EXPECT_EQ(parsed.delete_object->delete_time.value(), 1710000030U);
    EXPECT_TRUE(raftdemo::ValidateMetadataCommand(parsed, &error));
}

TEST(MetadataCommandTest, UnknownCommandTypeIsRejectedDuringParse)
{
    const std::string encoded =
        "META1\n"
        "operation=unknown\n"
        "request_id=req-1\n"
        "object_key=\n"
        "commit_info=\n"
        "delete_info=\n"
        "command_type=not_supported\n";

    raftdemo::MetadataCommand parsed;
    EXPECT_FALSE(raftdemo::ParseMetadataCommand(encoded, &parsed));
}

TEST(MetadataCommandTest, TruncatedCommitChunkPayloadIsRejectedDuringParse)
{
    const std::string encoded =
        "META1\n"
        "operation=commit\n"
        "request_id=req-1\n"
        "object_key=object/demo\n"
        "commit_info=\n"
        "delete_info=\n"
        "command_type=commit_object\n"
        "target_bucket=bucket-a\n"
        "target_object_key=object/demo\n"
        "target_object_id=obj-001\n"
        "target_version=2\n"
        "target_size=8192\n"
        "target_etag=etag\n"
        "target_chunk_count=1\n"
        "target_chunk_0_id=chunk-1\n"
        "target_chunk_0_offset=0\n"
        "target_chunk_0_size=4096\n"
        "target_chunk_0_replicas=node-a\\cnode-b\n";

    raftdemo::MetadataCommand parsed;
    EXPECT_FALSE(raftdemo::ParseMetadataCommand(encoded, &parsed));
}

TEST(MetadataCommandTest, CorruptedCommitChunkListIsRejectedDuringParse)
{
    const std::string encoded =
        "META1\n"
        "operation=commit\n"
        "request_id=req-1\n"
        "object_key=object/demo\n"
        "commit_info=\n"
        "delete_info=\n"
        "command_type=commit_object\n"
        "target_bucket=bucket-a\n"
        "target_object_key=object/demo\n"
        "target_object_id=obj-001\n"
        "target_version=2\n"
        "target_size=8192\n"
        "target_etag=etag\n"
        "target_chunk_count=1\n"
        "target_chunk_0_id=chunk-1\n"
        "target_chunk_0_offset=broken\n"
        "target_chunk_0_size=4096\n"
        "target_chunk_0_replicas=node-a\\cnode-b\n"
        "target_chunk_0_checksum=sha256:c1\n";

    raftdemo::MetadataCommand parsed;
    EXPECT_FALSE(raftdemo::ParseMetadataCommand(encoded, &parsed));
}

TEST(MetadataCommandTest, RequestContextCommandTypeMismatchIsRejected)
{
    raftdemo::MetadataCommand command = MakeCommitObjectV2Command();
    ASSERT_TRUE(command.request_context.has_value());
    command.request_context->command_type = raftdemo::MetadataRequestType::kDeleteObject;

    std::string error;
    EXPECT_FALSE(raftdemo::ValidateMetadataCommand(command, &error));
    EXPECT_EQ(error, "request_context command_type mismatch");
}
