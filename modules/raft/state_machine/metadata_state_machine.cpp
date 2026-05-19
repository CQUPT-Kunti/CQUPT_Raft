#include "raft/state_machine/metadata_state_machine.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "raft/common/command.h"

namespace raftdemo
{
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

        Tombstone MakeTombstone(const MetadataRecord &record,
                                const ClientRequestId &delete_request_id,
                                const std::string &delete_info,
                                const std::uint64_t log_index)
        {
            Tombstone tombstone;
            tombstone.object_key = record.object_key;
            tombstone.delete_request_id = delete_request_id;
            tombstone.deleted_at_log_index = log_index;
            tombstone.previous_commit_request_id = record.commit_request_id;
            if (!record.checksum.empty())
            {
                tombstone.checksum = record.checksum;
            }
            tombstone.delete_info = delete_info;
            return tombstone;
        }
    } // namespace

    ApplyResult StrongConsistencyMetadataStateMachine::Apply(std::uint64_t index,
                                                             const std::string &command_data)
    {
        MetadataCommand command;
        std::string metadata_payload = command_data;
        if (!ParseMetadataCommand(metadata_payload, &command))
        {
            Command wrapped_command;
            if (!Command::Deserialize(command_data, &wrapped_command) ||
                wrapped_command.type != CommandType::kMetadata)
            {
                return MakeApplyFailure("failed to parse metadata command");
            }

            metadata_payload = wrapped_command.metadata_payload;
            if (!ParseMetadataCommand(metadata_payload, &command))
            {
                return MakeApplyFailure("failed to parse metadata command");
            }
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
            if (tombstones_.find(command.object_key) != tombstones_.end())
            {
                return MakeApplyFailure("state conflict: object is tombstoned");
            }

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
                if (tombstones_.find(command.object_key) != tombstones_.end())
                {
                    return MakeApplyFailure("state conflict: record is deleted");
                }
                return MakeApplyFailure("not found: pending record does not exist");
            }

            MetadataRecord &record = existing->second;
            if (!record.IsPending())
            {
                if (record.IsDeleted())
                {
                    return MakeApplyFailure("state conflict: record is deleted");
                }
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

        if (command.IsDelete())
        {
            auto existing = records_.find(command.object_key);
            if (existing == records_.end())
            {
                if (tombstones_.find(command.object_key) != tombstones_.end())
                {
                    return MakeApplyFailure("state conflict: record is not committed");
                }
                return MakeApplyFailure("not found: record does not exist");
            }

            MetadataRecord &record = existing->second;
            if (record.IsPending())
            {
                return MakeApplyFailure("state conflict: pending record cannot be deleted");
            }
            if (record.IsDeleted())
            {
                return MakeApplyFailure("state conflict: record is not committed");
            }
            if (!record.IsCommitted())
            {
                return MakeApplyFailure("state conflict: record is not committed");
            }

            record.state = MetadataRecordState::kDeleted;
            record.delete_request_id = command.request_id;
            record.deleted_at_log_index = index;
            record.delete_info = command.delete_info;
            tombstones_[record.object_key] =
                MakeTombstone(record, command.request_id, command.delete_info, index);
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
