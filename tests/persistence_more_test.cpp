#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "raft/command.h"
#include "raft/config.h"
#include "raft/logging.h"
#include "raft/propose.h"
#include "raft/raft_node.h"

namespace raftdemo {
namespace {

using namespace std::chrono_literals;

struct RunningCluster {
  std::vector<std::shared_ptr<RaftNode>> nodes;
  std::vector<std::thread> threads;
};

const std::vector<std::string>& SnapshotKeys() {
  static const std::vector<std::string> keys = {
      "alpha",
      "beta",
      "gamma",
      "persist_marker",
      "recovery_probe",
  };
  return keys;
}

const char* ProposeStatusName(ProposeStatus status) {
  switch (status) {
    case ProposeStatus::kOk:
      return "Ok";
    case ProposeStatus::kNotLeader:
      return "NotLeader";
    case ProposeStatus::kInvalidCommand:
      return "InvalidCommand";
    case ProposeStatus::kNodeStopping:
      return "NodeStopping";
    case ProposeStatus::kReplicationFailed:
      return "ReplicationFailed";
    case ProposeStatus::kCommitFailed:
      return "CommitFailed";
    case ProposeStatus::kApplyFailed:
      return "ApplyFailed";
    case ProposeStatus::kTimeout:
      return "Timeout";
  }
  return "Unknown";
}

bool IsLeaderNode(const std::shared_ptr<RaftNode>& node) {
  return node->Describe().find("role=Leader") != std::string::npos;
}

std::shared_ptr<RaftNode> WaitForLeader(
    const std::vector<std::shared_ptr<RaftNode>>& nodes,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto& node : nodes) {
      if (IsLeaderNode(node)) {
        return node;
      }
    }
    std::this_thread::sleep_for(100ms);
  }
  return nullptr;
}

std::vector<NodeConfig> BuildThreeNodeConfigs(const std::filesystem::path& data_root, int base_port) {
  NodeConfig n1;
  n1.node_id = 1;
  n1.address = "127.0.0.1:" + std::to_string(base_port + 1);
  n1.peers = {
      PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
      PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
  };
  n1.election_timeout_min = std::chrono::milliseconds(300);
  n1.election_timeout_max = std::chrono::milliseconds(600);
  n1.heartbeat_interval = std::chrono::milliseconds(100);
  n1.rpc_deadline = std::chrono::milliseconds(500);
  n1.data_dir = (data_root / "node_1").string();

  NodeConfig n2;
  n2.node_id = 2;
  n2.address = "127.0.0.1:" + std::to_string(base_port + 2);
  n2.peers = {
      PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
      PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
  };
  n2.election_timeout_min = std::chrono::milliseconds(300);
  n2.election_timeout_max = std::chrono::milliseconds(600);
  n2.heartbeat_interval = std::chrono::milliseconds(100);
  n2.rpc_deadline = std::chrono::milliseconds(500);
  n2.data_dir = (data_root / "node_2").string();

  NodeConfig n3;
  n3.node_id = 3;
  n3.address = "127.0.0.1:" + std::to_string(base_port + 3);
  n3.peers = {
      PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
      PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
  };
  n3.election_timeout_min = std::chrono::milliseconds(300);
  n3.election_timeout_max = std::chrono::milliseconds(600);
  n3.heartbeat_interval = std::chrono::milliseconds(100);
  n3.rpc_deadline = std::chrono::milliseconds(500);
  n3.data_dir = (data_root / "node_3").string();

  return {n1, n2, n3};
}

RunningCluster StartCluster(const std::vector<NodeConfig>& configs) {
  RunningCluster cluster;
  cluster.nodes.reserve(configs.size());
  for (const auto& cfg : configs) {
    cluster.nodes.push_back(std::make_shared<RaftNode>(cfg));
  }

  cluster.threads.reserve(cluster.nodes.size());
  for (const auto& node : cluster.nodes) {
    cluster.threads.emplace_back([node]() {
      node->Start();
      node->Wait();
    });
  }
  return cluster;
}

void StopCluster(RunningCluster* cluster) {
  if (cluster == nullptr) {
    return;
  }
  Log("manual-test", "stopping cluster");
  for (auto& node : cluster->nodes) {
    node->Stop();
  }
  for (auto& t : cluster->threads) {
    if (t.joinable()) {
      t.join();
    }
  }
  Log("manual-test", "cluster stopped");
}

void LogClusterSnapshot(const std::vector<std::shared_ptr<RaftNode>>& nodes, const std::string& title) {
  Log("manual-test", "========== ", title, " ==========");
  for (const auto& node : nodes) {
    Log("manual-test", node->Describe());
  }
}

