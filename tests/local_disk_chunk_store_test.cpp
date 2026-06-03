#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"
#include "support/store_test_utils.h"

namespace storedemo
{
    namespace
    {
        struct FixtureBinaryPayload
        {
            std::string payload;
            std::filesystem::path source_path;
            bool used_repo_fixture{false};
        };

        FixtureBinaryPayload LoadFixtureBinaryPayload()
        {
            const std::filesystem::path repo_root =
                std::filesystem::path(__FILE__).parent_path().parent_path();
            const std::filesystem::path primary_path =
                repo_root / "tests" / "test_file" / "test_file.deb";
            const std::filesystem::path fallback_path =
                repo_root / "test" / "test_file" / "test_file.deb";

            for (const auto &candidate : {primary_path, fallback_path})
            {
                if (!std::filesystem::exists(candidate))
                {
                    continue;
                }

                std::ifstream input(candidate, std::ios::binary);
                if (!input.is_open())
                {
                    throw std::runtime_error("failed to open binary fixture: " +
                                             candidate.string());
                }

                return FixtureBinaryPayload{
                    .payload = std::string(std::istreambuf_iterator<char>(input),
                                           std::istreambuf_iterator<char>()),
                    .source_path = candidate,
                    .used_repo_fixture = true};
            }

            std::string payload;
            payload.reserve(4096);
            for (std::size_t index = 0; index < 4096; ++index)
            {
                payload.push_back(static_cast<char>(index % 251));
            }

            return FixtureBinaryPayload{
                .payload = std::move(payload),
                .source_path = {},
                .used_repo_fixture = false};
        }

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

