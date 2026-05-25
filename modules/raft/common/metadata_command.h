/*
    强一致元数据层的数据模型与命令契约。
*/

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "raft/metadata/metadata_command_types.h"
#include "raft/metadata/metadata_query.h"

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
        MetadataCommandType command_type = MetadataCommandType::kUnknown;
        std::optional<RequestRecord> request_context;
        std::optional<CreateBucketCommandPayload> create_bucket;
        std::optional<DeleteBucketCommandPayload> delete_bucket;
        std::optional<CreateObjectCommandPayload> create_object;
        std::optional<CommitObjectCommandPayload> commit_object;
        std::optional<AbortObjectCommandPayload> abort_object;
        std::optional<DeleteObjectCommandPayload> delete_object;

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

        bool IsWriteCommand() const
        {
            return command_type != MetadataCommandType::kUnknown;
        }

        bool IsCreateBucketCommand() const
        {
            return command_type == MetadataCommandType::kCreateBucket;
        }

        bool IsDeleteBucketCommand() const
        {
            return command_type == MetadataCommandType::kDeleteBucket;
        }

        bool IsCreateObjectCommand() const
        {
            return command_type == MetadataCommandType::kCreateObject;
        }

        bool IsCommitObjectCommand() const
        {
            return command_type == MetadataCommandType::kCommitObject;
        }

        bool IsAbortObjectCommand() const
        {
            return command_type == MetadataCommandType::kAbortObject;
        }

        bool IsDeleteObjectCommand() const
        {
            return command_type == MetadataCommandType::kDeleteObject;
        }

        bool CarriesChunkRefs() const
        {
            return commit_object.has_value() && commit_object->HasChunks();
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

    std::string SerializeMetadataCommand(const MetadataCommand &command);
    bool ParseMetadataCommand(const std::string &input, MetadataCommand *out);
    bool ValidateMetadataCommand(const MetadataCommand &command, std::string *error);
    std::string ComputeMetadataCommandFingerprint(const MetadataCommand &command);

} // namespace raftdemo
