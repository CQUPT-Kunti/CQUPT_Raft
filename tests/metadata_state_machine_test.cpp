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
    raftdemo::MetadataCommand MakeCreateBucketCommand(const std::string &bucket,
                                                      const std::string &request_id,
                                                      const std::uint64_t create_time = 1710000000)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateBucket;
        command.request_id = request_id;
        command.create_bucket = raftdemo::CreateBucketCommandPayload{
            raftdemo::BucketRecord{bucket, create_time, false, std::nullopt}};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kCreateBucket,
            bucket,
            "",
            "accepted",
            0,
            create_time,
            std::nullopt};
        return command;
    }

    raftdemo::MetadataCommand MakeDeleteBucketCommand(const std::string &bucket,
                                                      const std::string &request_id,
                                                      const bool if_empty = true)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kDeleteBucket;
        command.request_id = request_id;
        command.delete_bucket = raftdemo::DeleteBucketCommandPayload{bucket, if_empty};
        return command;
    }

    raftdemo::MetadataCommand MakeCreateObjectCommand(const std::string &bucket,
                                                      const std::string &object_key,
                                                      const std::string &object_id,
                                                      const std::string &request_id)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateObject;
        command.request_id = request_id;
        command.create_object = raftdemo::CreateObjectCommandPayload{
            raftdemo::ObjectRecord{bucket,
                                   object_key,
                                   object_id,
                                   1,
                                   64,
                                   "etag-" + object_id,
                                   raftdemo::ObjectState::PENDING,
                                   {},
                                   1710000001,
                                   std::nullopt,
                                   std::nullopt}};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kCreateObject,
            bucket,
            object_key,
            "accepted",
            0,
            1710000001,
            std::nullopt};
        return command;
    }

    raftdemo::MetadataCommand MakeCommitObjectCommand(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = request_id;
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            bucket,
            object_key,
            object_id,
            1,
            512,
            "etag-commit-" + object_id,
            {raftdemo::ChunkRef{"chunk-a", 0, 256, {"node-a", "node-b"}, "checksum-a"},
             raftdemo::ChunkRef{"chunk-b", 256, 256, {"node-c"}, "checksum-b"}},
            1710000005};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kCommitObject,
            bucket,
            object_key,
            "accepted",
            0,
            1710000005,
            std::nullopt};
        return command;
    }

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
    EXPECT_EQ(apply.message, "failed to parse metadata command");
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

TEST(MetadataStateMachineTest, CreateBucketApplyCreatesBucketAndUpdatesApplyPosition)
{
    raftdemo::MetadataStateMachine machine;

    const raftdemo::ApplyResult apply = machine.Apply(
        7, raftdemo::SerializeMetadataCommand(
               MakeCreateBucketCommand("bucket-a", "create-bucket-1")));

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 7U);
    EXPECT_EQ(machine.LastAppliedTerm(), 0U);
    EXPECT_EQ(machine.BucketCount(), 1U);
    EXPECT_EQ(machine.RequestCount(), 1U);

    const std::optional<raftdemo::BucketRecord> bucket = machine.FindBucket("bucket-a");
    ASSERT_TRUE(bucket.has_value());
    EXPECT_EQ(bucket->bucket, "bucket-a");
    EXPECT_FALSE(bucket->deleted);
    EXPECT_FALSE(bucket->delete_time.has_value());
}

TEST(MetadataStateMachineTest, DeleteBucketApplyMarksBucketDeletedAndUpdatesApplyPosition)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           10,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-b", "create-bucket-2")))
                    .Ok);

    const raftdemo::ApplyResult apply = machine.Apply(
        11, raftdemo::SerializeMetadataCommand(
                MakeDeleteBucketCommand("bucket-b", "delete-bucket-1")));

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 11U);
    EXPECT_EQ(machine.LastAppliedTerm(), 0U);
    EXPECT_EQ(machine.BucketCount(), 1U);
    EXPECT_EQ(machine.RequestCount(), 2U);

    const std::optional<raftdemo::BucketRecord> bucket = machine.FindBucket("bucket-b");
    ASSERT_TRUE(bucket.has_value());
    EXPECT_TRUE(bucket->deleted);
}

