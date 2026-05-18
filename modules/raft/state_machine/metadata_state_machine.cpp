#include "raft/state_machine/metadata_state_machine.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace raftdemo
{
    // T005 当前尚未在头文件公开 codec 声明，这里只引入最小原型以复用既有实现。
    std::string SerializeMetadataCommand(const MetadataCommand &command);
    bool ParseMetadataCommand(const std::string &input, MetadataCommand *out);
    bool ValidateMetadataCommand(const MetadataCommand &command, std::string *error);
    std::string ComputeMetadataCommandFingerprint(const MetadataCommand &command);

    namespace
    {
        ApplyResult MakeApplyFailure(const std::string &message)
        {
            return {false, message};
        }

        bool StartsWith(const std::string &value, const std::string &prefix)
        {
            return value.size() >= prefix.size() &&
                   value.compare(0, prefix.size(), prefix) == 0;
        }

        ApplyResult MakeReplayConflictFailure()
        {
            return {false, "idempotency conflict: request_id maps to different command"};
        }

        ApplyResult MakeReplaySuccess()
        {
            return {true, "idempotent replay"};
        }

        IdempotencyEntry MakeReplayEntry(const MetadataCommand &command,
                                         const std::string &fingerprint,
                                         const MetadataRecord &record,
                                         const std::uint64_t log_index)
        {
            IdempotencyEntry entry;
            entry.request_id = command.request_id;
            entry.operation = command.operation;
            entry.object_key = command.object_key;
            entry.command_fingerprint = fingerprint;
            entry.result_code = "OK";
            entry.result_state = record.state;
            entry.log_index = log_index;
            entry.response_record = record;
            return entry;
        }
    } // namespace

    ApplyResult StrongConsistencyMetadataStateMachine::Apply(std::uint64_t index,
                                                             const std::string &command_data)
    {
        MetadataCommand command;
        if (!ParseMetadataCommand(command_data, &command))
        {
            return MakeApplyFailure("failed to parse metadata command");
        }

        std::string validation_error;
        if (!ValidateMetadataCommand(command, &validation_error))
        {
            return MakeApplyFailure("invalid metadata command: " + validation_error);
        }

        const std::string fingerprint = ComputeMetadataCommandFingerprint(command);

        std::lock_guard<std::mutex> lk(mu_);

        auto replay = replay_table_.find(command.request_id);
        if (replay != replay_table_.end())
        {
            const IdempotencyEntry &entry = replay->second;
            if (entry.operation != command.operation ||
                entry.object_key != command.object_key ||
                entry.command_fingerprint != fingerprint)
            {
                return MakeReplayConflictFailure();
            }
            return MakeReplaySuccess();
        }

        if (command.IsCreate())
        {
            auto existing = records_.find(command.object_key);
            if (existing != records_.end())
            {
                return MakeApplyFailure("state conflict: object already exists");
            }

            if (!command.record.has_value())
            {
                return MakeApplyFailure("invalid metadata command: missing create record");
            }

            MetadataRecord record = *command.record;
            record.state = MetadataRecordState::kPending;
            record.created_at_log_index = index;
            record.commit_request_id.reset();
            record.delete_request_id.reset();
            record.committed_at_log_index.reset();
            record.deleted_at_log_index.reset();
            records_[record.object_key] = record;
            replay_table_[command.request_id] =
                MakeReplayEntry(command, fingerprint, record, index);
            return {true, "ok"};
        }

        if (command.IsCommit())
        {
            auto existing = records_.find(command.object_key);
            if (existing == records_.end())
            {
                return MakeApplyFailure("not found: pending record does not exist");
            }

            MetadataRecord &record = existing->second;
            if (!record.IsPending())
            {
                return MakeApplyFailure("state conflict: record is not pending");
            }

            record.state = MetadataRecordState::kCommitted;
            record.commit_request_id = command.request_id;
            record.committed_at_log_index = index;
            record.commit_info = command.commit_info;
            replay_table_[command.request_id] =
                MakeReplayEntry(command, fingerprint, record, index);
            return {true, "ok"};
        }

        return MakeApplyFailure("unknown metadata operation");
    }

    SnapshotResult StrongConsistencyMetadataStateMachine::SaveSnapshot(const std::string & /*file_path*/) const
    {
        return {SnapshotStatus::kInternalError,
                "metadata snapshot save is not implemented in T010"};
    }

    SnapshotResult StrongConsistencyMetadataStateMachine::LoadSnapshot(const std::string & /*file_path*/)
    {
        return {SnapshotStatus::kInternalError,
                "metadata snapshot load is not implemented in T010"};
    }

    MetadataHeadResponse StrongConsistencyMetadataStateMachine::HeadMetadataRecord(
        const MetadataHeadRequest &request) const
    {
        MetadataHeadResponse response;
        response.result.summary.object_key = request.object_key;

        if (request.object_key.empty())
        {
            response.result = MakeMetadataResult(
                MetadataStatusCode::kInvalidArgument,
                {.object_key = request.object_key, .message = "object_key is empty"});
            return response;
        }

        std::lock_guard<std::mutex> lk(mu_);
        auto it = records_.find(request.object_key);
        if (it == records_.end() || !it->second.IsCommitted())
        {
            response.result = MakeMetadataResult(
                MetadataStatusCode::kNotFound,
                {.object_key = request.object_key, .message = "committed record not found"});
            return response;
        }

        response.record = it->second;
        response.result = MakeMetadataResult(
            MetadataStatusCode::kOk,
            {.object_key = request.object_key,
             .result_state = it->second.state,
             .log_index = it->second.committed_at_log_index,
             .message = "ok"});
        return response;
    }

    MetadataListResponse StrongConsistencyMetadataStateMachine::ListMetadataRecords(
        const MetadataListRequest &request) const
    {
        MetadataListResponse response;

        std::vector<MetadataRecord> visible_records;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (const auto &[object_key, record] : records_)
            {
                if (!record.IsCommitted())
                {
                    continue;
                }
                if (!request.prefix.empty() && !StartsWith(object_key, request.prefix))
                {
                    continue;
                }
                visible_records.push_back(record);
            }
        }

        std::sort(visible_records.begin(), visible_records.end(),
                  [](const MetadataRecord &left, const MetadataRecord &right)
                  {
                      return left.object_key < right.object_key;
                  });

        if (request.limit.has_value() && visible_records.size() > *request.limit)
        {
            response.next_page_token = visible_records[*request.limit].object_key;
            visible_records.resize(*request.limit);
        }

        response.records = std::move(visible_records);
        response.result = MakeMetadataResult(
            MetadataStatusCode::kOk,
            {.message = "ok"});
        return response;
    }

} // namespace raftdemo
