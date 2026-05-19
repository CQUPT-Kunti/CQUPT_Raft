#include "raft/common/metadata_command.h"

#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
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

        if (parsed.operation == MetadataOperation::kCreate)
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
