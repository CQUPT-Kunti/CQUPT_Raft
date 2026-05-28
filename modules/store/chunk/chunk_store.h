#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "store/common/store_types.h"

namespace storedemo
{
    struct ChunkStoreResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::uint64_t retry_after_ms{0};

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    struct ChunkReadRange
    {
        std::uint64_t offset{0};
        std::uint64_t length{0};
    };

    struct ListChunksOptions
    {
        std::optional<ChunkState> state_filter;
        std::string prefix_filter;
        std::string page_token;
        std::size_t page_size{0};
        bool include_quarantine{false};
    };

    struct WriteChunkRequest
    {
        std::string request_id;
        ChunkIdentity identity;
        std::optional<std::uint64_t> expected_size;
        ChunkChecksum expected_checksum;
        std::string payload;
    };

    struct WriteChunkResponse : ChunkStoreResult
    {
        ChunkMetadata metadata;
        bool durable{false};
        bool already_exists{false};
    };

    struct ReadChunkRequest
    {
        std::string request_id;
        ChunkId chunk_id;
        std::optional<ChunkReadRange> range;
        ChunkChecksum expected_checksum;
        bool verify_checksum{false};
    };

    struct ReadChunkResponse : ChunkStoreResult
    {
        ChunkMetadata metadata;
        ChunkChecksum actual_checksum;
        std::string payload;
        bool verified{false};
    };

    struct DeleteChunkRequest
    {
        std::string request_id;
        ChunkId chunk_id;
        std::string reason;
        std::string metadata_boundary;
        ChunkChecksum expected_checksum;
    };

    struct DeleteChunkResponse : ChunkStoreResult
    {
        ChunkMetadata metadata;
        bool deleted{false};
        bool already_missing{false};
    };

    struct StatChunkRequest
    {
        std::string request_id;
        ChunkId chunk_id;
        bool include_quarantine{false};
        bool verify_checksum{false};
    };

    struct StatChunkResponse : ChunkStoreResult
    {
        ChunkMetadata metadata;
        bool verified{false};
    };

    struct ListChunksRequest
    {
        std::string request_id;
        ListChunksOptions options;
    };

    struct ListChunksResponse : ChunkStoreResult
    {
        std::vector<ChunkMetadata> chunks;
        std::string next_page_token;
        std::uint64_t snapshot_epoch{0};
    };

    class ChunkStore
    {
    public:
        virtual ~ChunkStore();

        virtual WriteChunkResponse WriteChunk(const WriteChunkRequest &request) = 0;
        virtual ReadChunkResponse ReadChunk(const ReadChunkRequest &request) = 0;
        virtual DeleteChunkResponse DeleteChunk(const DeleteChunkRequest &request) = 0;
        virtual StatChunkResponse StatChunk(const StatChunkRequest &request) = 0;
        virtual ListChunksResponse ListChunks(const ListChunksRequest &request) = 0;
    };
}
