/*
    强一致元数据层的数据模型与命令契约。
*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace raftdemo
{
    using ClientRequestId = std::string;

    enum class MetadataRecordState : uint8_t
    {
        kPending = 1,
        kCommitted = 2,
        kDeleted = 3,
    };

    enum class MetadataOperation : uint8_t
    {
        kUnknown = 0,
        kCreate = 1,
        kCommit = 2,
        kDelete = 3,
    };

    struct MetadataRecord
    {
        std::string object_key;
        MetadataRecordState state = MetadataRecordState::kPending;
        uint64_t object_size = 0;
        uint64_t chunk_size = 0;
        uint64_t chunk_count = 0;
        std::string checksum;
        std::vector<std::string> mock_locations;
        std::string payload;
        ClientRequestId create_request_id;
        std::optional<ClientRequestId> commit_request_id;
        std::optional<ClientRequestId> delete_request_id;
        uint64_t created_at_log_index = 0;
        std::optional<uint64_t> committed_at_log_index;
        std::optional<uint64_t> deleted_at_log_index;
        std::string commit_info;
        std::string delete_info;

        bool IsPending() const
        {
            return state == MetadataRecordState::kPending;
        }

        bool IsCommitted() const
        {
            return state == MetadataRecordState::kCommitted;
        }

        bool IsDeleted() const
        {
            return state == MetadataRecordState::kDeleted;
        }

        bool IsVisibleToClients() const
        {
            return IsCommitted();
        }
    };

    struct Tombstone
    {
        std::string object_key;
        ClientRequestId delete_request_id;
        uint64_t deleted_at_log_index = 0;
        std::optional<ClientRequestId> previous_commit_request_id;
        std::optional<std::string> checksum;
        std::string delete_info;
    };

    struct MetadataCommand
    {
        MetadataOperation operation = MetadataOperation::kUnknown;
        ClientRequestId request_id;
        std::string object_key;
        std::optional<MetadataRecord> record;
        std::string commit_info;
        std::string delete_info;

        bool IsCreate() const
        {
            return operation == MetadataOperation::kCreate;
        }

        bool IsCommit() const
        {
            return operation == MetadataOperation::kCommit;
        }

        bool IsDelete() const
        {
            return operation == MetadataOperation::kDelete;
        }

        bool HasRecordPayload() const
        {
            return record.has_value();
        }
    };

    struct IdempotencyEntry
    {
        ClientRequestId request_id;
        MetadataOperation operation = MetadataOperation::kUnknown;
        std::string object_key;
        std::string command_fingerprint;
        std::string result_code;
        std::optional<MetadataRecordState> result_state;
        std::optional<uint64_t> log_index;
        std::optional<MetadataRecord> response_record;
    };

    inline MetadataCommand MakeCreateMetadataCommand(MetadataRecord record)
    {
        record.state = MetadataRecordState::kPending;
        record.commit_request_id.reset();
        record.delete_request_id.reset();
        record.committed_at_log_index.reset();
        record.deleted_at_log_index.reset();

        MetadataCommand command;
        command.operation = MetadataOperation::kCreate;
        command.request_id = record.create_request_id;
        command.object_key = record.object_key;
        command.record = std::move(record);
        return command;
    }

} // namespace raftdemo
