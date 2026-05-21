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
        constexpr std::uint32_t kStrongConsistencyMetadataSnapshotVersion = 1U;
        constexpr std::uint32_t kMetadataStateMachineSnapshotVersion = 2U;
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

        bool IsValidObjectState(const ObjectState state)
        {
            switch (state)
            {
            case ObjectState::PENDING:
            case ObjectState::COMMITTED:
            case ObjectState::DELETED:
                return true;
            default:
                return false;
            }
        }

        bool IsValidMetadataRequestType(const MetadataRequestType type)
        {
            switch (type)
            {
            case MetadataRequestType::kUnknown:
            case MetadataRequestType::kCreateBucket:
            case MetadataRequestType::kDeleteBucket:
            case MetadataRequestType::kCreateObject:
            case MetadataRequestType::kCommitObject:
            case MetadataRequestType::kAbortObject:
            case MetadataRequestType::kDeleteObject:
                return true;
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

        bool WriteBucketRecord(std::ofstream &out,
                               const BucketRecord &record)
        {
            return WriteString(out, record.bucket) &&
                   WritePod(out, record.create_time) &&
                   WritePod(out, static_cast<std::uint8_t>(record.deleted ? 1U : 0U)) &&
                   WriteOptionalUInt64(out, record.delete_time);
        }

        bool ReadBucketRecord(std::ifstream &in,
                              BucketRecord *record)
        {
            if (record == nullptr)
            {
                return false;
            }

            BucketRecord decoded;
            std::uint8_t deleted = 0;
            if (!ReadString(in, &decoded.bucket) ||
                !ReadPod(in, &decoded.create_time) ||
                !ReadPod(in, &deleted) ||
                deleted > 1U ||
                !ReadOptionalUInt64(in, &decoded.delete_time))
            {
                return false;
            }
            decoded.deleted = deleted == 1U;
            *record = std::move(decoded);
            return true;
        }

        bool WriteChunkRef(std::ofstream &out,
                           const ChunkRef &chunk)
        {
            return WriteString(out, chunk.chunk_id) &&
                   WritePod(out, chunk.offset) &&
                   WritePod(out, chunk.size) &&
                   WriteStringVector(out, chunk.replica_nodes) &&
                   WriteString(out, chunk.checksum);
        }

        bool ReadChunkRef(std::ifstream &in,
                          ChunkRef *chunk)
        {
            if (chunk == nullptr)
            {
                return false;
            }

            ChunkRef decoded;
            if (!ReadString(in, &decoded.chunk_id) ||
                !ReadPod(in, &decoded.offset) ||
                !ReadPod(in, &decoded.size) ||
                !ReadStringVector(in, &decoded.replica_nodes) ||
                !ReadString(in, &decoded.checksum))
            {
                return false;
            }
            *chunk = std::move(decoded);
            return true;
        }

        bool WriteChunkRefVector(std::ofstream &out,
                                 const std::vector<ChunkRef> &chunks)
        {
            const std::uint64_t size = static_cast<std::uint64_t>(chunks.size());
            if (!WritePod(out, size))
            {
                return false;
            }
            for (const ChunkRef &chunk : chunks)
            {
                if (!WriteChunkRef(out, chunk))
                {
                    return false;
                }
            }
            return true;
        }

        bool ReadChunkRefVector(std::ifstream &in,
                                std::vector<ChunkRef> *chunks)
        {
            if (chunks == nullptr)
            {
                return false;
            }

            std::uint64_t size = 0;
            if (!ReadPod(in, &size))
            {
                return false;
            }

            std::vector<ChunkRef> decoded;
            decoded.reserve(static_cast<std::size_t>(size));
            for (std::uint64_t i = 0; i < size; ++i)
            {
                ChunkRef chunk;
                if (!ReadChunkRef(in, &chunk))
                {
                    return false;
                }
                decoded.push_back(std::move(chunk));
            }
            *chunks = std::move(decoded);
            return true;
        }

        bool WriteObjectRecord(std::ofstream &out,
                               const ObjectRecord &record)
        {
            return WriteString(out, record.bucket) &&
                   WriteString(out, record.object_key) &&
                   WriteString(out, record.object_id) &&
                   WritePod(out, record.version) &&
                   WritePod(out, record.size) &&
                   WriteString(out, record.etag) &&
                   WriteEnum(out, record.state) &&
                   WriteChunkRefVector(out, record.chunks) &&
                   WritePod(out, record.create_time) &&
                   WriteOptionalUInt64(out, record.commit_time) &&
                   WriteOptionalUInt64(out, record.delete_time);
        }

        bool ReadObjectRecord(std::ifstream &in,
                              ObjectRecord *record)
        {
            if (record == nullptr)
            {
                return false;
            }

            ObjectRecord decoded;
            if (!ReadString(in, &decoded.bucket) ||
                !ReadString(in, &decoded.object_key) ||
                !ReadString(in, &decoded.object_id) ||
                !ReadPod(in, &decoded.version) ||
                !ReadPod(in, &decoded.size) ||
                !ReadString(in, &decoded.etag) ||
                !ReadEnum(in, &decoded.state) ||
                !IsValidObjectState(decoded.state) ||
                !ReadChunkRefVector(in, &decoded.chunks) ||
                !ReadPod(in, &decoded.create_time) ||
                !ReadOptionalUInt64(in, &decoded.commit_time) ||
                !ReadOptionalUInt64(in, &decoded.delete_time))
            {
                return false;
            }
            *record = std::move(decoded);
            return true;
        }

        bool WriteRequestRecord(std::ofstream &out,
                                const RequestRecord &record)
        {
            return WriteString(out, record.request_id) &&
                   WriteEnum(out, record.command_type) &&
                   WriteString(out, record.bucket) &&
                   WriteString(out, record.object_key) &&
                   WriteString(out, record.result_status) &&
                   WritePod(out, record.applied_index) &&
                   WritePod(out, record.create_time) &&
                   WriteOptionalUInt64(out, record.finish_time);
        }

        bool ReadRequestRecord(std::ifstream &in,
                               RequestRecord *record)
        {
            if (record == nullptr)
            {
                return false;
            }

            RequestRecord decoded;
            if (!ReadString(in, &decoded.request_id) ||
                !ReadEnum(in, &decoded.command_type) ||
                !IsValidMetadataRequestType(decoded.command_type) ||
                !ReadString(in, &decoded.bucket) ||
                !ReadString(in, &decoded.object_key) ||
                !ReadString(in, &decoded.result_status) ||
                !ReadPod(in, &decoded.applied_index) ||
                !ReadPod(in, &decoded.create_time) ||
                !ReadOptionalUInt64(in, &decoded.finish_time))
            {
                return false;
            }
            *record = std::move(decoded);
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

        bool ChunkRefEquals(const ChunkRef &left,
                            const ChunkRef &right)
        {
            return left.chunk_id == right.chunk_id &&
                   left.offset == right.offset &&
                   left.size == right.size &&
                   left.replica_nodes == right.replica_nodes &&
                   left.checksum == right.checksum;
        }

        bool ChunkRefVectorEquals(const std::vector<ChunkRef> &left,
                                  const std::vector<ChunkRef> &right)
        {
            if (left.size() != right.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < left.size(); ++i)
            {
                if (!ChunkRefEquals(left[i], right[i]))
                {
                    return false;
                }
            }
            return true;
        }

        bool ValidateLoadedMetadataState(
            const std::unordered_map<std::string, BucketRecord> &buckets,
            const std::unordered_map<std::string, ObjectRecord> &objects,
            const std::unordered_map<std::string, std::vector<std::string>> &object_index,
            const std::unordered_map<std::string, std::vector<ChunkRef>> &chunk_ref_index,
            const std::unordered_map<ClientRequestId, RequestRecord> &requests,
            const std::unordered_map<ClientRequestId, std::string> &request_fingerprints,
            const std::unordered_map<std::string, Tombstone> &tombstones,
            std::string *error)
        {
            auto set_error = [error](std::string message)
            {
                if (error != nullptr)
                {
                    *error = std::move(message);
                }
                return false;
            };

            for (const auto &[request_id, record] : requests)
            {
                if (request_id.empty() || record.request_id != request_id)
                {
                    return set_error("invalid request table entry");
                }
                const auto fingerprint_it = request_fingerprints.find(request_id);
                if (fingerprint_it == request_fingerprints.end() ||
                    fingerprint_it->second.empty())
                {
                    return set_error("request fingerprint is missing");
                }
            }

            if (requests.size() != request_fingerprints.size())
            {
                return set_error("request table and fingerprint table size mismatch");
            }

            for (const auto &[request_id, fingerprint] : request_fingerprints)
            {
                if (request_id.empty() || fingerprint.empty())
                {
                    return set_error("invalid request fingerprint entry");
                }
                if (requests.find(request_id) == requests.end())
                {
                    return set_error("request fingerprint has no matching request");
                }
            }

            for (const auto &[identity, record] : objects)
            {
                if (identity != MakeObjectIdentity(record.bucket, record.object_key))
                {
                    return set_error("object identity does not match object record");
                }
                if (buckets.find(record.bucket) == buckets.end())
                {
                    return set_error("object references missing bucket");
                }

                const auto index_it = object_index.find(identity);
                if (record.IsDeleted())
                {
                    if (index_it != object_index.end())
                    {
                        return set_error("deleted object still present in object index");
                    }
                    if (chunk_ref_index.find(identity) != chunk_ref_index.end())
                    {
                        return set_error("deleted object still present in chunk ref index");
                    }
                    const auto tombstone_it = tombstones.find(identity);
                    if (tombstone_it == tombstones.end() ||
                        tombstone_it->second.object_key != record.object_key)
                    {
                        return set_error("deleted object is missing tombstone");
                    }
                    continue;
                }

                if (index_it == object_index.end() || index_it->second.empty())
                {
                    return set_error("live object is missing object index entry");
                }
                if (std::find(index_it->second.begin(), index_it->second.end(), record.object_id) ==
                    index_it->second.end())
                {
                    return set_error("object index does not reference object_id");
                }
                if (tombstones.find(identity) != tombstones.end())
                {
                    return set_error("live object unexpectedly has tombstone");
                }

                const auto chunk_it = chunk_ref_index.find(identity);
                if (record.IsCommitted())
                {
                    if (chunk_it == chunk_ref_index.end())
                    {
                        return set_error("committed object is missing chunk ref index");
                    }
                    if (!ChunkRefVectorEquals(chunk_it->second, record.chunks))
                    {
                        return set_error("chunk ref index does not match committed object");
                    }
                }
                else if (chunk_it != chunk_ref_index.end())
                {
                    return set_error("non-committed object unexpectedly has chunk ref index");
                }
            }

            for (const auto &[identity, ids] : object_index)
            {
                const auto object_it = objects.find(identity);
                if (object_it == objects.end())
                {
                    return set_error("object index references missing object");
                }
                if (object_it->second.IsDeleted())
                {
                    return set_error("object index references deleted object");
                }
                if (ids.empty())
                {
                    return set_error("object index entry is empty");
                }
            }

            for (const auto &[identity, chunks] : chunk_ref_index)
            {
                const auto object_it = objects.find(identity);
                if (object_it == objects.end())
                {
                    return set_error("chunk ref index references missing object");
                }
                if (!object_it->second.IsCommitted())
                {
                    return set_error("chunk ref index references non-committed object");
                }
                if (!ChunkRefVectorEquals(chunks, object_it->second.chunks))
                {
                    return set_error("chunk ref index entry does not match object chunks");
                }
            }

            for (const auto &[identity, tombstone] : tombstones)
            {
                const auto object_it = objects.find(identity);
                if (object_it == objects.end())
                {
                    return set_error("tombstone references missing object");
                }
                if (!object_it->second.IsDeleted())
                {
                    return set_error("tombstone references non-deleted object");
                }
                if (tombstone.object_key != object_it->second.object_key)
                {
                    return set_error("tombstone object_key mismatch");
                }
            }

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

        const std::string fingerprint = ComputeMetadataCommandFingerprint(command);
        std::unique_lock<std::shared_mutex> lk(mu_);
        const auto existing_request = requests_.find(command.request_id);
        if (existing_request != requests_.end())
        {
            const auto fingerprint_it = request_fingerprints_.find(command.request_id);
            if (fingerprint_it != request_fingerprints_.end() &&
                fingerprint_it->second == fingerprint)
            {
                return MakeReplaySuccess();
            }
            return MakeReplayConflictFailure();
        }

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
            request_fingerprints_[command.request_id] = fingerprint;
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
            request_fingerprints_[command.request_id] = fingerprint;
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
            request_fingerprints_[command.request_id] = fingerprint;
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
            request_fingerprints_[command.request_id] = fingerprint;
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
            request_fingerprints_[command.request_id] = fingerprint;
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
            request_fingerprints_[command.request_id] = fingerprint;
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

        std::vector<std::pair<std::string, BucketRecord>> buckets;
        std::vector<std::pair<std::string, ObjectRecord>> objects;
        std::vector<std::pair<std::string, std::vector<std::string>>> object_index;
        std::vector<std::pair<std::string, std::vector<ChunkRef>>> chunk_ref_index;
        std::vector<std::pair<ClientRequestId, RequestRecord>> requests;
        std::vector<std::pair<ClientRequestId, std::string>> request_fingerprints;
        std::vector<std::pair<std::string, Tombstone>> tombstones;
        std::uint64_t last_applied_index = 0;
        std::uint64_t last_applied_term = 0;
        {
            std::shared_lock<std::shared_mutex> lk(mu_);
            last_applied_index = last_applied_index_;
            last_applied_term = last_applied_term_;
            buckets.assign(buckets_.begin(), buckets_.end());
            objects.assign(objects_.begin(), objects_.end());
            object_index.assign(object_index_.begin(), object_index_.end());
            chunk_ref_index.assign(chunk_ref_index_.begin(), chunk_ref_index_.end());
            requests.assign(requests_.begin(), requests_.end());
            request_fingerprints.assign(request_fingerprints_.begin(), request_fingerprints_.end());
            tombstones.assign(tombstones_.begin(), tombstones_.end());
        }

        auto sort_by_key = [](auto &entries)
        {
            std::sort(entries.begin(), entries.end(),
                      [](const auto &left, const auto &right)
                      {
                          return left.first < right.first;
                      });
        };
        sort_by_key(buckets);
        sort_by_key(objects);
        sort_by_key(object_index);
        sort_by_key(chunk_ref_index);
        sort_by_key(requests);
        sort_by_key(request_fingerprints);
        sort_by_key(tombstones);

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
                        "create metadata state machine snapshot directory failed: " + ec.message()};
            }
        }

        {
            std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                return {SnapshotStatus::kIoError,
                        "open temp metadata state machine snapshot file failed: " + temp_path.string()};
            }

            if (!WritePod(out, kMetadataSnapshotMagic) ||
                !WritePod(out, kMetadataStateMachineSnapshotVersion) ||
                !WritePod(out, last_applied_index) ||
                !WritePod(out, last_applied_term) ||
                !WritePod(out, static_cast<std::uint64_t>(buckets.size())) ||
                !WritePod(out, static_cast<std::uint64_t>(objects.size())) ||
                !WritePod(out, static_cast<std::uint64_t>(object_index.size())) ||
                !WritePod(out, static_cast<std::uint64_t>(chunk_ref_index.size())) ||
                !WritePod(out, static_cast<std::uint64_t>(requests.size())) ||
                !WritePod(out, static_cast<std::uint64_t>(request_fingerprints.size())) ||
                !WritePod(out, static_cast<std::uint64_t>(tombstones.size())))
            {
                return {SnapshotStatus::kIoError,
                        "write metadata state machine snapshot header failed"};
            }

            for (const auto &[key, record] : buckets)
            {
                if (key != record.bucket || !WriteBucketRecord(out, record))
                {
                    return {SnapshotStatus::kIoError,
                            "write metadata state machine bucket failed"};
                }
            }
            for (const auto &[key, record] : objects)
            {
                if (key != MakeObjectIdentity(record.bucket, record.object_key) ||
                    !WriteObjectRecord(out, record))
                {
                    return {SnapshotStatus::kIoError,
                            "write metadata state machine object failed"};
                }
            }
            for (const auto &[key, values] : object_index)
            {
                if (!WriteString(out, key) || !WriteStringVector(out, values))
                {
                    return {SnapshotStatus::kIoError,
                            "write metadata state machine object index failed"};
                }
            }
            for (const auto &[key, values] : chunk_ref_index)
            {
                if (!WriteString(out, key) || !WriteChunkRefVector(out, values))
                {
                    return {SnapshotStatus::kIoError,
                            "write metadata state machine chunk ref index failed"};
                }
            }
            for (const auto &[key, record] : requests)
            {
                if (key != record.request_id || !WriteRequestRecord(out, record))
                {
                    return {SnapshotStatus::kIoError,
                            "write metadata state machine request failed"};
                }
            }
            for (const auto &[key, value] : request_fingerprints)
            {
                if (!WriteString(out, key) || !WriteString(out, value))
                {
                    return {SnapshotStatus::kIoError,
                            "write metadata state machine request fingerprint failed"};
                }
            }
            for (const auto &[key, tombstone] : tombstones)
            {
                if (!WriteString(out, key) || !WriteTombstone(out, tombstone))
                {
                    return {SnapshotStatus::kIoError,
                            "write metadata state machine tombstone failed"};
                }
            }

            out.flush();
            if (!out)
            {
                return {SnapshotStatus::kIoError,
                        "flush metadata state machine snapshot file failed"};
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
                        "remove old metadata state machine snapshot file failed: " + ec.message()};
            }
        }

        ec.clear();
        std::filesystem::rename(temp_path, snapshot_path, ec);
        if (ec)
        {
            return {SnapshotStatus::kIoError,
                    "rename metadata state machine snapshot file failed: " + ec.message()};
        }

        return {SnapshotStatus::kOk, "ok"};
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

        std::ifstream in(file_path, std::ios::binary);
        if (!in.is_open())
        {
            return {SnapshotStatus::kIoError,
                    "failed to open metadata state machine snapshot file: " + file_path};
        }

        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::uint64_t last_applied_index = 0;
        std::uint64_t last_applied_term = 0;
        std::uint64_t bucket_count = 0;
        std::uint64_t object_count = 0;
        std::uint64_t object_index_count = 0;
        std::uint64_t chunk_ref_index_count = 0;
        std::uint64_t request_count = 0;
        std::uint64_t request_fingerprint_count = 0;
        std::uint64_t tombstone_count = 0;
        if (!ReadPod(in, &magic) ||
            !ReadPod(in, &version) ||
            !ReadPod(in, &last_applied_index) ||
            !ReadPod(in, &last_applied_term) ||
            !ReadPod(in, &bucket_count) ||
            !ReadPod(in, &object_count) ||
            !ReadPod(in, &object_index_count) ||
            !ReadPod(in, &chunk_ref_index_count) ||
            !ReadPod(in, &request_count) ||
            !ReadPod(in, &request_fingerprint_count) ||
            !ReadPod(in, &tombstone_count))
        {
            return {SnapshotStatus::kCorruptedData,
                    "failed to read metadata state machine snapshot header"};
        }
        if (magic != kMetadataSnapshotMagic)
        {
            return {SnapshotStatus::kCorruptedData,
                    "invalid metadata state machine snapshot magic"};
        }
        if (version != kMetadataStateMachineSnapshotVersion)
        {
            return {SnapshotStatus::kVersionMismatch,
                    "unsupported metadata state machine snapshot version"};
        }

        std::unordered_map<std::string, BucketRecord> new_buckets;
        for (std::uint64_t i = 0; i < bucket_count; ++i)
        {
            BucketRecord record;
            if (!ReadBucketRecord(in, &record))
            {
                return {SnapshotStatus::kCorruptedData,
                        "failed to read metadata state machine bucket"};
            }
            if (!new_buckets.emplace(record.bucket, std::move(record)).second)
            {
                return {SnapshotStatus::kCorruptedData,
                        "duplicate metadata state machine bucket key"};
            }
        }

        std::unordered_map<std::string, ObjectRecord> new_objects;
        for (std::uint64_t i = 0; i < object_count; ++i)
        {
            ObjectRecord record;
            if (!ReadObjectRecord(in, &record))
            {
                return {SnapshotStatus::kCorruptedData,
                        "failed to read metadata state machine object"};
            }
            const std::string identity = MakeObjectIdentity(record.bucket, record.object_key);
            if (!new_objects.emplace(identity, std::move(record)).second)
            {
                return {SnapshotStatus::kCorruptedData,
                        "duplicate metadata state machine object key"};
            }
        }

        std::unordered_map<std::string, std::vector<std::string>> new_object_index;
        for (std::uint64_t i = 0; i < object_index_count; ++i)
        {
            std::string key;
            std::vector<std::string> values;
            if (!ReadString(in, &key) || !ReadStringVector(in, &values))
            {
                return {SnapshotStatus::kCorruptedData,
                        "failed to read metadata state machine object index"};
            }
            if (!new_object_index.emplace(std::move(key), std::move(values)).second)
            {
                return {SnapshotStatus::kCorruptedData,
                        "duplicate metadata state machine object index key"};
            }
        }

        std::unordered_map<std::string, std::vector<ChunkRef>> new_chunk_ref_index;
        for (std::uint64_t i = 0; i < chunk_ref_index_count; ++i)
        {
            std::string key;
            std::vector<ChunkRef> values;
            if (!ReadString(in, &key) || !ReadChunkRefVector(in, &values))
            {
                return {SnapshotStatus::kCorruptedData,
                        "failed to read metadata state machine chunk ref index"};
            }
            if (!new_chunk_ref_index.emplace(std::move(key), std::move(values)).second)
            {
                return {SnapshotStatus::kCorruptedData,
                        "duplicate metadata state machine chunk ref index key"};
            }
        }

        std::unordered_map<ClientRequestId, RequestRecord> new_requests;
        for (std::uint64_t i = 0; i < request_count; ++i)
        {
            RequestRecord record;
            if (!ReadRequestRecord(in, &record))
            {
                return {SnapshotStatus::kCorruptedData,
                        "failed to read metadata state machine request"};
            }
            if (!new_requests.emplace(record.request_id, std::move(record)).second)
            {
                return {SnapshotStatus::kCorruptedData,
                        "duplicate metadata state machine request_id"};
            }
        }

        std::unordered_map<ClientRequestId, std::string> new_request_fingerprints;
        for (std::uint64_t i = 0; i < request_fingerprint_count; ++i)
        {
            std::string key;
            std::string value;
            if (!ReadString(in, &key) || !ReadString(in, &value))
            {
                return {SnapshotStatus::kCorruptedData,
                        "failed to read metadata state machine request fingerprint"};
            }
            if (!new_request_fingerprints.emplace(std::move(key), std::move(value)).second)
            {
                return {SnapshotStatus::kCorruptedData,
                        "duplicate metadata state machine request fingerprint key"};
            }
        }

        std::unordered_map<std::string, Tombstone> new_tombstones;
        for (std::uint64_t i = 0; i < tombstone_count; ++i)
        {
            std::string key;
            Tombstone tombstone;
            if (!ReadString(in, &key) || !ReadTombstone(in, &tombstone))
            {
                return {SnapshotStatus::kCorruptedData,
                        "failed to read metadata state machine tombstone"};
            }
            if (!new_tombstones.emplace(std::move(key), std::move(tombstone)).second)
            {
                return {SnapshotStatus::kCorruptedData,
                        "duplicate metadata state machine tombstone key"};
            }
        }

        std::string validation_error;
        if (!ValidateLoadedMetadataState(new_buckets,
                                         new_objects,
                                         new_object_index,
                                         new_chunk_ref_index,
                                         new_requests,
                                         new_request_fingerprints,
                                         new_tombstones,
                                         &validation_error))
        {
            return {SnapshotStatus::kCorruptedData,
                    "invalid metadata state machine snapshot state: " + validation_error};
        }

        {
            std::unique_lock<std::shared_mutex> lk(mu_);
            last_applied_index_ = last_applied_index;
            last_applied_term_ = last_applied_term;
            buckets_ = std::move(new_buckets);
            objects_ = std::move(new_objects);
            object_index_ = std::move(new_object_index);
            chunk_ref_index_ = std::move(new_chunk_ref_index);
            requests_ = std::move(new_requests);
            request_fingerprints_ = std::move(new_request_fingerprints);
            tombstones_ = std::move(new_tombstones);
        }

        return {SnapshotStatus::kOk, "ok"};
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

        std::shared_lock<std::shared_mutex> lk(mu_);
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

        std::shared_lock<std::shared_mutex> lk(mu_);
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
        std::shared_lock<std::shared_mutex> lk(mu_);
        return last_applied_index_;
    }

    std::uint64_t MetadataStateMachine::LastAppliedTerm() const
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        return last_applied_term_;
    }

    std::size_t MetadataStateMachine::BucketCount() const
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        return buckets_.size();
    }

    std::size_t MetadataStateMachine::ObjectCount() const
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        return objects_.size();
    }

    std::size_t MetadataStateMachine::RequestCount() const
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        return requests_.size();
    }

    std::size_t MetadataStateMachine::TombstoneCount() const
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
        return tombstones_.size();
    }

    std::optional<BucketRecord> MetadataStateMachine::FindBucket(
        std::string_view bucket) const
    {
        std::shared_lock<std::shared_mutex> lk(mu_);
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
        std::shared_lock<std::shared_mutex> lk(mu_);
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
        std::shared_lock<std::shared_mutex> lk(mu_);
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
        std::shared_lock<std::shared_mutex> lk(mu_);
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
            const std::uint32_t version = kStrongConsistencyMetadataSnapshotVersion;
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
        if (version != kStrongConsistencyMetadataSnapshotVersion)
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
