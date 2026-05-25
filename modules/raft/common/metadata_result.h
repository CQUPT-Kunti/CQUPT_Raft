/*
    强一致元数据层的公共结果语义。
*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace raftdemo
{
    enum class MetadataRecordState : uint8_t;

    enum class MetadataStatusCode : uint8_t
    {
        kOk = 0,
        kNotLeader = 1,
        kInvalidArgument = 2,
        kNotFound = 3,
        kIdempotentReplay = 4,
        kIdempotencyConflict = 5,
        kStateConflict = 6,
        kInternalError = 7,
        kTimeout = 8,
    };

    struct MetadataLeaderHint
    {
        std::string leader_id;
        std::string leader_address;

        bool HasLeader() const
        {
            return !leader_id.empty() || !leader_address.empty();
        }
    };

    struct MetadataResponseSummary
    {
        std::string request_id;
        std::string object_key;
        std::optional<MetadataRecordState> result_state;
        std::optional<uint64_t> term;
        std::optional<uint64_t> log_index;
        MetadataLeaderHint leader_hint;
        std::string message;

        bool HasLogPosition() const
        {
            return term.has_value() || log_index.has_value();
        }
    };

    struct MetadataResult
    {
        MetadataStatusCode code = MetadataStatusCode::kInternalError;
        MetadataResponseSummary summary;

        bool Ok() const
        {
            return code == MetadataStatusCode::kOk;
        }

        bool IsRetryLikeSuccess() const
        {
            return code == MetadataStatusCode::kOk || code == MetadataStatusCode::kIdempotentReplay;
        }

        bool NeedsLeaderRetry() const
        {
            return code == MetadataStatusCode::kNotLeader || code == MetadataStatusCode::kTimeout;
        }
    };

    inline MetadataResult MakeMetadataResult(MetadataStatusCode code,
                                             MetadataResponseSummary summary = {})
    {
        MetadataResult result;
        result.code = code;
        result.summary = std::move(summary);
        return result;
    }

} // namespace raftdemo