TEST(MetadataStateMachineTest, RepeatedOrUnsupportedBucketCommandsReturnExplicitErrors)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           20,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-c", "create-bucket-3")))
                    .Ok);

    const raftdemo::ApplyResult duplicate_create = machine.Apply(
        21, raftdemo::SerializeMetadataCommand(
                MakeCreateBucketCommand("bucket-c", "create-bucket-4")));
    EXPECT_FALSE(duplicate_create.Ok);
    EXPECT_EQ(duplicate_create.message, "state conflict: bucket already exists");
    EXPECT_EQ(machine.LastAppliedIndex(), 20U);

    const raftdemo::ApplyResult missing_delete = machine.Apply(
        22, raftdemo::SerializeMetadataCommand(
                MakeDeleteBucketCommand("bucket-missing", "delete-bucket-2")));
    EXPECT_FALSE(missing_delete.Ok);
    EXPECT_EQ(missing_delete.message, "not found: bucket does not exist");
    EXPECT_EQ(machine.LastAppliedIndex(), 20U);

    raftdemo::MetadataCommand unsupported;
    unsupported.command_type = raftdemo::MetadataCommandType::kAbortObject;
    unsupported.request_id = "object-abort-1";
    unsupported.abort_object = raftdemo::AbortObjectCommandPayload{
        "bucket-c", "object/demo", "obj-1", 1};
    unsupported.request_context = raftdemo::RequestRecord{
        "object-abort-1",
        raftdemo::MetadataRequestType::kAbortObject,
        "bucket-c",
        "object/demo",
        "accepted",
        0,
        1710000002,
        std::nullopt};

    const raftdemo::ApplyResult unsupported_apply =
        machine.Apply(23, raftdemo::SerializeMetadataCommand(unsupported));
    EXPECT_FALSE(unsupported_apply.Ok);
    EXPECT_EQ(unsupported_apply.message, "unsupported metadata command type: abort_object");
    EXPECT_EQ(machine.LastAppliedIndex(), 20U);
}

TEST(MetadataStateMachineTest, CreateObjectApplyCreatesPendingRecordAndIndexEntry)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           30,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-d", "create-bucket-5")))
                    .Ok);

    const raftdemo::ApplyResult apply = machine.Apply(
        31, raftdemo::SerializeMetadataCommand(
                MakeCreateObjectCommand("bucket-d", "object/a", "obj-1", "create-object-1")));

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 31U);
    EXPECT_EQ(machine.LastAppliedTerm(), 0U);
    EXPECT_EQ(machine.ObjectCount(), 1U);
    EXPECT_EQ(machine.RequestCount(), 2U);

    const std::optional<raftdemo::ObjectRecord> object =
        machine.FindObject("bucket-d", "object/a");
    ASSERT_TRUE(object.has_value());
    EXPECT_EQ(object->bucket, "bucket-d");
    EXPECT_EQ(object->object_key, "object/a");
    EXPECT_EQ(object->object_id, "obj-1");
    EXPECT_EQ(object->version, 1U);
    EXPECT_EQ(object->size, 64U);
    EXPECT_TRUE(object->IsPending());

    const std::optional<std::string> indexed_id =
        machine.FindIndexedObjectId("bucket-d", "object/a");
    ASSERT_TRUE(indexed_id.has_value());
    EXPECT_EQ(*indexed_id, "obj-1");
}

