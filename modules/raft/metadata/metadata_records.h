/*
    元数据业务记录的基础类型定义。
*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace raftdemo
{
    enum class ObjectState : uint8_t
    {
        PENDING = 1,
        COMMITTED = 2,
        DELETED = 3,
    };

    enum class MetadataRequestType : uint8_t
    {
        kUnknown = 0,
        kCreateBucket = 1,
        kDeleteBucket = 2,
        kCreateObject = 3,
        kCommitObject = 4,
        kAbortObject = 5,
        kDeleteObject = 6,
    };

    struct ChunkRef
    {
        std::string chunk_id;
        uint64_t offset = 0;
        uint64_t size = 0;
        std::vector<std::string> replica_nodes;
        std::string checksum;

        bool HasReplicaNodes() const
        {
            return !replica_nodes.empty();
        }
    };

    struct BucketRecord
    {
        std::string bucket;
        uint64_t create_time = 0;
        bool deleted = false;
        std::optional<uint64_t> delete_time;

        bool IsActive() const
        {
            return !deleted;
        }
    };

    struct ObjectRecord
    {
        std::string bucket;
        std::string object_key;
        std::string object_id;
        uint64_t version = 0;
        uint64_t size = 0;
        std::string etag;
        ObjectState state = ObjectState::PENDING;
        std::vector<ChunkRef> chunks;
        uint64_t create_time = 0;
        std::optional<uint64_t> commit_time;
        std::optional<uint64_t> delete_time;

        bool IsPending() const
        {
            return state == ObjectState::PENDING;
        }

        bool IsCommitted() const
        {
            return state == ObjectState::COMMITTED;
        }

        bool IsDeleted() const
        {
            return state == ObjectState::DELETED;
        }
    };

    struct RequestRecord
    {
        std::string request_id;
        MetadataRequestType command_type = MetadataRequestType::kUnknown;
        std::string bucket;
        std::string object_key;
        std::string result_status;
        uint64_t applied_index = 0;
        uint64_t create_time = 0;
        std::optional<uint64_t> finish_time;

        bool Finished() const
        {
            return finish_time.has_value();
        }
    };

} // namespace raftdemo
