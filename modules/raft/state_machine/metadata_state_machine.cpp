#include "raft/state_machine/metadata_state_machine.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "raft/common/command.h"

namespace raftdemo
{
    namespace
    {
        constexpr std::uint32_t kMetadataSnapshotMagic = 0x4D445331U; // "MDS1"
        constexpr std::uint32_t kMetadataSnapshotVersion = 1U;
        ApplyResult MakeApplyFailure(const std::string &message)
        {
            return {false, message};
        }

        std::string CommandTypeToString(const MetadataCommandType type)
        {
            switch (type)
            {
            case MetadataCommandType::kCreateBucket:
                return "create_bucket";
            case MetadataCommandType::kDeleteBucket:
                return "delete_bucket";
            case MetadataCommandType::kCreateObject:
                return "create_object";
            case MetadataCommandType::kCommitObject:
                return "commit_object";
            case MetadataCommandType::kAbortObject:
                return "abort_object";
            case MetadataCommandType::kDeleteObject:
                return "delete_object";
            case MetadataCommandType::kUnknown:
            default:
                return "unknown";
            }
        }

        bool ParseMetadataStateMachineCommand(const std::string &command_data,
                                             MetadataCommand *command)
        {
            if (command == nullptr)
            {
                return false;
            }

            if (ParseMetadataCommand(command_data, command))
            {
                return true;
            }

            Command wrapped_command;
            if (!Command::Deserialize(command_data, &wrapped_command) ||
                wrapped_command.type != CommandType::kMetadata)
            {
                return false;
            }

            return ParseMetadataCommand(wrapped_command.metadata_payload, command);
        }

        RequestRecord MakeAppliedRequestRecord(const MetadataCommand &command,
                                              const std::string &bucket,
                                              const std::string &result_status,
                                              const std::uint64_t index)
        {
            RequestRecord request;
            request.request_id = command.request_id;
            request.bucket = bucket;
            request.result_status = result_status;
            request.applied_index = index;

            if (command.request_context.has_value())
            {
                request = *command.request_context;
                request.request_id = command.request_id;
                request.bucket = bucket;
                request.result_status = result_status;
                request.applied_index = index;
                return request;
            }

            switch (command.command_type)
            {
            case MetadataCommandType::kCreateBucket:
                request.command_type = MetadataRequestType::kCreateBucket;
                break;
            case MetadataCommandType::kDeleteBucket:
                request.command_type = MetadataRequestType::kDeleteBucket;
                break;
            case MetadataCommandType::kCreateObject:
                request.command_type = MetadataRequestType::kCreateObject;
                break;
            case MetadataCommandType::kCommitObject:
                request.command_type = MetadataRequestType::kCommitObject;
                break;
            case MetadataCommandType::kAbortObject:
                request.command_type = MetadataRequestType::kAbortObject;
                break;
            case MetadataCommandType::kDeleteObject:
                request.command_type = MetadataRequestType::kDeleteObject;
                break;
            default:
                request.command_type = MetadataRequestType::kUnknown;
                break;
            }

            return request;
        }

        bool BucketHasActiveObjects(
            const std::unordered_map<std::string, ObjectRecord> &objects,
            const std::string &bucket)
        {
            for (const auto &[identity, object] : objects)
            {
                static_cast<void>(identity);
                if (object.bucket == bucket && !object.IsDeleted())
                {
                    return true;
                }
            }
            return false;
        }

        std::string MakeObjectIdentity(std::string_view bucket,
                                       std::string_view object_key)
        {
            return std::string(bucket) + "\n" + std::string(object_key);
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

        bool IsValidRecordState(const MetadataRecordState state)
        {
            switch (state)
            {
            case MetadataRecordState::kPending:
            case MetadataRecordState::kCommitted:
            case MetadataRecordState::kDeleted:
                return true;
            default:
                return false;
            }
        }

        bool IsValidOperation(const MetadataOperation operation)
        {
            switch (operation)
            {
            case MetadataOperation::kCreate:
            case MetadataOperation::kCommit:
            case MetadataOperation::kDelete:
                return true;
            case MetadataOperation::kUnknown:
            default:
                return false;
            }
        }

        template <typename T>
        bool WritePod(std::ofstream &out, const T &value)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "WritePod requires trivially copyable type");
            out.write(reinterpret_cast<const char *>(&value), sizeof(T));
            return static_cast<bool>(out);
        }

        template <typename T>
        bool ReadPod(std::ifstream &in, T *value)
        {
            static_assert(std::is_trivially_copyable_v<T>,
                          "ReadPod requires trivially copyable type");
            if (value == nullptr)
            {
                return false;
            }
            in.read(reinterpret_cast<char *>(value), sizeof(T));
            return static_cast<bool>(in);
        }

        bool WriteString(std::ofstream &out, const std::string &value)
        {
            const std::uint64_t size = static_cast<std::uint64_t>(value.size());
            if (!WritePod(out, size))
            {
                return false;
            }
            if (size == 0)
            {
                return true;
            }
            out.write(value.data(), static_cast<std::streamsize>(size));
            return static_cast<bool>(out);
        }