std::string BuildKeyValueSection(const std::shared_ptr<RaftNode>& node) {
  std::ostringstream oss;
  for (const auto& key : SnapshotKeys()) {
    std::string value;
    if (node->DebugGetValue(key, &value)) {
      oss << key << "=" << value << "\n";
    } else {
      oss << key << "=<missing>\n";
    }
  }
  return oss.str();
}

void LogStateFiles(const std::filesystem::path& data_root) {
  Log("manual-test", "state files under: ", data_root.string());
  for (int id = 1; id <= 3; ++id) {
    const auto state_file = data_root / ("node_" + std::to_string(id)) / "raft_state.bin";
    std::error_code ec;
    const bool exists = std::filesystem::exists(state_file, ec);
    const auto size = exists ? std::filesystem::file_size(state_file, ec) : 0;
    Log("manual-test", "node-", id, " file=", state_file.string(), ", exists=",
        exists ? "true" : "false", ", size=", static_cast<unsigned long long>(size));
  }
}

void SaveTextFile(const std::filesystem::path& path, const std::string& content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

void SaveClusterSnapshotFiles(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                             const std::filesystem::path& data_root,
                             const std::string& phase_name) {
  const auto snapshot_dir = data_root / "snapshots" / phase_name;
  std::error_code ec;
  std::filesystem::create_directories(snapshot_dir, ec);

  std::ostringstream manifest;
  manifest << "snapshot_phase=" << phase_name << "\n";
  manifest << "snapshot_root=" << snapshot_dir.string() << "\n";
  manifest << "tracked_keys=";
  for (std::size_t i = 0; i < SnapshotKeys().size(); ++i) {
    if (i > 0) {
      manifest << ",";
    }
    manifest << SnapshotKeys()[i];
  }
  manifest << "\n\n";

  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto& node = nodes[i];
    const auto node_file = snapshot_dir / ("node_" + std::to_string(i + 1) + "_snapshot.txt");

    std::ostringstream oss;
    oss << "phase=" << phase_name << "\n";
    oss << "node_file=" << node_file.string() << "\n";
    oss << "describe=" << node->Describe() << "\n";
    oss << "kv_begin\n";
    oss << BuildKeyValueSection(node);
    oss << "kv_end\n";

    SaveTextFile(node_file, oss.str());
    manifest << "node_" << (i + 1) << "=" << node_file.string() << "\n";
  }

  SaveTextFile(snapshot_dir / "manifest.txt", manifest.str());
  Log("manual-test", "saved manual snapshot files to: ", snapshot_dir.string());
}

bool ProposeSet(const std::shared_ptr<RaftNode>& leader, const std::string& key,
                const std::string& value) {
  Command cmd;
  cmd.type = CommandType::kSet;
  cmd.key = key;
  cmd.value = value;
  const ProposeResult result = leader->Propose(cmd);
  Log("manual-test", "propose SET ", key, "=", value,
      ", status=", ProposeStatusName(result.status),
      ", leader_id=", result.leader_id,
      ", term=", result.term,
      ", log_index=", result.log_index,
      ", message=", result.message);
  return result.Ok();
}

bool WaitUntilValue(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                    const std::string& key,
                    const std::string& expected_value,
                    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    bool all_ok = true;
    for (const auto& node : nodes) {
      std::string actual;
      if (!node->DebugGetValue(key, &actual) || actual != expected_value) {
        all_ok = false;
        break;
      }
    }
    if (all_ok) {
      return true;
    }
    std::this_thread::sleep_for(100ms);
  }
  return false;
}

void WriteMarker(const std::filesystem::path& marker_path, const std::string& content) {
  std::ofstream out(marker_path, std::ios::binary | std::ios::trunc);
  out << content;
}

bool FileExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

