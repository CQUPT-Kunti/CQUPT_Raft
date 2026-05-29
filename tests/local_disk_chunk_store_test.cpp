#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "store/chunk/local_disk_chunk_store.h"
#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"
#include "support/store_test_utils.h"

namespace storedemo
{
    namespace
    {
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

        TEST_F(LocalDiskChunkStoreTest, UnsupportedOperationsReturnExplicitUnsupportedStatus)
        {
            test::ScopedStoreTestDir temp_dir("local_disk_chunk_store_unsupported_ops");
            LocalDiskChunkStore store(MakeConfig(temp_dir.Path("node-data"),
                                                 test::MakeStorageNodeIdFixture(6)));
            ASSERT_EQ(store.Initialize().status, StorageNodeStatusCode::kOk);

            const auto write_response = store.WriteChunk(WriteChunkRequest{});
            EXPECT_EQ(write_response.status, StorageNodeStatusCode::kUnsupported);
            EXPECT_FALSE(write_response.ok());
            EXPECT_NE(write_response.error_detail.find("WriteChunk"), std::string::npos);

            const auto read_response = store.ReadChunk(ReadChunkRequest{});
            EXPECT_EQ(read_response.status, StorageNodeStatusCode::kUnsupported);
            EXPECT_FALSE(read_response.ok());
            EXPECT_NE(read_response.error_detail.find("ReadChunk"), std::string::npos);

            const auto delete_response = store.DeleteChunk(DeleteChunkRequest{});
            EXPECT_EQ(delete_response.status, StorageNodeStatusCode::kUnsupported);
            EXPECT_FALSE(delete_response.ok());
            EXPECT_NE(delete_response.error_detail.find("DeleteChunk"), std::string::npos);

            const auto stat_response = store.StatChunk(StatChunkRequest{});
            EXPECT_EQ(stat_response.status, StorageNodeStatusCode::kUnsupported);
            EXPECT_FALSE(stat_response.ok());
            EXPECT_NE(stat_response.error_detail.find("StatChunk"), std::string::npos);

            const auto list_response = store.ListChunks(ListChunksRequest{});
            EXPECT_EQ(list_response.status, StorageNodeStatusCode::kUnsupported);
            EXPECT_FALSE(list_response.ok());
            EXPECT_NE(list_response.error_detail.find("ListChunks"), std::string::npos);
        }
    } // namespace
} // namespace storedemo
