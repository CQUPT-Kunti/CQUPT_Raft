/*
    元数据写命令类型与 payload 骨架。
*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "raft/metadata/metadata_records.h"

namespace raftdemo
{
    enum class MetadataCommandType : uint8_t
    {
        kUnknown = 0,
        kCreateBucket = 1,
        kDeleteBucket = 2,
        kCreateObject = 3,
        kCommitObject = 4,
        kAbortObject = 5,
        kDeleteObject = 6,
    };

    struct CreateBucketCommandPayload
    {
        BucketRecord bucket_record;
    };

    struct DeleteBucketCommandPayload
    {
        std::string bucket;
        bool if_empty = true;
    };

    struct CreateObjectCommandPayload
    {
        ObjectRecord object_record;
    };

    struct CommitObjectCommandPayload
    {
        std::string bucket;
        std::string object_key;
        std::string object_id;
        uint64_t version = 0;
        uint64_t size = 0;
        std::string etag;
        std::vector<ChunkRef> chunks;
        std::optional<uint64_t> commit_time;

        bool HasChunks() const
        {
            return !chunks.empty();
        }
    };

    struct AbortObjectCommandPayload
    {
        std::string bucket;
        std::string object_key;
        std::string object_id;
        uint64_t version = 0;
    };

    struct DeleteObjectCommandPayload
    {
        std::string bucket;
        std::string object_key;
        std::string object_id;
        uint64_t version = 0;
        std::optional<uint64_t> delete_time;
    };

} // namespace raftdemo
