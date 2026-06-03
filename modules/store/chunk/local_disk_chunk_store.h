#pragma once

#include <filesystem>
#include <memory>

#include "store/chunk/chunk_store.h"

namespace storedemo
{
    class DurableFile;
    class ChunkIndex;
    class BoundedStorageExecutor;

    struct LocalDiskChunkStorePaths
    {
        std::filesystem::path data_root;
        std::filesystem::path chunks_root;
        std::filesystem::path live_root;
        std::filesystem::path staging_root;

        [[nodiscard]] bool IsInitialized() const;
    };

    struct LocalDiskChunkStoreConfig
    {
        std::filesystem::path data_dir;
        StorageNodeId node_id;
        std::shared_ptr<DurableFile> durable_file;
        std::shared_ptr<ChunkIndex> chunk_index;
        std::shared_ptr<BoundedStorageExecutor> executor;
    };

    struct LocalDiskChunkStoreInitResult : ChunkStoreResult
    {
        LocalDiskChunkStorePaths paths;
        bool initialized{false};
    };

    class LocalDiskChunkStore : public ChunkStore
    {
    public:
        explicit LocalDiskChunkStore(LocalDiskChunkStoreConfig config);
        ~LocalDiskChunkStore() override;

        LocalDiskChunkStore(const LocalDiskChunkStore &) = delete;
        LocalDiskChunkStore &operator=(const LocalDiskChunkStore &) = delete;

        LocalDiskChunkStoreInitResult Initialize();
        ChunkStoreResult RebuildIndexFromDisk();

        [[nodiscard]] const LocalDiskChunkStoreConfig &config() const;
        [[nodiscard]] const LocalDiskChunkStorePaths &paths() const;
        [[nodiscard]] bool initialized() const;
        [[nodiscard]] DurableFile *durable_file() const;
        [[nodiscard]] ChunkIndex *chunk_index() const;
        [[nodiscard]] BoundedStorageExecutor *executor() const;

        WriteChunkResponse WriteChunk(const WriteChunkRequest &request) override;
        ReadChunkResponse ReadChunk(const ReadChunkRequest &request) override;
        DeleteChunkResponse DeleteChunk(const DeleteChunkRequest &request) override;
        StatChunkResponse StatChunk(const StatChunkRequest &request) override;
        ListChunksResponse ListChunks(const ListChunksRequest &request) override;

    private:
        LocalDiskChunkStoreConfig config_;
        LocalDiskChunkStorePaths paths_;
        bool initialized_{false};
    };
}
