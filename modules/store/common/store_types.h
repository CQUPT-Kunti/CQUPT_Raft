/*
    StorageNode data-plane 共享基础类型占位。
    T001 只建立最小可编译模块边界，不引入具体业务实现。
*/

#pragma once

#include <cstdint>
#include <string>

namespace raftdemo
{
    using StorageNodeId = std::string;
    using ChunkId = std::string;

    struct ChunkLocation
    {
        StorageNodeId node_id;
        ChunkId chunk_id;

        bool IsValid() const;
    };

    enum class StoreModuleStage : std::uint8_t
    {
        kPlaceholder = 0,
    };

} // namespace raftdemo
