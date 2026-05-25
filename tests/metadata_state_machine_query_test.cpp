#include "support/metadata_test_utils.h"
#include "raft/common/metadata_command.h"
#include "raft/state_machine/metadata_state_machine.h"

#include <gtest/gtest.h>

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
} // namespace

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
