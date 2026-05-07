#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "raft/config.h"
#include "raft/logging.h"
#include "raft/raft_node.h"

namespace raftdemo
{
    namespace
    {

        std::atomic<bool> g_stop{false};

        void HandleSignal(int)
        {
            g_stop.store(true);
        }

        std::string Trim(std::string s)
        {
            auto not_space = [](unsigned char ch)
            { return !std::isspace(ch); };
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
            s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
            return s;
        }

        bool ParseBool(const std::string &value)
        {
            const std::string v = Trim(value);
            return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES" || v == "on";
        }

        std::map<std::string, std::string> LoadKeyValueConfig(const std::filesystem::path &path)
        {
            std::ifstream in(path);
            if (!in)
            {
                throw std::runtime_error("failed to open config file: " + path.string());
            }

            std::map<std::string, std::string> kv;
            std::string line;
            while (std::getline(in, line))
            {
                const auto comment_pos = line.find('#');
                if (comment_pos != std::string::npos)
                {
                    line = line.substr(0, comment_pos);
                }

                line = Trim(line);
                if (line.empty())
                {
                    continue;
                }

                const auto eq = line.find('=');
                if (eq == std::string::npos)
                {
                    throw std::runtime_error("bad config line, expected key=value: " + line);
                }

                const std::string key = Trim(line.substr(0, eq));
                const std::string value = Trim(line.substr(eq + 1));
                if (key.empty())
                {
                    throw std::runtime_error("bad config line, empty key");
                }
                kv[key] = value;
            }
            return kv;
        }

        int GetInt(const std::map<std::string, std::string> &kv,
                   const std::string &key,
                   int default_value)
        {
            auto it = kv.find(key);
            if (it == kv.end())
            {
                return default_value;
            }
            return std::stoi(it->second);
        }

        std::string GetString(const std::map<std::string, std::string> &kv,
                              const std::string &key,
                              const std::string &default_value)
        {
            auto it = kv.find(key);
            if (it == kv.end())
            {
                return default_value;
            }
            return it->second;
        }

        bool GetBool(const std::map<std::string, std::string> &kv,
                     const std::string &key,
                     bool default_value)
        {
            auto it = kv.find(key);
            if (it == kv.end())
            {
                return default_value;
            }
            return ParseBool(it->second);
        }

        std::map<int, std::string> LoadMembers(const std::map<std::string, std::string> &kv)
        {
            std::map<int, std::string> members;
            constexpr const char *prefix = "node.";

            for (const auto &[key, value] : kv)
            {
                if (key.rfind(prefix, 0) != 0)
                {
                    continue;
                }

                const std::string id_part = key.substr(std::string(prefix).size());
                if (id_part.empty())
                {
                    continue;
                }

                const int node_id = std::stoi(id_part);
                if (node_id <= 0)
                {
                    throw std::runtime_error("node id must be positive: " + key);
                }

                members[node_id] = value;
            }

            if (members.empty())
            {
                throw std::runtime_error("config must contain at least one node.<id>=<host:port> entry");
            }

            return members;
        }

        NodeConfig BuildNodeConfig(const std::map<std::string, std::string> &kv,
                                   int override_node_id,
                                   const std::filesystem::path &config_path)
        {
            const auto members = LoadMembers(kv);
            const int node_id = override_node_id > 0 ? override_node_id : GetInt(kv, "node_id", 1);

            auto self = members.find(node_id);
            if (self == members.end())
            {
                std::ostringstream oss;
                oss << "node_id=" << node_id << " is not present in config members";
                throw std::runtime_error(oss.str());
            }

            NodeConfig config;
            config.node_id = node_id;
            config.address = self->second;

            for (const auto &[peer_id, address] : members)
            {
                if (peer_id == node_id)
                {
                    continue;
                }
                config.peers.push_back(PeerConfig{peer_id, address});
            }

            config.election_timeout_min =
                std::chrono::milliseconds(GetInt(kv, "election_timeout_min_ms", 800));
            config.election_timeout_max =
                std::chrono::milliseconds(GetInt(kv, "election_timeout_max_ms", 1600));
            config.heartbeat_interval =
                std::chrono::milliseconds(GetInt(kv, "heartbeat_interval_ms", 150));
            config.rpc_deadline =
                std::chrono::milliseconds(GetInt(kv, "rpc_deadline_ms", 500));

            config.kv_limits.max_key_bytes =
                static_cast<std::size_t>(GetInt(kv, "kv_max_key_bytes", 256));
            config.kv_limits.max_value_bytes =
                static_cast<std::size_t>(GetInt(kv, "kv_max_value_bytes", 64 * 1024));
            config.kv_limits.max_command_bytes =
                static_cast<std::size_t>(GetInt(kv, "kv_max_command_bytes", 1024 * 1024));

            const std::filesystem::path config_dir =
                config_path.has_parent_path() ? config_path.parent_path() : std::filesystem::current_path();

            const std::filesystem::path data_root =
                GetString(kv, "data_root", (config_dir / "raft_data").string());

            config.data_dir = (data_root / ("node_" + std::to_string(node_id))).string();
            return config;
        }

