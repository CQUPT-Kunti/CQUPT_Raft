#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/node/raft_node.h"
#include "raft/state_machine/metadata_state_machine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

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

    std::filesystem::path MakeSnapshotPath(const std::string &filename)
    {
        const std::filesystem::path dir = "tmp/metadata-state-machine-tests";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir / filename;
    }

    raftdemo::NodeConfig MakeSingleNodeConfig(const std::filesystem::path &root)
    {
        raftdemo::NodeConfig cfg;
        cfg.node_id = 1;
        cfg.address = "127.0.0.1:0";
        cfg.election_timeout_min = std::chrono::milliseconds(200);
        cfg.election_timeout_max = std::chrono::milliseconds(350);
        cfg.heartbeat_interval = std::chrono::milliseconds(60);
        cfg.rpc_deadline = std::chrono::milliseconds(250);
        cfg.data_dir = (root / "data" / "node_1").string();
        return cfg;
    }

    raftdemo::snapshotConfig MakeSingleNodeSnapshotConfig(const std::filesystem::path &root)
    {
        raftdemo::snapshotConfig cfg;
        cfg.enabled = false;
        cfg.snapshot_dir = (root / "snapshots" / "node_1").string();
        cfg.load_on_startup = false;
        cfg.file_prefix = "snapshot";
        return cfg;
    }

    template <typename T>
    void WritePod(std::ofstream &out, const T &value)
    {
        out.write(reinterpret_cast<const char *>(&value), sizeof(T));
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

TEST(MetadataStateMachineTest, RaftNodeDefaultStateMachineWiringUsesMetadataStateMachine)
{
    const std::filesystem::path root = MakeSnapshotPath("raft-node-default-metadata-wiring");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);

    raftdemo::RaftNode node(MakeSingleNodeConfig(root), MakeSingleNodeSnapshotConfig(root));

    EXPECT_NE(node.GetMetadataStateMachineV2(), nullptr);
    EXPECT_EQ(node.GetMetadataStateMachine(), nullptr);

    std::string value;
    EXPECT_FALSE(node.DebugGetValue("legacy-kv-key", &value));
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

TEST(MetadataStateMachineTest, DuplicateRequestIdReplaysSuccessWithoutReapplyingLifecycleCommands)
{
    raftdemo::MetadataStateMachine create_bucket_machine;
    const std::string create_bucket = raftdemo::SerializeMetadataCommand(
        MakeCreateBucketCommand("bucket-idem-a", "idem-bucket-create"));
    EXPECT_TRUE(create_bucket_machine.Apply(260, create_bucket).Ok);
    const raftdemo::ApplyResult create_bucket_replay =
        create_bucket_machine.Apply(261, create_bucket);
    EXPECT_TRUE(create_bucket_replay.Ok);
    EXPECT_EQ(create_bucket_replay.message, "idempotent replay");
    EXPECT_EQ(create_bucket_machine.BucketCount(), 1U);
    EXPECT_EQ(create_bucket_machine.RequestCount(), 1U);
    EXPECT_EQ(create_bucket_machine.LastAppliedIndex(), 260U);

    raftdemo::MetadataStateMachine delete_bucket_machine;
    EXPECT_TRUE(delete_bucket_machine.Apply(
                                    270, raftdemo::SerializeMetadataCommand(
                                             MakeCreateBucketCommand("bucket-idem-b",
                                                                     "idem-bucket-setup")))
                    .Ok);
    const std::string delete_bucket = raftdemo::SerializeMetadataCommand(
        MakeDeleteBucketCommand("bucket-idem-b", "idem-bucket-delete"));
    EXPECT_TRUE(delete_bucket_machine.Apply(271, delete_bucket).Ok);
    const raftdemo::ApplyResult delete_bucket_replay =
        delete_bucket_machine.Apply(272, delete_bucket);
    EXPECT_TRUE(delete_bucket_replay.Ok);
    EXPECT_EQ(delete_bucket_replay.message, "idempotent replay");
    EXPECT_EQ(delete_bucket_machine.RequestCount(), 2U);
    EXPECT_EQ(delete_bucket_machine.LastAppliedIndex(), 271U);

    raftdemo::MetadataStateMachine create_object_machine;
    EXPECT_TRUE(create_object_machine.Apply(
                                    280, raftdemo::SerializeMetadataCommand(
                                             MakeCreateBucketCommand("bucket-idem-c",
                                                                     "idem-object-setup")))
                    .Ok);
    const std::string create_object = raftdemo::SerializeMetadataCommand(
        MakeCreateObjectCommand("bucket-idem-c", "object/a", "obj-36", "idem-object-create"));
    EXPECT_TRUE(create_object_machine.Apply(281, create_object).Ok);
    const raftdemo::ApplyResult create_object_replay =
        create_object_machine.Apply(282, create_object);
    EXPECT_TRUE(create_object_replay.Ok);
    EXPECT_EQ(create_object_replay.message, "idempotent replay");
    EXPECT_EQ(create_object_machine.ObjectCount(), 1U);
    EXPECT_EQ(create_object_machine.RequestCount(), 2U);
    EXPECT_EQ(create_object_machine.LastAppliedIndex(), 281U);

    raftdemo::MetadataStateMachine commit_machine;
    EXPECT_TRUE(commit_machine.Apply(
                             290, raftdemo::SerializeMetadataCommand(
                                      MakeCreateBucketCommand("bucket-idem-d",
                                                              "idem-commit-setup-bucket")))
                    .Ok);
    EXPECT_TRUE(commit_machine.Apply(
                             291, raftdemo::SerializeMetadataCommand(
                                      MakeCreateObjectCommand("bucket-idem-d", "object/a", "obj-37",
                                                              "idem-commit-setup-object")))
                    .Ok);
    const std::string commit_object = raftdemo::SerializeMetadataCommand(
        MakeCommitObjectCommand("bucket-idem-d", "object/a", "obj-37", "idem-object-commit"));
    EXPECT_TRUE(commit_machine.Apply(292, commit_object).Ok);
    const auto chunks_before = commit_machine.FindChunkRefs("bucket-idem-d", "object/a");
    ASSERT_TRUE(chunks_before.has_value());
    ASSERT_EQ(chunks_before->size(), 2U);
    const raftdemo::ApplyResult commit_replay = commit_machine.Apply(293, commit_object);
    EXPECT_TRUE(commit_replay.Ok);
    EXPECT_EQ(commit_replay.message, "idempotent replay");
    const auto chunks_after = commit_machine.FindChunkRefs("bucket-idem-d", "object/a");
    ASSERT_TRUE(chunks_after.has_value());
    EXPECT_EQ(chunks_after->size(), 2U);
    EXPECT_EQ(commit_machine.RequestCount(), 3U);
    EXPECT_EQ(commit_machine.LastAppliedIndex(), 292U);

    raftdemo::MetadataStateMachine abort_machine;
    EXPECT_TRUE(abort_machine.Apply(
                            300, raftdemo::SerializeMetadataCommand(
                                     MakeCreateBucketCommand("bucket-idem-e",
                                                             "idem-abort-setup-bucket")))
                    .Ok);
    EXPECT_TRUE(abort_machine.Apply(
                            301, raftdemo::SerializeMetadataCommand(
                                     MakeCreateObjectCommand("bucket-idem-e", "object/a", "obj-38",
                                                             "idem-abort-setup-object")))
                    .Ok);
    const std::string abort_object = raftdemo::SerializeMetadataCommand(
        MakeAbortObjectCommand("bucket-idem-e", "object/a", "obj-38", "idem-object-abort"));
    EXPECT_TRUE(abort_machine.Apply(302, abort_object).Ok);
    EXPECT_EQ(abort_machine.TombstoneCount(), 1U);
    EXPECT_FALSE(abort_machine.FindIndexedObjectId("bucket-idem-e", "object/a").has_value());
    const raftdemo::ApplyResult abort_replay = abort_machine.Apply(303, abort_object);
    EXPECT_TRUE(abort_replay.Ok);
    EXPECT_EQ(abort_replay.message, "idempotent replay");
    EXPECT_EQ(abort_machine.TombstoneCount(), 1U);
    EXPECT_FALSE(abort_machine.FindIndexedObjectId("bucket-idem-e", "object/a").has_value());
    EXPECT_EQ(abort_machine.RequestCount(), 3U);
    EXPECT_EQ(abort_machine.LastAppliedIndex(), 302U);

    raftdemo::MetadataStateMachine delete_machine;
    EXPECT_TRUE(delete_machine.Apply(
                             310, raftdemo::SerializeMetadataCommand(
                                      MakeCreateBucketCommand("bucket-idem-f",
                                                              "idem-delete-setup-bucket")))
                    .Ok);
    EXPECT_TRUE(delete_machine.Apply(
                             311, raftdemo::SerializeMetadataCommand(
                                      MakeCreateObjectCommand("bucket-idem-f", "object/a", "obj-39",
                                                              "idem-delete-setup-object")))
                    .Ok);
    EXPECT_TRUE(delete_machine.Apply(
                             312, raftdemo::SerializeMetadataCommand(
                                      MakeCommitObjectCommand("bucket-idem-f", "object/a", "obj-39",
                                                              "idem-delete-setup-commit")))
                    .Ok);
    const std::string delete_object = raftdemo::SerializeMetadataCommand(
        MakeDeleteObjectCommand("bucket-idem-f", "object/a", "obj-39", "idem-object-delete"));
    EXPECT_TRUE(delete_machine.Apply(313, delete_object).Ok);
    EXPECT_EQ(delete_machine.TombstoneCount(), 1U);
    EXPECT_FALSE(delete_machine.FindIndexedObjectId("bucket-idem-f", "object/a").has_value());
    const raftdemo::ApplyResult delete_replay = delete_machine.Apply(314, delete_object);
    EXPECT_TRUE(delete_replay.Ok);
    EXPECT_EQ(delete_replay.message, "idempotent replay");
    EXPECT_EQ(delete_machine.TombstoneCount(), 1U);
    EXPECT_FALSE(delete_machine.FindIndexedObjectId("bucket-idem-f", "object/a").has_value());
    EXPECT_EQ(delete_machine.RequestCount(), 4U);
    EXPECT_EQ(delete_machine.LastAppliedIndex(), 313U);
}

TEST(MetadataStateMachineTest, SameRequestIdWithDifferentPayloadOrCommandTypeReturnsConflict)
{
    raftdemo::MetadataStateMachine payload_machine;
    EXPECT_TRUE(payload_machine.Apply(
                                     320, raftdemo::SerializeMetadataCommand(
                                              MakeCreateBucketCommand("bucket-conflict-a",
                                                                      "same-request-id")))
                    .Ok);
    const raftdemo::ApplyResult payload_conflict = payload_machine.Apply(
        321, raftdemo::SerializeMetadataCommand(
                 MakeCreateBucketCommand("bucket-conflict-b", "same-request-id")));
    EXPECT_FALSE(payload_conflict.Ok);
    EXPECT_EQ(payload_conflict.message,
              "idempotency conflict: request_id maps to different command");
    EXPECT_EQ(payload_machine.BucketCount(), 1U);
    EXPECT_EQ(payload_machine.RequestCount(), 1U);
    EXPECT_EQ(payload_machine.LastAppliedIndex(), 320U);

    raftdemo::MetadataStateMachine type_machine;
    EXPECT_TRUE(type_machine.Apply(
                                  330, raftdemo::SerializeMetadataCommand(
                                           MakeCreateBucketCommand("bucket-conflict-c",
                                                                   "request-type-id")))
                    .Ok);
    const raftdemo::ApplyResult type_conflict = type_machine.Apply(
        331, raftdemo::SerializeMetadataCommand(
                 MakeDeleteBucketCommand("bucket-conflict-c", "request-type-id")));
    EXPECT_FALSE(type_conflict.Ok);
    EXPECT_EQ(type_conflict.message,
              "idempotency conflict: request_id maps to different command");
    EXPECT_EQ(type_machine.RequestCount(), 1U);
    EXPECT_EQ(type_machine.LastAppliedIndex(), 330U);
}

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

TEST(MetadataStateMachineTest, ConcurrentDuplicateRequestIdApplyStaysIdempotent)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           400,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-concurrent-idem",
                                                       "concurrent-idem-bucket")))
                    .Ok);

    const std::string create_object = raftdemo::SerializeMetadataCommand(
        MakeCreateObjectCommand("bucket-concurrent-idem", "object/a", "obj-concurrent-idem",
                                "concurrent-idem-object"));

    constexpr int kThreadCount = 8;
    std::atomic<bool> start{false};
    std::vector<int> oks(kThreadCount, 0);
    std::vector<std::string> messages(kThreadCount);
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (int i = 0; i < kThreadCount; ++i)
    {
        threads.emplace_back(
            [&, i]()
            {
                while (!start.load(std::memory_order_acquire))
                {
                }
                const raftdemo::ApplyResult result = machine.Apply(401, create_object);
                oks[static_cast<std::size_t>(i)] = result.Ok ? 1 : 0;
                messages[static_cast<std::size_t>(i)] = result.message;
            });
    }

    start.store(true, std::memory_order_release);
    for (std::thread &thread : threads)
    {
        thread.join();
    }

    int ok_count = 0;
    int replay_count = 0;
    for (int i = 0; i < kThreadCount; ++i)
    {
        EXPECT_EQ(oks[static_cast<std::size_t>(i)], 1);
        if (messages[static_cast<std::size_t>(i)] == "ok")
        {
            ++ok_count;
        }
        else if (messages[static_cast<std::size_t>(i)] == "idempotent replay")
        {
            ++replay_count;
        }
    }

    EXPECT_EQ(ok_count, 1);
    EXPECT_EQ(replay_count, kThreadCount - 1);
    EXPECT_EQ(machine.LastAppliedIndex(), 401U);
    EXPECT_EQ(machine.ObjectCount(), 1U);
    EXPECT_EQ(machine.RequestCount(), 2U);
    EXPECT_EQ(machine.FindIndexedObjectId("bucket-concurrent-idem", "object/a"),
              std::optional<std::string>("obj-concurrent-idem"));
}