        DeleteChunkRequest MakeDeleteRequest(const ChunkId &chunk_id,
                                             const std::string &request_id)
        {
            return DeleteChunkRequest{
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

        std::string ReadBinaryFileOrThrow(const std::filesystem::path &path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
            {
                throw std::runtime_error("failed to open file: " + path.string());
            }

            return std::string(std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>());
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
                throw std::runtime_error("failed to build chunk layout: " + error_detail);
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
                throw std::runtime_error("failed to build chunk layout: " + error_detail);
            }

            std::filesystem::path staging_path;
            const auto resolve_status = ResolveDurablePathUnderRoot(data_root,
                                                                    layout.staging_relative_path,
                                                                    &staging_path,
                                                                    &error_detail);
            if (resolve_status != StorageNodeStatusCode::kOk)
            {
                throw std::runtime_error("failed to resolve staging path: " + error_detail);
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

        ChunkIndexEntry MakeIndexEntry(const ChunkIdentity &identity,
                                       const ChunkState state,
                                       const std::uint64_t size,
                                       const ChunkChecksum &checksum,
                                       const std::filesystem::path &final_path)
        {
            ChunkIndexEntry entry;
            entry.identity = identity;
            entry.state = state;
            entry.size = size;
            entry.checksum = checksum;
            entry.final_path = final_path;
            entry.updated_at = 1;
            return entry;
        }

        struct RecordingWriterState
        {
            DurableFileResult append_result;
            DurableFileResult flush_result{.durable_boundary_reached = true};
            DurableFileResult close_result;
            std::size_t append_calls{0};
            std::size_t flush_calls{0};
            std::size_t close_calls{0};
            std::string appended_payload;
        };

        class RecordingDurableFileWriter : public DurableFileWriter
        {
        public:
            RecordingDurableFileWriter(std::shared_ptr<RecordingWriterState> state,
                                       std::filesystem::path path)
                : state_(std::move(state))
                , path_(std::move(path))
            {
            }

            DurableFileResult Append(const DurableAppendRequest &request) override
            {
                ++state_->append_calls;
                const auto *chars =
                    reinterpret_cast<const char *>(request.buffer.data());
                state_->appended_payload.assign(chars, chars + request.buffer.size());

                DurableFileResult result = state_->append_result;
                if (result.ok())
                {
                    result.bytes_transferred = request.buffer.size();
                }
                return result;
            }

            DurableFileResult Flush(const DurableFlushRequest &) override
            {
                ++state_->flush_calls;
                return state_->flush_result;
            }

            DurableFileResult Close(const DurableCloseRequest &) override
            {
                ++state_->close_calls;
                return state_->close_result;
            }

            const std::filesystem::path &path() const override
            {
                return path_;
            }

        private:
            std::shared_ptr<RecordingWriterState> state_;
            std::filesystem::path path_;
        };

        class RecordingDurableFile : public DurableFile
        {
        public:
            explicit RecordingDurableFile(std::shared_ptr<RecordingWriterState> writer_state)
                : writer_state_(std::move(writer_state))
            {
                publish_result.durable_boundary_reached = true;
                sync_result.durable_boundary_reached = true;
            }

            DurableFileResult publish_result;
            DurableFileResult sync_result;
            std::size_t open_calls{0};
            std::size_t publish_calls{0};
            std::size_t sync_calls{0};
            std::filesystem::path last_open_relative_path;
            std::filesystem::path last_publish_staging_path;
            std::filesystem::path last_publish_final_path;
            std::filesystem::path last_sync_directory_path;

            NormalizeDurablePathResponse NormalizePath(
                const NormalizeDurablePathRequest &request) override
            {
                NormalizeDurablePathResponse response;
                response.normalized_path = request.relative_path;
                return response;
            }

            OpenStagingWriterResponse OpenStagingWriter(
                const OpenStagingWriterRequest &request) override
            {
                ++open_calls;
                last_open_relative_path = request.relative_path;

                OpenStagingWriterResponse response;
                response.normalized_path = request.relative_path;
                response.writer = std::make_unique<RecordingDurableFileWriter>(
                    writer_state_, request.relative_path);
                return response;
            }

            DurableFileResult PublishStagedFile(
                const PublishDurableFileRequest &request) override
            {
                ++publish_calls;
                last_publish_staging_path = request.staging_path;
                last_publish_final_path = request.final_path;
                return publish_result;
            }

            DurableFileResult SyncDirectory(
                const SyncDurableDirectoryRequest &request) override
            {
                ++sync_calls;
                last_sync_directory_path = request.directory_path;
                return sync_result;
            }

        private:
            std::shared_ptr<RecordingWriterState> writer_state_;
        };

        class LocalDiskChunkStoreTest : public ::testing::Test
        {
        protected:
            static LocalDiskChunkStoreConfig MakeConfig(
                const std::filesystem::path &data_dir,
                const StorageNodeId &node_id = test::MakeStorageNodeIdFixture(1))
            {
                return LocalDiskChunkStoreConfig{
                    .data_dir = data_dir,
                    .node_id = node_id};
            }

            static std::shared_ptr<ShardedChunkIndex> MakeSharedIndex()
            {
                return std::make_shared<ShardedChunkIndex>();
            }
        };

        TEST_F(LocalDiskChunkStoreTest, InitializeCreatesExpectedDirectoryLayout)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_init");
            const std::filesystem::path data_dir = temp_dir.Path("node-data");
            LocalDiskChunkStore store(MakeConfig(data_dir));

            const auto result = store.Initialize();
            ASSERT_EQ(result.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(result.initialized);
            ASSERT_TRUE(store.initialized());
            ASSERT_TRUE(result.paths.IsInitialized());

            const auto expected_data_root = std::filesystem::absolute(data_dir).lexically_normal();
            EXPECT_EQ(result.paths.data_root, expected_data_root);
            EXPECT_EQ(result.paths.chunks_root, expected_data_root / "chunks");
            EXPECT_EQ(result.paths.live_root, expected_data_root / "chunks" / "live");
            EXPECT_EQ(result.paths.staging_root,
                      expected_data_root / "chunks" / "staging");

            EXPECT_EQ(store.paths().data_root, result.paths.data_root);
            EXPECT_TRUE(std::filesystem::exists(result.paths.data_root));
            EXPECT_TRUE(std::filesystem::is_directory(result.paths.data_root));
            EXPECT_TRUE(std::filesystem::exists(result.paths.chunks_root));
            EXPECT_TRUE(std::filesystem::is_directory(result.paths.chunks_root));
            EXPECT_TRUE(std::filesystem::exists(result.paths.live_root));
            EXPECT_TRUE(std::filesystem::is_directory(result.paths.live_root));
            EXPECT_TRUE(std::filesystem::exists(result.paths.staging_root));
            EXPECT_TRUE(std::filesystem::is_directory(result.paths.staging_root));
        }

        TEST_F(LocalDiskChunkStoreTest, InitializeUsesTemporaryDirectoryWithoutFixedAbsolutePath)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_isolated_root");
            const std::filesystem::path data_dir = temp_dir.Path("isolated-data-root");
            LocalDiskChunkStore store(MakeConfig(data_dir, test::MakeStorageNodeIdFixture(2)));

            const auto result = store.Initialize();
            ASSERT_EQ(result.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(result.initialized);

            const auto temp_root = std::filesystem::absolute(temp_dir.root()).lexically_normal();
            const auto data_root = result.paths.data_root.lexically_normal();
            auto mismatch = std::mismatch(temp_root.begin(),
                                          temp_root.end(),
                                          data_root.begin(),
                                          data_root.end());
            EXPECT_EQ(mismatch.first, temp_root.end());
            EXPECT_NE(data_root, temp_root);
        }

        TEST_F(LocalDiskChunkStoreTest, InitializeRejectsEmptyDataDirAndNodeId)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_invalid_config");

            LocalDiskChunkStore empty_data_dir_store(LocalDiskChunkStoreConfig{
                .data_dir = {},
                .node_id = test::MakeStorageNodeIdFixture(3)});
            const auto empty_data_dir_result = empty_data_dir_store.Initialize();
            EXPECT_EQ(empty_data_dir_result.status,
                      StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(empty_data_dir_result.initialized);
            EXPECT_NE(empty_data_dir_result.error_detail.find("data_dir must not be empty"),
                      std::string::npos);

            LocalDiskChunkStore empty_node_id_store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = {}});
            const auto empty_node_id_result = empty_node_id_store.Initialize();
            EXPECT_EQ(empty_node_id_result.status,
                      StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(empty_node_id_result.initialized);
            EXPECT_NE(empty_node_id_result.error_detail.find("node_id must not be empty"),
                      std::string::npos);
        }

        TEST_F(LocalDiskChunkStoreTest, InitializeReturnsExplicitErrorWhenDirectoryPathConflicts)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_dir_conflict");
            const std::filesystem::path data_dir = temp_dir.Path("node-data");
            ASSERT_TRUE(std::filesystem::create_directories(data_dir));

            {
                std::ofstream conflict_file(data_dir / "chunks");
                ASSERT_TRUE(conflict_file.is_open());
                conflict_file << "not-a-directory";
            }

            LocalDiskChunkStore store(MakeConfig(data_dir, test::MakeStorageNodeIdFixture(4)));
            const auto result = store.Initialize();

            EXPECT_EQ(result.status, StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(result.initialized);
            EXPECT_NE(result.error_detail.find("not a directory"), std::string::npos);
            EXPECT_FALSE(store.initialized());
        }

        TEST_F(LocalDiskChunkStoreTest, InitializeCreatesDefaultDurableFileAndChunkIndexWhenUnset)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_default_dependencies");
            const std::filesystem::path data_dir = temp_dir.Path("node-data");
            LocalDiskChunkStore store(MakeConfig(data_dir, test::MakeStorageNodeIdFixture(5)));

            const auto result = store.Initialize();
            ASSERT_EQ(result.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(result.initialized);
            ASSERT_NE(store.durable_file(), nullptr);
            ASSERT_NE(store.chunk_index(), nullptr);
            EXPECT_EQ(store.executor(), nullptr);

            EXPECT_NE(dynamic_cast<ShardedChunkIndex *>(store.chunk_index()), nullptr);

#if defined(__linux__)
            auto *durable_file = dynamic_cast<LinuxDurableFile *>(store.durable_file());
            ASSERT_NE(durable_file, nullptr);
            EXPECT_EQ(durable_file->root_path(), result.paths.data_root);
#elif defined(_WIN32)
            auto *durable_file = dynamic_cast<WindowsDurableFile *>(store.durable_file());
            ASSERT_NE(durable_file, nullptr);
            EXPECT_EQ(durable_file->root_path(), result.paths.data_root);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, DeleteStatAndListRejectEmptyRequestId)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_unsupported_ops");
            LocalDiskChunkStore store(MakeConfig(temp_dir.Path("node-data"),
                                                 test::MakeStorageNodeIdFixture(6)));
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto delete_response = store.DeleteChunk(DeleteChunkRequest{});
            EXPECT_EQ(delete_response.status, StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(delete_response.ok());
            EXPECT_NE(delete_response.error_detail.find("DeleteChunk"), std::string::npos);

            const auto stat_response = store.StatChunk(StatChunkRequest{});
            EXPECT_EQ(stat_response.status, StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(stat_response.ok());
            EXPECT_NE(stat_response.error_detail.find("StatChunk"), std::string::npos);

            const auto list_response = store.ListChunks(ListChunksRequest{});
            EXPECT_EQ(list_response.status, StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(list_response.ok());
            EXPECT_NE(list_response.error_detail.find("ListChunks"), std::string::npos);
        }

        TEST_F(LocalDiskChunkStoreTest, WriteChunkRejectsExpectedSizeMismatchWithoutCreatingLiveEntry)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_size_mismatch");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(7),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("size-mismatch-object", 1, 0, 0);
            auto request = MakeWriteRequest(identity,
                                            test::MakeChunkPayload(32, "size-mismatch"),
                                            "size-mismatch-request");
            request.expected_size = static_cast<std::uint64_t>(request.payload.size() + 1);

            const auto response = store.WriteChunk(request);
            EXPECT_EQ(response.status, StorageNodeStatusCode::kInvalidArgument);
            EXPECT_FALSE(response.ok());

            const auto index_find = shared_index->Find(identity.chunk_id);
            EXPECT_EQ(index_find.status, StorageNodeStatusCode::kNotFound);

            const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id);
            EXPECT_FALSE(std::filesystem::exists(final_path));
        }

        TEST_F(LocalDiskChunkStoreTest, WriteChunkRejectsExpectedChecksumMismatchWithoutCreatingLiveEntry)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_checksum_mismatch");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(8),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("checksum-mismatch-object", 1, 0, 0);
            auto request = MakeWriteRequest(identity,
                                            test::MakeChunkPayload(48, "checksum-mismatch"),
                                            "checksum-mismatch-request");
            request.expected_checksum = ComputeChecksumOrThrow("different-payload");

            const auto response = store.WriteChunk(request);
            EXPECT_EQ(response.status, StorageNodeStatusCode::kChecksumMismatch);
            EXPECT_FALSE(response.ok());

            const auto index_find = shared_index->Find(identity.chunk_id);
            EXPECT_EQ(index_find.status, StorageNodeStatusCode::kNotFound);

            const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id);
            EXPECT_FALSE(std::filesystem::exists(final_path));
        }

