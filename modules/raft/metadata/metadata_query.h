/*
    元数据只读查询模型。
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace raftdemo
{
    struct HeadObjectQuery
    {
        std::string bucket;
        std::string object_key;
        std::optional<std::string> object_id;
        std::optional<uint64_t> version;
    };

    struct ListObjectsQuery
    {
        std::string bucket;
        std::string prefix;
        std::optional<std::size_t> limit;
        std::string continuation_token;
        bool include_deleted = false;
    };

} // namespace raftdemo