TEST(MetadataStateMachineTest, ConcurrentHeadAndListReadsRemainConsistent)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           410,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-concurrent-read",
                                                       "concurrent-read-bucket")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           411,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-concurrent-read", "logs/a",
                                                       "obj-read-a", "concurrent-read-create-a")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           412,
                           raftdemo::SerializeMetadataCommand(
                               MakeCommitObjectCommand("bucket-concurrent-read", "logs/a",
                                                       "obj-read-a", "concurrent-read-commit-a")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           413,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-concurrent-read", "logs/b",
                                                       "obj-read-b", "concurrent-read-create-b")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           414,
                           raftdemo::SerializeMetadataCommand(
                               MakeCommitObjectCommand("bucket-concurrent-read", "logs/b",
                                                       "obj-read-b", "concurrent-read-commit-b")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           415,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-concurrent-read", "logs/pending",
                                                       "obj-read-pending",
                                                       "concurrent-read-create-pending")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           416,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateObjectCommand("bucket-concurrent-read", "logs/deleted",
                                                       "obj-read-deleted",
                                                       "concurrent-read-create-deleted")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           417,
                           raftdemo::SerializeMetadataCommand(
                               MakeCommitObjectCommand("bucket-concurrent-read", "logs/deleted",
                                                       "obj-read-deleted",
                                                       "concurrent-read-commit-deleted")))
                    .Ok);
    EXPECT_TRUE(machine.Apply(
                           418,
                           raftdemo::SerializeMetadataCommand(
                               MakeDeleteObjectCommand("bucket-concurrent-read", "logs/deleted",
                                                       "obj-read-deleted",
                                                       "concurrent-read-delete-deleted")))
                    .Ok);

    std::atomic<int> violations{0};
    constexpr int kReaderThreads = 4;
    constexpr int kIterations = 200;
    std::vector<std::thread> readers;
    readers.reserve(kReaderThreads);

    for (int i = 0; i < kReaderThreads; ++i)
    {
        readers.emplace_back(
            [&]()
            {
                for (int round = 0; round < kIterations; ++round)
                {
                    const auto head_a = machine.HeadObject(
                        {.bucket = "bucket-concurrent-read", .object_key = "logs/a"});
                    if (head_a.result.code != raftdemo::MetadataStatusCode::kOk ||
                        !head_a.record.has_value() ||
                        !head_a.record->IsCommitted())
                    {
                        ++violations;
                    }
                    if (!machine.FindIndexedObjectId("bucket-concurrent-read", "logs/a")
                             .has_value() ||
                        !machine.FindChunkRefs("bucket-concurrent-read", "logs/a").has_value())
                    {
                        ++violations;
                    }

                    const auto pending = machine.HeadObject(
                        {.bucket = "bucket-concurrent-read", .object_key = "logs/pending"});
                    if (pending.result.code != raftdemo::MetadataStatusCode::kNotFound)
                    {
                        ++violations;
                    }

                    const auto deleted = machine.HeadObject(
                        {.bucket = "bucket-concurrent-read", .object_key = "logs/deleted"});
                    if (deleted.result.code != raftdemo::MetadataStatusCode::kNotFound)
                    {
                        ++violations;
                    }

                    const auto listed = machine.ListObjects(
                        {.bucket = "bucket-concurrent-read", .prefix = "logs/"});
                    if (listed.result.code != raftdemo::MetadataStatusCode::kOk ||
                        listed.records.size() != 2U)
                    {
                        ++violations;
                        continue;
                    }
                    if (listed.records[0].object_key != "logs/a" ||
                        listed.records[1].object_key != "logs/b")
                    {
                        ++violations;
                    }
                    for (const auto &record : listed.records)
                    {
                        if (!record.IsCommitted() ||
                            !machine.FindIndexedObjectId(record.bucket, record.object_key)
                                 .has_value() ||
                            !machine.FindChunkRefs(record.bucket, record.object_key).has_value())
                        {
                            ++violations;
                        }
                    }
                }
            });
    }

    for (std::thread &thread : readers)
    {
        thread.join();
    }

    EXPECT_EQ(violations.load(), 0);
}

