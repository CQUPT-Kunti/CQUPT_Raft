#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "store/chunk/chunk_store.h"
#include "store/runtime/storage_executor.h"

namespace storedemo
{
    struct StorageTransferTarget
    {
        StorageNodeId node_id;
        std::string endpoint;
    };

    struct StorageTransferResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::uint64_t retry_after_ms{0};
        bool retryable{false};
        StorageTransferTarget target;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    struct StorageTransferWriteRequest
    {
        // 同一 chunk 的幂等重试必须复用同一 request_id 和 chunk identity。
        std::string request_id;
        StorageTransferTarget target;
        ChunkIdentity identity;
        std::uint64_t offset{0};
        std::optional<std::uint64_t> expected_size;
        ChunkChecksum expected_checksum;
        StorageTaskContext context;
        // 这里只允许单个 bounded chunk payload，不能传整对象 buffer。
        std::string payload;
    };

    struct StorageTransferWriteResult : StorageTransferResult
    {
        ChunkMetadata metadata;
        bool durable{false};
        bool already_exists{false};
    };

    struct StorageTransferReadRequest
    {
        // download 重试必须复用同一 request_id / chunk identity，便于诊断和幂等追踪。
        std::string request_id;
        StorageTransferTarget target;
        ChunkIdentity identity;
        std::optional<ChunkReadRange> range;
        ChunkChecksum expected_checksum;
        bool verify_checksum{false};
        StorageTaskContext context;
    };

    struct StorageTransferReadResult : StorageTransferResult
    {
        ChunkMetadata metadata;
        ChunkChecksum actual_checksum;
        // 读取结果只承载单次 chunk read 的 bounded payload，不承担整文件缓存。
        std::string payload;
        bool verified{false};
    };

    class StorageTransferClient
    {
    public:
        virtual ~StorageTransferClient() = default;

        // 只负责对单个 StorageNode 发起 chunk 写入，不决定对象是否可见或是否 COMMITTED。
        virtual StorageTransferWriteResult WriteChunk(
            const StorageTransferWriteRequest &request) = 0;

        // 只负责对单个 StorageNode 发起 chunk 读取，不负责 manifest 选择或下载拼接编排。
        virtual StorageTransferReadResult ReadChunk(
            const StorageTransferReadRequest &request) = 0;
    };
}
