#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/state_machine/metadata_state_machine.h"

#include <gtest/gtest.h>

#include <string>
#include <type_traits>
#include <utility>

namespace raftdemo
{
    std::string SerializeMetadataCommand(const MetadataCommand &command);
} // namespace raftdemo

namespace
{
    raftdemo::MetadataRecord MakeCreateRecord(const std::string &object_key,
                                              const std::string &request_id,
                                              const std::string &payload = "payload")
    {
        raftdemo::MetadataRecord record;
        record.object_key = object_key;
        record.object_size = 16;
        record.chunk_size = 8;
        record.chunk_count = 2;
        record.checksum = "checksum";
        record.mock_locations = {"node-a", "node-b"};
        record.payload = payload;
        record.create_request_id = request_id;
        return record;
    }

    raftdemo::MetadataCommand MakeCommitCommand(const std::string &object_key,
                                                const std::string &request_id,
                                                const std::string &commit_info = "commit-note")
    {
        raftdemo::MetadataCommand command;
        command.operation = raftdemo::MetadataOperation::kCommit;
        command.request_id = request_id;
        command.object_key = object_key;
        command.commit_info = commit_info;
        return command;
    }
} // namespace

TEST(MetadataStateMachineTest, SkeletonImplementsIStateMachineAndStartsEmpty)
{
    static_assert(std::is_base_of_v<raftdemo::IStateMachine, raftdemo::MetadataStateMachine>);

    raftdemo::MetadataStateMachine machine;
    EXPECT_EQ(machine.LastAppliedIndex(), 0U);
    EXPECT_EQ(machine.LastAppliedTerm(), 0U);
    EXPECT_EQ(machine.BucketCount(), 0U);
    EXPECT_EQ(machine.ObjectCount(), 0U);
    EXPECT_EQ(machine.RequestCount(), 0U);
    EXPECT_EQ(machine.TombstoneCount(), 0U);
}

TEST(MetadataStateMachineTest, SkeletonApplyAndSnapshotReturnExplicitPlaceholderResults)
{
    raftdemo::MetadataStateMachine machine;

    const raftdemo::ApplyResult apply = machine.Apply(1, "placeholder-command");
    EXPECT_FALSE(apply.Ok);
    EXPECT_EQ(apply.message, "metadata state machine skeleton not implemented");
    EXPECT_EQ(machine.LastAppliedIndex(), 0U);

    const raftdemo::SnapshotResult save_empty = machine.SaveSnapshot("");
    EXPECT_EQ(save_empty.status, raftdemo::SnapshotStatus::kInvalidArgument);

    const raftdemo::SnapshotResult save_placeholder =
        machine.SaveSnapshot("tmp/metadata-skeleton.snapshot");
    EXPECT_EQ(save_placeholder.status, raftdemo::SnapshotStatus::kInternalError);

    const raftdemo::SnapshotResult load_missing =
        machine.LoadSnapshot("tmp/non-existent-metadata-skeleton.snapshot");
    EXPECT_EQ(load_missing.status, raftdemo::SnapshotStatus::kNotFound);
}

TEST(MetadataStateMachineTest, SkeletonHeadAndListExposePlaceholderQueryBoundary)
{
    raftdemo::MetadataStateMachine machine;

    const raftdemo::MetadataHeadObjectResponse invalid_head =
        machine.HeadObject({.bucket = "", .object_key = "object-a"});
    EXPECT_EQ(invalid_head.result.code, raftdemo::MetadataStatusCode::kInvalidArgument);
    EXPECT_FALSE(invalid_head.record.has_value());

    const raftdemo::MetadataHeadObjectResponse head =
        machine.HeadObject({.bucket = "bucket-a", .object_key = "object-a"});
    EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(head.record.has_value());

    const raftdemo::MetadataListObjectsResponse invalid_list =
        machine.ListObjects({.bucket = ""});
    EXPECT_EQ(invalid_list.result.code, raftdemo::MetadataStatusCode::kInvalidArgument);

    const raftdemo::MetadataListObjectsResponse list =
        machine.ListObjects({.bucket = "bucket-a", .prefix = "object/"});
    EXPECT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
    EXPECT_TRUE(list.records.empty());
}

TEST(MetadataStateMachineTest, CreateLeavesPendingInvisibleToHeadAndList)
{
    raftdemo::StrongConsistencyMetadataStateMachine machine;

    const raftdemo::MetadataCommand create =
        raftdemo::MakeCreateMetadataCommand(MakeCreateRecord("object/a", "create-1"));
    const raftdemo::ApplyResult apply =
        machine.Apply(1, raftdemo::SerializeMetadataCommand(create));

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");

    const raftdemo::MetadataHeadResponse head =
        machine.HeadMetadataRecord({.object_key = "object/a"});
    EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(head.record.has_value());

    const raftdemo::MetadataListResponse list =
        machine.ListMetadataRecords({.prefix = "object/"});
    EXPECT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
    EXPECT_TRUE(list.records.empty());
}