TEST(MetadataStateMachineTest, ConcurrentApplyAndQueryPreserveMetadataConsistency)
{
    raftdemo::MetadataStateMachine machine;
    EXPECT_TRUE(machine.Apply(
                           430,
                           raftdemo::SerializeMetadataCommand(
                               MakeCreateBucketCommand("bucket-concurrent-mixed",
                                                       "concurrent-mixed-bucket")))
                    .Ok);

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<int> violations{0};

    std::thread writer(
        [&]()
        {
            while (!start.load(std::memory_order_acquire))
            {
            }

            if (!machine.Apply(
                     431,
                     raftdemo::SerializeMetadataCommand(
                         MakeCreateObjectCommand("bucket-concurrent-mixed", "obj/live",
                                                 "obj-mixed-live",
                                                 "concurrent-mixed-create-live")))
                     .Ok)
            {
                ++violations;
            }
            std::this_thread::yield();
            if (!machine.Apply(
                     432,
                     raftdemo::SerializeMetadataCommand(
                         MakeCommitObjectCommand("bucket-concurrent-mixed", "obj/live",
                                                 "obj-mixed-live",
                                                 "concurrent-mixed-commit-live")))
                     .Ok)
            {
                ++violations;
            }
            std::this_thread::yield();
            if (!machine.Apply(
                     433,
                     raftdemo::SerializeMetadataCommand(
                         MakeCreateObjectCommand("bucket-concurrent-mixed", "obj/delete",
                                                 "obj-mixed-delete",
                                                 "concurrent-mixed-create-delete")))
                     .Ok)
            {
                ++violations;
            }
            std::this_thread::yield();
            if (!machine.Apply(
                     434,
                     raftdemo::SerializeMetadataCommand(
                         MakeCommitObjectCommand("bucket-concurrent-mixed", "obj/delete",
                                                 "obj-mixed-delete",
                                                 "concurrent-mixed-commit-delete")))
                     .Ok)
            {
                ++violations;
            }
            std::this_thread::yield();
            if (!machine.Apply(
                     435,
                     raftdemo::SerializeMetadataCommand(
                         MakeDeleteObjectCommand("bucket-concurrent-mixed", "obj/delete",
                                                 "obj-mixed-delete",
                                                 "concurrent-mixed-delete-delete")))
                     .Ok)
            {
                ++violations;
            }
            std::this_thread::yield();
            if (!machine.Apply(
                     436,
                     raftdemo::SerializeMetadataCommand(
                         MakeCreateObjectCommand("bucket-concurrent-mixed", "obj/abort",
                                                 "obj-mixed-abort",
                                                 "concurrent-mixed-create-abort")))
                     .Ok)
            {
                ++violations;
            }
            std::this_thread::yield();
            if (!machine.Apply(
                     437,
                     raftdemo::SerializeMetadataCommand(
                         MakeAbortObjectCommand("bucket-concurrent-mixed", "obj/abort",
                                                "obj-mixed-abort",
                                                "concurrent-mixed-abort-abort")))
                     .Ok)
            {
                ++violations;
            }
            done.store(true, std::memory_order_release);
        });

    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int i = 0; i < 3; ++i)
    {
        readers.emplace_back(
            [&]()
            {
                while (!start.load(std::memory_order_acquire))
                {
                }

                while (!done.load(std::memory_order_acquire))
                {
                    const auto live = machine.HeadObject(
                        {.bucket = "bucket-concurrent-mixed", .object_key = "obj/live"});
                    if (live.result.code == raftdemo::MetadataStatusCode::kOk)
                    {
                        if (!live.record.has_value() || !live.record->IsCommitted() ||
                            !machine.FindIndexedObjectId("bucket-concurrent-mixed", "obj/live")
                                 .has_value() ||
                            !machine.FindChunkRefs("bucket-concurrent-mixed", "obj/live")
                                 .has_value())
                        {
                            ++violations;
                        }
                    }

                    const auto deleted = machine.HeadObject(
                        {.bucket = "bucket-concurrent-mixed", .object_key = "obj/delete"});
                    if (deleted.result.code == raftdemo::MetadataStatusCode::kOk)
                    {
                        if (!deleted.record.has_value() || !deleted.record->IsCommitted() ||
                            !machine.FindIndexedObjectId("bucket-concurrent-mixed", "obj/delete")
                                 .has_value() ||
                            !machine.FindChunkRefs("bucket-concurrent-mixed", "obj/delete")
                                 .has_value())
                        {
                            ++violations;
                        }
                    }

                    const auto aborted = machine.HeadObject(
                        {.bucket = "bucket-concurrent-mixed", .object_key = "obj/abort"});
                    if (aborted.result.code == raftdemo::MetadataStatusCode::kOk)
                    {
                        ++violations;
                    }

                    const auto listed = machine.ListObjects(
                        {.bucket = "bucket-concurrent-mixed", .prefix = "obj/"});
                    if (listed.result.code != raftdemo::MetadataStatusCode::kOk)
                    {
                        ++violations;
                        continue;
                    }
                    for (const auto &record : listed.records)
                    {
                        if (!record.IsCommitted() ||
                            !machine.FindIndexedObjectId(record.bucket, record.object_key)
                                 .has_value() ||
                            !machine.FindChunkRefs(record.bucket, record.object_key).has_value() ||
                            record.object_key == "obj/abort")
                        {
                            ++violations;
                        }
                    }
                }
            });
    }

    start.store(true, std::memory_order_release);
    writer.join();
    for (std::thread &thread : readers)
    {
        thread.join();
    }

    EXPECT_EQ(violations.load(), 0);
    EXPECT_EQ(machine.LastAppliedIndex(), 437U);
    EXPECT_EQ(machine.LastAppliedTerm(), 0U);
    EXPECT_EQ(machine.ObjectCount(), 3U);
    EXPECT_EQ(machine.RequestCount(), 8U);
    EXPECT_EQ(machine.TombstoneCount(), 2U);

    const auto live = machine.HeadObject(
        {.bucket = "bucket-concurrent-mixed", .object_key = "obj/live"});
    ASSERT_EQ(live.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_TRUE(live.record.has_value());
    EXPECT_TRUE(live.record->IsCommitted());

    const auto deleted = machine.HeadObject(
        {.bucket = "bucket-concurrent-mixed", .object_key = "obj/delete"});
    EXPECT_EQ(deleted.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(machine.FindIndexedObjectId("bucket-concurrent-mixed", "obj/delete").has_value());
    EXPECT_FALSE(machine.FindChunkRefs("bucket-concurrent-mixed", "obj/delete").has_value());

    const auto aborted = machine.HeadObject(
        {.bucket = "bucket-concurrent-mixed", .object_key = "obj/abort"});
    EXPECT_EQ(aborted.result.code, raftdemo::MetadataStatusCode::kNotFound);
    EXPECT_FALSE(machine.FindIndexedObjectId("bucket-concurrent-mixed", "obj/abort").has_value());
    EXPECT_FALSE(machine.FindChunkRefs("bucket-concurrent-mixed", "obj/abort").has_value());

    const auto listed =
        machine.ListObjects({.bucket = "bucket-concurrent-mixed", .prefix = "obj/"});
    ASSERT_EQ(listed.result.code, raftdemo::MetadataStatusCode::kOk);
    ASSERT_EQ(listed.records.size(), 1U);
    EXPECT_EQ(listed.records[0].object_key, "obj/live");
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
