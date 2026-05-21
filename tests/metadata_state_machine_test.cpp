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

    raftdemo::MetadataCommand MakeAbortObjectCommand(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kAbortObject;
        command.request_id = request_id;
        command.abort_object = raftdemo::AbortObjectCommandPayload{
            bucket,
            object_key,
            object_id,
            1};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kAbortObject,
            bucket,
            object_key,
            "accepted",
            0,
            1710000006,
            std::nullopt};
        return command;
    }

    raftdemo::MetadataCommand MakeDeleteObjectCommand(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kDeleteObject;
        command.request_id = request_id;
        command.delete_object = raftdemo::DeleteObjectCommandPayload{
            bucket,
            object_key,
            object_id,
            1,
            1710000007};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kDeleteObject,
            bucket,
            object_key,
            "accepted",
            0,
            1710000007,
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

    raftdemo::MetadataCommand invalid;
    invalid.command_type = raftdemo::MetadataCommandType::kDeleteObject;
    invalid.request_id = "object-delete-1";
    invalid.delete_object = raftdemo::DeleteObjectCommandPayload{
        "bucket-c",
        "object/demo",
        "",
        1,
        1710000002};
    invalid.request_context = raftdemo::RequestRecord{
        "object-delete-1",
        raftdemo::MetadataRequestType::kDeleteObject,
        "bucket-c",
        "object/demo",
        "accepted",
        0,
        1710000002,
        std::nullopt};

    const raftdemo::ApplyResult invalid_apply =
        machine.Apply(23, raftdemo::SerializeMetadataCommand(invalid));
    EXPECT_FALSE(invalid_apply.Ok);
    EXPECT_EQ(invalid_apply.message,
              "invalid metadata command: delete_object command missing object_id");
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

TEST(MetadataStateMachineTest, AbortObjectApplyMarksPendingObjectDeletedAndHidesIt)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           110,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-l", "create-bucket-13")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           111,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-l", "object/a", "obj-13",
                                                       "create-object-9")))
                    .Ok);

    const raftdemo::ApplyResult apply = machine.Apply(
        112, raftdemo::SerializeMetadataCommand(
                 MakeAbortObjectCommand("bucket-l", "object/a", "obj-13", "abort-object-1")));

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 112U);
    EXPECT_EQ(machine.LastAppliedTerm(), 0U);
    EXPECT_EQ(machine.RequestCount(), 3U);
    EXPECT_EQ(machine.TombstoneCount(), 1U);

    const std::optional<raftdemo::ObjectRecord> object =
        machine.FindObject("bucket-l", "object/a");
    ASSERT_TRUE(object.has_value());
    EXPECT_TRUE(object->IsDeleted());
    ASSERT_TRUE(object->delete_time.has_value());
    EXPECT_EQ(*object->delete_time, 1710000006U);

    const std::optional<std::string> indexed_id =
        machine.FindIndexedObjectId("bucket-l", "object/a");
    EXPECT_FALSE(indexed_id.has_value());

    const std::optional<std::vector<raftdemo::ChunkRef>> indexed_chunks =
        machine.FindChunkRefs("bucket-l", "object/a");
    EXPECT_FALSE(indexed_chunks.has_value());

    const raftdemo::MetadataHeadObjectResponse head =
        machine.HeadObject({.bucket = "bucket-l", .object_key = "object/a"});
    EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(head.record.has_value());
}

