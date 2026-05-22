#include "raft/state_machine/state_machine.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

namespace raftdemo
{
    namespace
    {
        constexpr const char *kInternalNoOpCommand = "__raft_internal_noop__";
        constexpr std::uint32_t kSnapshotMagic = 0x4B565331U; // "KVS1"
        constexpr std::uint32_t kSnapshotVersion = 1U;

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
    }

    ApplyResult KvStateMachine::Apply(std::uint64_t /*index*/,
                                      std::uint64_t /*term*/,
                                      const std::string &command_data)
    {
        if (command_data == kInternalNoOpCommand)
        {
            return {true, "ok"};
        }

        Command cmd;
        if (!Command::Deserialize(command_data, &cmd))
        {
            return {false, "failed to deserialize command"};
        }

        if (!cmd.IsValid())
        {
            return {false, "invalid command"};
        }

        std::lock_guard<std::mutex> lk(mu_);

        switch (cmd.type)
        {
        case CommandType::kSet:
            kv_[cmd.key] = cmd.value;
            return {true, "ok"};
        case CommandType::kDelete:
            kv_.erase(cmd.key);
            return {true, "ok"};
        case CommandType::kUnknown:
        default:
            return {false, "unknown command type"};
        }
    }

    SnapshotResult KvStateMachine::SaveSnapshot(const std::string &file_path) const
    {
        if (file_path.empty())
        {
            return {SnapshotStatus::kInvalidArgument, "snapshot file path is empty"};
        }

        std::vector<std::pair<std::string, std::string>> items;
        {
            std::lock_guard<std::mutex> lk(mu_);
            items.assign(kv_.begin(), kv_.end());
        }

        std::sort(items.begin(), items.end(),
                  [](const auto &a, const auto &b)
                  {
                      return a.first < b.first;
                  });

        std::error_code ec;
        const std::filesystem::path snapshot_path(file_path);
        const std::filesystem::path parent = snapshot_path.parent_path();
        const std::filesystem::path temp_path = snapshot_path.parent_path() /
                                                (snapshot_path.filename().string() + ".tmp");

        if (!parent.empty())
        {
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                return {SnapshotStatus::kIoError,
                        "create snapshot directory failed: " + ec.message()};
            }
        }

        {
            std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                return {SnapshotStatus::kIoError,
                        "open temp snapshot file failed: " + temp_path.string()};
            }

            const std::uint32_t magic = kSnapshotMagic;
            const std::uint32_t version = kSnapshotVersion;
            const std::uint64_t kv_count = static_cast<std::uint64_t>(items.size());

            if (!WritePod(out, magic) ||
                !WritePod(out, version) ||
                !WritePod(out, kv_count))
            {
                return {SnapshotStatus::kIoError, "write snapshot header failed"};
            }

            for (const auto &item : items)
            {
                if (!WriteString(out, item.first) || !WriteString(out, item.second))
                {
                    return {SnapshotStatus::kIoError, "write snapshot kv entry failed"};
                }
            }

            out.flush();
            if (!out)
            {
                return {SnapshotStatus::kIoError, "flush snapshot file failed"};
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
                        "remove old snapshot file failed: " + ec.message()};
            }
        }

        ec.clear();
        std::filesystem::rename(temp_path, snapshot_path, ec);
        if (ec)
        {
            return {SnapshotStatus::kIoError,
                    "rename snapshot file failed: " + ec.message()};
        }

        return {SnapshotStatus::kOk, "ok"};
    }

    SnapshotResult KvStateMachine::LoadSnapshot(const std::string &file_path)
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
            return {SnapshotStatus::kIoError, "failed to open snapshot file: " + file_path};
        }

        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::uint64_t kv_count = 0;

        if (!ReadPod(in, &magic) || !ReadPod(in, &version) || !ReadPod(in, &kv_count))
        {
            return {SnapshotStatus::kCorruptedData, "failed to read snapshot header"};
        }

        if (magic != kSnapshotMagic)
        {
            return {SnapshotStatus::kCorruptedData, "invalid snapshot magic"};
        }
        if (version != kSnapshotVersion)
        {
            return {SnapshotStatus::kVersionMismatch, "unsupported snapshot version"};
        }

        std::unordered_map<std::string, std::string> new_kv;
        for (std::uint64_t i = 0; i < kv_count; ++i)
        {
            std::string key;
            std::string value;
            if (!ReadString(in, &key) || !ReadString(in, &value))
            {
                return {SnapshotStatus::kCorruptedData, "failed to read snapshot kv entry"};
            }
            new_kv[std::move(key)] = std::move(value);
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            kv_ = std::move(new_kv);
        }

        return {SnapshotStatus::kOk, "ok"};
    }

    bool KvStateMachine::Get(const std::string &key, std::string *value) const
    {
        if (value == nullptr)
        {
            return false;
        }
        std::lock_guard<std::mutex> lk(mu_);
        auto it = kv_.find(key);
        if (it == kv_.end())
        {
            return false;
        }
        *value = it->second;
        return true;
    }

    std::string KvStateMachine::DebugString() const
    {
        std::lock_guard<std::mutex> lk(mu_);

        std::vector<std::pair<std::string, std::string>> items(kv_.begin(), kv_.end());
        std::sort(items.begin(), items.end(),
                  [](const auto &a, const auto &b)
                  { return a.first < b.first; });

        std::ostringstream oss;
        oss << "{";
        for (std::size_t i = 0; i < items.size(); ++i)
        {
            if (i > 0)
            {
                oss << ", ";
            }
            oss << items[i].first << "=" << items[i].second;
        }
        oss << "}";
        return oss.str();
    }

} // namespace raftdemo