        TEST_F(LocalDiskChunkStoreTest, WriteChunkReturnsIoErrorWhenFlushFailsAndDoesNotUpdateLiveIndex)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_flush_failure");
            const auto shared_index = MakeSharedIndex();
            auto writer_state = std::make_shared<RecordingWriterState>();
            writer_state->flush_result.error = DurableFileErrorCode::kIoError;
            writer_state->flush_result.error_detail = "flush failed";
            auto durable_file = std::make_shared<RecordingDurableFile>(writer_state);

            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(9),
                .durable_file = durable_file,
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("flush-failure-object", 1, 0, 0);
            const auto request = MakeWriteRequest(identity,
                                                  test::MakeChunkPayload(24, "flush"),
                                                  "flush-request");

            const auto response = store.WriteChunk(request);
            EXPECT_EQ(response.status, StorageNodeStatusCode::kIoError);
            EXPECT_FALSE(response.ok());
            EXPECT_EQ(writer_state->append_calls, 1U);
            EXPECT_EQ(writer_state->flush_calls, 1U);
            EXPECT_EQ(writer_state->close_calls, 0U);
            EXPECT_EQ(durable_file->publish_calls, 0U);
            EXPECT_EQ(durable_file->sync_calls, 0U);

            const auto index_find = shared_index->Find(identity.chunk_id);
            EXPECT_EQ(index_find.status, StorageNodeStatusCode::kNotFound);
        }

        TEST_F(LocalDiskChunkStoreTest, WriteChunkReturnsExplicitErrorWhenDirectorySyncFailsAfterPublish)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_sync_failure");
            const auto shared_index = MakeSharedIndex();
            auto writer_state = std::make_shared<RecordingWriterState>();
            auto durable_file = std::make_shared<RecordingDurableFile>(writer_state);
            durable_file->publish_result.durable_boundary_reached = true;
            durable_file->sync_result.error = DurableFileErrorCode::kDirectorySyncFailed;
            durable_file->sync_result.error_detail = "directory sync failed";

            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(10),
                .durable_file = durable_file,
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("sync-failure-object", 1, 0, 0);
            const auto request = MakeWriteRequest(identity,
                                                  test::MakeChunkPayload(28, "sync"),
                                                  "sync-request");

