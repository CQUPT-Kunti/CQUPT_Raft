#include "support/metadata_test_utils.h"
#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/state_machine/metadata_state_machine.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace raftdemo
{
    std::string SerializeMetadataCommand(const MetadataCommand &command);
} // namespace raftdemo

namespace
{
    using raftdemo::test::MakeAbortObjectCommand;
    using raftdemo::test::MakeCommitObjectCommand;
    using raftdemo::test::MakeCreateBucketCommand;
    using raftdemo::test::MakeCreateObjectCommand;
    using raftdemo::test::MakeDeleteBucketCommand;
    using raftdemo::test::MakeDeleteObjectCommand;
    using raftdemo::test::MakeSnapshotPath;
    using raftdemo::test::WritePod;
} // namespace

TEST(MetadataStateMachineTest, SnapshotRoundTripRestoresStateIndexesAndIdempotency)
{
    raftdemo::MetadataStateMachine machine;

    const std::string create_bucket_live = raftdemo::SerializeMetadataCommand(
        MakeCreateBucketCommand("bucket-snap-live", "snap-create-bucket-live"));
    const std::string create_bucket_deleted = raftdemo::SerializeMetadataCommand(
        MakeCreateBucketCommand("bucket-snap-deleted", "snap-create-bucket-deleted"));
    const std::string delete_bucket = raftdemo::SerializeMetadataCommand(
        MakeDeleteBucketCommand("bucket-snap-deleted", "snap-delete-bucket"));
    const std::string create_pending = raftdemo::SerializeMetadataCommand(
        MakeCreateObjectCommand("bucket-snap-live", "pending", "obj-snap-pending",
                                "snap-create-pending"));
    const std::string create_committed = raftdemo::SerializeMetadataCommand(
        MakeCreateObjectCommand("bucket-snap-live", "committed", "obj-snap-committed",
                                "snap-create-committed"));
    const std::string commit_committed = raftdemo::SerializeMetadataCommand(
        MakeCommitObjectCommand("bucket-snap-live", "committed", "obj-snap-committed",
                                "snap-commit-committed"));
    const std::string create_aborted = raftdemo::SerializeMetadataCommand(
        MakeCreateObjectCommand("bucket-snap-live", "aborted", "obj-snap-aborted",
                                "snap-create-aborted"));
    const std::string abort_aborted = raftdemo::SerializeMetadataCommand(
        MakeAbortObjectCommand("bucket-snap-live", "aborted", "obj-snap-aborted",
                               "snap-abort-aborted"));
    const std::string create_deleted = raftdemo::SerializeMetadataCommand(
        MakeCreateObjectCommand("bucket-snap-live", "deleted", "obj-snap-deleted",
                                "snap-create-deleted"));
    const std::string commit_deleted = raftdemo::SerializeMetadataCommand(
        MakeCommitObjectCommand("bucket-snap-live", "deleted", "obj-snap-deleted",
                                "snap-commit-deleted"));
    const std::string delete_deleted = raftdemo::SerializeMetadataCommand(
        MakeDeleteObjectCommand("bucket-snap-live", "deleted", "obj-snap-deleted",
                                "snap-delete-deleted"));

    EXPECT_TRUE(machine.Apply(340, create_bucket_live).Ok);
    EXPECT_TRUE(machine.Apply(341, create_bucket_deleted).Ok);
    EXPECT_TRUE(machine.Apply(342, delete_bucket).Ok);
    EXPECT_TRUE(machine.Apply(343, create_pending).Ok);
    EXPECT_TRUE(machine.Apply(344, create_committed).Ok);
    EXPECT_TRUE(machine.Apply(345, commit_committed).Ok);
    EXPECT_TRUE(machine.Apply(346, create_aborted).Ok);
    EXPECT_TRUE(machine.Apply(347, abort_aborted).Ok);
    EXPECT_TRUE(machine.Apply(348, create_deleted).Ok);
    EXPECT_TRUE(machine.Apply(349, commit_deleted).Ok);
    EXPECT_TRUE(machine.Apply(350, delete_deleted).Ok);

    const std::filesystem::path snapshot_path = MakeSnapshotPath("metadata-roundtrip.snapshot");
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);
    const raftdemo::SnapshotResult save = machine.SaveSnapshot(snapshot_path.string());
    ASSERT_EQ(save.status, raftdemo::SnapshotStatus::kOk);

    raftdemo::MetadataStateMachine restored;
    const raftdemo::SnapshotResult load = restored.LoadSnapshot(snapshot_path.string());
    ASSERT_EQ(load.status, raftdemo::SnapshotStatus::kOk);

    EXPECT_EQ(restored.LastAppliedIndex(), 350U);
    EXPECT_EQ(restored.LastAppliedTerm(), 0U);
    EXPECT_EQ(restored.BucketCount(), 2U);
    EXPECT_EQ(restored.ObjectCount(), 4U);
    EXPECT_EQ(restored.RequestCount(), 11U);
    EXPECT_EQ(restored.TombstoneCount(), 2U);

    const auto restored_committed = restored.HeadObject(
        {.bucket = "bucket-snap-live", .object_key = "committed"});
    ASSERT_EQ(restored_committed.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(restored_committed.record.has_value());
    EXPECT_TRUE(restored_committed.record->IsCommitted());
    EXPECT_EQ(restored_committed.record->object_id, "obj-snap-committed");

    const auto restored_chunks = restored.FindChunkRefs("bucket-snap-live", "committed");
    ASSERT_TRUE(restored_chunks.has_value());
    ASSERT_EQ(restored_chunks->size(), 2U);
    EXPECT_EQ((*restored_chunks)[0].chunk_id, "chunk-a");
    EXPECT_EQ((*restored_chunks)[1].chunk_id, "chunk-b");

    const auto restored_index =
        restored.FindIndexedObjectId("bucket-snap-live", "committed");
    ASSERT_TRUE(restored_index.has_value());
    EXPECT_EQ(*restored_index, "obj-snap-committed");

    const auto pending_head =
        restored.HeadObject({.bucket = "bucket-snap-live", .object_key = "pending"});
    EXPECT_EQ(pending_head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(pending_head.record.has_value());

    const auto aborted_head =
        restored.HeadObject({.bucket = "bucket-snap-live", .object_key = "aborted"});
    EXPECT_EQ(aborted_head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(aborted_head.record.has_value());
    EXPECT_FALSE(restored.FindIndexedObjectId("bucket-snap-live", "aborted").has_value());
    EXPECT_FALSE(restored.FindChunkRefs("bucket-snap-live", "aborted").has_value());

    const auto deleted_head =
        restored.HeadObject({.bucket = "bucket-snap-live", .object_key = "deleted"});
    EXPECT_EQ(deleted_head.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(deleted_head.record.has_value());
    EXPECT_FALSE(restored.FindIndexedObjectId("bucket-snap-live", "deleted").has_value());
    EXPECT_FALSE(restored.FindChunkRefs("bucket-snap-live", "deleted").has_value());

    const auto deleted_bucket_list = restored.ListObjects({.bucket = "bucket-snap-deleted"});
    EXPECT_EQ(deleted_bucket_list.result.code, raftdemo::MetadataStatusCode::kNotFound);

    const auto listed = restored.ListObjects({.bucket = "bucket-snap-live", .prefix = ""});
    ASSERT_EQ(listed.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(listed.records.size(), 1U);
    EXPECT_EQ(listed.records[0].object_key, "committed");

    const auto prefixed = restored.ListObjects({.bucket = "bucket-snap-live",
                                                .prefix = "com",
                                                .limit = 1});
    ASSERT_EQ(prefixed.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(prefixed.records.size(), 1U);
    EXPECT_EQ(prefixed.records[0].object_key, "committed");

    EXPECT_TRUE(restored.Apply(351, create_bucket_live).Ok);
    EXPECT_TRUE(restored.Apply(352, delete_bucket).Ok);
    EXPECT_TRUE(restored.Apply(353, create_pending).Ok);
    EXPECT_TRUE(restored.Apply(354, commit_committed).Ok);
    EXPECT_TRUE(restored.Apply(355, abort_aborted).Ok);
    EXPECT_TRUE(restored.Apply(356, delete_deleted).Ok);
    EXPECT_EQ(restored.LastAppliedIndex(), 350U);
    EXPECT_EQ(restored.RequestCount(), 11U);
    EXPECT_EQ(restored.TombstoneCount(), 2U);
    EXPECT_FALSE(restored.FindIndexedObjectId("bucket-snap-live", "deleted").has_value());
    EXPECT_FALSE(restored.FindIndexedObjectId("bucket-snap-live", "aborted").has_value());
}

TEST(MetadataStateMachineTest, SnapshotLoadThenReplayRestoresFinalStateAndBoundary)
{
    raftdemo::MetadataStateMachine machine;

    const std::string create_bucket_live = raftdemo::SerializeMetadataCommand(
        MakeCreateBucketCommand("bucket-replay-live", "replay-create-bucket-live"));
    const std::string create_object_committed = raftdemo::SerializeMetadataCommand(
        MakeCreateObjectCommand("bucket-replay-live", "base-committed", "obj-base-committed",
                                "replay-create-base-committed"));
    const std::string commit_object_committed = raftdemo::SerializeMetadataCommand(
        MakeCommitObjectCommand("bucket-replay-live", "base-committed", "obj-base-committed",
                                "replay-commit-base-committed"));
    const std::string create_object_deleted = raftdemo::SerializeMetadataCommand(
        MakeCreateObjectCommand("bucket-replay-live", "base-deleted", "obj-base-deleted",
                                "replay-create-base-deleted"));
    const std::string commit_object_deleted = raftdemo::SerializeMetadataCommand(
        MakeCommitObjectCommand("bucket-replay-live", "base-deleted", "obj-base-deleted",
                                "replay-commit-base-deleted"));
    const std::string delete_object_deleted = raftdemo::SerializeMetadataCommand(
        MakeDeleteObjectCommand("bucket-replay-live", "base-deleted", "obj-base-deleted",
                                "replay-delete-base-deleted"));
    const std::string create_bucket_deleted = raftdemo::SerializeMetadataCommand(
        MakeCreateBucketCommand("bucket-replay-gone", "replay-create-bucket-gone"));
    const std::string delete_bucket_deleted = raftdemo::SerializeMetadataCommand(
        MakeDeleteBucketCommand("bucket-replay-gone", "replay-delete-bucket-gone"));

    EXPECT_TRUE(machine.Apply(380, create_bucket_live).Ok);
    EXPECT_TRUE(machine.Apply(381, create_object_committed).Ok);
    EXPECT_TRUE(machine.Apply(382, commit_object_committed).Ok);
    EXPECT_TRUE(machine.Apply(383, create_object_deleted).Ok);
    EXPECT_TRUE(machine.Apply(384, commit_object_deleted).Ok);
    EXPECT_TRUE(machine.Apply(385, delete_object_deleted).Ok);
    EXPECT_TRUE(machine.Apply(386, create_bucket_deleted).Ok);
    EXPECT_TRUE(machine.Apply(387, delete_bucket_deleted).Ok);

    const std::filesystem::path snapshot_path =
        MakeSnapshotPath("metadata-snapshot-replay.snapshot");
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);
    const raftdemo::SnapshotResult save = machine.SaveSnapshot(snapshot_path.string());
    ASSERT_EQ(save.status, raftdemo::SnapshotStatus::kOk);

    raftdemo::MetadataStateMachine restored;
    const raftdemo::SnapshotResult load = restored.LoadSnapshot(snapshot_path.string());
    ASSERT_EQ(load.status, raftdemo::SnapshotStatus::kOk);
    EXPECT_EQ(restored.LastAppliedIndex(), 387U);
    EXPECT_EQ(restored.LastAppliedTerm(), 0U);
    EXPECT_EQ(restored.RequestCount(), 8U);
    EXPECT_EQ(restored.TombstoneCount(), 1U);

    const raftdemo::ApplyResult old_replay = restored.Apply(385, delete_object_deleted);
    EXPECT_TRUE(old_replay.Ok);
    EXPECT_EQ(old_replay.message, "idempotent replay");
    EXPECT_EQ(restored.LastAppliedIndex(), 387U);
    EXPECT_EQ(restored.RequestCount(), 8U);
    EXPECT_EQ(restored.TombstoneCount(), 1U);

    const std::string create_replay_committed = raftdemo::SerializeMetadataCommand(
        MakeCreateObjectCommand("bucket-replay-live", "replay-committed", "obj-replay-committed",
                                "replay-create-replay-committed"));
    const std::string commit_replay_committed = raftdemo::SerializeMetadataCommand(
        MakeCommitObjectCommand("bucket-replay-live", "replay-committed", "obj-replay-committed",
                                "replay-commit-replay-committed"));
    const std::string create_replay_aborted = raftdemo::SerializeMetadataCommand(
        MakeCreateObjectCommand("bucket-replay-live", "replay-aborted", "obj-replay-aborted",
                                "replay-create-replay-aborted"));
    const std::string abort_replay_aborted = raftdemo::SerializeMetadataCommand(
        MakeAbortObjectCommand("bucket-replay-live", "replay-aborted", "obj-replay-aborted",
                               "replay-abort-replay-aborted"));

    EXPECT_TRUE(restored.Apply(388, create_replay_committed).Ok);
    EXPECT_TRUE(restored.Apply(389, commit_replay_committed).Ok);
    EXPECT_TRUE(restored.Apply(390, create_replay_aborted).Ok);
    EXPECT_TRUE(restored.Apply(391, abort_replay_aborted).Ok);
    EXPECT_EQ(restored.LastAppliedIndex(), 391U);
    EXPECT_EQ(restored.LastAppliedTerm(), 0U);
    EXPECT_EQ(restored.RequestCount(), 12U);
    EXPECT_EQ(restored.TombstoneCount(), 2U);

    const raftdemo::ApplyResult replay_commit_duplicate =
        restored.Apply(392, commit_replay_committed);
    EXPECT_TRUE(replay_commit_duplicate.Ok);
    EXPECT_EQ(replay_commit_duplicate.message, "idempotent replay");
    EXPECT_EQ(restored.LastAppliedIndex(), 391U);
    EXPECT_EQ(restored.RequestCount(), 12U);

    const auto base_committed =
        restored.HeadObject({.bucket = "bucket-replay-live", .object_key = "base-committed"});
    ASSERT_EQ(base_committed.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(base_committed.record.has_value());
    EXPECT_EQ(base_committed.record->object_id, "obj-base-committed");
    EXPECT_TRUE(base_committed.record->IsCommitted());

    const auto replay_committed =
        restored.HeadObject({.bucket = "bucket-replay-live", .object_key = "replay-committed"});
    ASSERT_EQ(replay_committed.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(replay_committed.record.has_value());
    EXPECT_EQ(replay_committed.record->object_id, "obj-replay-committed");
    EXPECT_TRUE(replay_committed.record->IsCommitted());

    const auto base_chunks = restored.FindChunkRefs("bucket-replay-live", "base-committed");
    ASSERT_TRUE(base_chunks.has_value());
    ASSERT_EQ(base_chunks->size(), 2U);
    const auto replay_chunks = restored.FindChunkRefs("bucket-replay-live", "replay-committed");
    ASSERT_TRUE(replay_chunks.has_value());
    ASSERT_EQ(replay_chunks->size(), 2U);

    const auto base_deleted =
        restored.HeadObject({.bucket = "bucket-replay-live", .object_key = "base-deleted"});
    EXPECT_EQ(base_deleted.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(base_deleted.record.has_value());
    EXPECT_FALSE(restored.FindIndexedObjectId("bucket-replay-live", "base-deleted").has_value());
    EXPECT_FALSE(restored.FindChunkRefs("bucket-replay-live", "base-deleted").has_value());

    const auto replay_aborted =
        restored.HeadObject({.bucket = "bucket-replay-live", .object_key = "replay-aborted"});
    EXPECT_EQ(replay_aborted.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(replay_aborted.record.has_value());
    EXPECT_FALSE(restored.FindIndexedObjectId("bucket-replay-live", "replay-aborted").has_value());
    EXPECT_FALSE(restored.FindChunkRefs("bucket-replay-live", "replay-aborted").has_value());

    const auto deleted_bucket = restored.ListObjects({.bucket = "bucket-replay-gone"});
    EXPECT_EQ(deleted_bucket.result.code, raftdemo::MetadataStatusCode::kNotFound);

    const auto listed =
        restored.ListObjects({.bucket = "bucket-replay-live", .prefix = "base"});
    ASSERT_EQ(listed.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(listed.records.size(), 1U);
    EXPECT_EQ(listed.records[0].object_key, "base-committed");

    const auto all_listed =
        restored.ListObjects({.bucket = "bucket-replay-live", .prefix = ""});
    ASSERT_EQ(all_listed.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(all_listed.records.size(), 2U);
    EXPECT_EQ(all_listed.records[0].object_key, "base-committed");
    EXPECT_EQ(all_listed.records[1].object_key, "replay-committed");
}

TEST(MetadataStateMachineTest, LoadSnapshotRejectsCorruptedDataAndPreservesState)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           360,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-corrupt", "corrupt-bucket")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           361,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-corrupt", "object/a", "obj-corrupt",
                                                       "corrupt-object")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           362,
                           raftdemo::SerializeMetadataCommand(
                               MakeCommitObjectCommand("bucket-corrupt", "object/a", "obj-corrupt",
                                                       "corrupt-commit")))
                    .Ok);

    const auto before_head =
        machine.HeadObject({.bucket = "bucket-corrupt", .object_key = "object/a"});
    ASSERT_EQ(before_head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(before_head.record.has_value());

    const std::filesystem::path snapshot_path = MakeSnapshotPath("metadata-corrupt.snapshot");
    std::ofstream out(snapshot_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    const std::uint32_t magic = 0x4D445331U;
    WritePod(out, magic);
    out.flush();
    out.close();

    const raftdemo::SnapshotResult load = machine.LoadSnapshot(snapshot_path.string());
    EXPECT_EQ(load.status, raftdemo::SnapshotStatus::kCorruptedData);
    EXPECT_EQ(machine.LastAppliedIndex(), 362U);
    EXPECT_EQ(machine.RequestCount(), 3U);
    EXPECT_EQ(machine.TombstoneCount(), 0U);

    const auto after_head =
        machine.HeadObject({.bucket = "bucket-corrupt", .object_key = "object/a"});
    ASSERT_EQ(after_head.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(after_head.record.has_value());
    EXPECT_EQ(after_head.record->object_id, "obj-corrupt");
}

TEST(MetadataStateMachineTest, LoadSnapshotRejectsUnknownVersionAndPreservesState)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           370,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-version", "version-bucket")))
                    .Ok);

    const std::filesystem::path snapshot_path = MakeSnapshotPath("metadata-version.snapshot");
    std::ofstream out(snapshot_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    const std::uint32_t magic = 0x4D445331U;
    const std::uint32_t version = 99U;
    const std::uint64_t zero = 0U;
    WritePod(out, magic);
    WritePod(out, version);
    for (int i = 0; i < 9; ++i)
    {
        WritePod(out, zero);
    }
    out.flush();
    out.close();

    const raftdemo::SnapshotResult load = machine.LoadSnapshot(snapshot_path.string());
    EXPECT_EQ(load.status, raftdemo::SnapshotStatus::kVersionMismatch);
    EXPECT_EQ(machine.LastAppliedIndex(), 370U);
    EXPECT_EQ(machine.BucketCount(), 1U);
    EXPECT_EQ(machine.RequestCount(), 1U);
}
