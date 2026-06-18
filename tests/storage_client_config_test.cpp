#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "store/common/store_types.h"
#include "support/store_test_utils.h"

#define main storage_client_app_main
#include "../apps/storage_client.cpp"
#undef main

namespace
{
    std::filesystem::path WriteStorageClientConfig(
        const std::filesystem::path &root,
        const bool include_chunk_size)
    {
        const auto config_path = root / "cluster.json";
        std::ofstream output(config_path);
        output << "{\n"
               << "  \"cluster_id\": \"storage-client-config-test\",\n"
               << "  \"view_nodes\": [\n"
               << "    {\n"
               << "      \"node_id\": \"view-1\",\n"
               << "      \"endpoint\": \"127.0.0.1:7301\",\n"
               << "      \"data_dir\": \"" << (root / "view").string() << "\"\n"
               << "    }\n"
               << "  ],\n"
               << "  \"chunk_policy\": {\n";
        if (include_chunk_size)
        {
            output << "    \"chunk_size_bytes\": 1024,\n";
        }
        output << "    \"replica_count\": 3,\n"
               << "    \"minimum_successful_writes\": 2,\n"
               << "    \"checksum_algorithm\": \"sha256\"\n"
               << "  }\n"
               << "}\n";
        return config_path;
    }
}

TEST(StorageClientConfigTest, ProductionChunkSizeConstantIs128MiB)
{
    EXPECT_EQ(storedemo::kProductionChunkSizeBytes,
              128ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(ClientConfig{}.chunk_size, storedemo::kProductionChunkSizeBytes);
}

TEST(StorageClientConfigTest,
     LoadClientConfigIgnoresChunkSizeBytesFromConfigForUploadDefault)
{
    storedemo::test::ScopedStoreTestDir temp_dir(
        "storage_client_config_ignores_chunk_size");
    const auto config_path = WriteStorageClientConfig(temp_dir.root(),
                                                      true);

    const ClientConfig config = LoadClientConfig(config_path);

    EXPECT_EQ(config.chunk_size, storedemo::kProductionChunkSizeBytes);
    EXPECT_EQ(config.replica_count, 3U);
    EXPECT_EQ(config.minimum_successful_writes, 2U);
}

TEST(StorageClientConfigTest,
     LoadClientConfigWithoutChunkSizeStillUsesProductionUploadDefault)
{
    storedemo::test::ScopedStoreTestDir temp_dir(
        "storage_client_config_default_chunk_size");
    const auto config_path = WriteStorageClientConfig(temp_dir.root(),
                                                      false);

    const ClientConfig config = LoadClientConfig(config_path);

    EXPECT_EQ(config.chunk_size, storedemo::kProductionChunkSizeBytes);
}

TEST(StorageClientConfigTest, MakeChunkPolicyUsesProductionDefaultChunkSize)
{
    ParsedArgs args;
    const auto policy = MakeChunkPolicy(args);

    EXPECT_EQ(policy.chunk_size_bytes, storedemo::kProductionChunkSizeBytes);
}