TEST(MetadataStateMachineTest, CreateObjectApplyRejectsMissingOrDeletedBucketAndConflicts)
{
    raftdemo::MetadataStateMachine machine;

    const raftdemo::ApplyResult missing_bucket = machine.Apply(
        40, raftdemo::SerializeMetadataCommand(
                MakeCreateObjectCommand("bucket-missing", "object/a", "obj-2", "create-object-2")));
    EXPECT_FALSE(missing_bucket.Ok);
    EXPECT_EQ(missing_bucket.message, "not found: bucket does not exist");
    EXPECT_EQ(machine.LastAppliedIndex(), 0U);

    EXPECT_TRUE(machine.Apply(
                           41,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-e", "create-bucket-6")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           42,
                           raftdemo::SerializeMetadataCommand(
                               MakeDeleteBucketCommand("bucket-e", "delete-bucket-3")))
                    .Ok);

    const raftdemo::ApplyResult deleted_bucket = machine.Apply(
        43, raftdemo::SerializeMetadataCommand(
                MakeCreateObjectCommand("bucket-e", "object/a", "obj-3", "create-object-3")));
    EXPECT_FALSE(deleted_bucket.Ok);
    EXPECT_EQ(deleted_bucket.message, "state conflict: bucket is deleted");
    EXPECT_EQ(machine.LastAppliedIndex(), 42U);

    raftdemo::MetadataStateMachine conflict_machine;
    EXPECT_TRUE(conflict_machine.Apply(
                                   50,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateBucketCommand("bucket-f", "create-bucket-7")))
                    .Ok);
    EXPECT_TRUE(conflict_machine.Apply(
                                   51,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateObjectCommand("bucket-f", "object/a", "obj-4",
                                                               "create-object-4")))
                    .Ok);

    const raftdemo::ApplyResult duplicate_object = conflict_machine.Apply(
        52, raftdemo::SerializeMetadataCommand(
                MakeCreateObjectCommand("bucket-f", "object/a", "obj-5", "create-object-5")));
    EXPECT_FALSE(duplicate_object.Ok);
    EXPECT_EQ(duplicate_object.message, "state conflict: object already exists");
    EXPECT_EQ(conflict_machine.LastAppliedIndex(), 51U);
}

TEST(MetadataStateMachineTest, CommitObjectApplyPromotesPendingObjectAndPersistsMetadata)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           60,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-g", "create-bucket-8")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           61,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-g", "object/a", "obj-6",
                                                       "create-object-6")))
                    .Ok);

    const raftdemo::ApplyResult apply = machine.Apply(
        62, raftdemo::SerializeMetadataCommand(
                MakeCommitObjectCommand("bucket-g", "object/a", "obj-6", "commit-object-1")));

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 62U);
    EXPECT_EQ(machine.LastAppliedTerm(), 0U);
    EXPECT_EQ(machine.ObjectCount(), 1U);
    EXPECT_EQ(machine.RequestCount(), 3U);

    const std::optional<raftdemo::ObjectRecord> object =
        machine.FindObject("bucket-g", "object/a");
    ASSERT_TRUE(object.has_value());
    EXPECT_TRUE(object->IsCommitted());
    EXPECT_EQ(object->size, 512U);
    EXPECT_EQ(object->etag, "etag-commit-obj-6");
    ASSERT_TRUE(object->commit_time.has_value());
    EXPECT_EQ(*object->commit_time, 1710000005U);
    ASSERT_EQ(object->chunks.size(), 2U);
    EXPECT_EQ(object->chunks[0].chunk_id, "chunk-a");
    EXPECT_EQ(object->chunks[1].chunk_id, "chunk-b");

    const std::optional<std::vector<raftdemo::ChunkRef>> indexed_chunks =
        machine.FindChunkRefs("bucket-g", "object/a");
    ASSERT_TRUE(indexed_chunks.has_value());
    ASSERT_EQ(indexed_chunks->size(), 2U);
    EXPECT_EQ((*indexed_chunks)[0].chunk_id, "chunk-a");
    EXPECT_EQ((*indexed_chunks)[1].chunk_id, "chunk-b");
}

