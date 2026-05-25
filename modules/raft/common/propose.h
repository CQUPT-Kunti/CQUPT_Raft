#pragma once

#include <cstdint>
#include <string>

namespace raftdemo
{
    // 枚举用于统一描述一次 Propose 调用的返回结果，
    enum class ProposeStatus : std::uint8_t
    {
        kOk = 0,
        kNotLeader = 1,
        kInvalidCommand = 2,
        kNodeStopping = 3, // 节点正在停止，无法处理新请求
        kReplicationFailed = 4,
        kCommitFailed = 5,
        kApplyFailed = 6, // 状态机执行失败
        kTimeout = 7,
        kOverloaded = 8 // metadata proposal admission/backpressure 主动拒绝

    };

    // 一次提案调用的统一返回结果
    struct ProposeResult
    {
        ProposeStatus status{ProposeStatus::kReplicationFailed};
        int leader_id{-1};          // -1 未知节点
        std::uint64_t term{0};      // 该节点当前任期
        std::uint64_t log_index{0}; // 本次提案对应的日志下标；失败时通常为 0
        std::string message;        // 额外的文本信息

        bool Ok() const
        {
            return status == ProposeStatus::kOk;
        }
    };
} // namespace raftdemo
