#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"
#include "support/store_test_utils.h"

namespace storedemo
{
    namespace
    {
        struct TestOnlyRebuildResult
        {
            StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
            std::string error_detail;
            std::vector<ChunkIndexEntry> recovered_entries;
            std::vector<std::filesystem::path> skipped_candidates;

            [[nodiscard]] bool ok() const
            {
                return status == StorageNodeStatusCode::kOk;
            }
        };

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

        StorageNodeStatusCode ReadBinaryFile(const std::filesystem::path &path,
                                             std::string *payload,
                                             std::string *error_detail)
        {
            if (payload == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "payload output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "failed to open live chunk payload: " +
                                    path.string();
                }
                return StorageNodeStatusCode::kIoError;
            }

            payload->assign(std::istreambuf_iterator<char>(input),
                            std::istreambuf_iterator<char>());
            if (!input.good() && !input.eof())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "failed while reading live chunk payload: " +
                                    path.string();
                }
                return StorageNodeStatusCode::kIoError;
            }

            return StorageNodeStatusCode::kOk;
        }

        TestOnlyRebuildResult TestOnlyRebuildLiveIndexFromDisk(
            const std::filesystem::path &data_root,
            ChunkIndex *chunk_index)
        {
            TestOnlyRebuildResult result;
            if (chunk_index == nullptr)
            {
                result.status = StorageNodeStatusCode::kInvalidArgument;
                result.error_detail = "chunk_index must not be null";
                return result;
            }

            const auto live_root = data_root / "chunks" / "live";
            std::error_code exists_error;
            const bool live_root_exists =
                std::filesystem::exists(live_root, exists_error);
            if (exists_error)
            {
                result.status = StorageNodeStatusCode::kIoError;
                result.error_detail = "failed to inspect live root: " +
                                      exists_error.message();
                return result;
            }
            if (!live_root_exists)
            {
                return result;
            }

            std::vector<std::filesystem::path> regular_candidate_paths;
            std::error_code iter_error;
            std::filesystem::recursive_directory_iterator iter(live_root, iter_error);
            if (iter_error)
            {
                result.status = StorageNodeStatusCode::kIoError;
                result.error_detail = "failed to iterate live root: " +
                                      iter_error.message();
                return result;
            }

            for (const auto end = std::filesystem::recursive_directory_iterator();
                 iter != end;
                 iter.increment(iter_error))
            {
                if (iter_error)
                {
                    result.status = StorageNodeStatusCode::kIoError;
                    result.error_detail = "failed while scanning live root: " +
                                          iter_error.message();
                    return result;
                }

                std::error_code status_error;
                const bool is_regular = iter->is_regular_file(status_error);
                if (status_error)
                {
                    result.status = StorageNodeStatusCode::kIoError;
                    result.error_detail =
                        "failed to inspect recovery candidate type: " +
                        status_error.message();
                    return result;
                }

                const auto relative_path =
                    iter->path().lexically_relative(data_root).lexically_normal();
                if (!is_regular)
                {
                    if (relative_path.extension() == ".chunk")
                    {
                        result.skipped_candidates.push_back(relative_path);
                    }
                    continue;
                }

                regular_candidate_paths.push_back(relative_path);
            }

            std::sort(regular_candidate_paths.begin(), regular_candidate_paths.end());

            std::map<ChunkId, std::vector<std::filesystem::path>> live_candidates_by_chunk_id;
            for (const auto &relative_path : regular_candidate_paths)
            {
                if (relative_path.extension() != ".chunk")
                {
                    result.skipped_candidates.push_back(relative_path);
                    continue;
                }

                const ChunkId chunk_id = relative_path.stem().string();
                std::string validation_error;
                if (ValidateChunkId(chunk_id, &validation_error) !=
                    StorageNodeStatusCode::kOk)
                {
                    result.skipped_candidates.push_back(relative_path);
                    continue;
                }

                live_candidates_by_chunk_id[chunk_id].push_back(relative_path);
            }

            for (const auto &[chunk_id, relative_paths] : live_candidates_by_chunk_id)
            {
                if (relative_paths.size() > 1U)
                {
                    result.status = StorageNodeStatusCode::kConflict;
                    result.error_detail =
                        "duplicate live chunk candidates found for chunk_id " +
                        chunk_id;
                    return result;
                }

                ChunkPathLayout layout;
                std::string layout_error;
                const auto layout_status =
                    BuildChunkPathLayout(chunk_id, "rebuild", &layout, &layout_error);
                if (layout_status != StorageNodeStatusCode::kOk)
                {
                    result.status = layout_status;
                    result.error_detail = layout_error;
                    return result;
                }

                if (relative_paths.front() != layout.final_relative_path)
                {
                    result.skipped_candidates.push_back(relative_paths.front());
                }
            }

            for (const auto &[chunk_id, relative_paths] : live_candidates_by_chunk_id)
            {
                ChunkPathLayout layout;
                std::string layout_error;
                const auto layout_status =
                    BuildChunkPathLayout(chunk_id, "rebuild", &layout, &layout_error);
                if (layout_status != StorageNodeStatusCode::kOk)
                {
                    result.status = layout_status;
                    result.error_detail = layout_error;
                    return result;
                }

                const auto &relative_path = relative_paths.front();
                if (relative_path != layout.final_relative_path)
                {
                    continue;
                }

                std::filesystem::path final_path;
                std::string resolve_error;
                const auto resolve_status = ResolveDurablePathUnderRoot(data_root,
                                                                        relative_path,
                                                                        &final_path,
                                                                        &resolve_error);
                if (resolve_status != StorageNodeStatusCode::kOk)
                {
                    result.status = resolve_status;
                    result.error_detail = resolve_error;
                    return result;
                }

                std::string payload;
                result.status =
                    ReadBinaryFile(final_path, &payload, &result.error_detail);
                if (!result.ok())
                {
                    return result;
                }

                ChunkChecksum checksum;
                result.status =
                    ComputeChunkChecksum(payload, &checksum, &result.error_detail);
                if (!result.ok())
                {
                    return result;
                }

                ChunkIdentity identity;
                result.status =
                    ParseChunkId(chunk_id, &identity, &result.error_detail);
                if (!result.ok())
                {
                    return result;
                }

                ChunkIndexEntry entry;
                entry.identity = std::move(identity);
                entry.state = ChunkState::kLive;
                entry.size = static_cast<std::uint64_t>(payload.size());
                entry.checksum = checksum;
                entry.final_path = final_path;

                const auto insert_response = chunk_index->Insert(entry);
                if (!insert_response.ok())
                {
                    result.status = insert_response.status;
                    result.error_detail = insert_response.error_detail;
                    return result;
                }

                result.recovered_entries.push_back(insert_response.entry);
            }

            return result;
        }

        LocalDiskChunkStore MakeStore(const std::filesystem::path &data_dir,
                                      std::shared_ptr<ChunkIndex> chunk_index)
        {
            return LocalDiskChunkStore(LocalDiskChunkStoreConfig{
                .data_dir = data_dir,
                .node_id = "store-node-recovery",
                .durable_file = nullptr,
                .chunk_index = std::move(chunk_index),
                .executor = nullptr});
        }

        void ExpectChecksumEq(const ChunkChecksum &actual,
                              const ChunkChecksum &expected)
        {
            EXPECT_EQ(actual.algorithm, expected.algorithm);
            EXPECT_EQ(actual.value, expected.value);
            EXPECT_EQ(actual.size_bytes, expected.size_bytes);
        }
    }

    TEST(StorageNodeRecoveryTest, RebuildsPublishedLiveChunksIntoFreshIndexInStableOrder)
    {
        test::ScopedStoreTestDir temp_dir("storage_node_recovery_rebuilds_live_chunks");

        auto original_index = std::make_shared<ShardedChunkIndex>();
        auto original_store = MakeStore(temp_dir.root(), original_index);
        const auto init_result = original_store.Initialize();
        ASSERT_EQ(init_result.status, StorageNodeStatusCode::kOk);

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
        const auto rebuild_result =
            TestOnlyRebuildLiveIndexFromDisk(temp_dir.root(), rebuilt_index.get());
        ASSERT_EQ(rebuild_result.status, StorageNodeStatusCode::kOk)
            << rebuild_result.error_detail;
        ASSERT_EQ(rebuild_result.recovered_entries.size(), 2U);

        EXPECT_EQ(rebuild_result.recovered_entries[0].identity.chunk_id,
                  first_identity.chunk_id);
        EXPECT_EQ(rebuild_result.recovered_entries[0].size,
                  static_cast<std::uint64_t>(first_payload.size()));
        ExpectChecksumEq(rebuild_result.recovered_entries[0].checksum,
                         ComputeChecksumOrThrow(first_payload));
        EXPECT_EQ(rebuild_result.recovered_entries[0].state, ChunkState::kLive);

        EXPECT_EQ(rebuild_result.recovered_entries[1].identity.chunk_id,
                  second_identity.chunk_id);
        EXPECT_EQ(rebuild_result.recovered_entries[1].size,
                  static_cast<std::uint64_t>(second_payload.size()));
        ExpectChecksumEq(rebuild_result.recovered_entries[1].checksum,
                         ComputeChecksumOrThrow(second_payload));
        EXPECT_EQ(rebuild_result.recovered_entries[1].state, ChunkState::kLive);

        auto restarted_store = MakeStore(temp_dir.root(), rebuilt_index);
        const auto restarted_init = restarted_store.Initialize();
        ASSERT_EQ(restarted_init.status, StorageNodeStatusCode::kOk);

        const auto first_stat =
            restarted_store.StatChunk(MakeStatRequest(first_identity.chunk_id,
                                                      "restart-live-stat-a"));
        ASSERT_EQ(first_stat.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(first_stat.metadata.state, ChunkState::kLive);
        EXPECT_EQ(first_stat.metadata.size,
                  static_cast<std::uint64_t>(first_payload.size()));
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

    TEST(StorageNodeRecoveryTest, RebuildSkipsStagingPartialAndNonLiveStatusFacts)
    {
        test::ScopedStoreTestDir temp_dir("storage_node_recovery_skips_non_live_facts");

        auto original_index = std::make_shared<ShardedChunkIndex>();
        auto original_store = MakeStore(temp_dir.root(), original_index);
        const auto init_result = original_store.Initialize();
        ASSERT_EQ(init_result.status, StorageNodeStatusCode::kOk);

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
        const auto rebuild_result =
            TestOnlyRebuildLiveIndexFromDisk(temp_dir.root(), rebuilt_index.get());
        ASSERT_EQ(rebuild_result.status, StorageNodeStatusCode::kOk)
            << rebuild_result.error_detail;
        ASSERT_EQ(rebuild_result.recovered_entries.size(), 1U);
        EXPECT_EQ(rebuild_result.recovered_entries[0].identity.chunk_id,
                  live_identity.chunk_id);

        EXPECT_EQ(rebuilt_index->Find(live_identity.chunk_id).status,
                  StorageNodeStatusCode::kOk);
        EXPECT_EQ(rebuilt_index->Find(staging_identity.chunk_id).status,
                  StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(rebuilt_index->Find(partial_staging_identity.chunk_id).status,
                  StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(rebuilt_index->Find(deleting_identity.chunk_id).status,
                  StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(rebuilt_index->Find(deleted_identity.chunk_id).status,
                  StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(rebuilt_index->Find(quarantined_identity.chunk_id).status,
                  StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(rebuilt_index->Find(corrupted_identity.chunk_id).status,
                  StorageNodeStatusCode::kNotFound);
    }

    TEST(StorageNodeRecoveryTest, RebuildSkipsMalformedInvalidAndNonRegularLiveCandidates)
    {
        test::ScopedStoreTestDir temp_dir(
            "storage_node_recovery_skips_malformed_candidates");

        auto original_index = std::make_shared<ShardedChunkIndex>();
        auto original_store = MakeStore(temp_dir.root(), original_index);
        const auto init_result = original_store.Initialize();
        ASSERT_EQ(init_result.status, StorageNodeStatusCode::kOk);

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

        const auto non_regular_path =
            temp_dir.root() / "chunks" / "live" / "00" / "00" /
            "restart-directory-live~3~1.chunk";
        std::error_code create_error;
        std::filesystem::create_directories(non_regular_path, create_error);
        ASSERT_FALSE(create_error);

        auto rebuilt_index = std::make_shared<ShardedChunkIndex>();
        const auto rebuild_result =
            TestOnlyRebuildLiveIndexFromDisk(temp_dir.root(), rebuilt_index.get());
        ASSERT_EQ(rebuild_result.status, StorageNodeStatusCode::kOk)
            << rebuild_result.error_detail;
        ASSERT_EQ(rebuild_result.recovered_entries.size(), 1U);
        EXPECT_EQ(rebuild_result.recovered_entries[0].identity.chunk_id,
                  empty_identity.chunk_id);
        EXPECT_EQ(rebuild_result.recovered_entries[0].size, 0U);
        ExpectChecksumEq(rebuild_result.recovered_entries[0].checksum,
                         ComputeChecksumOrThrow(""));
        EXPECT_GE(rebuild_result.skipped_candidates.size(), 3U);
    }

    TEST(StorageNodeRecoveryTest, RebuildRejectsDuplicateLiveChunkIdsAcrossPaths)
    {
        test::ScopedStoreTestDir temp_dir(
            "storage_node_recovery_rejects_duplicate_live_chunk_ids");

        auto original_index = std::make_shared<ShardedChunkIndex>();
        auto original_store = MakeStore(temp_dir.root(), original_index);
        const auto init_result = original_store.Initialize();
        ASSERT_EQ(init_result.status, StorageNodeStatusCode::kOk);

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
        const auto rebuild_result =
            TestOnlyRebuildLiveIndexFromDisk(temp_dir.root(), rebuilt_index.get());
        EXPECT_EQ(rebuild_result.status, StorageNodeStatusCode::kConflict);
        EXPECT_NE(rebuild_result.error_detail.find(duplicate_identity.chunk_id),
                  std::string::npos);
    }
} // namespace storedemo
