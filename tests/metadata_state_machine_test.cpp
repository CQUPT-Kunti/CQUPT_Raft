#include "support/metadata_test_utils.h"
#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/node/raft_node.h"
#include "raft/state_machine/metadata_state_machine.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <type_traits>

namespace raftdemo
{
    std::string SerializeMetadataCommand(const MetadataCommand &command);
} // namespace raftdemo

namespace
{
    using raftdemo::test::ApplyMetadataCommand;
    using raftdemo::test::MakeAbortObjectCommand;
    using raftdemo::test::MakeCommitObjectCommand;
    using raftdemo::test::MakeCreateBucketCommand;
    using raftdemo::test::MakeCreateObjectCommand;
    using raftdemo::test::MakeDeleteBucketCommand;
    using raftdemo::test::MakeDeleteObjectCommand;
    using raftdemo::test::MakeSingleNodeConfig;
    using raftdemo::test::MakeSingleNodeSnapshotConfig;
    using raftdemo::test::MakeSnapshotPath;
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

TEST(MetadataStateMachineTest, RaftNodeDefaultStateMachineWiringUsesMetadataStateMachine)
{
    const std::filesystem::path root = MakeSnapshotPath("raft-node-default-metadata-wiring");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    raftdemo::RaftNode node(MakeSingleNodeConfig(root), MakeSingleNodeSnapshotConfig(root));

    EXPECT_NE(node.GetMetadataStateMachineV2(), nullptr);
    EXPECT_EQ(node.GetMetadataStateMachine(), nullptr);
    EXPECT_EQ(node.GetMetadataStateMachineV2()->LastAppliedIndex(), 0U);
    EXPECT_EQ(node.GetMetadataStateMachineV2()->LastAppliedTerm(), 0U);
}

TEST(MetadataStateMachineTest, SkeletonApplyAndSnapshotReturnExplicitResults)
{
    raftdemo::MetadataStateMachine machine;

    const raftdemo::ApplyResult apply = machine.Apply(1, "placeholder-command");
    EXPECT_FALSE(apply.Ok);
    EXPECT_EQ(apply.message, "failed to parse metadata command");
    EXPECT_EQ(machine.LastAppliedIndex(), 0U);

    const raftdemo::SnapshotResult save_empty = machine.SaveSnapshot("");
    EXPECT_EQ(save_empty.status, raftdemo::SnapshotStatus::kInvalidArgument);

    const std::filesystem::path snapshot_path = MakeSnapshotPath("metadata-skeleton.snapshot");
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);
    const raftdemo::SnapshotResult save_placeholder = machine.SaveSnapshot(snapshot_path.string());
    EXPECT_EQ(save_placeholder.status, raftdemo::SnapshotStatus::kOk);

    const raftdemo::SnapshotResult load_missing =
        machine.LoadSnapshot("tmp/non-existent-metadata-skeleton.snapshot");
    EXPECT_EQ(load_missing.status, raftdemo::SnapshotStatus::kNotFound);
}

TEST(MetadataStateMachineTest, CreateBucketApplyCreatesBucketAndUpdatesApplyPosition)
{
    raftdemo::MetadataStateMachine machine;
    constexpr std::uint64_t kAppliedTerm = 3;

    const raftdemo::ApplyResult apply = ApplyMetadataCommand(
        machine, 7, MakeCreateBucketCommand("bucket-a", "create-bucket-1"), kAppliedTerm);

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 7U);
    EXPECT_EQ(machine.LastAppliedTerm(), kAppliedTerm);
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
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 10,
                    MakeCreateBucketCommand("bucket-b", "create-bucket-2"), 4)
                    .Ok);
    constexpr std::uint64_t kAppliedTerm = 5;

    const raftdemo::ApplyResult apply = ApplyMetadataCommand(
        machine, 11, MakeDeleteBucketCommand("bucket-b", "delete-bucket-1"), kAppliedTerm);

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 11U);
    EXPECT_EQ(machine.LastAppliedTerm(), kAppliedTerm);
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

TEST(MetadataStateMachineTest, EmptyRequestIdReturnsExplicitErrorAndIsNotRecorded)
{
    raftdemo::MetadataStateMachine machine;
    const raftdemo::ApplyResult apply = machine.Apply(
        24, raftdemo::SerializeMetadataCommand(
                MakeCreateBucketCommand("bucket-empty", "")));

    EXPECT_FALSE(apply.Ok);
    EXPECT_EQ(apply.message, "invalid metadata command: missing request_id");
    EXPECT_EQ(machine.RequestCount(), 0U);
    EXPECT_EQ(machine.LastAppliedIndex(), 0U);
}

