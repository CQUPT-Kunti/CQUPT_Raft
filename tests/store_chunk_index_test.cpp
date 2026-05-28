#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "store/common/store_types.h"
#include "store/index/chunk_index.h"
#include "support/store_test_utils.h"

namespace storedemo
{
    namespace
    {
        using namespace std::chrono_literals;

        ChunkChecksum MakeEntryChecksum(const std::size_t size,
                                        const std::string_view seed)
        {
            ChunkChecksum checksum;
            const std::string payload = test::MakeChunkPayload(size, seed);
            std::string error_detail;
            const auto status = ComputeChunkChecksum(payload, &checksum, &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to build checksum fixture: " +
                                         error_detail);
            }
            return checksum;
        }

        ChunkIndexEntry MakeChunkIndexEntry(const std::string_view object_id,
                                            const std::uint64_t version,
                                            const std::uint32_t chunk_index,
                                            const ChunkState state,
                                            const std::uint64_t size = 16,
                                            const std::uint64_t updated_at = 0)
        {
            std::string chunk_id;
            std::string error_detail;
            const auto status = MakeChunkId(object_id,
                                            version,
                                            chunk_index,
                                            &chunk_id,
                                            &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to build chunk id: " + error_detail);
            }

            ChunkIndexEntry entry;
            entry.identity.chunk_id = chunk_id;
            entry.identity.object_id = std::string(object_id);
            entry.identity.version = version;
            entry.identity.chunk_index = chunk_index;
            entry.identity.offset = static_cast<std::uint64_t>(chunk_index) * 4096;
            entry.state = state;
            entry.size = size;
            entry.checksum = MakeEntryChecksum(size,
                                               std::string(object_id) + "-" +
                                                   std::to_string(chunk_index));
            entry.updated_at = updated_at;
            return entry;
        }

        std::string MakeChunkIdValue(const std::string_view object_id,
                                     const std::uint64_t version,
                                     const std::uint32_t chunk_index)
        {
            std::string chunk_id;
            std::string error_detail;
            const auto status = MakeChunkId(object_id,
                                            version,
                                            chunk_index,
                                            &chunk_id,
                                            &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to build chunk id: " + error_detail);
            }
            return chunk_id;
        }

        std::string FindChunkIdOnDifferentStripe(ShardedChunkIndex &index,
                                                 const std::size_t excluded_stripe)
        {
            for (std::size_t candidate = 0; candidate < 512; ++candidate)
            {
                const std::string chunk_id = MakeChunkIdValue(
                    "parallel-lock-" + std::to_string(candidate),
                    1,
                    0);
                const std::size_t stripe_index =
                    std::hash<std::string_view>{}(chunk_id) %
                    index.config().lock_stripe_count;
                if (stripe_index != excluded_stripe)
                {
                    return chunk_id;
                }
            }

            throw std::runtime_error("failed to find chunk id on different lock stripe");
        }

        std::vector<std::string> CollectChunkIds(const ChunkIndexListResponse &response)
        {
            std::vector<std::string> chunk_ids;
            chunk_ids.reserve(response.entries.size());
            for (const auto &entry : response.entries)
            {
                chunk_ids.push_back(entry.identity.chunk_id);
            }
            return chunk_ids;
        }

        class StoreChunkIndexTest : public ::testing::Test
        {
        protected:
            static ChunkIndexConfig MakeConfig()
            {
                return ChunkIndexConfig{
                    .shard_count = 4,
                    .lock_stripe_count = 8,
                    .default_page_size = 3,
                    .max_page_size = 16};
            }
        };

