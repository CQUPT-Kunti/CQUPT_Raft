#include "raft/common/metadata_command.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace raftdemo
{
    namespace
    {
        constexpr char kFieldSeparator = '\n';
        constexpr char kKeyValueSeparator = '=';
        constexpr std::string_view kEnvelopeTag = "META1";
        constexpr std::string_view kListSeparator = ",";
        constexpr uint64_t kMaxPayloadBytes = 4096;

        using FieldMap = std::unordered_map<std::string, std::string>;

        std::string Escape(std::string_view input)
        {
            std::string output;
            output.reserve(input.size());

            for (const char ch : input)
            {
                switch (ch)
                {
                case '\\':
                    output += "\\\\";
                    break;
                case '\n':
                    output += "\\n";
                    break;
                case '=':
                    output += "\\e";
                    break;
                case ',':
                    output += "\\c";
                    break;
                default:
                    output.push_back(ch);
                    break;
                }
            }

            return output;
        }

        bool Unescape(std::string_view input, std::string *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            std::string output;
            output.reserve(input.size());

            for (size_t i = 0; i < input.size(); ++i)
            {
                const char ch = input[i];
                if (ch != '\\')
                {
                    output.push_back(ch);
                    continue;
                }

                if (i + 1 >= input.size())
                {
                    return false;
                }

                const char escaped = input[++i];
                switch (escaped)
                {
                case '\\':
                    output.push_back('\\');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'e':
                    output.push_back('=');
                    break;
                case 'c':
                    output.push_back(',');
                    break;
                default:
                    return false;
                }
            }

            *out = std::move(output);
            return true;
        }

        std::string JoinEscaped(const std::vector<std::string> &items)
        {
            std::ostringstream oss;
            for (size_t i = 0; i < items.size(); ++i)
            {
                if (i != 0)
                {
                    oss << kListSeparator;
                }
                oss << Escape(items[i]);
            }
            return oss.str();
        }

        bool SplitEscaped(std::string_view input, std::vector<std::string> *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            out->clear();
            if (input.empty())
            {
                return true;
            }

            std::string current;
            for (size_t i = 0; i < input.size(); ++i)
            {
                const char ch = input[i];
                if (ch == '\\')
                {
                    if (i + 1 >= input.size())
                    {
                        return false;
                    }

                    const char escaped = input[++i];
                    switch (escaped)
                    {
                    case '\\':
                        current.push_back('\\');
                        break;
                    case 'n':
                        current.push_back('\n');
                        break;
                    case 'e':
                        current.push_back('=');
                        break;
                    case 'c':
                        current.push_back(',');
                        break;
                    default:
                        return false;
                    }
                    continue;
                }

                if (ch == ',')
                {
                    out->push_back(current);
                    current.clear();
                    continue;
                }

                current.push_back(ch);
            }

            out->push_back(std::move(current));
            return true;
        }

        std::string OperationToString(const MetadataOperation operation)
        {
            switch (operation)
            {
            case MetadataOperation::kCreate:
                return "create";
            case MetadataOperation::kCommit:
                return "commit";
            case MetadataOperation::kDelete:
                return "delete";
            case MetadataOperation::kUnknown:
            default:
                return "unknown";
            }
        }

        bool StringToOperation(std::string_view input, MetadataOperation *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            if (input == "create")
            {
                *out = MetadataOperation::kCreate;
                return true;
            }
            if (input == "commit")
            {
                *out = MetadataOperation::kCommit;
                return true;
            }
            if (input == "delete")
            {
                *out = MetadataOperation::kDelete;
                return true;
            }

            if (input == "unknown")
            {
                *out = MetadataOperation::kUnknown;
                return true;
            }

            *out = MetadataOperation::kUnknown;
            return false;
        }

        std::string StateToString(const MetadataRecordState state)
        {
            switch (state)
            {
            case MetadataRecordState::kPending:
                return "pending";
            case MetadataRecordState::kCommitted:
                return "committed";
            case MetadataRecordState::kDeleted:
                return "deleted";
            default:
                return "unknown";
            }
        }

        bool StringToState(std::string_view input, MetadataRecordState *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            if (input == "pending")
            {
                *out = MetadataRecordState::kPending;
                return true;
            }
            if (input == "committed")
            {
                *out = MetadataRecordState::kCommitted;
                return true;
            }
            if (input == "deleted")
            {
                *out = MetadataRecordState::kDeleted;
                return true;
            }
            return false;
        }

        std::string ObjectStateToString(const ObjectState state)
        {
            switch (state)
            {
            case ObjectState::PENDING:
                return "pending";
            case ObjectState::COMMITTED:
                return "committed";
            case ObjectState::DELETED:
                return "deleted";
            default:
                return "unknown";
            }
        }

        bool StringToObjectState(std::string_view input, ObjectState *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            if (input == "pending")
            {
                *out = ObjectState::PENDING;
                return true;
            }
            if (input == "committed")
            {
                *out = ObjectState::COMMITTED;
                return true;
            }
            if (input == "deleted")
            {
                *out = ObjectState::DELETED;
                return true;
            }
            return false;
        }

        std::string RequestTypeToString(const MetadataRequestType type)
        {
            switch (type)
            {
            case MetadataRequestType::kCreateBucket:
                return "create_bucket";
            case MetadataRequestType::kDeleteBucket:
                return "delete_bucket";
            case MetadataRequestType::kCreateObject:
                return "create_object";
            case MetadataRequestType::kCommitObject:
                return "commit_object";
            case MetadataRequestType::kAbortObject:
                return "abort_object";
            case MetadataRequestType::kDeleteObject:
                return "delete_object";
            case MetadataRequestType::kUnknown:
            default:
                return "unknown";
            }
        }

        bool StringToRequestType(std::string_view input, MetadataRequestType *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            if (input == "create_bucket")
            {
                *out = MetadataRequestType::kCreateBucket;
                return true;
            }
            if (input == "delete_bucket")
            {
                *out = MetadataRequestType::kDeleteBucket;
                return true;
            }
            if (input == "create_object")
            {
                *out = MetadataRequestType::kCreateObject;
                return true;
            }
            if (input == "commit_object")
            {
                *out = MetadataRequestType::kCommitObject;
                return true;
            }
            if (input == "abort_object")
            {
                *out = MetadataRequestType::kAbortObject;
                return true;
            }
            if (input == "delete_object")
            {
                *out = MetadataRequestType::kDeleteObject;
                return true;
            }
            *out = MetadataRequestType::kUnknown;
            return false;
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

        bool StringToCommandType(std::string_view input, MetadataCommandType *out)
        {
            if (out == nullptr)
            {
                return false;
            }

            if (input == "create_bucket")
            {
                *out = MetadataCommandType::kCreateBucket;
                return true;
            }
            if (input == "delete_bucket")
            {
                *out = MetadataCommandType::kDeleteBucket;
                return true;
            }
            if (input == "create_object")
            {
                *out = MetadataCommandType::kCreateObject;
                return true;
            }
            if (input == "commit_object")
            {
                *out = MetadataCommandType::kCommitObject;
                return true;
            }
            if (input == "abort_object")
            {
                *out = MetadataCommandType::kAbortObject;
                return true;
            }
            if (input == "delete_object")
            {
                *out = MetadataCommandType::kDeleteObject;
                return true;
            }

            *out = MetadataCommandType::kUnknown;
            return false;
        }

        bool ParseUint64(std::string_view input, uint64_t *out)
        {
            if (out == nullptr || input.empty())
            {
                return false;
            }

            uint64_t value = 0;
            for (const char ch : input)
            {
                if (!std::isdigit(static_cast<unsigned char>(ch)))
                {
                    return false;
                }
                value = value * 10 + static_cast<uint64_t>(ch - '0');
            }

            *out = value;
            return true;
        }

        void AppendField(std::ostringstream *oss, std::string_view key, std::string_view value)
        {
            *oss << key << kKeyValueSeparator << value << kFieldSeparator;
        }

        std::string SerializeRecordForCreate(const MetadataRecord &record)
        {
            std::ostringstream oss;
            AppendField(&oss, "record_state", StateToString(record.state));
            AppendField(&oss, "record_object_size", std::to_string(record.object_size));
            AppendField(&oss, "record_chunk_size", std::to_string(record.chunk_size));
            AppendField(&oss, "record_chunk_count", std::to_string(record.chunk_count));
            AppendField(&oss, "record_checksum", Escape(record.checksum));
            AppendField(&oss, "record_mock_locations", JoinEscaped(record.mock_locations));
            AppendField(&oss, "record_payload", Escape(record.payload));
            AppendField(&oss, "record_create_request_id", Escape(record.create_request_id));
            return oss.str();
        }

        void AppendOptionalUint64Field(std::ostringstream *oss,
                                       std::string_view key,
                                       const std::optional<uint64_t> &value)
        {
            if (value.has_value())
            {
                AppendField(oss, key, std::to_string(*value));
            }
        }

        void AppendRequestContext(std::ostringstream *oss, const RequestRecord &request)
        {
            AppendField(oss, "request_ctx_request_id", Escape(request.request_id));
            AppendField(oss, "request_ctx_command_type", RequestTypeToString(request.command_type));
            AppendField(oss, "request_ctx_bucket", Escape(request.bucket));
            AppendField(oss, "request_ctx_object_key", Escape(request.object_key));
            AppendField(oss, "request_ctx_result_status", Escape(request.result_status));
            AppendField(oss, "request_ctx_applied_index", std::to_string(request.applied_index));
            AppendField(oss, "request_ctx_create_time", std::to_string(request.create_time));
            AppendOptionalUint64Field(oss, "request_ctx_finish_time", request.finish_time);
        }

        void AppendChunkFields(std::ostringstream *oss, const std::vector<ChunkRef> &chunks)
        {
            AppendField(oss, "target_chunk_count", std::to_string(chunks.size()));
            for (size_t index = 0; index < chunks.size(); ++index)
            {
                const ChunkRef &chunk = chunks[index];
                const std::string prefix = "target_chunk_" + std::to_string(index) + "_";
                AppendField(oss, prefix + "id", Escape(chunk.chunk_id));
                AppendField(oss, prefix + "offset", std::to_string(chunk.offset));
                AppendField(oss, prefix + "size", std::to_string(chunk.size));
                AppendField(oss, prefix + "replicas", JoinEscaped(chunk.replica_nodes));
                AppendField(oss, prefix + "checksum", Escape(chunk.checksum));
            }
        }

        void AppendObjectRecordFields(std::ostringstream *oss, const ObjectRecord &record)
        {
            AppendField(oss, "target_bucket", Escape(record.bucket));
            AppendField(oss, "target_object_key", Escape(record.object_key));
            AppendField(oss, "target_object_id", Escape(record.object_id));
            AppendField(oss, "target_version", std::to_string(record.version));
            AppendField(oss, "target_size", std::to_string(record.size));
            AppendField(oss, "target_etag", Escape(record.etag));
            AppendField(oss, "target_state", ObjectStateToString(record.state));
            AppendField(oss, "target_create_time", std::to_string(record.create_time));
            AppendOptionalUint64Field(oss, "target_commit_time", record.commit_time);
            AppendOptionalUint64Field(oss, "target_delete_time", record.delete_time);
            AppendChunkFields(oss, record.chunks);
        }

        void AppendV2CommandPayload(std::ostringstream *oss, const MetadataCommand &command)
        {
            AppendField(oss, "command_type", CommandTypeToString(command.command_type));

            if (command.request_context.has_value())
            {
                AppendRequestContext(oss, *command.request_context);
            }

            switch (command.command_type)
            {
            case MetadataCommandType::kCreateBucket:
                AppendField(oss, "target_bucket", Escape(command.create_bucket->bucket_record.bucket));
                AppendField(oss, "target_create_time",
                            std::to_string(command.create_bucket->bucket_record.create_time));
                AppendField(oss, "target_deleted",
                            command.create_bucket->bucket_record.deleted ? "true" : "false");
                AppendOptionalUint64Field(oss, "target_delete_time",
                                          command.create_bucket->bucket_record.delete_time);
                break;
            case MetadataCommandType::kDeleteBucket:
                AppendField(oss, "target_bucket", Escape(command.delete_bucket->bucket));
                AppendField(oss, "target_if_empty", command.delete_bucket->if_empty ? "true" : "false");
                break;
            case MetadataCommandType::kCreateObject:
                AppendObjectRecordFields(oss, command.create_object->object_record);
                break;
            case MetadataCommandType::kCommitObject:
                AppendField(oss, "target_bucket", Escape(command.commit_object->bucket));
                AppendField(oss, "target_object_key", Escape(command.commit_object->object_key));
                AppendField(oss, "target_object_id", Escape(command.commit_object->object_id));
                AppendField(oss, "target_version", std::to_string(command.commit_object->version));
                AppendField(oss, "target_size", std::to_string(command.commit_object->size));
                AppendField(oss, "target_etag", Escape(command.commit_object->etag));
                AppendOptionalUint64Field(oss, "target_commit_time", command.commit_object->commit_time);
                AppendChunkFields(oss, command.commit_object->chunks);
                break;
            case MetadataCommandType::kAbortObject:
                AppendField(oss, "target_bucket", Escape(command.abort_object->bucket));
                AppendField(oss, "target_object_key", Escape(command.abort_object->object_key));
                AppendField(oss, "target_object_id", Escape(command.abort_object->object_id));
                AppendField(oss, "target_version", std::to_string(command.abort_object->version));
                break;
            case MetadataCommandType::kDeleteObject:
                AppendField(oss, "target_bucket", Escape(command.delete_object->bucket));
                AppendField(oss, "target_object_key", Escape(command.delete_object->object_key));
                AppendField(oss, "target_object_id", Escape(command.delete_object->object_id));
                AppendField(oss, "target_version", std::to_string(command.delete_object->version));
                AppendOptionalUint64Field(oss, "target_delete_time", command.delete_object->delete_time);
                break;
            case MetadataCommandType::kUnknown:
            default:
                break;
            }
        }

        bool ParseFields(const std::string &input, FieldMap *fields)
        {
            if (fields == nullptr)
            {
                return false;
            }

            fields->clear();
            std::stringstream ss(input);
            std::string line;

            if (!std::getline(ss, line))
            {
                return false;
            }
            if (line != kEnvelopeTag)
            {
                return false;
            }

            while (std::getline(ss, line))
            {
                if (line.empty())
                {
                    continue;
                }

                const size_t pos = line.find(kKeyValueSeparator);
                if (pos == std::string::npos || pos == 0)
                {
                    return false;
                }

                const std::string key = line.substr(0, pos);
                const std::string value = line.substr(pos + 1);
                (*fields)[key] = value;
            }

            return true;
        }

        bool ParseCreateRecord(const FieldMap &fields, MetadataRecord *record)
        {
            if (record == nullptr)
            {
                return false;
            }

            MetadataRecord parsed;
            parsed.state = MetadataRecordState::kPending;

            auto checksum_it = fields.find("record_checksum");
            auto payload_it = fields.find("record_payload");
            auto create_request_it = fields.find("record_create_request_id");
            auto state_it = fields.find("record_state");
            auto locations_it = fields.find("record_mock_locations");
            auto object_size_it = fields.find("record_object_size");
            auto chunk_size_it = fields.find("record_chunk_size");
            auto chunk_count_it = fields.find("record_chunk_count");

            if (checksum_it == fields.end() || payload_it == fields.end() || create_request_it == fields.end() ||
                state_it == fields.end() || locations_it == fields.end() || object_size_it == fields.end() ||
                chunk_size_it == fields.end() || chunk_count_it == fields.end())
            {
                return false;
            }

            if (!StringToState(state_it->second, &parsed.state))
            {
                return false;
            }
            if (!ParseUint64(object_size_it->second, &parsed.object_size) ||
                !ParseUint64(chunk_size_it->second, &parsed.chunk_size) ||
                !ParseUint64(chunk_count_it->second, &parsed.chunk_count))
            {
                return false;
            }
            if (!Unescape(checksum_it->second, &parsed.checksum) ||
                !Unescape(payload_it->second, &parsed.payload) ||
                !Unescape(create_request_it->second, &parsed.create_request_id) ||
                !SplitEscaped(locations_it->second, &parsed.mock_locations))
            {
                return false;
            }

            *record = std::move(parsed);
            return true;
        }

        bool ValidateCreateRecord(const MetadataRecord &record, std::string *error)
        {
            auto set_error = [&](std::string_view message) {
                if (error != nullptr)
                {
                    *error = std::string(message);
                }
            };

            const auto is_blank = [](std::string_view value) {
                for (const char ch : value)
                {
                    if (!std::isspace(static_cast<unsigned char>(ch)))
                    {
                        return false;
                    }
                }
                return true;
            };

            if (record.state != MetadataRecordState::kPending)
            {
                set_error("create record must start as pending");
                return false;
            }
            if (record.create_request_id.empty())
            {
                set_error("create record missing create_request_id");
                return false;
            }
            if (record.object_key.empty())
            {
                set_error("create record missing object_key");
                return false;
            }
            if (record.object_size == 0)
            {
                set_error("create record object_size must be positive");
                return false;
            }
            if (record.chunk_size == 0 || record.chunk_count == 0)
            {
                set_error("create record chunk_size and chunk_count must be positive");
                return false;
            }
            const uint64_t expected_chunk_count =
                1 + ((record.object_size - 1) / record.chunk_size);
            if (record.chunk_count != expected_chunk_count)
            {
                set_error("create record chunk_count must match object_size and chunk_size");
                return false;
            }
            if (record.mock_locations.empty())
            {
                set_error("create record missing mock_locations");
                return false;
            }
            for (const auto &location : record.mock_locations)
            {
                if (location.empty() || is_blank(location))
                {
                    set_error("create record mock_locations must not contain empty entries");
                    return false;
                }
            }
            if (record.checksum.empty() || is_blank(record.checksum))
            {
                set_error("create record missing checksum");
                return false;
            }
            if (record.payload.size() > kMaxPayloadBytes)
            {
                set_error("create record payload exceeds limit");
                return false;
            }
            return true;
        }

        bool HasAnyRequestContextField(const FieldMap &fields)
        {
            return fields.find("request_ctx_request_id") != fields.end() ||
                   fields.find("request_ctx_command_type") != fields.end() ||
                   fields.find("request_ctx_bucket") != fields.end() ||
                   fields.find("request_ctx_object_key") != fields.end() ||
                   fields.find("request_ctx_result_status") != fields.end() ||
                   fields.find("request_ctx_applied_index") != fields.end() ||
                   fields.find("request_ctx_create_time") != fields.end() ||
                   fields.find("request_ctx_finish_time") != fields.end();
        }

        bool ParseRequestContext(const FieldMap &fields, RequestRecord *request)
        {
            if (request == nullptr)
            {
                return false;
            }

            auto request_id_it = fields.find("request_ctx_request_id");
            auto command_type_it = fields.find("request_ctx_command_type");
            auto bucket_it = fields.find("request_ctx_bucket");
            auto object_key_it = fields.find("request_ctx_object_key");
            auto result_status_it = fields.find("request_ctx_result_status");
            auto applied_index_it = fields.find("request_ctx_applied_index");
            auto create_time_it = fields.find("request_ctx_create_time");

            if (request_id_it == fields.end() || command_type_it == fields.end() || bucket_it == fields.end() ||
                object_key_it == fields.end() || result_status_it == fields.end() ||
                applied_index_it == fields.end() || create_time_it == fields.end())
            {
                return false;
            }

            RequestRecord parsed;
            if (!Unescape(request_id_it->second, &parsed.request_id) ||
                !Unescape(bucket_it->second, &parsed.bucket) ||
                !Unescape(object_key_it->second, &parsed.object_key) ||
                !Unescape(result_status_it->second, &parsed.result_status) ||
                !StringToRequestType(command_type_it->second, &parsed.command_type) ||
                !ParseUint64(applied_index_it->second, &parsed.applied_index) ||
                !ParseUint64(create_time_it->second, &parsed.create_time))
            {
                return false;
            }

            if (parsed.command_type == MetadataRequestType::kUnknown)
            {
                return false;
            }

            const auto finish_time_it = fields.find("request_ctx_finish_time");
            if (finish_time_it != fields.end())
            {
                uint64_t finish_time = 0;
                if (!ParseUint64(finish_time_it->second, &finish_time))
                {
                    return false;
                }
                parsed.finish_time = finish_time;
            }

            *request = std::move(parsed);
            return true;
        }

        bool ParseChunkList(const FieldMap &fields, std::vector<ChunkRef> *chunks)
        {
            if (chunks == nullptr)
            {
                return false;
            }

            chunks->clear();
            const auto count_it = fields.find("target_chunk_count");
            if (count_it == fields.end())
            {
                return true;
            }

            uint64_t count = 0;
            if (!ParseUint64(count_it->second, &count))
            {
                return false;
            }

            for (uint64_t index = 0; index < count; ++index)
            {
                const std::string prefix = "target_chunk_" + std::to_string(index) + "_";
                const auto id_it = fields.find(prefix + "id");
                const auto offset_it = fields.find(prefix + "offset");
                const auto size_it = fields.find(prefix + "size");
                const auto replicas_it = fields.find(prefix + "replicas");
                const auto checksum_it = fields.find(prefix + "checksum");

                if (id_it == fields.end() || offset_it == fields.end() || size_it == fields.end() ||
                    replicas_it == fields.end() || checksum_it == fields.end())
                {
                    return false;
                }

                ChunkRef chunk;
                if (!Unescape(id_it->second, &chunk.chunk_id) ||
                    !ParseUint64(offset_it->second, &chunk.offset) ||
                    !ParseUint64(size_it->second, &chunk.size) ||
                    !SplitEscaped(replicas_it->second, &chunk.replica_nodes) ||
                    !Unescape(checksum_it->second, &chunk.checksum))
                {
                    return false;
                }

                chunks->push_back(std::move(chunk));
            }

            return true;
        }

        bool ParseBoolString(std::string_view value, bool *out)
        {
            if (out == nullptr)
            {
                return false;
            }
            if (value == "true")
            {
                *out = true;
                return true;
            }
            if (value == "false")
            {
                *out = false;
                return true;
            }
            return false;
        }

        bool ParseOptionalUint64Field(const FieldMap &fields,
                                      std::string_view key,
                                      std::optional<uint64_t> *out)
        {
            if (out == nullptr)
            {
                return false;
            }
            const auto it = fields.find(std::string(key));
            if (it == fields.end())
            {
                out->reset();
                return true;
            }
            uint64_t value = 0;
            if (!ParseUint64(it->second, &value))
            {
                return false;
            }
            *out = value;
            return true;
        }

        bool ParseRequiredEscapedField(const FieldMap &fields,
                                       std::string_view key,
                                       std::string *out)
        {
            if (out == nullptr)
            {
                return false;
            }
            const auto it = fields.find(std::string(key));
            if (it == fields.end())
            {
                return false;
            }
            return Unescape(it->second, out);
        }

        bool ParseRequiredUint64Field(const FieldMap &fields,
                                      std::string_view key,
                                      uint64_t *out)
        {
            const auto it = fields.find(std::string(key));
            return it != fields.end() && ParseUint64(it->second, out);
        }

        bool ParseCreateBucketPayload(const FieldMap &fields, CreateBucketCommandPayload *payload)
        {
            if (payload == nullptr)
            {
                return false;
            }
            CreateBucketCommandPayload parsed;
            auto deleted_it = fields.find("target_deleted");
            if (!ParseRequiredEscapedField(fields, "target_bucket", &parsed.bucket_record.bucket) ||
                !ParseRequiredUint64Field(fields, "target_create_time", &parsed.bucket_record.create_time) ||
                deleted_it == fields.end() ||
                !ParseBoolString(deleted_it->second, &parsed.bucket_record.deleted) ||
                !ParseOptionalUint64Field(fields, "target_delete_time", &parsed.bucket_record.delete_time))
            {
                return false;
            }
            *payload = std::move(parsed);
            return true;
        }

        bool ParseDeleteBucketPayload(const FieldMap &fields, DeleteBucketCommandPayload *payload)
        {
            if (payload == nullptr)
            {
                return false;
            }
            DeleteBucketCommandPayload parsed;
            auto if_empty_it = fields.find("target_if_empty");
            if (!ParseRequiredEscapedField(fields, "target_bucket", &parsed.bucket) || if_empty_it == fields.end() ||
                !ParseBoolString(if_empty_it->second, &parsed.if_empty))
            {
                return false;
            }
            *payload = std::move(parsed);
            return true;
        }

        bool ParseCreateObjectPayload(const FieldMap &fields, CreateObjectCommandPayload *payload)
        {
            if (payload == nullptr)
            {
                return false;
            }

            CreateObjectCommandPayload parsed;
            auto state_it = fields.find("target_state");
            if (!ParseRequiredEscapedField(fields, "target_bucket", &parsed.object_record.bucket) ||
                !ParseRequiredEscapedField(fields, "target_object_key", &parsed.object_record.object_key) ||
                !ParseRequiredEscapedField(fields, "target_object_id", &parsed.object_record.object_id) ||
                !ParseRequiredUint64Field(fields, "target_version", &parsed.object_record.version) ||
                !ParseRequiredUint64Field(fields, "target_size", &parsed.object_record.size) ||
                !ParseRequiredEscapedField(fields, "target_etag", &parsed.object_record.etag) ||
                state_it == fields.end() ||
                !StringToObjectState(state_it->second, &parsed.object_record.state) ||
                !ParseRequiredUint64Field(fields, "target_create_time", &parsed.object_record.create_time) ||
                !ParseOptionalUint64Field(fields, "target_commit_time", &parsed.object_record.commit_time) ||
                !ParseOptionalUint64Field(fields, "target_delete_time", &parsed.object_record.delete_time) ||
                !ParseChunkList(fields, &parsed.object_record.chunks))
            {
                return false;
            }
            *payload = std::move(parsed);
            return true;
        }

        bool ParseCommitObjectPayload(const FieldMap &fields, CommitObjectCommandPayload *payload)
        {
            if (payload == nullptr)
            {
                return false;
            }

            CommitObjectCommandPayload parsed;
            if (!ParseRequiredEscapedField(fields, "target_bucket", &parsed.bucket) ||
                !ParseRequiredEscapedField(fields, "target_object_key", &parsed.object_key) ||
                !ParseRequiredEscapedField(fields, "target_object_id", &parsed.object_id) ||
                !ParseRequiredUint64Field(fields, "target_version", &parsed.version) ||
                !ParseRequiredUint64Field(fields, "target_size", &parsed.size) ||
                !ParseRequiredEscapedField(fields, "target_etag", &parsed.etag) ||
                !ParseOptionalUint64Field(fields, "target_commit_time", &parsed.commit_time) ||
                !ParseChunkList(fields, &parsed.chunks))
            {
                return false;
            }
            *payload = std::move(parsed);
            return true;
        }

        bool ParseAbortObjectPayload(const FieldMap &fields, AbortObjectCommandPayload *payload)
        {
            if (payload == nullptr)
            {
                return false;
            }

            AbortObjectCommandPayload parsed;
            if (!ParseRequiredEscapedField(fields, "target_bucket", &parsed.bucket) ||
                !ParseRequiredEscapedField(fields, "target_object_key", &parsed.object_key) ||
                !ParseRequiredEscapedField(fields, "target_object_id", &parsed.object_id) ||
                !ParseRequiredUint64Field(fields, "target_version", &parsed.version))
            {
                return false;
            }
            *payload = std::move(parsed);
            return true;
        }

        bool ParseDeleteObjectPayload(const FieldMap &fields, DeleteObjectCommandPayload *payload)
        {
            if (payload == nullptr)
            {
                return false;
            }

            DeleteObjectCommandPayload parsed;
            if (!ParseRequiredEscapedField(fields, "target_bucket", &parsed.bucket) ||
                !ParseRequiredEscapedField(fields, "target_object_key", &parsed.object_key) ||
                !ParseRequiredEscapedField(fields, "target_object_id", &parsed.object_id) ||
                !ParseRequiredUint64Field(fields, "target_version", &parsed.version) ||
                !ParseOptionalUint64Field(fields, "target_delete_time", &parsed.delete_time))
            {
                return false;
            }
            *payload = std::move(parsed);
            return true;
        }

        void PopulateLegacyCompatibilityFields(MetadataCommand *command)
        {
            if (command == nullptr)
            {
                return;
            }

            switch (command->command_type)
            {
            case MetadataCommandType::kCreateObject:
                command->operation = MetadataOperation::kCreate;
                if (command->create_object.has_value())
                {
                    command->object_key = command->create_object->object_record.object_key;
                }
                break;
            case MetadataCommandType::kCommitObject:
                command->operation = MetadataOperation::kCommit;
                if (command->commit_object.has_value())
                {
                    command->object_key = command->commit_object->object_key;
                }
                break;
            case MetadataCommandType::kDeleteObject:
                command->operation = MetadataOperation::kDelete;
                if (command->delete_object.has_value())
                {
                    command->object_key = command->delete_object->object_key;
                }
                break;
            case MetadataCommandType::kAbortObject:
                if (command->abort_object.has_value())
                {
                    command->object_key = command->abort_object->object_key;
                }
                break;
            case MetadataCommandType::kCreateBucket:
            case MetadataCommandType::kDeleteBucket:
            case MetadataCommandType::kUnknown:
            default:
                break;
            }
        }

        bool ValidateRequestContextConsistency(const MetadataCommand &command, std::string *error)
        {
            if (!command.request_context.has_value())
            {
                return true;
            }

            const RequestRecord &request = *command.request_context;
            if (request.request_id != command.request_id)
            {
                if (error != nullptr)
                {
                    *error = "request_context request_id mismatch";
                }
                return false;
            }

            switch (command.command_type)
            {
            case MetadataCommandType::kCreateBucket:
                if (request.command_type != MetadataRequestType::kCreateBucket)
                {
                    if (error != nullptr)
                    {
                        *error = "request_context command_type mismatch";
                    }
                    return false;
                }
                break;
            case MetadataCommandType::kDeleteBucket:
                if (request.command_type != MetadataRequestType::kDeleteBucket)
                {
                    if (error != nullptr)
                    {
                        *error = "request_context command_type mismatch";
                    }
                    return false;
                }
                break;
            case MetadataCommandType::kCreateObject:
                if (request.command_type != MetadataRequestType::kCreateObject)
                {
                    if (error != nullptr)
                    {
                        *error = "request_context command_type mismatch";
                    }
                    return false;
                }
                break;
            case MetadataCommandType::kCommitObject:
                if (request.command_type != MetadataRequestType::kCommitObject)
                {
                    if (error != nullptr)
                    {
                        *error = "request_context command_type mismatch";
                    }
                    return false;
                }
                break;
            case MetadataCommandType::kAbortObject:
                if (request.command_type != MetadataRequestType::kAbortObject)
                {
                    if (error != nullptr)
                    {
                        *error = "request_context command_type mismatch";
                    }
                    return false;
                }
                break;
            case MetadataCommandType::kDeleteObject:
                if (request.command_type != MetadataRequestType::kDeleteObject)
                {
                    if (error != nullptr)
                    {
                        *error = "request_context command_type mismatch";
                    }
                    return false;
                }
                break;
            case MetadataCommandType::kUnknown:
            default:
                break;
            }

            return true;
        }
    } // namespace

    std::string SerializeMetadataCommand(const MetadataCommand &command)
    {
        std::ostringstream oss;
        oss << kEnvelopeTag << kFieldSeparator;
        AppendField(&oss, "operation", OperationToString(command.operation));
        AppendField(&oss, "request_id", Escape(command.request_id));
        AppendField(&oss, "object_key", Escape(command.object_key));
        AppendField(&oss, "commit_info", Escape(command.commit_info));
        AppendField(&oss, "delete_info", Escape(command.delete_info));

        if (command.command_type != MetadataCommandType::kUnknown)
        {
            AppendV2CommandPayload(&oss, command);
        }

        if (command.record.has_value())
        {
            oss << SerializeRecordForCreate(*command.record);
        }

        return oss.str();
    }

    bool ParseMetadataCommand(const std::string &input, MetadataCommand *out)
    {
        if (out == nullptr)
        {
            return false;
        }

        FieldMap fields;
        if (!ParseFields(input, &fields))
        {
            return false;
        }

        MetadataCommand parsed;
        auto operation_it = fields.find("operation");
        auto request_id_it = fields.find("request_id");
        auto object_key_it = fields.find("object_key");
        auto commit_info_it = fields.find("commit_info");
        auto delete_info_it = fields.find("delete_info");

        if (operation_it == fields.end() || request_id_it == fields.end() || object_key_it == fields.end() ||
            commit_info_it == fields.end() || delete_info_it == fields.end())
        {
            return false;
        }

        if (!StringToOperation(operation_it->second, &parsed.operation))
        {
            return false;
        }
        if (!Unescape(request_id_it->second, &parsed.request_id) ||
            !Unescape(object_key_it->second, &parsed.object_key) ||
            !Unescape(commit_info_it->second, &parsed.commit_info) ||
            !Unescape(delete_info_it->second, &parsed.delete_info))
        {
            return false;
        }

        const auto command_type_it = fields.find("command_type");
        if (command_type_it != fields.end())
        {
            if (!StringToCommandType(command_type_it->second, &parsed.command_type) ||
                parsed.command_type == MetadataCommandType::kUnknown)
            {
                return false;
            }

            if (HasAnyRequestContextField(fields))
            {
                RequestRecord request_context;
                if (!ParseRequestContext(fields, &request_context))
                {
                    return false;
                }
                parsed.request_context = std::move(request_context);
            }

            switch (parsed.command_type)
            {
            case MetadataCommandType::kCreateBucket:
            {
                CreateBucketCommandPayload payload;
                if (!ParseCreateBucketPayload(fields, &payload))
                {
                    return false;
                }
                parsed.create_bucket = std::move(payload);
                break;
            }
            case MetadataCommandType::kDeleteBucket:
            {
                DeleteBucketCommandPayload payload;
                if (!ParseDeleteBucketPayload(fields, &payload))
                {
                    return false;
                }
                parsed.delete_bucket = std::move(payload);
                break;
            }
            case MetadataCommandType::kCreateObject:
            {
                CreateObjectCommandPayload payload;
                if (!ParseCreateObjectPayload(fields, &payload))
                {
                    return false;
                }
                parsed.create_object = std::move(payload);
                break;
            }
            case MetadataCommandType::kCommitObject:
            {
                CommitObjectCommandPayload payload;
                if (!ParseCommitObjectPayload(fields, &payload))
                {
                    return false;
                }
                parsed.commit_object = std::move(payload);
                break;
            }
            case MetadataCommandType::kAbortObject:
            {
                AbortObjectCommandPayload payload;
                if (!ParseAbortObjectPayload(fields, &payload))
                {
                    return false;
                }
                parsed.abort_object = std::move(payload);
                break;
            }
            case MetadataCommandType::kDeleteObject:
            {
                DeleteObjectCommandPayload payload;
                if (!ParseDeleteObjectPayload(fields, &payload))
                {
                    return false;
                }
                parsed.delete_object = std::move(payload);
                break;
            }
            case MetadataCommandType::kUnknown:
            default:
                return false;
            }

            PopulateLegacyCompatibilityFields(&parsed);
        }
        else if (parsed.operation == MetadataOperation::kCreate)
        {
            MetadataRecord record;
            if (!ParseCreateRecord(fields, &record))
            {
                return false;
            }
            record.object_key = parsed.object_key;
            parsed.record = std::move(record);
        }

        *out = std::move(parsed);
        return true;
    }

    bool ValidateMetadataCommand(const MetadataCommand &command, std::string *error)
    {
        if (command.request_id.empty())
        {
            if (error != nullptr)
            {
                *error = "missing request_id";
            }
            return false;
        }

        if (command.command_type != MetadataCommandType::kUnknown)
        {
            if (!ValidateRequestContextConsistency(command, error))
            {
                return false;
            }

            switch (command.command_type)
            {
            case MetadataCommandType::kCreateBucket:
                if (!command.create_bucket.has_value())
                {
                    if (error != nullptr)
                    {
                        *error = "create_bucket command missing payload";
                    }
                    return false;
                }
                if (command.create_bucket->bucket_record.bucket.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "create_bucket command missing bucket";
                    }
                    return false;
                }
                return true;
            case MetadataCommandType::kDeleteBucket:
                if (!command.delete_bucket.has_value())
                {
                    if (error != nullptr)
                    {
                        *error = "delete_bucket command missing payload";
                    }
                    return false;
                }
                if (command.delete_bucket->bucket.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "delete_bucket command missing bucket";
                    }
                    return false;
                }
                return true;
            case MetadataCommandType::kCreateObject:
                if (!command.create_object.has_value())
                {
                    if (error != nullptr)
                    {
                        *error = "create_object command missing payload";
                    }
                    return false;
                }
                if (command.create_object->object_record.bucket.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "create_object command missing bucket";
                    }
                    return false;
                }
                if (command.create_object->object_record.object_key.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "create_object command missing object_key";
                    }
                    return false;
                }
                if (command.create_object->object_record.object_id.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "create_object command missing object_id";
                    }
                    return false;
                }
                if (command.create_object->object_record.state != ObjectState::PENDING)
                {
                    if (error != nullptr)
                    {
                        *error = "create_object command must start as pending";
                    }
                    return false;
                }
                return true;
            case MetadataCommandType::kCommitObject:
                if (!command.commit_object.has_value())
                {
                    if (error != nullptr)
                    {
                        *error = "commit_object command missing payload";
                    }
                    return false;
                }
                if (command.commit_object->bucket.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "commit_object command missing bucket";
                    }
                    return false;
                }
                if (command.commit_object->object_key.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "commit_object command missing object_key";
                    }
                    return false;
                }
                if (command.commit_object->object_id.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "commit_object command missing object_id";
                    }
                    return false;
                }
                return true;
            case MetadataCommandType::kAbortObject:
                if (!command.abort_object.has_value())
                {
                    if (error != nullptr)
                    {
                        *error = "abort_object command missing payload";
                    }
                    return false;
                }
                if (command.abort_object->bucket.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "abort_object command missing bucket";
                    }
                    return false;
                }
                if (command.abort_object->object_key.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "abort_object command missing object_key";
                    }
                    return false;
                }
                if (command.abort_object->object_id.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "abort_object command missing object_id";
                    }
                    return false;
                }
                return true;
            case MetadataCommandType::kDeleteObject:
                if (!command.delete_object.has_value())
                {
                    if (error != nullptr)
                    {
                        *error = "delete_object command missing payload";
                    }
                    return false;
                }
                if (command.delete_object->bucket.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "delete_object command missing bucket";
                    }
                    return false;
                }
                if (command.delete_object->object_key.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "delete_object command missing object_key";
                    }
                    return false;
                }
                if (command.delete_object->object_id.empty())
                {
                    if (error != nullptr)
                    {
                        *error = "delete_object command missing object_id";
                    }
                    return false;
                }
                return true;
            case MetadataCommandType::kUnknown:
            default:
                if (error != nullptr)
                {
                    *error = "unknown metadata command type";
                }
                return false;
            }
        }

        if (command.object_key.empty())
        {
            if (error != nullptr)
            {
                *error = "missing object_key";
            }
            return false;
        }

        switch (command.operation)
        {
        case MetadataOperation::kCreate:
            if (!command.record.has_value())
            {
                if (error != nullptr)
                {
                    *error = "create command missing record payload";
                }
                return false;
            }
            if (command.record->object_key != command.object_key)
            {
                if (error != nullptr)
                {
                    *error = "create record object_key mismatch";
                }
                return false;
            }
            return ValidateCreateRecord(*command.record, error);
        case MetadataOperation::kCommit:
            if (command.record.has_value())
            {
                if (error != nullptr)
                {
                    *error = "commit command must not carry create record payload";
                }
                return false;
            }
            return true;
        case MetadataOperation::kDelete:
            if (command.record.has_value())
            {
                if (error != nullptr)
                {
                    *error = "delete command must not carry create record payload";
                }
                return false;
            }
            return true;
        case MetadataOperation::kUnknown:
        default:
            if (error != nullptr)
            {
                *error = "unknown metadata operation";
            }
            return false;
        }
    }

    std::string ComputeMetadataCommandFingerprint(const MetadataCommand &command)
    {
        std::ostringstream oss;
        if (command.command_type != MetadataCommandType::kUnknown)
        {
            oss << CommandTypeToString(command.command_type) << '|'
                << command.request_id;

            if (command.request_context.has_value())
            {
                const RequestRecord &request = *command.request_context;
                oss << '|'
                    << RequestTypeToString(request.command_type) << '|'
                    << request.bucket << '|'
                    << request.object_key << '|'
                    << request.result_status << '|'
                    << request.applied_index << '|'
                    << request.create_time << '|';
                if (request.finish_time.has_value())
                {
                    oss << *request.finish_time;
                }
            }

            switch (command.command_type)
            {
            case MetadataCommandType::kCreateBucket:
                oss << '|'
                    << command.create_bucket->bucket_record.bucket << '|'
                    << command.create_bucket->bucket_record.create_time << '|'
                    << command.create_bucket->bucket_record.deleted;
                if (command.create_bucket->bucket_record.delete_time.has_value())
                {
                    oss << '|' << *command.create_bucket->bucket_record.delete_time;
                }
                break;
            case MetadataCommandType::kDeleteBucket:
                oss << '|'
                    << command.delete_bucket->bucket << '|'
                    << command.delete_bucket->if_empty;
                break;
            case MetadataCommandType::kCreateObject:
            {
                const ObjectRecord &record = command.create_object->object_record;
                oss << '|'
                    << record.bucket << '|'
                    << record.object_key << '|'
                    << record.object_id << '|'
                    << record.version << '|'
                    << record.size << '|'
                    << record.etag << '|'
                    << ObjectStateToString(record.state) << '|'
                    << record.create_time << '|'
                    << JoinEscaped(std::vector<std::string>{});
                for (const ChunkRef &chunk : record.chunks)
                {
                    oss << '|'
                        << chunk.chunk_id << '@'
                        << chunk.offset << '@'
                        << chunk.size << '@'
                        << JoinEscaped(chunk.replica_nodes) << '@'
                        << chunk.checksum;
                }
                break;
            }
            case MetadataCommandType::kCommitObject:
            {
                const CommitObjectCommandPayload &commit = *command.commit_object;
                oss << '|'
                    << commit.bucket << '|'
                    << commit.object_key << '|'
                    << commit.object_id << '|'
                    << commit.version << '|'
                    << commit.size << '|'
                    << commit.etag << '|'
                    << commit.chunks.size();
                for (const ChunkRef &chunk : commit.chunks)
                {
                    oss << '|'
                        << chunk.chunk_id << '@'
                        << chunk.offset << '@'
                        << chunk.size << '@'
                        << JoinEscaped(chunk.replica_nodes) << '@'
                        << chunk.checksum;
                }
                break;
            }
            case MetadataCommandType::kAbortObject:
                oss << '|'
                    << command.abort_object->bucket << '|'
                    << command.abort_object->object_key << '|'
                    << command.abort_object->object_id << '|'
                    << command.abort_object->version;
                break;
            case MetadataCommandType::kDeleteObject:
                oss << '|'
                    << command.delete_object->bucket << '|'
                    << command.delete_object->object_key << '|'
                    << command.delete_object->object_id << '|'
                    << command.delete_object->version;
                if (command.delete_object->delete_time.has_value())
                {
                    oss << '|' << *command.delete_object->delete_time;
                }
                break;
            case MetadataCommandType::kUnknown:
            default:
                break;
            }

            return oss.str();
        }

        oss << OperationToString(command.operation) << '|'
            << command.request_id << '|'
            << command.object_key << '|'
            << command.commit_info << '|'
            << command.delete_info;

        if (command.record.has_value())
        {
            const MetadataRecord &record = *command.record;
            oss << '|'
                << StateToString(record.state) << '|'
                << record.object_size << '|'
                << record.chunk_size << '|'
                << record.chunk_count << '|'
                << record.checksum << '|'
                << JoinEscaped(record.mock_locations) << '|'
                << Escape(record.payload) << '|'
                << record.create_request_id;
        }

        return oss.str();
    }

} // namespace raftdemo