        bool ReadString(std::ifstream &in, std::string *value)
        {
            if (value == nullptr)
            {
                return false;
            }

            std::uint64_t size = 0;
            if (!ReadPod(in, &size))
            {
                return false;
            }

            value->clear();
            if (size == 0)
            {
                return true;
            }

            value->resize(static_cast<std::size_t>(size));
            in.read(value->data(), static_cast<std::streamsize>(size));
            return static_cast<bool>(in);
        }

        template <typename EnumType>
        bool WriteEnum(std::ofstream &out, const EnumType value)
        {
            using RawType = std::underlying_type_t<EnumType>;
            return WritePod(out, static_cast<RawType>(value));
        }

        template <typename EnumType>
        bool ReadEnum(std::ifstream &in, EnumType *value)
        {
            if (value == nullptr)
            {
                return false;
            }
            using RawType = std::underlying_type_t<EnumType>;
            RawType raw = 0;
            if (!ReadPod(in, &raw))
            {
                return false;
            }
            *value = static_cast<EnumType>(raw);
            return true;
        }

        bool WriteOptionalString(std::ofstream &out,
                                 const std::optional<std::string> &value)
        {
            const std::uint8_t has_value = value.has_value() ? 1U : 0U;
            if (!WritePod(out, has_value))
            {
                return false;
            }
            if (!value.has_value())
            {
                return true;
            }
            return WriteString(out, *value);
        }

        bool ReadOptionalString(std::ifstream &in,
                                std::optional<std::string> *value)
        {
            if (value == nullptr)
            {
                return false;
            }

            std::uint8_t has_value = 0;
            if (!ReadPod(in, &has_value))
            {
                return false;
            }
            if (has_value > 1U)
            {
                return false;
            }
            if (has_value == 0U)
            {
                value->reset();
                return true;
            }

            std::string decoded;
            if (!ReadString(in, &decoded))
            {
                return false;
            }
            *value = std::move(decoded);
            return true;
        }

        bool WriteOptionalUInt64(std::ofstream &out,
                                 const std::optional<std::uint64_t> &value)
        {
            const std::uint8_t has_value = value.has_value() ? 1U : 0U;
            if (!WritePod(out, has_value))
            {
                return false;
            }
            if (!value.has_value())
            {
                return true;
            }
            return WritePod(out, *value);
        }

        bool ReadOptionalUInt64(std::ifstream &in,
                                std::optional<std::uint64_t> *value)
        {
            if (value == nullptr)
            {
                return false;
            }

            std::uint8_t has_value = 0;
            if (!ReadPod(in, &has_value))
            {
                return false;
            }
            if (has_value > 1U)
            {
                return false;
            }
            if (has_value == 0U)
            {
                value->reset();
                return true;
            }

            std::uint64_t decoded = 0;
            if (!ReadPod(in, &decoded))
            {
                return false;
            }
            *value = decoded;
            return true;
        }

        bool WriteStringVector(std::ofstream &out,
                               const std::vector<std::string> &values)
        {
            const std::uint64_t size = static_cast<std::uint64_t>(values.size());
            if (!WritePod(out, size))
            {
                return false;
            }
            for (const auto &value : values)
            {
                if (!WriteString(out, value))
                {
                    return false;
                }
            }
            return true;
        }

        bool ReadStringVector(std::ifstream &in,
                              std::vector<std::string> *values)
        {
            if (values == nullptr)
            {
                return false;
            }

            std::uint64_t size = 0;
            if (!ReadPod(in, &size))
            {
                return false;
            }

            std::vector<std::string> decoded;
            decoded.reserve(static_cast<std::size_t>(size));
            for (std::uint64_t i = 0; i < size; ++i)
            {
                std::string value;
                if (!ReadString(in, &value))
                {
                    return false;
                }
                decoded.push_back(std::move(value));
            }

            *values = std::move(decoded);
            return true;
        }

        bool WriteMetadataRecord(std::ofstream &out,
                                 const MetadataRecord &record)
        {
            return WriteString(out, record.object_key) &&
                   WriteEnum(out, record.state) &&
                   WritePod(out, record.object_size) &&
                   WritePod(out, record.chunk_size) &&
                   WritePod(out, record.chunk_count) &&
                   WriteString(out, record.checksum) &&
                   WriteStringVector(out, record.mock_locations) &&
                   WriteString(out, record.payload) &&
                   WriteString(out, record.create_request_id) &&
                   WriteOptionalString(out, record.commit_request_id) &&
                   WriteOptionalString(out, record.delete_request_id) &&
                   WritePod(out, record.created_at_log_index) &&
                   WriteOptionalUInt64(out, record.committed_at_log_index) &&
                   WriteOptionalUInt64(out, record.deleted_at_log_index) &&
                   WriteString(out, record.commit_info) &&
                   WriteString(out, record.delete_info);
        }