            const auto response = store.WriteChunk(request);
            EXPECT_EQ(response.status, StorageNodeStatusCode::kIoError);
            EXPECT_FALSE(response.ok());
            EXPECT_EQ(writer_state->append_calls, 1U);
            EXPECT_EQ(writer_state->flush_calls, 1U);
            EXPECT_EQ(writer_state->close_calls, 1U);
            EXPECT_EQ(durable_file->publish_calls, 1U);
            EXPECT_EQ(durable_file->sync_calls, 1U);

            const auto index_find = shared_index->Find(identity.chunk_id);
            EXPECT_EQ(index_find.status, StorageNodeStatusCode::kNotFound);
        }

        TEST_F(LocalDiskChunkStoreTest, WriteChunkWithSmallPayloadPublishesFinalFileAndLiveIndex)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real durable publish path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_small_payload");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(11),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto payload = test::MakeChunkPayload(64, "small-binary");
            const auto identity = MakeIdentityOrThrow("small-payload-object", 1, 0, 0);
            const auto request = MakeWriteRequest(identity, payload, "small-write-request");

            const auto response = store.WriteChunk(request);
            ASSERT_EQ(response.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(response.ok());
            EXPECT_TRUE(response.durable);
            EXPECT_FALSE(response.already_exists);
            EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);
            EXPECT_EQ(response.metadata.node_id, test::MakeStorageNodeIdFixture(11));
            EXPECT_EQ(response.metadata.size, payload.size());
            EXPECT_EQ(response.metadata.state, ChunkState::kLive);
            EXPECT_EQ(response.metadata.checksum.value, request.expected_checksum.value);

            const auto index_find = shared_index->Find(identity.chunk_id);
            ASSERT_EQ(index_find.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(index_find.found);
            EXPECT_EQ(index_find.entry.state, ChunkState::kLive);
            EXPECT_EQ(index_find.entry.size, payload.size());
            EXPECT_EQ(index_find.entry.checksum.value, request.expected_checksum.value);
            ASSERT_TRUE(std::filesystem::exists(index_find.entry.final_path));
            EXPECT_EQ(ReadBinaryFileOrThrow(index_find.entry.final_path), payload);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, WriteChunkWithBinaryFixturePublishesExpectedSizeAndChecksum)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real durable publish path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_binary_fixture");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(12),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto fixture = LoadFixtureBinaryPayload();
            ASSERT_FALSE(fixture.payload.empty());

            const auto identity = MakeIdentityOrThrow("binary-fixture-object", 1, 0, 0);
            const auto request =
                MakeWriteRequest(identity, fixture.payload, "binary-fixture-request");

            const auto response = store.WriteChunk(request);
            ASSERT_EQ(response.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(response.ok());
            EXPECT_TRUE(response.durable);
            EXPECT_EQ(response.metadata.size, fixture.payload.size());
            EXPECT_EQ(response.metadata.checksum.value, request.expected_checksum.value);

            const auto index_find = shared_index->Find(identity.chunk_id);
            ASSERT_EQ(index_find.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(index_find.found);
            EXPECT_EQ(index_find.entry.state, ChunkState::kLive);
            EXPECT_EQ(index_find.entry.size, fixture.payload.size());
            EXPECT_EQ(index_find.entry.checksum.value, request.expected_checksum.value);
            ASSERT_TRUE(std::filesystem::exists(index_find.entry.final_path));
            EXPECT_EQ(ReadBinaryFileOrThrow(index_find.entry.final_path), fixture.payload);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, WriteChunkDuplicateWithSameContentReturnsAlreadyExists)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real durable publish path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_duplicate_same");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(13),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("duplicate-same-object", 1, 0, 0);
            const auto payload = test::MakeChunkPayload(40, "duplicate-same");
            const auto first_request = MakeWriteRequest(identity, payload, "duplicate-first");
            const auto second_request = MakeWriteRequest(identity, payload, "duplicate-second");

            ASSERT_EQ(store.WriteChunk(first_request).status, StorageNodeStatusCode::kOk);
            const auto duplicate_response = store.WriteChunk(second_request);
            EXPECT_EQ(duplicate_response.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(duplicate_response.ok());
            EXPECT_TRUE(duplicate_response.durable);
            EXPECT_TRUE(duplicate_response.already_exists);

            const auto index_find = shared_index->Find(identity.chunk_id);
            ASSERT_EQ(index_find.status, StorageNodeStatusCode::kOk);
            EXPECT_EQ(index_find.entry.size, payload.size());
            EXPECT_EQ(index_find.entry.checksum.value,
                      first_request.expected_checksum.value);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, WriteChunkDuplicateWithDifferentContentReturnsConflict)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real durable publish path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_duplicate_conflict");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(14),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("duplicate-conflict-object", 1, 0, 0);
            const auto original_payload = test::MakeChunkPayload(44, "duplicate-original");
            const auto conflicting_payload = test::MakeChunkPayload(44, "duplicate-other");
            const auto original_request =
                MakeWriteRequest(identity, original_payload, "conflict-first");
            const auto conflicting_request =
                MakeWriteRequest(identity, conflicting_payload, "conflict-second");

            ASSERT_EQ(store.WriteChunk(original_request).status, StorageNodeStatusCode::kOk);
            const auto conflict_response = store.WriteChunk(conflicting_request);
            EXPECT_EQ(conflict_response.status, StorageNodeStatusCode::kConflict);
            EXPECT_FALSE(conflict_response.ok());

            const auto index_find = shared_index->Find(identity.chunk_id);
            ASSERT_EQ(index_find.status, StorageNodeStatusCode::kOk);
            EXPECT_EQ(index_find.entry.size, original_payload.size());
            EXPECT_EQ(index_find.entry.checksum.value,
                      original_request.expected_checksum.value);
            EXPECT_NE(index_find.entry.checksum.value,
                      conflicting_request.expected_checksum.value);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkAfterWriteReturnsOriginalSmallPayload)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local file read path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_small");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(15),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto payload = test::MakeChunkPayload(72, "read-small");
            const auto identity = MakeIdentityOrThrow("read-small-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, payload, "read-small-write")).status,
                      StorageNodeStatusCode::kOk);

            const auto response =
                store.ReadChunk(MakeReadRequest(identity.chunk_id, "read-small-read"));
            ASSERT_EQ(response.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(response.ok());
            EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);
            EXPECT_EQ(response.metadata.state, ChunkState::kLive);
            EXPECT_EQ(response.payload, payload);
            EXPECT_EQ(response.actual_checksum.value, ComputeChecksumOrThrow(payload).value);
            EXPECT_TRUE(response.verified);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkAfterWriteReturnsOriginalBinaryFixture)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local file read path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_fixture");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(16),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto fixture = LoadFixtureBinaryPayload();
            ASSERT_FALSE(fixture.payload.empty());
            const auto identity = MakeIdentityOrThrow("read-fixture-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, fixture.payload, "read-fixture-write")).status,
                      StorageNodeStatusCode::kOk);

            auto request = MakeReadRequest(identity.chunk_id, "read-fixture-read");
            request.expected_checksum = ComputeChecksumOrThrow(fixture.payload);
            const auto response = store.ReadChunk(request);
            ASSERT_EQ(response.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(response.ok());
            EXPECT_EQ(response.payload, fixture.payload);
            EXPECT_EQ(response.actual_checksum.value, request.expected_checksum.value);
            EXPECT_TRUE(response.verified);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkSupportsEmptyPayload)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local file read path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_empty");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(17),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("read-empty-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, "", "read-empty-write")).status,
                      StorageNodeStatusCode::kOk);

            const auto response =
                store.ReadChunk(MakeReadRequest(identity.chunk_id, "read-empty-read"));
            ASSERT_EQ(response.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(response.ok());
            EXPECT_TRUE(response.payload.empty());
            EXPECT_EQ(response.metadata.size, 0U);
            EXPECT_EQ(response.actual_checksum.value, ComputeChecksumOrThrow("").value);
            EXPECT_TRUE(response.verified);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkRejectsExpectedChecksumMismatch)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local file read path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_checksum_mismatch");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(18),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto payload = test::MakeChunkPayload(64, "read-checksum");
            const auto identity = MakeIdentityOrThrow("read-checksum-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, payload, "read-checksum-write")).status,
                      StorageNodeStatusCode::kOk);

            auto request = MakeReadRequest(identity.chunk_id, "read-checksum-read");
            request.expected_checksum = ComputeChecksumOrThrow("different-payload");
            const auto response = store.ReadChunk(request);
            EXPECT_EQ(response.status, StorageNodeStatusCode::kChecksumMismatch);
            EXPECT_FALSE(response.ok());
            EXPECT_TRUE(response.payload.empty());
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkRejectsTamperedFinalFile)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local file read path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_tampered");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(19),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto payload = test::MakeChunkPayload(80, "read-tampered");
            const auto identity = MakeIdentityOrThrow("read-tampered-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, payload, "read-tampered-write")).status,
                      StorageNodeStatusCode::kOk);

            const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id);
            {
                std::ofstream output(final_path, std::ios::binary | std::ios::trunc);
                ASSERT_TRUE(output.is_open());
                output << test::MakeChunkPayload(payload.size(), "tampered");
            }

            const auto response =
                store.ReadChunk(MakeReadRequest(identity.chunk_id, "read-tampered-read"));
            EXPECT_EQ(response.status, StorageNodeStatusCode::kCorrupted);
            EXPECT_FALSE(response.ok());
            EXPECT_TRUE(response.payload.empty());

            const auto quarantine_path = ResolveStatusPathOrThrow(temp_dir.Path("node-data"),
                                                                  "quarantine",
                                                                  identity.chunk_id);
            EXPECT_FALSE(std::filesystem::exists(final_path));
            EXPECT_TRUE(std::filesystem::exists(quarantine_path));

            const auto entry_response = shared_index->Find(identity.chunk_id);
            ASSERT_TRUE(entry_response.ok()) << entry_response.error_detail;
            EXPECT_EQ(entry_response.entry.state, ChunkState::kQuarantined);
            EXPECT_EQ(entry_response.entry.final_path,
                      quarantine_path.lexically_relative(temp_dir.Path("node-data"))
                          .lexically_normal());

            const auto second_read =
                store.ReadChunk(MakeReadRequest(identity.chunk_id,
                                                "read-tampered-read-after-quarantine"));
            EXPECT_EQ(second_read.status, StorageNodeStatusCode::kCorrupted);
            EXPECT_FALSE(second_read.ok());

            auto list_request = MakeListRequest("read-tampered-list");
            list_request.options.include_quarantine = true;
            const auto list_response = store.ListChunks(list_request);
            ASSERT_EQ(list_response.status, StorageNodeStatusCode::kOk);
            ASSERT_EQ(list_response.chunks.size(), 1U);
            EXPECT_EQ(list_response.chunks[0].identity.chunk_id, identity.chunk_id);
            EXPECT_EQ(list_response.chunks[0].state, ChunkState::kQuarantined);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkReturnsNotFoundWhenChunkIsMissingFromIndex)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_not_found");
            LocalDiskChunkStore store(MakeConfig(temp_dir.Path("node-data"),
                                                 test::MakeStorageNodeIdFixture(20)));
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto missing_identity = MakeIdentityOrThrow("read-missing-object", 1, 0, 0);
            const auto response =
                store.ReadChunk(MakeReadRequest(missing_identity.chunk_id, "read-missing-read"));
            EXPECT_EQ(response.status, StorageNodeStatusCode::kNotFound);
            EXPECT_FALSE(response.ok());
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkRejectsNonLiveCorruptedState)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_corrupted_state");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(21),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("read-corrupted-object", 1, 0, 0);
            const auto payload = test::MakeChunkPayload(16, "read-corrupted");
            const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id);
            ASSERT_TRUE(std::filesystem::create_directories(final_path.parent_path()));
            {
                std::ofstream output(final_path, std::ios::binary | std::ios::trunc);
                ASSERT_TRUE(output.is_open());
                output << payload;
            }

            ASSERT_TRUE(shared_index->Insert(MakeIndexEntry(identity,
                                                            ChunkState::kCorrupted,
                                                            payload.size(),
                                                            ComputeChecksumOrThrow(payload),
                                                            final_path))
                            .ok());

            const auto response =
                store.ReadChunk(MakeReadRequest(identity.chunk_id, "read-corrupted-state"));
            EXPECT_EQ(response.status, StorageNodeStatusCode::kCorrupted);
            EXPECT_FALSE(response.ok());
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkRejectsNonLiveStagingState)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_staging_state");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(22),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("read-staging-object", 1, 0, 0);
            const auto payload = test::MakeChunkPayload(16, "read-staging");
            const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id);

            ASSERT_TRUE(shared_index->Insert(MakeIndexEntry(identity,
                                                            ChunkState::kStaging,
                                                            payload.size(),
                                                            ComputeChecksumOrThrow(payload),
                                                            final_path))
                            .ok());

            const auto response =
                store.ReadChunk(MakeReadRequest(identity.chunk_id, "read-staging-state"));
            EXPECT_EQ(response.status, StorageNodeStatusCode::kConflict);
            EXPECT_FALSE(response.ok());
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkRejectsNonLiveQuarantinedState)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_quarantined_state");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(23),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("read-quarantined-object", 1, 0, 0);
            const auto payload = test::MakeChunkPayload(16, "read-quarantined");
            const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id);
            ASSERT_TRUE(std::filesystem::create_directories(final_path.parent_path()));
            {
                std::ofstream output(final_path, std::ios::binary | std::ios::trunc);
                ASSERT_TRUE(output.is_open());
                output << payload;
            }

            ASSERT_TRUE(shared_index->Insert(MakeIndexEntry(identity,
                                                            ChunkState::kQuarantined,
                                                            payload.size(),
                                                            ComputeChecksumOrThrow(payload),
                                                            final_path))
                            .ok());

            const auto response =
                store.ReadChunk(MakeReadRequest(identity.chunk_id, "read-quarantined-state"));
            EXPECT_EQ(response.status, StorageNodeStatusCode::kCorrupted);
            EXPECT_FALSE(response.ok());
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkRejectsRangeReadInCurrentStage)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_range");
            LocalDiskChunkStore store(MakeConfig(temp_dir.Path("node-data"),
                                                 test::MakeStorageNodeIdFixture(24)));
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("read-range-object", 1, 0, 0);
            auto request = MakeReadRequest(identity.chunk_id, "read-range-request");
            request.range = ChunkReadRange{.offset = 0, .length = 4};

            const auto response = store.ReadChunk(request);
            EXPECT_EQ(response.status, StorageNodeStatusCode::kUnsupported);
            EXPECT_FALSE(response.ok());
        }

        TEST_F(LocalDiskChunkStoreTest, ReadChunkDoesNotFallBackToStagingFile)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local file read path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_read_staging_fallback");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(25),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto payload = test::MakeChunkPayload(32, "read-staging-fallback");
            const auto identity = MakeIdentityOrThrow("read-staging-fallback-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, payload, "read-staging-fallback-write")).status,
                      StorageNodeStatusCode::kOk);

            const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id);
            ASSERT_TRUE(std::filesystem::remove(final_path));

            const auto staging_path = ResolveStagingPathOrThrow(store.paths().data_root,
                                                                identity.chunk_id,
                                                                "manual-staging");
            std::filesystem::create_directories(staging_path.parent_path());
            ASSERT_TRUE(std::filesystem::exists(staging_path.parent_path()));
            {
                std::ofstream output(staging_path, std::ios::binary | std::ios::trunc);
                ASSERT_TRUE(output.is_open());
                output << payload;
            }

            const auto response =
                store.ReadChunk(MakeReadRequest(identity.chunk_id, "read-staging-fallback-read"));
            EXPECT_EQ(response.status, StorageNodeStatusCode::kNotFound);
            EXPECT_FALSE(response.ok());
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, StatChunkAfterWriteReturnsLiveMetadata)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local file stat path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_stat_live");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(25),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto fixture = LoadFixtureBinaryPayload();
            const auto identity = MakeIdentityOrThrow("stat-live-object", 1, 0, 0);
            const auto write_request =
                MakeWriteRequest(identity, fixture.payload, "stat-live-write");
            ASSERT_EQ(store.WriteChunk(write_request).status, StorageNodeStatusCode::kOk);

            auto stat_request = MakeStatRequest(identity.chunk_id, "stat-live-request");
            stat_request.verify_checksum = true;
            const auto response = store.StatChunk(stat_request);
            ASSERT_EQ(response.status, StorageNodeStatusCode::kOk);
            ASSERT_TRUE(response.ok());
            EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);
            EXPECT_EQ(response.metadata.state, ChunkState::kLive);
            EXPECT_EQ(response.metadata.size, fixture.payload.size());
            EXPECT_EQ(response.metadata.checksum.value, write_request.expected_checksum.value);
            EXPECT_TRUE(response.verified);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, StatChunkVerifyChecksumQuarantinesTamperedFinalFile)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local file stat path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_stat_tampered");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(26),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto payload = test::MakeChunkPayload(40, "stat-tampered");
            const auto identity = MakeIdentityOrThrow("stat-tampered-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, payload, "stat-tampered-write")).status,
                      StorageNodeStatusCode::kOk);

            const auto final_path = ResolveFinalPathOrThrow(temp_dir.Path("node-data"),
                                                            identity.chunk_id);
            {
                std::ofstream output(final_path, std::ios::binary | std::ios::trunc);
                ASSERT_TRUE(output.is_open());
                output << test::MakeChunkPayload(payload.size(), "tampered-stat");
            }

            auto stat_request = MakeStatRequest(identity.chunk_id, "stat-tampered-request");
            stat_request.verify_checksum = true;
            const auto response = store.StatChunk(stat_request);
            EXPECT_EQ(response.status, StorageNodeStatusCode::kCorrupted);
            EXPECT_FALSE(response.ok());

            const auto quarantine_path = ResolveStatusPathOrThrow(temp_dir.Path("node-data"),
                                                                  "quarantine",
                                                                  identity.chunk_id);
            EXPECT_FALSE(std::filesystem::exists(final_path));
            EXPECT_TRUE(std::filesystem::exists(quarantine_path));

            const auto stat_after_quarantine =
                store.StatChunk(MakeStatRequest(identity.chunk_id,
                                                "stat-tampered-after-quarantine"));
            ASSERT_EQ(stat_after_quarantine.status, StorageNodeStatusCode::kOk);
            EXPECT_EQ(stat_after_quarantine.metadata.state, ChunkState::kQuarantined);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, StatChunkReturnsDeletedStateAfterDelete)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local delete/stat path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_stat_deleted");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(26),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto payload = test::MakeChunkPayload(24, "stat-deleted");
            const auto identity = MakeIdentityOrThrow("stat-deleted-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, payload, "stat-deleted-write")).status,
                      StorageNodeStatusCode::kOk);
            ASSERT_EQ(store.DeleteChunk(MakeDeleteRequest(identity.chunk_id, "stat-deleted-delete")).status,
                      StorageNodeStatusCode::kOk);

            const auto response =
                store.StatChunk(MakeStatRequest(identity.chunk_id, "stat-deleted-stat"));
            ASSERT_EQ(response.status, StorageNodeStatusCode::kOk);
            EXPECT_EQ(response.metadata.state, ChunkState::kDeleted);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, ListChunksReturnsLiveEntriesAndSupportsPagination)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local list path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_list_live");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(27),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto payload = test::MakeChunkPayload(12, "list-live");
            const auto first = MakeIdentityOrThrow("list-live-a", 1, 0, 0);
            const auto second = MakeIdentityOrThrow("list-live-b", 1, 0, 0);
            const auto third = MakeIdentityOrThrow("list-live-c", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(first, payload, "list-live-write-a")).status,
                      StorageNodeStatusCode::kOk);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(second, payload, "list-live-write-b")).status,
                      StorageNodeStatusCode::kOk);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(third, payload, "list-live-write-c")).status,
                      StorageNodeStatusCode::kOk);

            auto first_page = MakeListRequest("list-live-page-1");
            first_page.options.state_filter = ChunkState::kLive;
            first_page.options.page_size = 2;
            const auto first_response = store.ListChunks(first_page);
            ASSERT_EQ(first_response.status, StorageNodeStatusCode::kOk);
            ASSERT_EQ(first_response.chunks.size(), 2U);
            ASSERT_FALSE(first_response.next_page_token.empty());
            EXPECT_EQ(first_response.chunks[0].state, ChunkState::kLive);
            EXPECT_EQ(first_response.chunks[1].state, ChunkState::kLive);

            auto second_page = MakeListRequest("list-live-page-2");
            second_page.options.state_filter = ChunkState::kLive;
            second_page.options.page_size = 2;
            second_page.options.page_token = first_response.next_page_token;
            const auto second_response = store.ListChunks(second_page);
            ASSERT_EQ(second_response.status, StorageNodeStatusCode::kOk);
            ASSERT_EQ(second_response.chunks.size(), 1U);
            EXPECT_TRUE(second_response.next_page_token.empty());
            EXPECT_EQ(second_response.chunks[0].state, ChunkState::kLive);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, ListChunksCanFilterDeletedEntries)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local delete/list path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_list_deleted");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(28),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto fixture = LoadFixtureBinaryPayload();
            const auto live_identity = MakeIdentityOrThrow("list-deleted-live", 1, 0, 0);
            const auto deleted_identity = MakeIdentityOrThrow("list-deleted-deleted", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(live_identity,
                                                        test::MakeChunkPayload(10, "list-del-live"),
                                                        "list-deleted-write-live"))
                          .status,
                      StorageNodeStatusCode::kOk);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(deleted_identity,
                                                        fixture.payload,
                                                        "list-deleted-write-deleted"))
                          .status,
                      StorageNodeStatusCode::kOk);
            ASSERT_EQ(store.DeleteChunk(MakeDeleteRequest(deleted_identity.chunk_id,
                                                          "list-deleted-delete"))
                          .status,
                      StorageNodeStatusCode::kOk);

            auto deleted_request = MakeListRequest("list-deleted-request");
            deleted_request.options.state_filter = ChunkState::kDeleted;
            const auto deleted_response = store.ListChunks(deleted_request);
            ASSERT_EQ(deleted_response.status, StorageNodeStatusCode::kOk);
            ASSERT_EQ(deleted_response.chunks.size(), 1U);
            EXPECT_EQ(deleted_response.chunks[0].identity.chunk_id, deleted_identity.chunk_id);
            EXPECT_EQ(deleted_response.chunks[0].state, ChunkState::kDeleted);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, DeleteChunkAfterWritePreventsFutureReadAndRemovesFile)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local delete path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_delete_success");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(29),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto payload = test::MakeChunkPayload(36, "delete-success");
            const auto identity = MakeIdentityOrThrow("delete-success-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, payload, "delete-success-write")).status,
                      StorageNodeStatusCode::kOk);

            const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id);
            ASSERT_TRUE(std::filesystem::exists(final_path));

            const auto delete_response =
                store.DeleteChunk(MakeDeleteRequest(identity.chunk_id, "delete-success-delete"));
            ASSERT_EQ(delete_response.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(delete_response.deleted);
            EXPECT_FALSE(delete_response.already_missing);
            EXPECT_FALSE(std::filesystem::exists(final_path));

            const auto read_response =
                store.ReadChunk(MakeReadRequest(identity.chunk_id, "delete-success-read"));
            EXPECT_EQ(read_response.status, StorageNodeStatusCode::kNotFound);
            EXPECT_FALSE(read_response.ok());
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, DeleteChunkIsIdempotentForRepeatedDelete)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local delete path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_delete_idempotent");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(30),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto payload = test::MakeChunkPayload(18, "delete-idempotent");
            const auto identity = MakeIdentityOrThrow("delete-idempotent-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, payload, "delete-idempotent-write")).status,
                      StorageNodeStatusCode::kOk);
            ASSERT_EQ(store.DeleteChunk(MakeDeleteRequest(identity.chunk_id, "delete-idempotent-first")).status,
                      StorageNodeStatusCode::kOk);

            const auto second_response =
                store.DeleteChunk(MakeDeleteRequest(identity.chunk_id, "delete-idempotent-second"));
            EXPECT_EQ(second_response.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(second_response.already_missing);
            EXPECT_EQ(second_response.metadata.state, ChunkState::kDeleted);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, DeleteChunkRejectsExpectedChecksumMismatchWithoutRemovingFile)
        {
#if !defined(__linux__)
            GTEST_SKIP() << "real local delete path is only verified on Linux in this environment";
#else
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_delete_checksum_mismatch");
            const auto shared_index = MakeSharedIndex();
            LocalDiskChunkStore store(LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("node-data"),
                .node_id = test::MakeStorageNodeIdFixture(31),
                .chunk_index = shared_index});
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto fixture = LoadFixtureBinaryPayload();
            const auto identity = MakeIdentityOrThrow("delete-checksum-mismatch-object", 1, 0, 0);
            ASSERT_EQ(store.WriteChunk(MakeWriteRequest(identity, fixture.payload, "delete-checksum-write")).status,
                      StorageNodeStatusCode::kOk);

            auto delete_request =
                MakeDeleteRequest(identity.chunk_id, "delete-checksum-delete");
            delete_request.expected_checksum = ComputeChecksumOrThrow("different-payload");
            const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id);

            const auto response = store.DeleteChunk(delete_request);
            EXPECT_EQ(response.status, StorageNodeStatusCode::kChecksumMismatch);
            EXPECT_FALSE(response.ok());
            EXPECT_TRUE(std::filesystem::exists(final_path));

            const auto stat_response =
                store.StatChunk(MakeStatRequest(identity.chunk_id, "delete-checksum-stat"));
            EXPECT_EQ(stat_response.status, StorageNodeStatusCode::kOk);
            EXPECT_EQ(stat_response.metadata.state, ChunkState::kLive);