TEST(MetadataStateMachineTest, AbortObjectApplyRejectsMissingBucketObjectIdAndStateConflicts)
{
    raftdemo::MetadataStateMachine machine;

    const raftdemo::ApplyResult missing_bucket = machine.Apply(
        120, raftdemo::SerializeMetadataCommand(
                 MakeAbortObjectCommand("bucket-missing", "object/a", "obj-14", "abort-object-2")));
    EXPECT_FALSE(missing_bucket.Ok);
    EXPECT_EQ(missing_bucket.message, "not found: bucket does not exist");
    EXPECT_EQ(machine.LastAppliedIndex(), 0U);

    EXPECT_TRUE(machine.Apply(
                           121,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-m", "create-bucket-14")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           122,
                           raftdemo::SerializeMetadataCommand(
                               MakeDeleteBucketCommand("bucket-m", "delete-bucket-5")))
                    .Ok);

    const raftdemo::ApplyResult deleted_bucket = machine.Apply(
        123, raftdemo::SerializeMetadataCommand(
                 MakeAbortObjectCommand("bucket-m", "object/a", "obj-15", "abort-object-3")));
    EXPECT_FALSE(deleted_bucket.Ok);
    EXPECT_EQ(deleted_bucket.message, "state conflict: bucket is deleted");
    EXPECT_EQ(machine.LastAppliedIndex(), 122U);

    raftdemo::MetadataStateMachine missing_object_machine;
    EXPECT_TRUE(missing_object_machine.Apply(
                                   130,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateBucketCommand("bucket-n", "create-bucket-15")))
                    .Ok);

    const raftdemo::ApplyResult missing_object = missing_object_machine.Apply(
        131, raftdemo::SerializeMetadataCommand(
                 MakeAbortObjectCommand("bucket-n", "object/a", "obj-16", "abort-object-4")));
    EXPECT_FALSE(missing_object.Ok);
    EXPECT_EQ(missing_object.message, "not found: object does not exist");
    EXPECT_EQ(missing_object_machine.LastAppliedIndex(), 130U);

    raftdemo::MetadataStateMachine mismatch_machine;
    EXPECT_TRUE(mismatch_machine.Apply(
                                   140,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateBucketCommand("bucket-o", "create-bucket-16")))
                    .Ok);
    EXPECT_TRUE(mismatch_machine.Apply(
                                   141,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateObjectCommand("bucket-o", "object/a", "obj-17",
                                                               "create-object-10")))
                    .Ok);

    const raftdemo::ApplyResult object_id_mismatch = mismatch_machine.Apply(
        142, raftdemo::SerializeMetadataCommand(
                 MakeAbortObjectCommand("bucket-o", "object/a", "obj-18", "abort-object-5")));
    EXPECT_FALSE(object_id_mismatch.Ok);
    EXPECT_EQ(object_id_mismatch.message, "state conflict: object_id mismatch");
    EXPECT_EQ(mismatch_machine.LastAppliedIndex(), 141U);

    raftdemo::MetadataStateMachine committed_machine;
    EXPECT_TRUE(committed_machine.Apply(
                                    150,
                                    raftdemo::SerializeMetadataCommand(
                                        MakeCreateBucketCommand("bucket-p", "create-bucket-17")))
                    .Ok);
    EXPECT_TRUE(committed_machine.Apply(
                                    151,
                                    raftdemo::SerializeMetadataCommand(
                                        MakeCreateObjectCommand("bucket-p", "object/a", "obj-19",
                                                                "create-object-11")))
                    .Ok);
    EXPECT_TRUE(committed_machine.Apply(
                                    152,
                                    raftdemo::SerializeMetadataCommand(
                                        MakeCommitObjectCommand("bucket-p", "object/a", "obj-19",
                                                                "commit-object-8")))
                    .Ok);

    const raftdemo::ApplyResult committed_abort = committed_machine.Apply(
        153, raftdemo::SerializeMetadataCommand(
                 MakeAbortObjectCommand("bucket-p", "object/a", "obj-19", "abort-object-6")));
    EXPECT_FALSE(committed_abort.Ok);
    EXPECT_EQ(committed_abort.message, "state conflict: object already committed");
    EXPECT_EQ(committed_machine.LastAppliedIndex(), 152U);

    raftdemo::MetadataStateMachine aborted_machine;
    EXPECT_TRUE(aborted_machine.Apply(
                                  160,
                                  raftdemo::SerializeMetadataCommand(
                                      MakeCreateBucketCommand("bucket-q", "create-bucket-18")))
                    .Ok);
    EXPECT_TRUE(aborted_machine.Apply(
                                  161,
                                  raftdemo::SerializeMetadataCommand(
                                      MakeCreateObjectCommand("bucket-q", "object/a", "obj-20",
                                                              "create-object-12")))
                    .Ok);
    EXPECT_TRUE(aborted_machine.Apply(
                                  162,
                                  raftdemo::SerializeMetadataCommand(
                                      MakeAbortObjectCommand("bucket-q", "object/a", "obj-20",
                                                             "abort-object-7")))
                    .Ok);

    const raftdemo::ApplyResult already_aborted = aborted_machine.Apply(
        163, raftdemo::SerializeMetadataCommand(
                 MakeAbortObjectCommand("bucket-q", "object/a", "obj-20", "abort-object-8")));
    EXPECT_FALSE(already_aborted.Ok);
    EXPECT_EQ(already_aborted.message, "state conflict: object already aborted");
    EXPECT_EQ(aborted_machine.LastAppliedIndex(), 162U);
}