        bool ReadMetadataRecord(std::ifstream &in,
                                MetadataRecord *record)
        {
            if (record == nullptr)
            {
                return false;
            }

            MetadataRecord decoded;
            if (!ReadString(in, &decoded.object_key) ||
                !ReadEnum(in, &decoded.state) ||
                !IsValidRecordState(decoded.state) ||
                !ReadPod(in, &decoded.object_size) ||
                !ReadPod(in, &decoded.chunk_size) ||
                !ReadPod(in, &decoded.chunk_count) ||
                !ReadString(in, &decoded.checksum) ||
                !ReadStringVector(in, &decoded.mock_locations) ||
                !ReadString(in, &decoded.payload) ||
                !ReadString(in, &decoded.create_request_id) ||
                !ReadOptionalString(in, &decoded.commit_request_id) ||
                !ReadOptionalString(in, &decoded.delete_request_id) ||
                !ReadPod(in, &decoded.created_at_log_index) ||
                !ReadOptionalUInt64(in, &decoded.committed_at_log_index) ||
                !ReadOptionalUInt64(in, &decoded.deleted_at_log_index) ||
                !ReadString(in, &decoded.commit_info) ||
                !ReadString(in, &decoded.delete_info))
            {
                return false;
            }

            *record = std::move(decoded);
            return true;
        }

        bool WriteTombstone(std::ofstream &out,
                            const Tombstone &tombstone)
        {
            return WriteString(out, tombstone.object_key) &&
                   WriteString(out, tombstone.delete_request_id) &&
                   WritePod(out, tombstone.deleted_at_log_index) &&
                   WriteOptionalString(out, tombstone.previous_commit_request_id) &&
                   WriteOptionalString(out, tombstone.checksum) &&
                   WriteString(out, tombstone.delete_info);
        }

        bool ReadTombstone(std::ifstream &in,
                           Tombstone *tombstone)
        {
            if (tombstone == nullptr)
            {
                return false;
            }

            Tombstone decoded;
            if (!ReadString(in, &decoded.object_key) ||
                !ReadString(in, &decoded.delete_request_id) ||
                !ReadPod(in, &decoded.deleted_at_log_index) ||
                !ReadOptionalString(in, &decoded.previous_commit_request_id) ||
                !ReadOptionalString(in, &decoded.checksum) ||
                !ReadString(in, &decoded.delete_info))
            {
                return false;
            }

            *tombstone = std::move(decoded);
            return true;
        }

        bool WriteOptionalMetadataRecord(std::ofstream &out,
                                         const std::optional<MetadataRecord> &record)
        {
            const std::uint8_t has_value = record.has_value() ? 1U : 0U;
            if (!WritePod(out, has_value))
            {
                return false;
            }
            if (!record.has_value())
            {
                return true;
            }
            return WriteMetadataRecord(out, *record);
        }

        bool ReadOptionalMetadataRecord(std::ifstream &in,
                                        std::optional<MetadataRecord> *record)
        {
            if (record == nullptr)
            {
                return false;
            }

            std::uint8_t has_value = 0;
            if (!ReadPod(in, &has_value))
            {
                return false;
            }
            if (has_value > 1U)
            {
                return false;
            }
            if (has_value == 0U)
            {
                record->reset();
                return true;
            }

            MetadataRecord decoded;
            if (!ReadMetadataRecord(in, &decoded))
            {
                return false;
            }
            *record = std::move(decoded);
            return true;
        }

        bool WriteOptionalRecordState(std::ofstream &out,
                                      const std::optional<MetadataRecordState> &state)
        {
            const std::uint8_t has_value = state.has_value() ? 1U : 0U;
            if (!WritePod(out, has_value))
            {
                return false;
            }
            if (!state.has_value())
            {
                return true;
            }
            return WriteEnum(out, *state);
        }

        bool ReadOptionalRecordState(std::ifstream &in,
                                     std::optional<MetadataRecordState> *state)
        {
            if (state == nullptr)
            {
                return false;
            }

            std::uint8_t has_value = 0;
            if (!ReadPod(in, &has_value))
            {
                return false;
            }
            if (has_value > 1U)
            {
                return false;
            }
            if (has_value == 0U)
            {
                state->reset();
                return true;
            }

            MetadataRecordState decoded = MetadataRecordState::kPending;
            if (!ReadEnum(in, &decoded) || !IsValidRecordState(decoded))
            {
                return false;
            }
            *state = decoded;
            return true;
        }

        bool WriteIdempotencyEntry(std::ofstream &out,
                                   const IdempotencyEntry &entry)
        {
            return WriteString(out, entry.request_id) &&
                   WriteEnum(out, entry.operation) &&
                   WriteString(out, entry.object_key) &&
                   WriteString(out, entry.command_fingerprint) &&
                   WriteString(out, entry.result_code) &&
                   WriteOptionalRecordState(out, entry.result_state) &&
                   WriteOptionalUInt64(out, entry.log_index) &&
                   WriteOptionalMetadataRecord(out, entry.response_record);
        }