#endif
        }

        TEST_F(LocalDiskChunkStoreTest, DeleteChunkReturnsAlreadyMissingForUnknownChunk)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_delete_missing");
            LocalDiskChunkStore store(MakeConfig(temp_dir.Path("node-data"),
                                                 test::MakeStorageNodeIdFixture(32)));
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("delete-missing-object", 1, 0, 0);
            const auto response =
                store.DeleteChunk(MakeDeleteRequest(identity.chunk_id, "delete-missing-request"));
            EXPECT_EQ(response.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(response.already_missing);
            EXPECT_EQ(response.metadata.state, ChunkState::kMissing);
        }

        TEST_F(LocalDiskChunkStoreTest, ListChunksDoesNotReturnUnindexedFinalFile)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_list_unindexed");
            LocalDiskChunkStore store(MakeConfig(temp_dir.Path("node-data"),
                                                 test::MakeStorageNodeIdFixture(33)));
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto identity = MakeIdentityOrThrow("list-unindexed-object", 1, 0, 0);
            const auto final_path = ResolveFinalPathOrThrow(store.paths().data_root,
                                                            identity.chunk_id);
            ASSERT_TRUE(std::filesystem::create_directories(final_path.parent_path()));
            {
                std::ofstream output(final_path, std::ios::binary | std::ios::trunc);
                ASSERT_TRUE(output.is_open());
                output << "orphan";
            }

            const auto response = store.ListChunks(MakeListRequest("list-unindexed-request"));
            EXPECT_EQ(response.status, StorageNodeStatusCode::kOk);
            EXPECT_TRUE(response.chunks.empty());
        }
    } // namespace
} // namespace storedemo
