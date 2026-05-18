/*
    客户端提交给 Raft 的命令
*/

#pragma once

#include <string>
#include <cstdint>

namespace raftdemo
{
    // 定义命令类型
    enum class CommandType : uint8_t
    {
        kUnknown = 0,
        kSet = 1,
        kDelete = 2,
        kMetadata = 3,
    };

    // 业务层提交给 Raft 的原始命令
    struct Command
    {
        CommandType type;
        std::string key;
        std::string value;
        std::string metadata_payload;

        // 判断当前命令对象是否合法。
        bool IsValid() const;

        // 将命令序列化为字符串，供写入 Raft 日志。
        std::string Serialize() const;

        // 将字符串反序列化为命令对象。
        static bool Deserialize(const std::string &data, Command *out);
    };

} // namespace raftdemo
