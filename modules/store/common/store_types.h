/*
    StorageNode data-plane 共享基础类型占位。
    T007 定义基础数据结构和轻量判定 helper，不引入具体业务实现。
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace raftdemo
{
    using StorageNodeId = std::string;
    using ChunkId = std::string;

    // StorageNode data-plane 的统一结果码。
    // 仅表达 store 侧的错误分类，不映射到 proto 或 Raft 状态。
    enum class StorageNodeStatusCode : std::uint8_t
    {
        kOk = 0,
        kAlreadyExists = 1,
        kNotFound = 2,
        kConflict = 3,
        kChecksumMismatch = 4,
        kCorrupted = 5,
        kDiskFull = 6,
        kPermissionDenied = 7,
        kIoError = 8,
        kTimeout = 9,
        kCancelled = 10,
        kOverloaded = 11,
        kNodeUnavailable = 12,
        kUnsupported = 13,
        kInvalidArgument = 14,
    };

    // Chunk 在本地节点生命周期中的基础状态。
    enum class ChunkState : std::uint8_t
    {
        kStaging = 0,
        kLive = 1,
        kDeleting = 2,
        kDeleted = 3,
        kQuarantined = 4,
        kCorrupted = 5,
        kMissing = 6,
    };

    // Checksum 算法标识。
    // T007 只定义类型，不实现真实 checksum 计算。
    enum class ChunkChecksumAlgorithm : std::uint8_t
    {
        kUnknown = 0,
        kSha256 = 1,
    };

    // 将状态码转成稳定字符串，便于日志、测试断言和后续错误映射。
    const char *ToString(StorageNodeStatusCode code);

    // 将 chunk 生命周期状态转成稳定字符串。
    const char *ToString(ChunkState state);

    // 判断错误是否适合在 store 侧做重试。
    bool IsRetriableStatus(StorageNodeStatusCode code);

    // 判断该状态下的 chunk 是否允许作为正常可读副本对外提供数据。
    bool IsReadableChunkState(ChunkState state);

    // 判断该状态是否表示本轮生命周期已经结束，后续通常需要人工处理或新流程重建。
    bool IsTerminalChunkState(ChunkState state);

    // 标识某个 chunk 当前位于哪个节点。
    // 用于轻量引用，不包含内容、路径或元数据。
    struct ChunkLocation
    {
        StorageNodeId node_id;
        ChunkId chunk_id;

        // 仅校验最小引用键是否齐全。
        bool IsValid() const;
    };

    // Chunk 的校验和载体。
    // 保存算法、摘要值和对应 payload 大小，不负责执行 checksum 计算。
    struct ChunkChecksum
    {
        ChunkChecksumAlgorithm algorithm{ChunkChecksumAlgorithm::kUnknown};
        std::string value;
        std::uint64_t size_bytes{0};
        std::uint64_t computed_at{0};

        // 判断该 checksum 结构是否已经装入一组可用的基础值。
        bool IsSet() const;
    };

    // Chunk 的逻辑身份信息。
    // chunk_id 之外保留 object/version/index/offset，供后续索引、落盘和副本流程复用。
    struct ChunkIdentity
    {
        ChunkId chunk_id;
        std::string object_id;
        std::uint64_t version{0};
        std::uint32_t chunk_index{0};
        std::uint64_t offset{0};

        // 判断是否已经具备最小 chunk 主键。
        bool HasChunkKey() const;
    };

    // 单个副本的基础事实视图。
    // 面向副本选择、校验和健康状态判断，不承载副本内容本身。
    struct ChunkReplica
    {
        ChunkId chunk_id;
        StorageNodeId node_id;
        std::uint64_t size{0};
        ChunkChecksum checksum;
        ChunkState state{ChunkState::kMissing};
        std::uint64_t last_verified_at{0};
        std::uint64_t last_read_at{0};
        std::uint32_t failure_count{0};
        StorageNodeStatusCode last_error{StorageNodeStatusCode::kOk};

        // 判断该副本是否具备“可读副本”的最小条件。
        bool IsReadable() const;
    };

    // 单个 chunk 在某个 StorageNode 上的本地元信息。
    // 只表达 store 数据面事实，不写入 Raft log/snapshot。
    struct ChunkMetadata
    {
        ChunkIdentity identity;
        StorageNodeId node_id;
        std::uint64_t size{0};
        ChunkChecksum checksum;
        ChunkState state{ChunkState::kMissing};
        std::string write_request_id;
        std::string delete_request_id;
        std::uint64_t created_at{0};
        std::uint64_t published_at{0};
        std::uint64_t deleted_at{0};
        std::uint64_t last_verified_at{0};
        StorageNodeStatusCode last_error{StorageNodeStatusCode::kOk};
        std::string quarantine_reason;

        // 判断本地元信息是否已经描述了一个可读 live chunk。
        bool IsReadable() const;
    };

    // ChunkIndex 中单条记录的轻量载体。
    // 同时保留身份、状态和本地路径，供后续索引与落盘组件复用。
    struct ChunkIndexEntry
    {
        ChunkIdentity identity;
        ChunkState state{ChunkState::kMissing};
        std::uint64_t size{0};
        ChunkChecksum checksum;
        std::filesystem::path final_path;
        std::filesystem::path staging_path;
        std::filesystem::path metadata_path;
        std::size_t lock_shard{0};
        std::uint64_t updated_at{0};

        // 判断是否已经绑定到最终可见路径。
        bool HasFinalPath() const;
    };

    // store 模块早期占位阶段标识，供最小模块接线与回归测试使用。
    enum class StoreModuleStage : std::uint8_t
    {
        kPlaceholder = 0,
    };

} // namespace raftdemo
