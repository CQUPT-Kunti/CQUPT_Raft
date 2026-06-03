#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"
#include "support/store_test_utils.h"

namespace storedemo
{
    namespace
    {
        ChunkChecksum ComputeChecksumOrThrow(const std::string_view payload)
        {
            ChunkChecksum checksum;
            std::string error_detail;
            const auto status =
                ComputeChunkChecksum(payload, &checksum, &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to compute checksum: " + error_detail);
            }
            return checksum;
        }

        ChunkIdentity MakeIdentityOrThrow(const std::string_view object_id,
                                          const std::uint64_t version,
                                          const std::uint32_t chunk_index,
                                          const std::uint64_t offset = 0)
        {
            ChunkId chunk_id;
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

            ChunkIdentity identity;
            identity.chunk_id = std::move(chunk_id);
            identity.object_id = std::string(object_id);
            identity.version = version;
            identity.chunk_index = chunk_index;
            identity.offset = offset;
            return identity;
        }

        WriteChunkRequest MakeWriteRequest(const ChunkIdentity &identity,
                                           const std::string &payload,
                                           const std::string &request_id)
        {
            return WriteChunkRequest{
                .request_id = request_id,
                .identity = identity,
                .expected_size = static_cast<std::uint64_t>(payload.size()),
                .expected_checksum = ComputeChecksumOrThrow(payload),
                .payload = payload};
        }

        ReadChunkRequest MakeReadRequest(const ChunkId &chunk_id,
                                         const std::string &request_id)
        {
            return ReadChunkRequest{
                .request_id = request_id,
                .chunk_id = chunk_id};
        }

        StatChunkRequest MakeStatRequest(const ChunkId &chunk_id,
                                         const std::string &request_id)
        {
            return StatChunkRequest{
                .request_id = request_id,
                .chunk_id = chunk_id};
        }

        ListChunksRequest MakeListRequest(const std::string &request_id)
        {
            return ListChunksRequest{
                .request_id = request_id};
        }

        std::filesystem::path ResolveFinalPathOrThrow(const std::filesystem::path &data_root,
                                                      const ChunkId &chunk_id)
        {
            ChunkPathLayout layout;
            std::string error_detail;
            const auto layout_status =
                BuildChunkPathLayout(chunk_id, "probe", &layout, &error_detail);
            if (layout_status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to build final path layout: " +
                                         error_detail);
            }

            std::filesystem::path final_path;
            const auto resolve_status = ResolveDurablePathUnderRoot(data_root,
                                                                    layout.final_relative_path,
                                                                    &final_path,
                                                                    &error_detail);
            if (resolve_status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to resolve final path: " + error_detail);
            }

            return final_path;
        }

        std::filesystem::path ResolveStagingPathOrThrow(const std::filesystem::path &data_root,
                                                        const ChunkId &chunk_id,
                                                        const std::string_view token)
        {
            ChunkPathLayout layout;
            std::string error_detail;
            const auto layout_status =
                BuildChunkPathLayout(chunk_id, token, &layout, &error_detail);
            if (layout_status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to build staging path layout: " +
                                         error_detail);
            }