TEST(MetadataStateMachineTest, DeleteObjectApplyMarksCommittedObjectDeletedAndHidesIt)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           170,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-r", "create-bucket-19")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           171,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-r", "object/a", "obj-21",
                                                       "create-object-13")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           172,
                           raftdemo::SerializeMetadataCommand(
                               MakeCommitObjectCommand("bucket-r", "object/a", "obj-21",
                                                       "commit-object-9")))
                    .Ok);

    const raftdemo::ApplyResult apply = machine.Apply(
        173, raftdemo::SerializeMetadataCommand(
                 MakeDeleteObjectCommand("bucket-r", "object/a", "obj-21", "delete-object-1")));

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 173U);
    EXPECT_EQ(machine.LastAppliedTerm(), 0U);
    EXPECT_EQ(machine.RequestCount(), 4U);
    EXPECT_EQ(machine.TombstoneCount(), 1U);

    const std::optional<raftdemo::ObjectRecord> object =
        machine.FindObject("bucket-r", "object/a");
    ASSERT_TRUE(object.has_value());
    EXPECT_TRUE(object->IsDeleted());
    ASSERT_TRUE(object->delete_time.has_value());
    EXPECT_EQ(*object->delete_time, 1710000007U);

    const std::optional<std::string> indexed_id =
        machine.FindIndexedObjectId("bucket-r", "object/a");
    EXPECT_FALSE(indexed_id.has_value());

    const std::optional<std::vector<raftdemo::ChunkRef>> indexed_chunks =
        machine.FindChunkRefs("bucket-r", "object/a");
    EXPECT_FALSE(indexed_chunks.has_value());

    const raftdemo::MetadataHeadObjectResponse head =
        machine.HeadObject({.bucket = "bucket-r", .object_key = "object/a"});
    EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(head.record.has_value());
}