TEST(MetadataStateMachineTest, CommitObjectApplyRejectsMissingBucketObjectIdAndStateConflicts)
{
    raftdemo::MetadataStateMachine machine;

    const raftdemo::ApplyResult missing_bucket = machine.Apply(
        70, raftdemo::SerializeMetadataCommand(
                MakeCommitObjectCommand("bucket-missing", "object/a", "obj-7", "commit-object-2")));
    EXPECT_FALSE(missing_bucket.Ok);
    EXPECT_EQ(missing_bucket.message, "not found: bucket does not exist");
    EXPECT_EQ(machine.LastAppliedIndex(), 0U);

    EXPECT_TRUE(machine.Apply(
                           71,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-h", "create-bucket-9")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           72,
                           raftdemo::SerializeMetadataCommand(
                               MakeDeleteBucketCommand("bucket-h", "delete-bucket-4")))
                    .Ok);

    const raftdemo::ApplyResult deleted_bucket = machine.Apply(
        73, raftdemo::SerializeMetadataCommand(
                MakeCommitObjectCommand("bucket-h", "object/a", "obj-8", "commit-object-3")));
    EXPECT_FALSE(deleted_bucket.Ok);
    EXPECT_EQ(deleted_bucket.message, "state conflict: bucket is deleted");
    EXPECT_EQ(machine.LastAppliedIndex(), 72U);

    raftdemo::MetadataStateMachine missing_object_machine;
    EXPECT_TRUE(missing_object_machine.Apply(
                                   80,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateBucketCommand("bucket-i", "create-bucket-10")))
                    .Ok);

    const raftdemo::ApplyResult missing_object = missing_object_machine.Apply(
        81, raftdemo::SerializeMetadataCommand(
                MakeCommitObjectCommand("bucket-i", "object/a", "obj-9", "commit-object-4")));
    EXPECT_FALSE(missing_object.Ok);
    EXPECT_EQ(missing_object.message, "not found: object does not exist");
    EXPECT_EQ(missing_object_machine.LastAppliedIndex(), 80U);

    raftdemo::MetadataStateMachine mismatch_machine;
    EXPECT_TRUE(mismatch_machine.Apply(
                                   90,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateBucketCommand("bucket-j", "create-bucket-11")))
                    .Ok);
    EXPECT_TRUE(mismatch_machine.Apply(
                                   91,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateObjectCommand("bucket-j", "object/a", "obj-10",
                                                               "create-object-7")))
                    .Ok);

    const raftdemo::ApplyResult object_id_mismatch = mismatch_machine.Apply(
        92, raftdemo::SerializeMetadataCommand(
                MakeCommitObjectCommand("bucket-j", "object/a", "obj-11", "commit-object-5")));
    EXPECT_FALSE(object_id_mismatch.Ok);
    EXPECT_EQ(object_id_mismatch.message, "state conflict: object_id mismatch");
    EXPECT_EQ(mismatch_machine.LastAppliedIndex(), 91U);

    raftdemo::MetadataStateMachine committed_machine;
    EXPECT_TRUE(committed_machine.Apply(
                                    100,
                                    raftdemo::SerializeMetadataCommand(
                                        MakeCreateBucketCommand("bucket-k", "create-bucket-12")))
                    .Ok);
    EXPECT_TRUE(committed_machine.Apply(
                                    101,
                                    raftdemo::SerializeMetadataCommand(
                                        MakeCreateObjectCommand("bucket-k", "object/a", "obj-12",
                                                                "create-object-8")))
                    .Ok);
    EXPECT_TRUE(committed_machine.Apply(
                                    102,
                                    raftdemo::SerializeMetadataCommand(
                                        MakeCommitObjectCommand("bucket-k", "object/a", "obj-12",
                                                                "commit-object-6")))
                    .Ok);

    const raftdemo::ApplyResult already_committed = committed_machine.Apply(
        103, raftdemo::SerializeMetadataCommand(
                 MakeCommitObjectCommand("bucket-k", "object/a", "obj-12", "commit-object-7")));
    EXPECT_FALSE(already_committed.Ok);
    EXPECT_EQ(already_committed.message, "state conflict: object already committed");
    EXPECT_EQ(committed_machine.LastAppliedIndex(), 102U);
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
