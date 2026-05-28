#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "store/common/store_types.h"

namespace storedemo
{
    // 统一承载 ChunkStore 接口层的状态和错误信息，便于后续 service/store 复用同一套结果语义。
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

    // range 未设置时表示读取整个 chunk；length 必须大于 0，具体边界由实现侧校验。
    struct ChunkReadRange
    {
        std::uint64_t offset{0};
        std::uint64_t length{0};
    };

    // page_size 为 0 时表示由具体实现使用默认分页大小；page_token 只用于继续同一轮分页扫描。
    struct ListChunksOptions
    {
        std::optional<ChunkState> state_filter;
        std::string prefix_filter;
        std::string page_token;
        std::size_t page_size{0};
        bool include_quarantine{false};
    };

    // payload 当前使用 std::string 承载二进制字节，便于后续本地实现和 gRPC 适配共享一套接口。
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

    // verify_checksum=true 表示读取路径需要做完整性校验；range 只描述返回字节范围，不改变 chunk_id 语义。
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

    // metadata_boundary 是控制面传入的数据面删除安全边界，本接口只透传，不解释其来源。
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

    // ChunkStore 只定义 data-plane 语义边界，不负责 durable file、目录布局或本地索引实现细节。
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