        snapshotConfig BuildSnapshotConfig(const std::map<std::string, std::string> &kv,
                                           int node_id,
                                           const std::filesystem::path &config_path)
        {
            const std::filesystem::path config_dir =
                config_path.has_parent_path() ? config_path.parent_path() : std::filesystem::current_path();

            const std::filesystem::path snapshot_root =
                GetString(kv, "snapshot_root", (config_dir / "raft_snapshots").string());

            snapshotConfig config;
            config.enabled = GetBool(kv, "snapshot_enabled", true);
            config.snapshot_dir = (snapshot_root / ("node_" + std::to_string(node_id))).string();
            config.log_threshold = static_cast<std::uint64_t>(GetInt(kv, "snapshot_log_threshold", 30));
            config.snapshot_interval =
                std::chrono::milliseconds(GetInt(kv, "snapshot_interval_ms", 600000));
            config.max_snapshot_count =
                static_cast<std::size_t>(GetInt(kv, "snapshot_max_count", 5));
            config.load_on_startup = GetBool(kv, "snapshot_load_on_startup", true);
            config.file_prefix = GetString(kv, "snapshot_file_prefix", "snapshot");
            return config;
        }

        void PrintConfigSummary(const std::filesystem::path &config_path,
                                const NodeConfig &node_config,
                                const snapshotConfig &snapshot_config)
        {
            Log("raft-node-main", "config=", config_path.string());
            Log("raft-node-main", "node_id=", node_config.node_id);
            Log("raft-node-main", "address=", node_config.address);
            Log("raft-node-main", "known_peers=", node_config.peers.size(),
                " (addresses only; this process starts node_id=", node_config.node_id, ")");

            for (const auto &peer : node_config.peers)
            {
                Log("raft-node-main", "remote peer.", peer.node_id, "=", peer.address);
            }

            Log("raft-node-main", "cluster_size=", node_config.peers.size() + 1,
                ", quorum=", ((node_config.peers.size() + 1) / 2 + 1));

            if (node_config.peers.empty())
            {
                Log("raft-node-main",
                    "single-node mode: this node can elect itself as leader; "
                    "use node.2/node.3 entries for a 3-node cluster");
            }

            Log("raft-node-main", "data_dir=", node_config.data_dir);
            Log("raft-node-main", "kv_max_key_bytes=", node_config.kv_limits.max_key_bytes,
                ", kv_max_value_bytes=", node_config.kv_limits.max_value_bytes,
                ", kv_max_command_bytes=", node_config.kv_limits.max_command_bytes);
            Log("raft-node-main", "snapshot_enabled=", snapshot_config.enabled ? "true" : "false");
            Log("raft-node-main", "snapshot_dir=", snapshot_config.snapshot_dir);
        }

    } // namespace
} // namespace raftdemo

int main(int argc, char **argv)
{
    using namespace raftdemo;

    try
    {
        std::signal(SIGINT, HandleSignal);
        std::signal(SIGTERM, HandleSignal);

        std::filesystem::path config_path = "config.txt";
        int node_id_override = -1;

        if (argc >= 2)
        {
            config_path = argv[1];
        }

        if (argc >= 3)
        {
            node_id_override = std::stoi(argv[2]);
        }

        const auto kv = LoadKeyValueConfig(config_path);
        const std::string log_level_text = GetString(kv, "log_level", "info");
        LogLevel log_level = LogLevel::kInfo;
        if (!TryParseLogLevel(log_level_text, &log_level))
        {
            throw std::runtime_error("invalid log_level: " + log_level_text);
        }
        SetGlobalLogLevel(log_level);
        NodeConfig node_config = BuildNodeConfig(kv, node_id_override, config_path);
        snapshotConfig snapshot_config =
            BuildSnapshotConfig(kv, node_config.node_id, config_path);

        PrintConfigSummary(config_path, node_config, snapshot_config);

        auto node = std::make_shared<RaftNode>(node_config, snapshot_config);
        node->Start();

        const auto print_interval =
            std::chrono::milliseconds(GetInt(kv, "describe_interval_ms", 5000));

        while (!g_stop.load())
        {
            std::this_thread::sleep_for(print_interval);
            Log("raft-node-main", node->Describe());
        }

        Log("raft-node-main", "stopping node");
        node->Stop();
        Log("raft-node-main", "node stopped");
        return 0;
    }
    catch (const std::exception &ex)
    {
        Log("raft-node-main", "fatal: ", ex.what());
        return 1;
    }
}