        bool ReadIdempotencyEntry(std::ifstream &in,
                                  IdempotencyEntry *entry)
        {
            if (entry == nullptr)
            {
                return false;
            }

            IdempotencyEntry decoded;
            if (!ReadString(in, &decoded.request_id) ||
                !ReadEnum(in, &decoded.operation) ||
                !IsValidOperation(decoded.operation) ||
                !ReadString(in, &decoded.object_key) ||
                !ReadString(in, &decoded.command_fingerprint) ||
                !ReadString(in, &decoded.result_code) ||
                !ReadOptionalRecordState(in, &decoded.result_state) ||
                !ReadOptionalUInt64(in, &decoded.log_index) ||
                !ReadOptionalMetadataRecord(in, &decoded.response_record))
            {
                return false;
            }

            *entry = std::move(decoded);
            return true;
        }
    } // namespace

    ApplyResult MetadataStateMachine::Apply(std::uint64_t index,
                                            const std::string &command_data)
    {
        if (index == 0)
        {
            return MakeApplyFailure("metadata state machine skeleton requires index > 0");
        }
        if (command_data.empty())
        {
            return MakeApplyFailure("metadata state machine skeleton requires non-empty command");
        }

        MetadataCommand command;
        if (!ParseMetadataStateMachineCommand(command_data, &command))
        {
            return MakeApplyFailure("failed to parse metadata command");
        }

        std::string validation_error;
        if (!ValidateMetadataCommand(command, &validation_error))
        {
            return MakeApplyFailure("invalid metadata command: " + validation_error);
        }

        if (command.command_type == MetadataCommandType::kUnknown)
        {
            return MakeApplyFailure("unsupported metadata command type: unknown");
        }

        std::lock_guard<std::mutex> lk(mu_);

        if (command.IsCreateBucketCommand())
        {
            const BucketRecord &payload = command.create_bucket->bucket_record;
            auto it = buckets_.find(payload.bucket);
            if (it != buckets_.end() && !it->second.deleted)
            {
                return MakeApplyFailure("state conflict: bucket already exists");
            }

            BucketRecord record = payload;
            record.deleted = false;
            record.delete_time.reset();
            if (record.create_time == 0 && command.request_context.has_value())
            {
                record.create_time = command.request_context->create_time;
            }

            buckets_[record.bucket] = record;
            requests_[command.request_id] =
                MakeAppliedRequestRecord(command, record.bucket, "ok", index);
            last_applied_index_ = index;
            last_applied_term_ = 0;
            return {true, "ok"};
        }

        if (command.IsDeleteBucketCommand())
        {
            auto it = buckets_.find(command.delete_bucket->bucket);
            if (it == buckets_.end())
            {
                return MakeApplyFailure("not found: bucket does not exist");
            }
            if (it->second.deleted)
            {
                return MakeApplyFailure("state conflict: bucket already deleted");
            }
            if (command.delete_bucket->if_empty &&
                BucketHasActiveObjects(objects_, command.delete_bucket->bucket))
            {
                return MakeApplyFailure("state conflict: bucket is not empty");
            }

            it->second.deleted = true;
            if (command.request_context.has_value())
            {
                if (command.request_context->finish_time.has_value())
                {
                    it->second.delete_time = command.request_context->finish_time;
                }
                else if (command.request_context->create_time != 0)
                {
                    it->second.delete_time = command.request_context->create_time;
                }
            }

            requests_[command.request_id] =
                MakeAppliedRequestRecord(command, it->second.bucket, "ok", index);
            last_applied_index_ = index;
            last_applied_term_ = 0;
            return {true, "ok"};
        }

        if (command.IsCreateObjectCommand())
        {
            const ObjectRecord &payload = command.create_object->object_record;
            const auto bucket_it = buckets_.find(payload.bucket);
            if (bucket_it == buckets_.end())
            {
                return MakeApplyFailure("not found: bucket does not exist");
            }
            if (bucket_it->second.deleted)
            {
                return MakeApplyFailure("state conflict: bucket is deleted");
            }

            const std::string object_identity =
                MakeObjectIdentity(payload.bucket, payload.object_key);
            const auto existing = objects_.find(object_identity);
            if (existing != objects_.end() && !existing->second.IsDeleted())
            {
                return MakeApplyFailure("state conflict: object already exists");
            }

            ObjectRecord record = payload;
            record.state = ObjectState::PENDING;
            objects_[object_identity] = record;
            object_index_[object_identity] = {record.object_id};
            requests_[command.request_id] =
                MakeAppliedRequestRecord(command, record.bucket, "ok", index);
            requests_[command.request_id].object_key = record.object_key;
            last_applied_index_ = index;
            last_applied_term_ = 0;
            return {true, "ok"};
        }

        if (command.IsCommitObjectCommand())
        {
            const CommitObjectCommandPayload &payload = *command.commit_object;
            const auto bucket_it = buckets_.find(payload.bucket);
            if (bucket_it == buckets_.end())
            {
                return MakeApplyFailure("not found: bucket does not exist");
            }
            if (bucket_it->second.deleted)
            {
                return MakeApplyFailure("state conflict: bucket is deleted");
            }

            const std::string object_identity =
                MakeObjectIdentity(payload.bucket, payload.object_key);
            const auto object_it = objects_.find(object_identity);
            if (object_it == objects_.end())
            {
                return MakeApplyFailure("not found: object does not exist");
            }
            if (object_it->second.object_id != payload.object_id)
            {
                return MakeApplyFailure("state conflict: object_id mismatch");
            }
            if (object_it->second.IsCommitted())
            {
                return MakeApplyFailure("state conflict: object already committed");
            }
            if (!object_it->second.IsPending())
            {
                return MakeApplyFailure("state conflict: object is not pending");
            }

            ObjectRecord &record = object_it->second;
            record.state = ObjectState::COMMITTED;
            record.version = payload.version != 0 ? payload.version : record.version;
            record.size = payload.size;
            record.etag = payload.etag;
            record.chunks = payload.chunks;
            if (payload.commit_time.has_value())
            {
                record.commit_time = payload.commit_time;
            }
            else if (command.request_context.has_value())
            {
                if (command.request_context->finish_time.has_value())
                {
                    record.commit_time = command.request_context->finish_time;
                }
                else if (command.request_context->create_time != 0)
                {
                    record.commit_time = command.request_context->create_time;
                }
            }

            chunk_ref_index_[object_identity] = record.chunks;
            requests_[command.request_id] =
                MakeAppliedRequestRecord(command, record.bucket, "ok", index);
            requests_[command.request_id].object_key = record.object_key;
            last_applied_index_ = index;
            last_applied_term_ = 0;
            return {true, "ok"};
        }

        if (command.IsAbortObjectCommand())
        {
            const AbortObjectCommandPayload &payload = *command.abort_object;
            const auto bucket_it = buckets_.find(payload.bucket);
            if (bucket_it == buckets_.end())
            {
                return MakeApplyFailure("not found: bucket does not exist");
            }
            if (bucket_it->second.deleted)
            {
                return MakeApplyFailure("state conflict: bucket is deleted");
            }

            const std::string object_identity =
                MakeObjectIdentity(payload.bucket, payload.object_key);
            const auto object_it = objects_.find(object_identity);
            if (object_it == objects_.end())
            {
                return MakeApplyFailure("not found: object does not exist");
            }
            if (object_it->second.object_id != payload.object_id)
            {
                return MakeApplyFailure("state conflict: object_id mismatch");
            }
            if (object_it->second.IsCommitted())
            {
                return MakeApplyFailure("state conflict: object already committed");
            }
            if (object_it->second.IsDeleted())
            {
                return MakeApplyFailure("state conflict: object already aborted");
            }
            if (!object_it->second.IsPending())
            {
                return MakeApplyFailure("state conflict: object is not pending");
            }

            ObjectRecord &record = object_it->second;
            record.state = ObjectState::DELETED;
            if (command.request_context.has_value())
            {
                if (command.request_context->finish_time.has_value())
                {
                    record.delete_time = command.request_context->finish_time;
                }
                else if (command.request_context->create_time != 0)
                {
                    record.delete_time = command.request_context->create_time;
                }
            }

            object_index_.erase(object_identity);
            chunk_ref_index_.erase(object_identity);

            Tombstone tombstone;
            tombstone.object_key = record.object_key;
            tombstone.delete_request_id = command.request_id;
            tombstone.deleted_at_log_index = index;
            if (!record.etag.empty())
            {
                tombstone.checksum = record.etag;
            }
            tombstone.delete_info = "aborted pending object";
            tombstones_[object_identity] = std::move(tombstone);

            requests_[command.request_id] =
                MakeAppliedRequestRecord(command, record.bucket, "ok", index);
            requests_[command.request_id].object_key = record.object_key;
            last_applied_index_ = index;
            last_applied_term_ = 0;
            return {true, "ok"};
        }

        if (command.IsDeleteObjectCommand())
        {
            const DeleteObjectCommandPayload &payload = *command.delete_object;
            const auto bucket_it = buckets_.find(payload.bucket);
            if (bucket_it == buckets_.end())
            {
                return MakeApplyFailure("not found: bucket does not exist");
            }
            if (bucket_it->second.deleted)
            {
                return MakeApplyFailure("state conflict: bucket is deleted");
            }

            const std::string object_identity =
                MakeObjectIdentity(payload.bucket, payload.object_key);
            const auto object_it = objects_.find(object_identity);
            if (object_it == objects_.end())
            {
                return MakeApplyFailure("not found: object does not exist");
            }
            if (object_it->second.object_id != payload.object_id)
            {
                return MakeApplyFailure("state conflict: object_id mismatch");
            }
            if (object_it->second.IsDeleted())
            {
                return MakeApplyFailure("state conflict: object already deleted");
            }
            if (!object_it->second.IsCommitted())
            {
                return MakeApplyFailure("state conflict: object is not committed");
            }

            ObjectRecord &record = object_it->second;
            record.state = ObjectState::DELETED;
            if (payload.delete_time.has_value())
            {
                record.delete_time = payload.delete_time;
            }
            else if (command.request_context.has_value())
            {
                if (command.request_context->finish_time.has_value())
                {
                    record.delete_time = command.request_context->finish_time;
                }
                else if (command.request_context->create_time != 0)
                {
                    record.delete_time = command.request_context->create_time;
                }
            }

            object_index_.erase(object_identity);
            chunk_ref_index_.erase(object_identity);

            Tombstone tombstone;
            tombstone.object_key = record.object_key;
            tombstone.delete_request_id = command.request_id;
            tombstone.deleted_at_log_index = index;
            if (!record.etag.empty())
            {
                tombstone.checksum = record.etag;
            }
            tombstone.delete_info = "deleted committed object";
            tombstones_[object_identity] = std::move(tombstone);

            requests_[command.request_id] =
                MakeAppliedRequestRecord(command, record.bucket, "ok", index);
            requests_[command.request_id].object_key = record.object_key;
            last_applied_index_ = index;
            last_applied_term_ = 0;
            return {true, "ok"};
        }

        return MakeApplyFailure("unsupported metadata command type: " +
                                CommandTypeToString(command.command_type));
    }

    SnapshotResult MetadataStateMachine::SaveSnapshot(const std::string &file_path) const
    {
        if (file_path.empty())
        {
            return {SnapshotStatus::kInvalidArgument, "snapshot file path is empty"};
        }
        return {SnapshotStatus::kInternalError,
                "metadata state machine skeleton snapshot save not implemented"};
    }

    SnapshotResult MetadataStateMachine::LoadSnapshot(const std::string &file_path)
    {
        if (file_path.empty())
        {
            return {SnapshotStatus::kInvalidArgument, "snapshot file path is empty"};
        }

        std::error_code ec;
        if (!std::filesystem::exists(file_path, ec))
        {
            return {SnapshotStatus::kNotFound, "snapshot file not found: " + file_path};
        }

        return {SnapshotStatus::kInternalError,
                "metadata state machine skeleton snapshot load not implemented"};
    }

    MetadataHeadObjectResponse MetadataStateMachine::HeadObject(
        const HeadObjectQuery &query) const
    {
        if (query.bucket.empty() || query.object_key.empty())
        {
            return {
                MakeMetadataResult(
                    MetadataStatusCode::kInvalidArgument,
                    {.object_key = query.object_key,
                     .message = "bucket and object_key are required"}),
                std::nullopt};
        }

        std::lock_guard<std::mutex> lk(mu_);
        const auto bucket_it = buckets_.find(query.bucket);
        if (bucket_it == buckets_.end() || bucket_it->second.deleted)
        {
            return {
                MakeMetadataResult(
                    MetadataStatusCode::kNotFound,
                    {.object_key = query.object_key,
                     .message = "metadata skeleton bucket not found"}),
                std::nullopt};
        }

        const std::string object_identity = query.bucket + "\n" + query.object_key;
        const auto it = objects_.find(object_identity);
        if (it == objects_.end())
        {
            return {
                MakeMetadataResult(
                    MetadataStatusCode::kNotFound,
                    {.object_key = query.object_key,
                     .message = "metadata skeleton object not found"}),
                std::nullopt};
        }
        if (!it->second.IsCommitted())
        {
            return {
                MakeMetadataResult(
                    MetadataStatusCode::kNotFound,
                    {.object_key = query.object_key,
                     .message = "metadata skeleton object not found"}),
                std::nullopt};
        }
        const auto indexed = object_index_.find(object_identity);
        if (indexed == object_index_.end() || indexed->second.empty())
        {
            return {
                MakeMetadataResult(
                    MetadataStatusCode::kNotFound,
                    {.object_key = query.object_key,
                     .message = "metadata skeleton object not found"}),
                std::nullopt};
        }
        if (query.object_id.has_value() && *query.object_id != it->second.object_id)
        {
            return {
                MakeMetadataResult(
                    MetadataStatusCode::kNotFound,
                    {.object_key = query.object_key,
                     .message = "metadata skeleton object not found"}),
                std::nullopt};
        }
        if (query.version.has_value() && *query.version != it->second.version)
        {
            return {
                MakeMetadataResult(
                    MetadataStatusCode::kNotFound,
                    {.object_key = query.object_key,
                     .message = "metadata skeleton object not found"}),
                std::nullopt};
        }

        return {
            MakeMetadataResult(
                MetadataStatusCode::kOk,
                {.object_key = query.object_key,
                 .message = "ok"}),
            it->second};
    }

    MetadataListObjectsResponse MetadataStateMachine::ListObjects(
        const ListObjectsQuery &query) const
    {
        if (query.bucket.empty())
        {
            return {
                MakeMetadataResult(
                    MetadataStatusCode::kInvalidArgument,
                    {.message = "bucket is required"}),
                {},
                {}};
        }

        std::lock_guard<std::mutex> lk(mu_);
        const auto bucket_it = buckets_.find(query.bucket);
        if (bucket_it == buckets_.end() || bucket_it->second.deleted)
        {
            return {
                MakeMetadataResult(
                    MetadataStatusCode::kNotFound,
                    {.message = "metadata skeleton bucket not found"}),
                {},
                {}};
        }

        std::vector<ObjectRecord> visible_records;
        visible_records.reserve(objects_.size());
        for (const auto &[identity, object] : objects_)
        {
            static_cast<void>(identity);
            if (object.bucket != query.bucket || !object.IsCommitted())
            {
                continue;
            }
            if (!query.prefix.empty() &&
                !StartsWith(object.object_key, query.prefix))
            {
                continue;
            }
            if (!query.continuation_token.empty() &&
                object.object_key <= query.continuation_token)
            {
                continue;
            }

            const auto indexed = object_index_.find(
                MakeObjectIdentity(object.bucket, object.object_key));
            if (indexed == object_index_.end() || indexed->second.empty())
            {
                continue;
            }
            if (tombstones_.find(MakeObjectIdentity(object.bucket, object.object_key)) !=
                tombstones_.end())
            {
                continue;
            }

            visible_records.push_back(object);
        }

        std::sort(visible_records.begin(), visible_records.end(),
                  [](const ObjectRecord &lhs, const ObjectRecord &rhs)
                  {
                      return lhs.object_key < rhs.object_key;
                  });

        std::string next_page_token;
        if (query.limit.has_value() && visible_records.size() > *query.limit)
        {
            visible_records.resize(*query.limit);
            if (!visible_records.empty())
            {
                next_page_token = visible_records.back().object_key;
            }
        }

        return {
            MakeMetadataResult(MetadataStatusCode::kOk,
                               {.message = "ok"}),
            std::move(visible_records),
            next_page_token};
    }

    std::uint64_t MetadataStateMachine::LastAppliedIndex() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return last_applied_index_;
    }

    std::uint64_t MetadataStateMachine::LastAppliedTerm() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return last_applied_term_;
    }

    std::size_t MetadataStateMachine::BucketCount() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return buckets_.size();
    }

    std::size_t MetadataStateMachine::ObjectCount() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return objects_.size();
    }

    std::size_t MetadataStateMachine::RequestCount() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return requests_.size();
    }

    std::size_t MetadataStateMachine::TombstoneCount() const
    {
        std::lock_guard<std::mutex> lk(mu_);
        return tombstones_.size();
    }

    std::optional<BucketRecord> MetadataStateMachine::FindBucket(
        std::string_view bucket) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = buckets_.find(std::string(bucket));
        if (it == buckets_.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<ObjectRecord> MetadataStateMachine::FindObject(
        std::string_view bucket,
        std::string_view object_key) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = objects_.find(MakeObjectIdentity(bucket, object_key));
        if (it == objects_.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<std::string> MetadataStateMachine::FindIndexedObjectId(
        std::string_view bucket,
        std::string_view object_key) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = object_index_.find(MakeObjectIdentity(bucket, object_key));
        if (it == object_index_.end() || it->second.empty())
        {
            return std::nullopt;
        }
        return it->second.front();
    }

    std::optional<std::vector<ChunkRef>> MetadataStateMachine::FindChunkRefs(
        std::string_view bucket,
        std::string_view object_key) const
    {
        std::lock_guard<std::mutex> lk(mu_);
        const auto it = chunk_ref_index_.find(MakeObjectIdentity(bucket, object_key));
        if (it == chunk_ref_index_.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

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
            replay_table_[command.request_id] =
                MakeReplayEntry(command, fingerprint, record, index);
            return {true, "ok"};
        }

        return MakeApplyFailure("unknown metadata operation");
    }

    SnapshotResult StrongConsistencyMetadataStateMachine::SaveSnapshot(const std::string &file_path) const
    {
        if (file_path.empty())
        {
            return {SnapshotStatus::kInvalidArgument, "snapshot file path is empty"};
        }

        std::vector<std::pair<std::string, MetadataRecord>> records;
        std::vector<std::pair<std::string, Tombstone>> tombstones;
        std::vector<std::pair<ClientRequestId, IdempotencyEntry>> replay_entries;
        {
            std::lock_guard<std::mutex> lk(mu_);
            records.assign(records_.begin(), records_.end());
            tombstones.assign(tombstones_.begin(), tombstones_.end());
            replay_entries.assign(replay_table_.begin(), replay_table_.end());
        }

        std::sort(records.begin(), records.end(),
                  [](const auto &left, const auto &right)
                  {
                      return left.first < right.first;
                  });
        std::sort(tombstones.begin(), tombstones.end(),
                  [](const auto &left, const auto &right)
                  {
                      return left.first < right.first;
                  });
        std::sort(replay_entries.begin(), replay_entries.end(),
                  [](const auto &left, const auto &right)
                  {
                      return left.first < right.first;
                  });

        std::error_code ec;
        const std::filesystem::path snapshot_path(file_path);
        const std::filesystem::path parent = snapshot_path.parent_path();
        const std::filesystem::path temp_path = parent /
                                                (snapshot_path.filename().string() + ".tmp");

        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                return {SnapshotStatus::kIoError,
                        "create metadata snapshot directory failed: " + ec.message()};
            }
        }

        {
            std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                return {SnapshotStatus::kIoError,
                        "open temp metadata snapshot file failed: " + temp_path.string()};
            }

            const std::uint32_t magic = kMetadataSnapshotMagic;
            const std::uint32_t version = kMetadataSnapshotVersion;
            const std::uint64_t record_count = static_cast<std::uint64_t>(records.size());
            const std::uint64_t tombstone_count = static_cast<std::uint64_t>(tombstones.size());
            const std::uint64_t replay_count = static_cast<std::uint64_t>(replay_entries.size());

            if (!WritePod(out, magic) ||
                !WritePod(out, version) ||
                !WritePod(out, record_count) ||
                !WritePod(out, tombstone_count) ||
                !WritePod(out, replay_count))
            {
                return {SnapshotStatus::kIoError, "write metadata snapshot header failed"};
            }

            for (const auto &[object_key, record] : records)
            {
                if (object_key != record.object_key || !WriteMetadataRecord(out, record))
                {
                    return {SnapshotStatus::kIoError, "write metadata snapshot record failed"};
                }
            }

            for (const auto &[object_key, tombstone] : tombstones)
            {
                if (object_key != tombstone.object_key || !WriteTombstone(out, tombstone))
                {
                    return {SnapshotStatus::kIoError, "write metadata snapshot tombstone failed"};
                }
            }

            for (const auto &[request_id, entry] : replay_entries)
            {
                if (request_id != entry.request_id || !WriteIdempotencyEntry(out, entry))
                {
                    return {SnapshotStatus::kIoError, "write metadata snapshot replay entry failed"};
                }
            }

            out.flush();
            if (!out)
            {
                return {SnapshotStatus::kIoError, "flush metadata snapshot file failed"};
            }
        }

        ec.clear();
        if (std::filesystem::exists(snapshot_path, ec))
        {
            ec.clear();
            std::filesystem::remove(snapshot_path, ec);
            if (ec)
            {
                return {SnapshotStatus::kIoError,
                        "remove old metadata snapshot file failed: " + ec.message()};
            }
        }

        ec.clear();
        std::filesystem::rename(temp_path, snapshot_path, ec);
        if (ec)
        {
            return {SnapshotStatus::kIoError,
                    "rename metadata snapshot file failed: " + ec.message()};
        }

        return {SnapshotStatus::kOk, "ok"};
    }

    SnapshotResult StrongConsistencyMetadataStateMachine::LoadSnapshot(const std::string &file_path)
    {
        if (file_path.empty())
        {
            return {SnapshotStatus::kInvalidArgument, "snapshot file path is empty"};
        }

        std::error_code ec;
        if (!std::filesystem::exists(file_path, ec))
        {
            return {SnapshotStatus::kNotFound, "snapshot file not found: " + file_path};
        }

        std::ifstream in(file_path, std::ios::binary);
        if (!in.is_open())
        {
            return {SnapshotStatus::kIoError,
                    "failed to open metadata snapshot file: " + file_path};
        }

        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::uint64_t record_count = 0;
        std::uint64_t tombstone_count = 0;
        std::uint64_t replay_count = 0;
        if (!ReadPod(in, &magic) ||
            !ReadPod(in, &version) ||
            !ReadPod(in, &record_count) ||
            !ReadPod(in, &tombstone_count) ||
            !ReadPod(in, &replay_count))
        {
            return {SnapshotStatus::kCorruptedData, "failed to read metadata snapshot header"};
        }

        if (magic != kMetadataSnapshotMagic)
        {
            return {SnapshotStatus::kCorruptedData, "invalid metadata snapshot magic"};
        }
        if (version != kMetadataSnapshotVersion)
        {
            return {SnapshotStatus::kVersionMismatch,
                    "unsupported metadata snapshot version"};
        }

        std::unordered_map<std::string, MetadataRecord> new_records;
        for (std::uint64_t i = 0; i < record_count; ++i)
        {
            MetadataRecord record;
            if (!ReadMetadataRecord(in, &record))
            {
                return {SnapshotStatus::kCorruptedData,
                        "failed to read metadata snapshot record"};
            }
            const std::string object_key = record.object_key;
            if (!new_records.emplace(object_key, std::move(record)).second)
            {
                return {SnapshotStatus::kCorruptedData,
                        "duplicate metadata snapshot record key"};
            }
        }

        std::unordered_map<std::string, Tombstone> new_tombstones;
        for (std::uint64_t i = 0; i < tombstone_count; ++i)
        {
            Tombstone tombstone;
            if (!ReadTombstone(in, &tombstone))
            {
                return {SnapshotStatus::kCorruptedData,
                        "failed to read metadata snapshot tombstone"};
            }
            const std::string object_key = tombstone.object_key;
            if (!new_tombstones.emplace(object_key, std::move(tombstone)).second)
            {
                return {SnapshotStatus::kCorruptedData,
                        "duplicate metadata snapshot tombstone key"};
            }
        }

        std::unordered_map<ClientRequestId, IdempotencyEntry> new_replay_table;
        for (std::uint64_t i = 0; i < replay_count; ++i)
        {
            IdempotencyEntry entry;
            if (!ReadIdempotencyEntry(in, &entry))
            {
                return {SnapshotStatus::kCorruptedData,
                        "failed to read metadata snapshot replay entry"};
            }
            const ClientRequestId request_id = entry.request_id;
            if (!new_replay_table.emplace(request_id, std::move(entry)).second)
            {
                return {SnapshotStatus::kCorruptedData,
                        "duplicate metadata snapshot replay request_id"};
            }
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            records_ = std::move(new_records);
            tombstones_ = std::move(new_tombstones);
            replay_table_ = std::move(new_replay_table);
        }

        return {SnapshotStatus::kOk, "ok"};
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