TEST(MetadataStateMachineTest, CreateObjectApplyCreatesPendingRecordAndIndexEntry)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 30,
                    MakeCreateBucketCommand("bucket-d", "create-bucket-5"), 6)
                    .Ok);
    constexpr std::uint64_t kAppliedTerm = 7;

    const raftdemo::ApplyResult apply = ApplyMetadataCommand(
        machine, 31,
        MakeCreateObjectCommand("bucket-d", "object/a", "obj-1", "create-object-1"),
        kAppliedTerm);

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 31U);
    EXPECT_EQ(machine.LastAppliedTerm(), kAppliedTerm);
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
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 60,
                    MakeCreateBucketCommand("bucket-g", "create-bucket-8"), 8)
                    .Ok);
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 61,
                    MakeCreateObjectCommand("bucket-g", "object/a", "obj-6",
                                            "create-object-6"),
                    8)
                    .Ok);

    constexpr std::uint64_t kAppliedTerm = 9;
    const raftdemo::ApplyResult apply = ApplyMetadataCommand(
        machine, 62,
        MakeCommitObjectCommand("bucket-g", "object/a", "obj-6", "commit-object-1"),
        kAppliedTerm);

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 62U);
    EXPECT_EQ(machine.LastAppliedTerm(), kAppliedTerm);
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
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 110,
                    MakeCreateBucketCommand("bucket-l", "create-bucket-13"), 10)
                    .Ok);
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 111,
                    MakeCreateObjectCommand("bucket-l", "object/a", "obj-13",
                                            "create-object-9"),
                    10)
                    .Ok);

    constexpr std::uint64_t kAppliedTerm = 11;
    const raftdemo::ApplyResult apply = ApplyMetadataCommand(
        machine, 112,
        MakeAbortObjectCommand("bucket-l", "object/a", "obj-13", "abort-object-1"),
        kAppliedTerm);

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 112U);
    EXPECT_EQ(machine.LastAppliedTerm(), kAppliedTerm);
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
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 170,
                    MakeCreateBucketCommand("bucket-r", "create-bucket-19"), 12)
                    .Ok);
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 171,
                    MakeCreateObjectCommand("bucket-r", "object/a", "obj-21",
                                            "create-object-13"),
                    12)
                    .Ok);
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 172,
                    MakeCommitObjectCommand("bucket-r", "object/a", "obj-21",
                                            "commit-object-9"),
                    12)
                    .Ok);

    constexpr std::uint64_t kAppliedTerm = 13;
    const raftdemo::ApplyResult apply = ApplyMetadataCommand(
        machine, 173,
        MakeDeleteObjectCommand("bucket-r", "object/a", "obj-21", "delete-object-1"),
        kAppliedTerm);

    EXPECT_TRUE(apply.Ok);
    EXPECT_EQ(apply.message, "ok");
    EXPECT_EQ(machine.LastAppliedIndex(), 173U);
    EXPECT_EQ(machine.LastAppliedTerm(), kAppliedTerm);
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

TEST(MetadataStateMachineTest, DuplicateRequestReplayDoesNotAdvanceAppliedTerm)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 300,
                    MakeCreateBucketCommand("bucket-term-replay", "term-replay-bucket"), 21)
                    .Ok);

    const auto create = MakeCreateObjectCommand(
        "bucket-term-replay", "object/a", "obj-term-replay", "term-replay-create");
    const auto first_apply = ApplyMetadataCommand(machine, 301, create, 22);
    ASSERT_TRUE(first_apply.Ok) << first_apply.message;
    EXPECT_EQ(machine.LastAppliedIndex(), 301U);
    EXPECT_EQ(machine.LastAppliedTerm(), 22U);

    const auto replay_apply = ApplyMetadataCommand(machine, 999, create, 99);
    EXPECT_TRUE(replay_apply.Ok);
    EXPECT_EQ(replay_apply.message, "idempotent replay");
    EXPECT_EQ(machine.LastAppliedIndex(), 301U);
    EXPECT_EQ(machine.LastAppliedTerm(), 22U);
}

TEST(MetadataStateMachineTest, SaveSnapshotAndLoadSnapshotPreserveAppliedTerm)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 320,
                    MakeCreateBucketCommand("bucket-snapshot-term", "snapshot-term-bucket"), 31)
                    .Ok);
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 321,
                    MakeCreateObjectCommand("bucket-snapshot-term", "object/live",
                                            "obj-snapshot-term", "snapshot-term-create"),
                    32)
                    .Ok);
    EXPECT_TRUE(ApplyMetadataCommand(
                    machine, 322,
                    MakeCommitObjectCommand("bucket-snapshot-term", "object/live",
                                            "obj-snapshot-term", "snapshot-term-commit"),
                    33)
                    .Ok);

    const std::filesystem::path snapshot_path =
        MakeSnapshotPath("metadata-applied-term-preserved.snapshot");
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);

    const auto save_result = machine.SaveSnapshot(snapshot_path.string());
    ASSERT_EQ(save_result.status, raftdemo::SnapshotStatus::kOk) << save_result.message;

    raftdemo::MetadataStateMachine restored;
    const auto load_result = restored.LoadSnapshot(snapshot_path.string());
    ASSERT_EQ(load_result.status, raftdemo::SnapshotStatus::kOk) << load_result.message;

    EXPECT_EQ(restored.LastAppliedIndex(), 322U);
    EXPECT_EQ(restored.LastAppliedTerm(), 33U);
    EXPECT_EQ(restored.RequestCount(), 3U);
    EXPECT_EQ(restored.TombstoneCount(), 0U);

    const auto head = restored.HeadObject(
        {.bucket = "bucket-snapshot-term", .object_key = "object/live"});
    ASSERT_TRUE(head.result.Ok()) << head.result.summary.message;
    ASSERT_TRUE(head.record.has_value());
    EXPECT_EQ(head.record->object_id, "obj-snapshot-term");
    EXPECT_TRUE(head.record->IsCommitted());
}