TEST(MetadataStateMachineTest, DeleteObjectApplyRejectsMissingBucketObjectIdAndStateConflicts)
{
    raftdemo::MetadataStateMachine machine;

    const raftdemo::ApplyResult missing_bucket = machine.Apply(
        180, raftdemo::SerializeMetadataCommand(
                 MakeDeleteObjectCommand("bucket-missing", "object/a", "obj-22", "delete-object-2")));
    EXPECT_FALSE(missing_bucket.Ok);
    EXPECT_EQ(missing_bucket.message, "not found: bucket does not exist");
    EXPECT_EQ(machine.LastAppliedIndex(), 0U);

    EXPECT_TRUE(machine.Apply(
                           181,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-s", "create-bucket-20")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           182,
                           raftdemo::SerializeMetadataCommand(
                               MakeDeleteBucketCommand("bucket-s", "delete-bucket-6")))
                    .Ok);

    const raftdemo::ApplyResult deleted_bucket = machine.Apply(
        183, raftdemo::SerializeMetadataCommand(
                 MakeDeleteObjectCommand("bucket-s", "object/a", "obj-23", "delete-object-3")));
    EXPECT_FALSE(deleted_bucket.Ok);
    EXPECT_EQ(deleted_bucket.message, "state conflict: bucket is deleted");
    EXPECT_EQ(machine.LastAppliedIndex(), 182U);

    raftdemo::MetadataStateMachine missing_object_machine;
    EXPECT_TRUE(missing_object_machine.Apply(
                                   190,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateBucketCommand("bucket-t", "create-bucket-21")))
                    .Ok);

    const raftdemo::ApplyResult missing_object = missing_object_machine.Apply(
        191, raftdemo::SerializeMetadataCommand(
                 MakeDeleteObjectCommand("bucket-t", "object/a", "obj-24", "delete-object-4")));
    EXPECT_FALSE(missing_object.Ok);
    EXPECT_EQ(missing_object.message, "not found: object does not exist");
    EXPECT_EQ(missing_object_machine.LastAppliedIndex(), 190U);

    raftdemo::MetadataStateMachine mismatch_machine;
    EXPECT_TRUE(mismatch_machine.Apply(
                                   200,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateBucketCommand("bucket-u", "create-bucket-22")))
                    .Ok);
    EXPECT_TRUE(mismatch_machine.Apply(
                                   201,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCreateObjectCommand("bucket-u", "object/a", "obj-25",
                                                               "create-object-14")))
                    .Ok);
    EXPECT_TRUE(mismatch_machine.Apply(
                                   202,
                                   raftdemo::SerializeMetadataCommand(
                                       MakeCommitObjectCommand("bucket-u", "object/a", "obj-25",
                                                               "commit-object-10")))
                    .Ok);

    const raftdemo::ApplyResult object_id_mismatch = mismatch_machine.Apply(
        203, raftdemo::SerializeMetadataCommand(
                 MakeDeleteObjectCommand("bucket-u", "object/a", "obj-26", "delete-object-5")));
    EXPECT_FALSE(object_id_mismatch.Ok);
    EXPECT_EQ(object_id_mismatch.message, "state conflict: object_id mismatch");
    EXPECT_EQ(mismatch_machine.LastAppliedIndex(), 202U);

    raftdemo::MetadataStateMachine pending_machine;
    EXPECT_TRUE(pending_machine.Apply(
                                  210,
                                  raftdemo::SerializeMetadataCommand(
                                      MakeCreateBucketCommand("bucket-v", "create-bucket-23")))
                    .Ok);
    EXPECT_TRUE(pending_machine.Apply(
                                  211,
                                  raftdemo::SerializeMetadataCommand(
                                      MakeCreateObjectCommand("bucket-v", "object/a", "obj-27",
                                                              "create-object-15")))
                    .Ok);

    const raftdemo::ApplyResult pending_delete = pending_machine.Apply(
        212, raftdemo::SerializeMetadataCommand(
                 MakeDeleteObjectCommand("bucket-v", "object/a", "obj-27", "delete-object-6")));
    EXPECT_FALSE(pending_delete.Ok);
    EXPECT_EQ(pending_delete.message, "state conflict: object is not committed");
    EXPECT_EQ(pending_machine.LastAppliedIndex(), 211U);

    raftdemo::MetadataStateMachine deleted_machine;
    EXPECT_TRUE(deleted_machine.Apply(
                                  220,
                                  raftdemo::SerializeMetadataCommand(
                                      MakeCreateBucketCommand("bucket-w", "create-bucket-24")))
                    .Ok);
    EXPECT_TRUE(deleted_machine.Apply(
                                  221,
                                  raftdemo::SerializeMetadataCommand(
                                      MakeCreateObjectCommand("bucket-w", "object/a", "obj-28",
                                                              "create-object-16")))
                    .Ok);
    EXPECT_TRUE(deleted_machine.Apply(
                                  222,
                                  raftdemo::SerializeMetadataCommand(
                                      MakeCommitObjectCommand("bucket-w", "object/a", "obj-28",
                                                              "commit-object-11")))
                    .Ok);
    EXPECT_TRUE(deleted_machine.Apply(
                                  223,
                                  raftdemo::SerializeMetadataCommand(
                                      MakeDeleteObjectCommand("bucket-w", "object/a", "obj-28",
                                                              "delete-object-7")))
                    .Ok);

    const raftdemo::ApplyResult already_deleted = deleted_machine.Apply(
        224, raftdemo::SerializeMetadataCommand(
                 MakeDeleteObjectCommand("bucket-w", "object/a", "obj-28", "delete-object-8")));
    EXPECT_FALSE(already_deleted.Ok);
    EXPECT_EQ(already_deleted.message, "state conflict: object already deleted");
    EXPECT_EQ(deleted_machine.LastAppliedIndex(), 223U);
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
    EXPECT_EQ(list.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_TRUE(list.records.empty());
}