TEST(MetadataStateMachineTest, CommitMakesCommittedRecordVisibleToHeadAndList)
{
    raftdemo::StrongConsistencyMetadataStateMachine machine;

    const raftdemo::MetadataCommand create =
        raftdemo::MakeCreateMetadataCommand(MakeCreateRecord("object/b", "create-2"));
    EXPECT_TRUE(machine.Apply(10, raftdemo::SerializeMetadataCommand(create)).Ok);

    const raftdemo::MetadataCommand commit =
        MakeCommitCommand("object/b", "commit-1");
    const raftdemo::ApplyResult commit_apply =
        machine.Apply(11, raftdemo::SerializeMetadataCommand(commit));
    EXPECT_TRUE(commit_apply.Ok);
    EXPECT_EQ(commit_apply.message, "ok");

    const raftdemo::MetadataHeadResponse head =
        machine.HeadMetadataRecord({.object_key = "object/b"});
    ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(head.record.has_value());
    EXPECT_EQ(head.record->state, raftdemo::MetadataRecordState::kCommitted);
    ASSERT_TRUE(head.record->commit_request_id.has_value());
    EXPECT_EQ(*head.record->commit_request_id, "commit-1");

    const raftdemo::MetadataListResponse list =
        machine.ListMetadataRecords({.prefix = "object/"});
    ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(list.records.size(), 1U);
    EXPECT_EQ(list.records[0].object_key, "object/b");
    EXPECT_EQ(list.records[0].state, raftdemo::MetadataRecordState::kCommitted);
}

TEST(MetadataStateMachineTest, DuplicateCreateWithSameRequestIdReturnsIdempotentReplay)
{
    raftdemo::StrongConsistencyMetadataStateMachine machine;

    const raftdemo::MetadataCommand create =
        raftdemo::MakeCreateMetadataCommand(MakeCreateRecord("object/c", "create-3"));

    EXPECT_TRUE(machine.Apply(20, raftdemo::SerializeMetadataCommand(create)).Ok);
    const raftdemo::ApplyResult replay =
        machine.Apply(21, raftdemo::SerializeMetadataCommand(create));

    EXPECT_TRUE(replay.Ok);
    EXPECT_EQ(replay.message, "idempotent replay");

    const raftdemo::MetadataHeadResponse head =
        machine.HeadMetadataRecord({.object_key = "object/c"});
    EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);

    const raftdemo::MetadataListResponse list =
        machine.ListMetadataRecords({.prefix = "object/"});
    EXPECT_TRUE(list.records.empty());
}

TEST(MetadataStateMachineTest, DuplicateCommitDoesNotProduceDuplicateVisibleRecord)
{
    raftdemo::StrongConsistencyMetadataStateMachine machine;

    const raftdemo::MetadataCommand create =
        raftdemo::MakeCreateMetadataCommand(MakeCreateRecord("object/d", "create-4"));
    EXPECT_TRUE(machine.Apply(30, raftdemo::SerializeMetadataCommand(create)).Ok);

    const raftdemo::MetadataCommand commit =
        MakeCommitCommand("object/d", "commit-2");
    EXPECT_TRUE(machine.Apply(31, raftdemo::SerializeMetadataCommand(commit)).Ok);
    const raftdemo::ApplyResult replay =
        machine.Apply(32, raftdemo::SerializeMetadataCommand(commit));

    EXPECT_TRUE(replay.Ok);
    EXPECT_EQ(replay.message, "idempotent replay");

    const raftdemo::MetadataHeadResponse head =
        machine.HeadMetadataRecord({.object_key = "object/d"});
    ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(head.record.has_value());
    EXPECT_EQ(head.record->state, raftdemo::MetadataRecordState::kCommitted);

    const raftdemo::MetadataListResponse list =
        machine.ListMetadataRecords({.prefix = "object/"});
    ASSERT_EQ(list.records.size(), 1U);
    EXPECT_EQ(list.records[0].object_key, "object/d");
}

TEST(MetadataStateMachineTest, SameRequestIdDifferentContentTriggersIdempotencyConflict)
{
    raftdemo::StrongConsistencyMetadataStateMachine machine;

    const raftdemo::MetadataCommand original =
        raftdemo::MakeCreateMetadataCommand(MakeCreateRecord("object/e", "create-5", "payload-a"));
    const raftdemo::MetadataCommand conflicting =
        raftdemo::MakeCreateMetadataCommand(MakeCreateRecord("object/e", "create-5", "payload-b"));

    EXPECT_TRUE(machine.Apply(40, raftdemo::SerializeMetadataCommand(original)).Ok);
    const raftdemo::ApplyResult conflict =
        machine.Apply(41, raftdemo::SerializeMetadataCommand(conflicting));

    EXPECT_FALSE(conflict.Ok);
    EXPECT_EQ(conflict.message, "idempotency conflict: request_id maps to different command");
}

TEST(MetadataStateMachineTest, MissingPendingCommitReturnsExplicitError)
{
    raftdemo::StrongConsistencyMetadataStateMachine machine;

    const raftdemo::MetadataCommand commit =
        MakeCommitCommand("object/missing", "commit-missing");
    const raftdemo::ApplyResult apply =
        machine.Apply(50, raftdemo::SerializeMetadataCommand(commit));

    EXPECT_FALSE(apply.Ok);
    EXPECT_EQ(apply.message, "not found: pending record does not exist");

    const raftdemo::MetadataHeadResponse head =
        machine.HeadMetadataRecord({.object_key = "object/missing"});
    EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(head.record.has_value());
}