int RunPhase1(const std::filesystem::path& data_root) {
  Log("manual-test", "phase-1: clean start, write some keys, save manual snapshot, then exit");
  std::error_code ec;
  std::filesystem::remove_all(data_root, ec);
  std::filesystem::create_directories(data_root, ec);

  const auto configs = BuildThreeNodeConfigs(data_root, 53250);
  auto cluster = StartCluster(configs);

  auto leader = WaitForLeader(cluster.nodes, 6s);
  if (!leader) {
    Log("manual-test", "phase-1 failed: leader election timeout");
    StopCluster(&cluster);
    return 1;
  }

  LogClusterSnapshot(cluster.nodes, "phase-1 snapshot after leader election");

  bool ok = true;
  ok = ok && ProposeSet(leader, "alpha", "1");
  ok = ok && ProposeSet(leader, "beta", "2");
  ok = ok && ProposeSet(leader, "gamma", "3");
  ok = ok && ProposeSet(leader, "persist_marker", "phase1");

  if (!ok) {
    Log("manual-test", "phase-1 failed: propose error");
    StopCluster(&cluster);
    return 1;
  }

  const bool replicated =
      WaitUntilValue(cluster.nodes, "alpha", "1", 8s) &&
      WaitUntilValue(cluster.nodes, "beta", "2", 8s) &&
      WaitUntilValue(cluster.nodes, "gamma", "3", 8s) &&
      WaitUntilValue(cluster.nodes, "persist_marker", "phase1", 8s);

  LogClusterSnapshot(cluster.nodes, "phase-1 snapshot before stop");
  SaveClusterSnapshotFiles(cluster.nodes, data_root, "phase1_before_stop");

  StopCluster(&cluster);
  LogStateFiles(data_root);

  if (!replicated) {
    Log("manual-test", "phase-1 failed: replicated values not visible on all nodes before stop");
    return 1;
  }

  WriteMarker(data_root / "phase1.done", "run the same executable again to verify recovery\n");
  Log("manual-test", "phase-1 complete. Re-run the same executable to start phase-2 verification.");
  Log("manual-test", "manual snapshot saved under: ", (data_root / "snapshots" / "phase1_before_stop").string());
  Log("manual-test", "data root kept at: ", data_root.string());
  return 0;
}

int RunPhase2(const std::filesystem::path& data_root) {
  Log("manual-test", "phase-2: restart from existing data, verify restored logs, save another snapshot, then end");
  LogStateFiles(data_root);

  const auto configs = BuildThreeNodeConfigs(data_root, 53250);
  auto cluster = StartCluster(configs);

  auto leader = WaitForLeader(cluster.nodes, 6s);
  if (!leader) {
    Log("manual-test", "phase-2 failed: leader election timeout");
    StopCluster(&cluster);
    return 1;
  }

  LogClusterSnapshot(cluster.nodes, "phase-2 snapshot right after restart");
  SaveClusterSnapshotFiles(cluster.nodes, data_root, "phase2_after_restart_before_probe");

  Log("manual-test", "phase-2: sending one probe write to advance commit/apply after restart");
  if (!ProposeSet(leader, "recovery_probe", "phase2")) {
    StopCluster(&cluster);
    return 1;
  }

  const bool restored =
      WaitUntilValue(cluster.nodes, "alpha", "1", 8s) &&
      WaitUntilValue(cluster.nodes, "beta", "2", 8s) &&
      WaitUntilValue(cluster.nodes, "gamma", "3", 8s) &&
      WaitUntilValue(cluster.nodes, "persist_marker", "phase1", 8s) &&
      WaitUntilValue(cluster.nodes, "recovery_probe", "phase2", 8s);

  LogClusterSnapshot(cluster.nodes, "phase-2 snapshot after recovery probe");
  SaveClusterSnapshotFiles(cluster.nodes, data_root, "phase2_after_recovery_probe");

  StopCluster(&cluster);
  LogStateFiles(data_root);

  if (!restored) {
    Log("manual-test", "phase-2 failed: persisted values were not restored to all nodes");
    return 1;
  }

  WriteMarker(data_root / "phase2.ok", "recovery verified\n");
  Log("manual-test", "phase-2 success: persistence and restart recovery verified.");
  Log("manual-test", "manual snapshot after restart is under: ",
      (data_root / "snapshots" / "phase2_after_recovery_probe").string());
  Log("manual-test", "you can inspect the files under: ", data_root.string());
  return 0;
}

}  // namespace
}  // namespace raftdemo

int main(int argc, char** argv) {
  using namespace raftdemo;

  std::filesystem::path data_root;
  if (argc >= 2 && argv[1] != nullptr && std::string(argv[1]).size() > 0) {
    data_root = std::filesystem::path(argv[1]);
  } else {
    data_root = std::filesystem::current_path() / "raft_data" / "manual_restart_demo";
  }

  const auto marker = data_root / "phase1.done";
  Log("manual-test", "executable mode = auto two-phase persistence demo with manual snapshot export");
  Log("manual-test", "data root = ", data_root.string());
  Log("manual-test", "marker file = ", marker.string());

  if (!FileExists(marker)) {
    return RunPhase1(data_root);
  }
  return RunPhase2(data_root);
}
