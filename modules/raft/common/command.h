/*
    提交给 Raft 的 metadata-only 命令封装。
*/

#pragma once

#include <string>
#include <cstdint>

namespace raftdemo
{
    enum class CommandType : uint8_t
    {
        kUnknown = 0,
        kMetadata = 1,
    };

    struct Command
    {
        CommandType type{CommandType::kUnknown};
        std::string metadata_payload;

        bool IsValid() const;
        std::string Serialize() const;
        static bool Deserialize(const std::string &data, Command *out);
    };

} // namespace raftdemo