TEST(MetadataStateMachineTest, HeadObjectOnlyReturnsCommittedVisibleObject)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           230,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-x", "create-bucket-25")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           231,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-x", "pending", "obj-29",
                                                       "create-object-17")))
                    .Ok);

    const raftdemo::MetadataHeadObjectResponse pending_head =
        machine.HeadObject({.bucket = "bucket-x", .object_key = "pending"});
    EXPECT_EQ(pending_head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(pending_head.record.has_value());

    EXPECT_TRUE(machine.Apply(
                           232,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-x", "committed", "obj-30",
                                                       "create-object-18")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           233,
                           raftdemo::SerializeMetadataCommand(
                               MakeCommitObjectCommand("bucket-x", "committed", "obj-30",
                                                       "commit-object-12")))
                    .Ok);

    const raftdemo::MetadataHeadObjectResponse committed_head =
        machine.HeadObject({.bucket = "bucket-x", .object_key = "committed"});
    ASSERT_EQ(committed_head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(committed_head.record.has_value());
    EXPECT_EQ(committed_head.record->object_key, "committed");
    EXPECT_TRUE(committed_head.record->IsCommitted());

    const raftdemo::MetadataHeadObjectResponse matched_id_head =
        machine.HeadObject({.bucket = "bucket-x",
                            .object_key = "committed",
                            .object_id = std::string("obj-30")});
    EXPECT_EQ(matched_id_head.result.code, raftdemo::MetadataStatusCode::kOk);

    const raftdemo::MetadataHeadObjectResponse mismatched_id_head =
        machine.HeadObject({.bucket = "bucket-x",
                            .object_key = "committed",
                            .object_id = std::string("obj-31")});
    EXPECT_EQ(mismatched_id_head.result.code, raftdemo::MetadataStatusCode::kNotFound);

    EXPECT_TRUE(machine.Apply(
                           234,
                           raftdemo::SerializeMetadataCommand(
                               MakeDeleteObjectCommand("bucket-x", "committed", "obj-30",
                                                       "delete-object-9")))
                    .Ok);
    const raftdemo::MetadataHeadObjectResponse deleted_head =
        machine.HeadObject({.bucket = "bucket-x", .object_key = "committed"});
    EXPECT_EQ(deleted_head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(deleted_head.record.has_value());

    EXPECT_TRUE(machine.Apply(
                           235,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-y", "create-bucket-26")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           236,
                           raftdemo::SerializeMetadataCommand(
                               MakeDeleteBucketCommand("bucket-y", "delete-bucket-7")))
                    .Ok);
    const raftdemo::MetadataHeadObjectResponse deleted_bucket_head =
        machine.HeadObject({.bucket = "bucket-y", .object_key = "anything"});
    EXPECT_EQ(deleted_bucket_head.result.code, raftdemo::MetadataStatusCode::kNotFound);
}

TEST(MetadataStateMachineTest, ListObjectsReturnsCommittedObjectsWithPrefixOrderAndLimit)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           240,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-z", "create-bucket-27")))
                    .Ok);

    EXPECT_TRUE(machine.Apply(
                           241,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-z", "logs/b", "obj-31",
                                                       "create-object-19")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           242,
                           raftdemo::SerializeMetadataCommand(
                               MakeCommitObjectCommand("bucket-z", "logs/b", "obj-31",
                                                       "commit-object-13")))
                    .Ok);

    EXPECT_TRUE(machine.Apply(
                           243,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-z", "logs/a", "obj-32",
                                                       "create-object-20")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           244,
                           raftdemo::SerializeMetadataCommand(
                               MakeCommitObjectCommand("bucket-z", "logs/a", "obj-32",
                                                       "commit-object-14")))
                    .Ok);

    EXPECT_TRUE(machine.Apply(
                           245,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-z", "logs/pending", "obj-33",
                                                       "create-object-21")))
                    .Ok);

    EXPECT_TRUE(machine.Apply(
                           246,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-z", "logs/deleted", "obj-34",
                                                       "create-object-22")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           247,
                           raftdemo::SerializeMetadataCommand(
                               MakeCommitObjectCommand("bucket-z", "logs/deleted", "obj-34",
                                                       "commit-object-15")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           248,
                           raftdemo::SerializeMetadataCommand(
                               MakeDeleteObjectCommand("bucket-z", "logs/deleted", "obj-34",
                                                       "delete-object-10")))
                    .Ok);

    EXPECT_TRUE(machine.Apply(
                           249,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-z", "other/x", "obj-35",
                                                       "create-object-23")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           250,
                           raftdemo::SerializeMetadataCommand(
                               MakeCommitObjectCommand("bucket-z", "other/x", "obj-35",
                                                       "commit-object-16")))
                    .Ok);

    const raftdemo::MetadataListObjectsResponse missing_bucket =
        machine.ListObjects({.bucket = "bucket-missing"});
    EXPECT_EQ(missing_bucket.result.code, raftdemo::MetadataStatusCode::kNotFound);

    EXPECT_TRUE(machine.Apply(
                           251,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-deleted", "create-bucket-28")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           252,
                           raftdemo::SerializeMetadataCommand(
                               MakeDeleteBucketCommand("bucket-deleted", "delete-bucket-8")))
                    .Ok);
    const raftdemo::MetadataListObjectsResponse deleted_bucket =
        machine.ListObjects({.bucket = "bucket-deleted"});
    EXPECT_EQ(deleted_bucket.result.code, raftdemo::MetadataStatusCode::kNotFound);

    const raftdemo::MetadataListObjectsResponse filtered =
        machine.ListObjects({.bucket = "bucket-z", .prefix = "logs/"});
    ASSERT_EQ(filtered.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(filtered.records.size(), 2U);
    EXPECT_EQ(filtered.records[0].object_key, "logs/a");
    EXPECT_EQ(filtered.records[1].object_key, "logs/b");
    EXPECT_TRUE(filtered.records[0].IsCommitted());
    EXPECT_TRUE(filtered.records[1].IsCommitted());
    EXPECT_TRUE(filtered.next_page_token.empty());

    const raftdemo::MetadataListObjectsResponse limited =
        machine.ListObjects({.bucket = "bucket-z", .prefix = "logs/", .limit = 1});
    ASSERT_EQ(limited.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(limited.records.size(), 1U);
    EXPECT_EQ(limited.records[0].object_key, "logs/a");
    EXPECT_EQ(limited.next_page_token, "logs/a");

    const raftdemo::MetadataListObjectsResponse continued =
        machine.ListObjects({.bucket = "bucket-z",
                             .prefix = "logs/",
                             .continuation_token = limited.next_page_token});
    ASSERT_EQ(continued.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(continued.records.size(), 1U);
    EXPECT_EQ(continued.records[0].object_key, "logs/b");
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
