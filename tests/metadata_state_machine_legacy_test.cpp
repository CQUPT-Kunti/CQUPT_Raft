#include "support/metadata_test_utils.h"
#include "raft/common/metadata_command.h"
#include "raft/state_machine/metadata_state_machine.h"

#include <gtest/gtest.h>

namespace raftdemo
{
    std::string SerializeMetadataCommand(const MetadataCommand &command);
} // namespace raftdemo

namespace
{
    using raftdemo::test::MakeLegacyCommitCommand;
    using raftdemo::test::MakeLegacyCreateRecord;
} // namespace

TEST(MetadataStateMachineTest, CreateLeavesPendingInvisibleToHeadAndList)
{
    raftdemo::StrongConsistencyMetadataStateMachine machine;

    const raftdemo::MetadataCommand create =
        raftdemo::MakeCreateMetadataCommand(MakeLegacyCreateRecord("object/a", "create-1"));
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
        raftdemo::MakeCreateMetadataCommand(MakeLegacyCreateRecord("object/b", "create-2"));
    EXPECT_TRUE(machine.Apply(10, raftdemo::SerializeMetadataCommand(create)).Ok);

    const raftdemo::MetadataCommand commit =
        MakeLegacyCommitCommand("object/b", "commit-1");
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
        raftdemo::MakeCreateMetadataCommand(MakeLegacyCreateRecord("object/c", "create-3"));

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
        raftdemo::MakeCreateMetadataCommand(MakeLegacyCreateRecord("object/d", "create-4"));
    EXPECT_TRUE(machine.Apply(30, raftdemo::SerializeMetadataCommand(create)).Ok);

    const raftdemo::MetadataCommand commit =
        MakeLegacyCommitCommand("object/d", "commit-2");
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
        raftdemo::MakeCreateMetadataCommand(
            MakeLegacyCreateRecord("object/e", "create-5", "payload-a"));
    const raftdemo::MetadataCommand conflicting =
        raftdemo::MakeCreateMetadataCommand(
            MakeLegacyCreateRecord("object/e", "create-5", "payload-b"));

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
        MakeLegacyCommitCommand("object/missing", "commit-missing");
    const raftdemo::ApplyResult apply =
        machine.Apply(50, raftdemo::SerializeMetadataCommand(commit));

    EXPECT_FALSE(apply.Ok);
    EXPECT_EQ(apply.message, "not found: pending record does not exist");

    const raftdemo::MetadataHeadResponse head =
        machine.HeadMetadataRecord({.object_key = "object/missing"});
    EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(head.record.has_value());
}
