#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"

#include <gtest/gtest.h>

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
