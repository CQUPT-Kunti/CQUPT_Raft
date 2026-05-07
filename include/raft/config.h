#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace raftdemo
{

  struct PeerConfig
  {
    int node_id;
    std::string address;
  };

  struct KvRequestLimits
  {
    std::size_t max_key_bytes{256};
    std::size_t max_value_bytes{64 * 1024};
    std::size_t max_command_bytes{1024 * 1024};
  };

  struct NodeConfig
  {
    int node_id;
    std::string address;
    std::vector<PeerConfig> peers;

    std::chrono::milliseconds election_timeout_min{300};
    std::chrono::milliseconds election_timeout_max{600};
    std::chrono::milliseconds heartbeat_interval{80};
    std::chrono::milliseconds rpc_deadline{250};

    // Raft 硬状态和日志持久化目录。
    std::string data_dir;

    // 业务 KV 请求限制。
    KvRequestLimits kv_limits;
  };

  struct snapshotConfig
  {
    bool enabled{true};

    // 快照保存目录。
    std::string snapshot_dir;

    // 自上次快照后，新增多少条“已应用日志”触发一次快照。
    std::uint64_t log_threshold{30};

    // 定时快照间隔。
    std::chrono::milliseconds snapshot_interval{std::chrono::minutes(10)};

    // 最多保留多少个快照（包含最新那个）。
    std::size_t max_snapshot_count{5};

    // 启动时是否自动加载最新快照。
    bool load_on_startup{true};

    // 快照文件名前缀。
    std::string file_prefix{"snapshot"};
  };

} // namespace raftdemo