            std::filesystem::path staging_path;
            const auto resolve_status = ResolveDurablePathUnderRoot(data_root,
                                                                    layout.staging_relative_path,
                                                                    &staging_path,
                                                                    &error_detail);
            if (resolve_status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to resolve staging path: " +
                                         error_detail);
            }

            return staging_path;
        }

        std::filesystem::path ResolveStatusPathOrThrow(const std::filesystem::path &data_root,
                                                       const std::string_view status_directory,
                                                       const ChunkId &chunk_id)
        {
            ChunkPathLayout layout;
            std::string error_detail;
            const auto layout_status =
                BuildChunkPathLayout(chunk_id, "probe", &layout, &error_detail);
            if (layout_status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to build status path layout: " +
                                         error_detail);
            }

            const auto relative_under_live =
                layout.final_relative_path.lexically_relative(
                    std::filesystem::path("chunks") / "live");
            const auto status_relative_path =
                std::filesystem::path("chunks") /
                std::filesystem::path(std::string(status_directory)) /
                relative_under_live;

            std::filesystem::path resolved_path;
            const auto resolve_status = ResolveDurablePathUnderRoot(data_root,
                                                                    status_relative_path,
                                                                    &resolved_path,
                                                                    &error_detail);
            if (resolve_status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to resolve status path: " +
                                         error_detail);
            }

            return resolved_path;
        }

        std::filesystem::path ResolveMisplacedLivePathOrThrow(
            const std::filesystem::path &data_root,
            const ChunkId &chunk_id)
        {
            ChunkPathLayout layout;
            std::string error_detail;
            const auto layout_status =
                BuildChunkPathLayout(chunk_id, "probe", &layout, &error_detail);
            if (layout_status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to build misplaced path layout: " +
                                         error_detail);
            }

            const auto relative_under_live =
                layout.final_relative_path.lexically_relative(
                    std::filesystem::path("chunks") / "live");
            auto component_it = relative_under_live.begin();
            if (component_it == relative_under_live.end())
            {
                throw std::runtime_error("relative live path is unexpectedly empty");
            }
            const std::string canonical_level_one = (*component_it++).string();
            if (component_it == relative_under_live.end())
            {
                throw std::runtime_error("relative live path is missing shard level two");
            }
            const std::string canonical_level_two = (*component_it++).string();
            if (component_it == relative_under_live.end())
            {
                throw std::runtime_error("relative live path is missing filename");
            }
            const std::string filename = (*component_it).string();

            const std::string alternate_level_one =
                canonical_level_one == "ff" ? "ee" : "ff";
            const std::string alternate_level_two =
                canonical_level_two == "ff" ? "ee" : "ff";
            const auto misplaced_relative_path =
                std::filesystem::path("chunks") / "live" / alternate_level_one /
                alternate_level_two / filename;

            std::filesystem::path resolved_path;
            const auto resolve_status = ResolveDurablePathUnderRoot(data_root,
                                                                    misplaced_relative_path,
                                                                    &resolved_path,
                                                                    &error_detail);
            if (resolve_status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to resolve misplaced path: " +
                                         error_detail);
            }

            return resolved_path;
        }

        void WriteBinaryFileOrThrow(const std::filesystem::path &path,
                                    const std::string_view payload)
        {
            std::error_code create_error;
            std::filesystem::create_directories(path.parent_path(), create_error);
            if (create_error)
            {
                throw std::runtime_error("failed to create parent directories for " +
                                         path.string() + ": " +
                                         create_error.message());
            }

            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output.is_open())
            {
                throw std::runtime_error("failed to open file for write: " +
                                         path.string());
            }
            output.write(payload.data(),
                         static_cast<std::streamsize>(payload.size()));
            output.close();
            if (!output)
            {
                throw std::runtime_error("failed to write file payload: " +
                                         path.string());
            }
        }

        void SetLastWriteTimeOrThrow(
            const std::filesystem::path &path,
            const std::filesystem::file_time_type write_time)
        {
            std::error_code error;
            std::filesystem::last_write_time(path, write_time, error);
            if (error)
            {
                throw std::runtime_error("failed to set last_write_time for " +
                                         path.string() + ": " +
                                         error.message());
            }
        }

        LocalDiskChunkStore MakeStore(const std::filesystem::path &data_dir,
                                      std::shared_ptr<ChunkIndex> chunk_index,
                                      const std::uint64_t staging_cleanup_grace_period_ms =
                                          5U * 60U * 1000U)
        {
            return LocalDiskChunkStore(LocalDiskChunkStoreConfig{
                .data_dir = data_dir,
                .node_id = "store-node-recovery",
                .durable_file = nullptr,
                .chunk_index = std::move(chunk_index),
                .executor = nullptr,
                .staging_cleanup_grace_period_ms =
                    staging_cleanup_grace_period_ms});
        }

        void ExpectChecksumEq(const ChunkChecksum &actual,
                              const ChunkChecksum &expected)
        {
            EXPECT_EQ(actual.algorithm, expected.algorithm);
            EXPECT_EQ(actual.value, expected.value);
            EXPECT_EQ(actual.size_bytes, expected.size_bytes);
        }
    }

    TEST(StorageNodeRecoveryTest, InitializeRebuildsPublishedLiveChunksIntoFreshIndexInStableOrder)
    {
        test::ScopedStoreTestDir temp_dir("storage_node_recovery_rebuilds_live_chunks");

        auto original_index = std::make_shared<ShardedChunkIndex>();
        auto original_store = MakeStore(temp_dir.root(), original_index);
        ASSERT_EQ(original_store.Initialize().status, StorageNodeStatusCode::kOk);

        const auto first_identity = MakeIdentityOrThrow("restart-live-a", 1, 0, 0);
        const auto second_identity = MakeIdentityOrThrow("restart-live-b", 1, 0, 128);
        const auto first_payload = test::MakeChunkPayload(17, "restart-live-a");
        const auto second_payload = test::MakeChunkPayload(23, "restart-live-b");

        ASSERT_EQ(original_store.WriteChunk(
                      MakeWriteRequest(first_identity,
                                       first_payload,
                                       "restart-live-write-a"))
                      .status,
                  StorageNodeStatusCode::kOk);
        ASSERT_EQ(original_store.WriteChunk(
                      MakeWriteRequest(second_identity,
                                       second_payload,
                                       "restart-live-write-b"))
                      .status,
                  StorageNodeStatusCode::kOk);

        auto rebuilt_index = std::make_shared<ShardedChunkIndex>();
        auto restarted_store = MakeStore(temp_dir.root(), rebuilt_index);
        const auto restarted_init = restarted_store.Initialize();
        ASSERT_EQ(restarted_init.status, StorageNodeStatusCode::kOk)
            << restarted_init.error_detail;

        const auto list_response =
            restarted_store.ListChunks(MakeListRequest("restart-live-list"));
        ASSERT_EQ(list_response.status, StorageNodeStatusCode::kOk);
        ASSERT_EQ(list_response.chunks.size(), 2U);
        EXPECT_EQ(list_response.chunks[0].identity.chunk_id, first_identity.chunk_id);
        EXPECT_EQ(list_response.chunks[1].identity.chunk_id, second_identity.chunk_id);

        const auto first_stat =
            restarted_store.StatChunk(MakeStatRequest(first_identity.chunk_id,
                                                      "restart-live-stat-a"));
        ASSERT_EQ(first_stat.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(first_stat.metadata.identity.chunk_id, first_identity.chunk_id);
        EXPECT_EQ(first_stat.metadata.identity.object_id, first_identity.object_id);
        EXPECT_EQ(first_stat.metadata.identity.version, first_identity.version);
        EXPECT_EQ(first_stat.metadata.identity.chunk_index, first_identity.chunk_index);
        EXPECT_EQ(first_stat.metadata.size,
                  static_cast<std::uint64_t>(first_payload.size()));
        EXPECT_EQ(first_stat.metadata.state, ChunkState::kLive);
        ExpectChecksumEq(first_stat.metadata.checksum,
                         ComputeChecksumOrThrow(first_payload));

        const auto second_read =
            restarted_store.ReadChunk(MakeReadRequest(second_identity.chunk_id,
                                                      "restart-live-read-b"));
        ASSERT_EQ(second_read.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(second_read.metadata.state, ChunkState::kLive);
        EXPECT_EQ(second_read.payload, second_payload);
        ExpectChecksumEq(second_read.actual_checksum,
                         ComputeChecksumOrThrow(second_payload));
    }

    TEST(StorageNodeRecoveryTest, InitializeSkipsStagingPartialAndNonLiveStatusFacts)
    {
        test::ScopedStoreTestDir temp_dir("storage_node_recovery_skips_non_live_facts");

        auto original_index = std::make_shared<ShardedChunkIndex>();
        auto original_store = MakeStore(temp_dir.root(), original_index);
        ASSERT_EQ(original_store.Initialize().status, StorageNodeStatusCode::kOk);

        const auto live_identity = MakeIdentityOrThrow("restart-live-main", 2, 0, 0);
        const auto staging_identity =
            MakeIdentityOrThrow("restart-staging", 2, 1, 64);
        const auto partial_staging_identity =
            MakeIdentityOrThrow("restart-partial-staging", 2, 2, 128);
        const auto deleting_identity =
            MakeIdentityOrThrow("restart-deleting", 2, 3, 192);
        const auto deleted_identity =
            MakeIdentityOrThrow("restart-deleted", 2, 4, 256);
        const auto quarantined_identity =
            MakeIdentityOrThrow("restart-quarantined", 2, 5, 320);
        const auto corrupted_identity =
            MakeIdentityOrThrow("restart-corrupted", 2, 6, 384);

        const auto live_payload = test::MakeChunkPayload(29, "restart-live-main");
        ASSERT_EQ(original_store.WriteChunk(
                      MakeWriteRequest(live_identity,
                                       live_payload,
                                       "restart-live-main-write"))
                      .status,
                  StorageNodeStatusCode::kOk);

        WriteBinaryFileOrThrow(
            ResolveStagingPathOrThrow(temp_dir.root(),
                                      staging_identity.chunk_id,
                                      "complete-stage"),
            test::MakeChunkPayload(13, "restart-staging"));
        WriteBinaryFileOrThrow(
            ResolveStagingPathOrThrow(temp_dir.root(),
                                      partial_staging_identity.chunk_id,
                                      "partial-stage"),
            test::MakeChunkPayload(5, "partial"));

        WriteBinaryFileOrThrow(
            ResolveStatusPathOrThrow(temp_dir.root(),
                                     "deleting",
                                     deleting_identity.chunk_id),
            test::MakeChunkPayload(7, "deleting"));
        WriteBinaryFileOrThrow(
            ResolveStatusPathOrThrow(temp_dir.root(),
                                     "deleted",
                                     deleted_identity.chunk_id),
            test::MakeChunkPayload(7, "deleted"));
        WriteBinaryFileOrThrow(
            ResolveStatusPathOrThrow(temp_dir.root(),
                                     "quarantine",
                                     quarantined_identity.chunk_id),
            test::MakeChunkPayload(11, "quarantine"));
        WriteBinaryFileOrThrow(
            ResolveStatusPathOrThrow(temp_dir.root(),
                                     "corrupted",
                                     corrupted_identity.chunk_id),
            test::MakeChunkPayload(11, "corrupted"));

        auto rebuilt_index = std::make_shared<ShardedChunkIndex>();
        auto restarted_store = MakeStore(temp_dir.root(), rebuilt_index);
        ASSERT_EQ(restarted_store.Initialize().status, StorageNodeStatusCode::kOk);

        const auto list_response =
            restarted_store.ListChunks(MakeListRequest("restart-skip-list"));
        ASSERT_EQ(list_response.status, StorageNodeStatusCode::kOk);
        ASSERT_EQ(list_response.chunks.size(), 1U);
        EXPECT_EQ(list_response.chunks[0].identity.chunk_id, live_identity.chunk_id);

        EXPECT_EQ(restarted_store.StatChunk(
                      MakeStatRequest(staging_identity.chunk_id, "restart-stage-stat"))
                      .status,
                  StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(restarted_store.StatChunk(
                      MakeStatRequest(partial_staging_identity.chunk_id,
                                      "restart-partial-stage-stat"))
                      .status,
                  StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(restarted_store.StatChunk(
                      MakeStatRequest(deleting_identity.chunk_id, "restart-deleting-stat"))
                      .status,
                  StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(restarted_store.StatChunk(
                      MakeStatRequest(deleted_identity.chunk_id, "restart-deleted-stat"))
                      .status,
                  StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(restarted_store.StatChunk(
                      MakeStatRequest(quarantined_identity.chunk_id,
                                      "restart-quarantined-stat"))
                      .status,
                  StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(restarted_store.StatChunk(
                      MakeStatRequest(corrupted_identity.chunk_id,
                                      "restart-corrupted-stat"))
                      .status,
                  StorageNodeStatusCode::kNotFound);
    }

    TEST(StorageNodeRecoveryTest, InitializeSkipsMalformedInvalidAndMisplacedLiveCandidates)
    {
        test::ScopedStoreTestDir temp_dir(
            "storage_node_recovery_skips_malformed_candidates");

        auto original_index = std::make_shared<ShardedChunkIndex>();
        auto original_store = MakeStore(temp_dir.root(), original_index);
        ASSERT_EQ(original_store.Initialize().status, StorageNodeStatusCode::kOk);

        const auto empty_identity = MakeIdentityOrThrow("restart-empty-live", 3, 0, 0);
        ASSERT_EQ(original_store.WriteChunk(
                      MakeWriteRequest(empty_identity, "", "restart-empty-live-write"))
                      .status,
                  StorageNodeStatusCode::kOk);

        WriteBinaryFileOrThrow(temp_dir.root() / "chunks" / "live" / "garbage.txt",
                               "garbage");
        WriteBinaryFileOrThrow(temp_dir.root() / "chunks" / "live" / "ff" / "ff" /
                                   "invalid chunk.chunk",
                               "invalid");

        const auto misplaced_identity =
            MakeIdentityOrThrow("restart-misplaced-live", 3, 1, 64);
        WriteBinaryFileOrThrow(
            ResolveMisplacedLivePathOrThrow(temp_dir.root(), misplaced_identity.chunk_id),
            test::MakeChunkPayload(8, "misplaced"));

        const auto non_regular_path =
            temp_dir.root() / "chunks" / "live" / "00" / "00" /
            "restart-directory-live~3~1.chunk";
        std::error_code create_error;
        std::filesystem::create_directories(non_regular_path, create_error);
        ASSERT_FALSE(create_error);

        auto rebuilt_index = std::make_shared<ShardedChunkIndex>();
        auto restarted_store = MakeStore(temp_dir.root(), rebuilt_index);
        ASSERT_EQ(restarted_store.Initialize().status, StorageNodeStatusCode::kOk);

        const auto list_response =
            restarted_store.ListChunks(MakeListRequest("restart-malformed-list"));
        ASSERT_EQ(list_response.status, StorageNodeStatusCode::kOk);
        ASSERT_EQ(list_response.chunks.size(), 1U);
        EXPECT_EQ(list_response.chunks[0].identity.chunk_id, empty_identity.chunk_id);
        EXPECT_EQ(list_response.chunks[0].size, 0U);
        ExpectChecksumEq(list_response.chunks[0].checksum, ComputeChecksumOrThrow(""));
        EXPECT_EQ(restarted_store.StatChunk(
                      MakeStatRequest(misplaced_identity.chunk_id,
                                      "restart-misplaced-stat"))
                      .status,
                  StorageNodeStatusCode::kNotFound);
    }

    TEST(StorageNodeRecoveryTest, InitializeRejectsDuplicateLiveChunkIdsAcrossPaths)
    {
        test::ScopedStoreTestDir temp_dir(
            "storage_node_recovery_rejects_duplicate_live_chunk_ids");

        auto original_index = std::make_shared<ShardedChunkIndex>();
        auto original_store = MakeStore(temp_dir.root(), original_index);
        ASSERT_EQ(original_store.Initialize().status, StorageNodeStatusCode::kOk);

        const auto duplicate_identity =
            MakeIdentityOrThrow("restart-duplicate-live", 4, 0, 0);
        const auto duplicate_payload =
            test::MakeChunkPayload(19, "restart-duplicate-live");
        ASSERT_EQ(original_store.WriteChunk(
                      MakeWriteRequest(duplicate_identity,
                                       duplicate_payload,
                                       "restart-duplicate-write"))
                      .status,
                  StorageNodeStatusCode::kOk);

        WriteBinaryFileOrThrow(
            ResolveMisplacedLivePathOrThrow(temp_dir.root(),
                                            duplicate_identity.chunk_id),
            duplicate_payload);

        auto rebuilt_index = std::make_shared<ShardedChunkIndex>();
        auto restarted_store = MakeStore(temp_dir.root(), rebuilt_index);
        const auto init_result = restarted_store.Initialize();
        EXPECT_EQ(init_result.status, StorageNodeStatusCode::kConflict);
        EXPECT_FALSE(init_result.initialized);
        EXPECT_NE(init_result.error_detail.find(duplicate_identity.chunk_id),
                  std::string::npos);
    }

    TEST(StorageNodeRecoveryTest, ExplicitRebuildClearsStaleIndexEntriesAndRehydratesLiveFacts)
    {
        test::ScopedStoreTestDir temp_dir(
            "storage_node_recovery_explicit_rebuild_clears_stale_entries");

        auto shared_index = std::make_shared<ShardedChunkIndex>();
        auto store = MakeStore(temp_dir.root(), shared_index);
        ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

        const auto live_identity = MakeIdentityOrThrow("restart-rebuild-live", 5, 0, 0);
        const auto stale_identity = MakeIdentityOrThrow("restart-stale-index", 5, 1, 64);
        const auto live_payload = test::MakeChunkPayload(31, "restart-rebuild-live");
        ASSERT_EQ(store.WriteChunk(
                      MakeWriteRequest(live_identity,
                                       live_payload,
                                       "restart-rebuild-write"))
                      .status,
                  StorageNodeStatusCode::kOk);

        ChunkIndexEntry stale_entry;
        stale_entry.identity = stale_identity;
        stale_entry.state = ChunkState::kLive;
        stale_entry.size = 123;
        stale_entry.checksum = ComputeChecksumOrThrow("stale");
        stale_entry.final_path = "chunks/live/aa/bb/stale.chunk";
        ASSERT_EQ(shared_index->Insert(stale_entry).status,
                  StorageNodeStatusCode::kOk);

        const auto rebuild_result = store.RebuildIndexFromDisk();
        ASSERT_EQ(rebuild_result.status, StorageNodeStatusCode::kOk)
            << rebuild_result.error_detail;

        EXPECT_EQ(shared_index->Find(stale_identity.chunk_id).status,
                  StorageNodeStatusCode::kNotFound);

        const auto live_entry = shared_index->Find(live_identity.chunk_id);
        ASSERT_EQ(live_entry.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(live_entry.entry.identity.chunk_id, live_identity.chunk_id);
        EXPECT_EQ(live_entry.entry.final_path,
                  ResolveFinalPathOrThrow(temp_dir.root(), live_identity.chunk_id)
                      .lexically_relative(temp_dir.root())
                      .lexically_normal());
        EXPECT_EQ(live_entry.entry.size,
                  static_cast<std::uint64_t>(live_payload.size()));
        ExpectChecksumEq(live_entry.entry.checksum,
                         ComputeChecksumOrThrow(live_payload));
    }

    TEST(StorageNodeRecoveryTest, InitializeCleansUpStaleAndPartialStagingWithoutAffectingLiveChunks)
    {
        test::ScopedStoreTestDir temp_dir(
            "storage_node_recovery_cleans_stale_staging");

        auto original_index = std::make_shared<ShardedChunkIndex>();
        auto original_store = MakeStore(temp_dir.root(), original_index);
        ASSERT_EQ(original_store.Initialize().status, StorageNodeStatusCode::kOk);

        const auto live_identity = MakeIdentityOrThrow("restart-live-cleanup", 6, 0, 0);
        const auto live_payload = test::MakeChunkPayload(21, "restart-live-cleanup");
        ASSERT_EQ(original_store.WriteChunk(
                      MakeWriteRequest(live_identity,
                                       live_payload,
                                       "restart-live-cleanup-write"))
                      .status,
                  StorageNodeStatusCode::kOk);

        const auto stale_staging_path =
            ResolveStagingPathOrThrow(temp_dir.root(),
                                      MakeIdentityOrThrow("restart-stale-staging", 6, 1, 64)
                                          .chunk_id,
                                      "stale-stage");
        const auto partial_staging_path =
            ResolveStagingPathOrThrow(temp_dir.root(),
                                      MakeIdentityOrThrow("restart-partial-staging-cleanup", 6, 2, 128)
                                          .chunk_id,
                                      "partial-stage");
        const auto fresh_staging_path =
            ResolveStagingPathOrThrow(temp_dir.root(),
                                      MakeIdentityOrThrow("restart-fresh-staging", 6, 3, 192)
                                          .chunk_id,
                                      "fresh-stage");
        const auto malformed_staging_path =
            temp_dir.root() / "chunks" / "staging" / "zz" / "zz" / "garbage.partial";

        WriteBinaryFileOrThrow(stale_staging_path, test::MakeChunkPayload(9, "stale"));
        WriteBinaryFileOrThrow(partial_staging_path, "partial");
        WriteBinaryFileOrThrow(fresh_staging_path, test::MakeChunkPayload(9, "fresh"));
        WriteBinaryFileOrThrow(malformed_staging_path, "garbage");

        const auto stale_time =
            std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
        SetLastWriteTimeOrThrow(stale_staging_path, stale_time);
        SetLastWriteTimeOrThrow(partial_staging_path, stale_time);
        SetLastWriteTimeOrThrow(malformed_staging_path, stale_time);

        auto rebuilt_index = std::make_shared<ShardedChunkIndex>();
        auto restarted_store = MakeStore(temp_dir.root(), rebuilt_index, 60U * 1000U);
        const auto init_result = restarted_store.Initialize();
        ASSERT_EQ(init_result.status, StorageNodeStatusCode::kOk)
            << init_result.error_detail;

        EXPECT_FALSE(std::filesystem::exists(stale_staging_path));
        EXPECT_FALSE(std::filesystem::exists(partial_staging_path));
        EXPECT_FALSE(std::filesystem::exists(malformed_staging_path));
        EXPECT_TRUE(std::filesystem::exists(fresh_staging_path));
        EXPECT_TRUE(std::filesystem::exists(ResolveFinalPathOrThrow(temp_dir.root(),
                                                                    live_identity.chunk_id)));

        const auto read_response =
            restarted_store.ReadChunk(MakeReadRequest(live_identity.chunk_id,
                                                      "restart-live-cleanup-read"));
        ASSERT_EQ(read_response.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(read_response.payload, live_payload);
    }

    TEST(StorageNodeRecoveryTest, InitializePreservesFreshStagingWithinGracePeriod)
    {
        test::ScopedStoreTestDir temp_dir(
            "storage_node_recovery_preserves_fresh_staging");

        const auto fresh_identity =
            MakeIdentityOrThrow("restart-keep-fresh-staging", 7, 0, 0);
        const auto fresh_staging_path =
            ResolveStagingPathOrThrow(temp_dir.root(),
                                      fresh_identity.chunk_id,
                                      "fresh-stage");
        WriteBinaryFileOrThrow(fresh_staging_path, test::MakeChunkPayload(7, "fresh"));

        auto rebuilt_index = std::make_shared<ShardedChunkIndex>();
        auto restarted_store = MakeStore(temp_dir.root(), rebuilt_index, 60U * 60U * 1000U);
        const auto init_result = restarted_store.Initialize();
        ASSERT_EQ(init_result.status, StorageNodeStatusCode::kOk)
            << init_result.error_detail;

        EXPECT_TRUE(std::filesystem::exists(fresh_staging_path));
        EXPECT_EQ(restarted_store.StatChunk(
                      MakeStatRequest(fresh_identity.chunk_id,
                                      "restart-keep-fresh-staging-stat"))
                      .status,
                  StorageNodeStatusCode::kNotFound);
    }

    TEST(StorageNodeRecoveryTest, InitializeReturnsExplicitErrorWhenStaleStagingCleanupFails)
    {
        test::ScopedStoreTestDir temp_dir(
            "storage_node_recovery_stale_staging_cleanup_failure");

        const auto first_stale_path =
            temp_dir.root() / "chunks" / "staging" / "00" / "00" / "aaa.tmp";
        const auto second_stale_path =
            temp_dir.root() / "chunks" / "staging" / "ff" / "ff" / "zzz.tmp";
        WriteBinaryFileOrThrow(first_stale_path, "first");
        WriteBinaryFileOrThrow(second_stale_path, "second");

        const auto stale_time =
            std::filesystem::file_time_type::clock::now() - std::chrono::minutes(10);
        SetLastWriteTimeOrThrow(first_stale_path, stale_time);
        SetLastWriteTimeOrThrow(second_stale_path, stale_time);

        std::error_code permission_error;
        std::filesystem::permissions(first_stale_path.parent_path(),
                                     std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::remove,
                                     permission_error);
        ASSERT_FALSE(permission_error) << permission_error.message();

        auto rebuilt_index = std::make_shared<ShardedChunkIndex>();
        auto restarted_store = MakeStore(temp_dir.root(), rebuilt_index, 60U * 1000U);
        const auto init_result = restarted_store.Initialize();

        std::filesystem::permissions(first_stale_path.parent_path(),
                                     std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::add,
                                     permission_error);
        ASSERT_FALSE(permission_error) << permission_error.message();

        EXPECT_EQ(init_result.status, StorageNodeStatusCode::kPermissionDenied);
        EXPECT_FALSE(init_result.initialized);
        EXPECT_NE(init_result.error_detail.find("aaa.tmp"), std::string::npos);
        EXPECT_TRUE(std::filesystem::exists(first_stale_path));
        EXPECT_TRUE(std::filesystem::exists(second_stale_path));
    }
} // namespace storedemo
