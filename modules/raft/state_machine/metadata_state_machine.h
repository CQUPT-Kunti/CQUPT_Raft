#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/state_machine/state_machine_interface.h"

namespace raftdemo
{
    struct MetadataHeadRequest
    {
        std::string object_key;
    };

    struct MetadataHeadResponse
    {
        MetadataResult result;
        std::optional<MetadataRecord> record;
    };

    struct MetadataListRequest
    {
        std::string prefix;
        std::optional<std::size_t> limit;
        std::string page_token;
    };

    struct MetadataListResponse
    {
        MetadataResult result;
        std::vector<MetadataRecord> records;
        std::string next_page_token;
    };

    class StrongConsistencyMetadataStateMachine final : public IStateMachine
    {
    public:
        ApplyResult Apply(std::uint64_t index,
                          const std::string &command_data) override;

        SnapshotResult SaveSnapshot(const std::string &file_path) const override;
        SnapshotResult LoadSnapshot(const std::string &file_path) override;

        // 只暴露 committed 记录；Pending / Deleted 必须通过结果语义隐藏。
        MetadataHeadResponse HeadMetadataRecord(const MetadataHeadRequest &request) const;

        // 当前阶段只声明 committed-only 查询边界；分页/过滤的具体行为在实现中细化。
        MetadataListResponse ListMetadataRecords(const MetadataListRequest &request) const;

    private:
        mutable std::mutex mu_;
        std::unordered_map<std::string, MetadataRecord> records_;
        std::unordered_map<std::string, Tombstone> tombstones_;
        std::unordered_map<ClientRequestId, IdempotencyEntry> replay_table_;
    };

} // namespace raftdemo
