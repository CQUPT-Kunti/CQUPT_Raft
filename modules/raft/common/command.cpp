#include "raft/common/command.h"

#include <stdexcept>
#include <sstream>
#include <vector>

namespace raftdemo
{
    namespace
    {
        // 反序列化内部函数，(***,)
        std::vector<std::string> Split(const std::string &str, char delim)
        {
            std::vector<std::string> parts;
            std::stringstream ss(str);
            std::string item;

            while (std::getline(ss, item, delim))
            {
                parts.push_back(item);
            }

            return parts;
        }
    } // namespace

    bool Command::IsValid() const
    {
        switch (type)
        {
        case CommandType::kMetadata:
            return !metadata_payload.empty();
        case CommandType::kUnknown:
        default:
            return false;
        }
    }

    std::string Command::Serialize() const
    {
        switch (type)
        {
        case CommandType::kMetadata:
            return "META|" + std::to_string(metadata_payload.size()) + "|" + metadata_payload;
        case CommandType::kUnknown:
        default:
            return "";
        }
    }

    bool Command::Deserialize(const std::string &data, Command *out)
    {
        if (out == nullptr)
        {
            return false;
        }

        const std::vector<std::string> parts = Split(data, '|');
        if (parts.empty())
        {
            return false;
        }

        out->metadata_payload.clear();

        if (data.rfind("META|", 0) == 0)
        {
            const std::size_t size_pos = data.find('|', 5);
            if (size_pos == std::string::npos)
            {
                return false;
            }

            std::size_t expected_size = 0;
            try
            {
                expected_size = static_cast<std::size_t>(
                    std::stoull(data.substr(5, size_pos - 5)));
            }
            catch (const std::exception &)
            {
                return false;
            }

            const std::string payload = data.substr(size_pos + 1);
            if (payload.size() != expected_size)
            {
                return false;
            }

            out->type = CommandType::kMetadata;
            out->metadata_payload = payload;
            return out->IsValid();
        }

        return false;
    }

} // namespace raftdemo
