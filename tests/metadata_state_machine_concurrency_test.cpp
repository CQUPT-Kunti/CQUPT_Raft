#include "support/metadata_test_utils.h"
#include "raft/common/metadata_command.h"
#include "raft/state_machine/metadata_state_machine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

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
    using raftdemo::test::MakeDeleteObjectCommand;
} // namespace

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
                        if (!live.record.has_value() || !live.record->IsCommitted())
                        {
                            ++violations;
                        }
                    }

                    const auto deleted = machine.HeadObject(
                        {.bucket = "bucket-concurrent-mixed", .object_key = "obj/delete"});
                    if (deleted.result.code == raftdemo::MetadataStatusCode::kOk)
                    {
                        if (!deleted.record.has_value() || !deleted.record->IsCommitted())
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
                        if (!record.IsCommitted() || record.object_key == "obj/abort")
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