        TEST_F(StoreChunkIndexTest, InsertAndFindRoundTripStoresEntryAndShardHint)
        {
            ShardedChunkIndex index(MakeConfig());
            const ChunkIndexEntry entry =
                MakeChunkIndexEntry("object-live-a", 1, 0, ChunkState::kLive, 64, 1001);

            const auto insert_response = index.Insert(entry);
            EXPECT_EQ(insert_response.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(insert_response.inserted);
            EXPECT_EQ(insert_response.entry.identity.chunk_id, entry.identity.chunk_id);
            EXPECT_EQ(insert_response.entry.state, ChunkState::kLive);
            EXPECT_EQ(insert_response.entry.size, 64U);
            EXPECT_LT(insert_response.entry.lock_shard, index.config().shard_count);

            const auto find_response = index.Find(entry.identity.chunk_id);
            EXPECT_EQ(find_response.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(find_response.found);
            EXPECT_EQ(find_response.entry.identity.chunk_id, entry.identity.chunk_id);
            EXPECT_EQ(find_response.entry.identity.object_id, "object-live-a");
            EXPECT_EQ(find_response.entry.state, ChunkState::kLive);
            EXPECT_EQ(find_response.entry.size, 64U);
            EXPECT_EQ(find_response.entry.updated_at, 1001U);
        }

        TEST_F(StoreChunkIndexTest, DuplicateInsertReturnsAlreadyExistsAndKeepsStoredEntry)
        {
            ShardedChunkIndex index(MakeConfig());
            const ChunkIndexEntry original =
                MakeChunkIndexEntry("object-dup-a", 1, 0, ChunkState::kStaging, 32, 1002);
            ChunkIndexEntry duplicate = original;
            duplicate.state = ChunkState::kDeleted;
            duplicate.size = 96;
            duplicate.updated_at = 2002;

            EXPECT_EQ(index.Insert(original).status, StorageNodeStatusCode::kOk);

            const auto duplicate_response = index.Insert(duplicate);
            EXPECT_EQ(duplicate_response.status, StorageNodeStatusCode::kAlreadyExists);
            EXPECT_FALSE(duplicate_response.inserted);
            EXPECT_EQ(duplicate_response.entry.identity.chunk_id,
                      original.identity.chunk_id);
            EXPECT_EQ(duplicate_response.entry.state, ChunkState::kStaging);
            EXPECT_EQ(duplicate_response.entry.size, 32U);

            const auto find_response = index.Find(original.identity.chunk_id);
            EXPECT_EQ(find_response.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(find_response.found);
            EXPECT_EQ(find_response.entry.state, ChunkState::kStaging);
            EXPECT_EQ(find_response.entry.size, 32U);
            EXPECT_EQ(find_response.entry.updated_at, 1002U);
        }

        TEST_F(StoreChunkIndexTest, UpdateExistingEntrySupportsRepeatedOverwrite)
        {
            ShardedChunkIndex index(MakeConfig());
            const ChunkIndexEntry original =
                MakeChunkIndexEntry("object-update-a", 2, 1, ChunkState::kStaging, 48, 1003);
            ASSERT_EQ(index.Insert(original).status, StorageNodeStatusCode::kOk);

            ChunkIndexEntry live_entry = original;
            live_entry.state = ChunkState::kLive;
            live_entry.size = 80;
            live_entry.updated_at = 2003;
            const auto first_update = index.Update(live_entry);
            EXPECT_EQ(first_update.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(first_update.updated);
            EXPECT_EQ(first_update.entry.state, ChunkState::kLive);
            EXPECT_EQ(first_update.entry.size, 80U);

            ChunkIndexEntry corrupted_entry = live_entry;
            corrupted_entry.state = ChunkState::kCorrupted;
            corrupted_entry.size = 96;
            corrupted_entry.updated_at = 3003;
            const auto second_update = index.Update(corrupted_entry);
            EXPECT_EQ(second_update.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(second_update.updated);
            EXPECT_EQ(second_update.entry.state, ChunkState::kCorrupted);
            EXPECT_EQ(second_update.entry.size, 96U);

            const auto find_response = index.Find(original.identity.chunk_id);
            EXPECT_EQ(find_response.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(find_response.found);
            EXPECT_EQ(find_response.entry.state, ChunkState::kCorrupted);
            EXPECT_EQ(find_response.entry.size, 96U);
            EXPECT_EQ(find_response.entry.updated_at, 3003U);
        }

        TEST_F(StoreChunkIndexTest, UpdateFindAndRemoveMissingReturnNotFound)
        {
            ShardedChunkIndex index(MakeConfig());
            const ChunkIndexEntry missing_entry =
                MakeChunkIndexEntry("object-missing-a", 3, 0, ChunkState::kMissing, 0, 1004);
            std::string missing_chunk_id;
            std::string error_detail;
            ASSERT_EQ(MakeChunkId("object-missing-b",
                                  3,
                                  1,
                                  &missing_chunk_id,
                                  &error_detail),
                      StorageNodeStatusCode::kOk);

            const auto update_response = index.Update(missing_entry);
            EXPECT_EQ(update_response.status, StorageNodeStatusCode::kNotFound);
            EXPECT_FALSE(update_response.updated);

            const auto find_response = index.Find(missing_chunk_id);
            EXPECT_EQ(find_response.status, StorageNodeStatusCode::kNotFound);
            EXPECT_FALSE(find_response.found);

            const auto remove_response = index.Remove(missing_chunk_id);
            EXPECT_EQ(remove_response.status, StorageNodeStatusCode::kNotFound);
            EXPECT_FALSE(remove_response.removed);
        }

        TEST_F(StoreChunkIndexTest, RemoveExistingEntryMakesSubsequentLookupMissing)
        {
            ShardedChunkIndex index(MakeConfig());
            const ChunkIndexEntry entry =
                MakeChunkIndexEntry("object-remove-a", 4, 0, ChunkState::kDeleting, 24, 1005);
            ASSERT_EQ(index.Insert(entry).status, StorageNodeStatusCode::kOk);

            const auto remove_response = index.Remove(entry.identity.chunk_id);
            EXPECT_EQ(remove_response.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(remove_response.removed);
            EXPECT_EQ(remove_response.entry.identity.chunk_id, entry.identity.chunk_id);
            EXPECT_EQ(remove_response.entry.state, ChunkState::kDeleting);

            const auto find_response = index.Find(entry.identity.chunk_id);
            EXPECT_EQ(find_response.status, StorageNodeStatusCode::kNotFound);
            EXPECT_FALSE(find_response.found);
        }

        TEST_F(StoreChunkIndexTest, ListSupportsStateFilteringAndQuarantineFlag)
        {
            ShardedChunkIndex index(MakeConfig());
            const std::vector<ChunkIndexEntry> entries{
                MakeChunkIndexEntry("state-live", 1, 0, ChunkState::kLive),
                MakeChunkIndexEntry("state-staging", 1, 0, ChunkState::kStaging),
                MakeChunkIndexEntry("state-deleting", 1, 0, ChunkState::kDeleting),
                MakeChunkIndexEntry("state-deleted", 1, 0, ChunkState::kDeleted),
                MakeChunkIndexEntry("state-quarantined", 1, 0, ChunkState::kQuarantined),
                MakeChunkIndexEntry("state-corrupted", 1, 0, ChunkState::kCorrupted),
                MakeChunkIndexEntry("state-missing", 1, 0, ChunkState::kMissing),
            };
            for (const auto &entry : entries)
            {
                ASSERT_EQ(index.Insert(entry).status, StorageNodeStatusCode::kOk);
            }

            ChunkIndexListOptions all_options;
            all_options.page_size = entries.size();
            const auto all_response = index.List(all_options);
            EXPECT_EQ(all_response.status, StorageNodeStatusCode::kOk);
            EXPECT_EQ(all_response.entries.size(), entries.size());
            EXPECT_GT(all_response.snapshot_epoch, 0U);

            ChunkIndexListOptions live_options;
            live_options.state_filter = ChunkState::kLive;
            const auto live_response = index.List(live_options);
            ASSERT_EQ(live_response.status, StorageNodeStatusCode::kOk);
            ASSERT_EQ(live_response.entries.size(), 1U);
            EXPECT_EQ(live_response.entries.front().state, ChunkState::kLive);

            ChunkIndexListOptions quarantined_hidden_options;
            quarantined_hidden_options.page_size = entries.size();
            quarantined_hidden_options.include_quarantine = false;
            const auto quarantined_hidden_response =
                index.List(quarantined_hidden_options);
            EXPECT_EQ(quarantined_hidden_response.status, StorageNodeStatusCode::kOk);
            EXPECT_EQ(quarantined_hidden_response.entries.size(), entries.size() - 1);
            EXPECT_TRUE(std::none_of(quarantined_hidden_response.entries.begin(),
                                     quarantined_hidden_response.entries.end(),
                                     [](const ChunkIndexEntry &entry)
                                     {
                                         return entry.state == ChunkState::kQuarantined;
                                     }));

            ChunkIndexListOptions quarantined_only_options;
            quarantined_only_options.state_filter = ChunkState::kQuarantined;
            quarantined_only_options.include_quarantine = false;
            const auto quarantined_only_response =
                index.List(quarantined_only_options);
            ASSERT_EQ(quarantined_only_response.status, StorageNodeStatusCode::kOk);
            ASSERT_EQ(quarantined_only_response.entries.size(), 1U);
            EXPECT_EQ(quarantined_only_response.entries.front().state,
                      ChunkState::kQuarantined);
        }

        TEST_F(StoreChunkIndexTest, ListPaginatesInChunkIdOrderAcrossMultipleShards)
        {
            ShardedChunkIndex index(MakeConfig());
            const std::vector<std::string> object_ids{
                "page-a", "page-b", "page-c", "page-d",
                "page-e", "page-f", "page-g", "page-h"};

            std::vector<std::string> expected_chunk_ids;
            expected_chunk_ids.reserve(object_ids.size());
            std::set<std::size_t> touched_shards;
            for (std::size_t i = 0; i < object_ids.size(); ++i)
            {
                const ChunkIndexEntry entry =
                    MakeChunkIndexEntry(object_ids[i],
                                        1,
                                        0,
                                        ChunkState::kLive,
                                        16 + i,
                                        4000 + i);
                const auto insert_response = index.Insert(entry);
                ASSERT_EQ(insert_response.status, StorageNodeStatusCode::kOk);
                expected_chunk_ids.push_back(entry.identity.chunk_id);
                touched_shards.insert(insert_response.entry.lock_shard);
            }
            ASSERT_GT(touched_shards.size(), 1U);
            std::sort(expected_chunk_ids.begin(), expected_chunk_ids.end());

            ChunkIndexListOptions first_page_options;
            first_page_options.page_size = 3;
            const auto first_page = index.List(first_page_options);
            ASSERT_EQ(first_page.status, StorageNodeStatusCode::kOk);
            ASSERT_EQ(first_page.entries.size(), 3U);
            EXPECT_EQ(CollectChunkIds(first_page),
                      std::vector<std::string>(expected_chunk_ids.begin(),
                                               expected_chunk_ids.begin() + 3));
            EXPECT_EQ(first_page.next_page_token, expected_chunk_ids[2]);

            ChunkIndexListOptions second_page_options;
            second_page_options.page_size = 3;
            second_page_options.page_token = first_page.next_page_token;
            const auto second_page = index.List(second_page_options);
            ASSERT_EQ(second_page.status, StorageNodeStatusCode::kOk);
            ASSERT_EQ(second_page.entries.size(), 3U);
            EXPECT_EQ(CollectChunkIds(second_page),
                      std::vector<std::string>(expected_chunk_ids.begin() + 3,
                                               expected_chunk_ids.begin() + 6));
            EXPECT_EQ(second_page.next_page_token, expected_chunk_ids[5]);
            EXPECT_EQ(first_page.snapshot_epoch, second_page.snapshot_epoch);

            ChunkIndexListOptions third_page_options;
            third_page_options.page_size = 3;
            third_page_options.page_token = second_page.next_page_token;
            const auto third_page = index.List(third_page_options);
            ASSERT_EQ(third_page.status, StorageNodeStatusCode::kOk);
            ASSERT_EQ(third_page.entries.size(), 2U);
            EXPECT_EQ(CollectChunkIds(third_page),
                      std::vector<std::string>(expected_chunk_ids.begin() + 6,
                                               expected_chunk_ids.end()));
            EXPECT_TRUE(third_page.next_page_token.empty());
            EXPECT_EQ(second_page.snapshot_epoch, third_page.snapshot_epoch);
        }

        TEST_F(StoreChunkIndexTest, AcquireChunkLockRejectsInvalidChunkId)
        {
            ShardedChunkIndex index(MakeConfig());

            const auto empty_response = index.AcquireChunkLock("");
            EXPECT_EQ(empty_response.status, StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(empty_response.acquired);
            EXPECT_FALSE(empty_response.guard.owns_lock());

            const auto unsafe_response = index.AcquireChunkLock("../chunk");
            EXPECT_EQ(unsafe_response.status, StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(unsafe_response.acquired);
            EXPECT_FALSE(unsafe_response.guard.owns_lock());
        }

        TEST_F(StoreChunkIndexTest, SameChunkLockSerializesConflictingOperations)
        {
            ShardedChunkIndex index(MakeConfig());
            const std::string chunk_id = MakeChunkIdValue("lock-serial-a", 1, 0);

            std::promise<void> worker_ready_promise;
            std::future<void> worker_ready = worker_ready_promise.get_future();
            std::promise<void> acquired_promise;
            std::future<void> acquired_future = acquired_promise.get_future();
            std::atomic<bool> entered_critical{false};

            std::thread worker;
            {
                auto first_lock = index.AcquireChunkLock(chunk_id);
                ASSERT_EQ(first_lock.status, StorageNodeStatusCode::kOk);
                ASSERT_TRUE(first_lock.acquired);
                ASSERT_TRUE(first_lock.guard.owns_lock());

                worker = std::thread([&]()
                {
                    worker_ready_promise.set_value();
                    auto second_lock = index.AcquireChunkLock(chunk_id);
                    EXPECT_EQ(second_lock.status, StorageNodeStatusCode::kOk);
                    EXPECT_TRUE(second_lock.acquired);
                    EXPECT_TRUE(second_lock.guard.owns_lock());
                    entered_critical.store(true, std::memory_order_release);
                    acquired_promise.set_value();
                });

                ASSERT_EQ(worker_ready.wait_for(200ms), std::future_status::ready);
                EXPECT_EQ(acquired_future.wait_for(80ms), std::future_status::timeout);
                EXPECT_FALSE(entered_critical.load(std::memory_order_acquire));
            }

            EXPECT_EQ(acquired_future.wait_for(500ms), std::future_status::ready);
            EXPECT_TRUE(entered_critical.load(std::memory_order_acquire));
            worker.join();
        }

        TEST_F(StoreChunkIndexTest, DifferentChunkLocksOnDifferentStripesCanProceedInParallel)
        {
            ShardedChunkIndex index(MakeConfig());
            const std::string first_chunk_id = MakeChunkIdValue("lock-parallel-a", 1, 0);

            auto first_lock = index.AcquireChunkLock(first_chunk_id);
            ASSERT_EQ(first_lock.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(first_lock.acquired);
            ASSERT_TRUE(first_lock.guard.owns_lock());

            const std::size_t first_stripe = first_lock.guard.stripe_index();
            const std::string second_chunk_id =
                FindChunkIdOnDifferentStripe(index, first_stripe);

            std::promise<void> acquired_promise;
            std::future<void> acquired_future = acquired_promise.get_future();
            std::promise<std::size_t> stripe_promise;
            std::future<std::size_t> stripe_future = stripe_promise.get_future();

            std::thread worker([&]()
            {
                auto second_lock = index.AcquireChunkLock(second_chunk_id);
                EXPECT_EQ(second_lock.status, StorageNodeStatusCode::kOk);
                EXPECT_TRUE(second_lock.acquired);
                EXPECT_TRUE(second_lock.guard.owns_lock());
                stripe_promise.set_value(second_lock.guard.stripe_index());
                acquired_promise.set_value();
                std::this_thread::sleep_for(40ms);
            });

            EXPECT_EQ(acquired_future.wait_for(200ms), std::future_status::ready);
            EXPECT_NE(stripe_future.get(), first_stripe);
            worker.join();
        }

        TEST_F(StoreChunkIndexTest, GuardReleaseAllowsSameChunkToBeLockedAgain)
        {
            ShardedChunkIndex index(MakeConfig());
            const std::string chunk_id = MakeChunkIdValue("lock-release-a", 1, 0);

            std::size_t first_stripe = 0;
            {
                auto first_lock = index.AcquireChunkLock(chunk_id);
                ASSERT_EQ(first_lock.status, StorageNodeStatusCode::kOk);
                ASSERT_TRUE(first_lock.acquired);
                ASSERT_TRUE(first_lock.guard.owns_lock());
                first_stripe = first_lock.guard.stripe_index();
            }

            const auto second_lock = index.AcquireChunkLock(chunk_id);
            EXPECT_EQ(second_lock.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(second_lock.acquired);
            EXPECT_TRUE(second_lock.guard.owns_lock());
            EXPECT_EQ(second_lock.guard.stripe_index(), first_stripe);
            EXPECT_EQ(second_lock.guard.chunk_id(), chunk_id);
        }
    } // namespace
} // namespace storedemo
