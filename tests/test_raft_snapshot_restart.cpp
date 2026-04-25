#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "raft/command.h"
#include "raft/config.h"
#include "raft/propose.h"
#include "raft/raft_node.h"

namespace raftdemo {
namespace {

using Clock = std::chrono::steady_clock;

std::string ProposeStatusName(ProposeStatus status) {
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

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

bool IsLeaderNode(const std::shared_ptr<RaftNode>& node) {
  return node && Contains(node->Describe(), "role=Leader");
}

Command SetCommand(const std::string& key, const std::string& value) {
  Command command;
  command.type = CommandType::kSet;
  command.key = key;
  command.value = value;
  return command;
}

std::uint64_t NowForPath() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

int PickBasePort(const std::string& test_name) {
  const int name_offset = static_cast<int>(std::hash<std::string>{}(test_name) % 2000);

  if (const char* env = std::getenv("RAFT_TEST_BASE_PORT")) {
    try {
      return std::stoi(env) + name_offset;
    } catch (...) {
      // Fall through to a generated port range.
    }
  }

  std::random_device rd;
  const int jitter = static_cast<int>(rd() % 10000);
  return 40000 + name_offset + jitter;
}

std::filesystem::path MakeTestRoot(const std::string& test_name) {
  std::random_device rd;
  std::string safe_name = test_name;
  for (char& ch : safe_name) {
    if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ') {
      ch = '_';
    }
  }

  const std::string name = "raft_snapshot_restart_" + safe_name + "_" +
                           std::to_string(NowForPath()) + "_" +
                           std::to_string(rd());
  return std::filesystem::temp_directory_path() / name;
}

std::optional<std::uint64_t> ExtractUintField(const std::string& describe,
                                              const std::string& field_name) {
  const std::string prefix = field_name + "=";
  const std::size_t begin = describe.find(prefix);
  if (begin == std::string::npos) {
    return std::nullopt;
  }

  std::size_t pos = begin + prefix.size();
  std::size_t end = pos;
  while (end < describe.size() && describe[end] >= '0' && describe[end] <= '9') {
    ++end;
  }

  if (end == pos) {
    return std::nullopt;
  }

  try {
    return static_cast<std::uint64_t>(std::stoull(describe.substr(pos, end - pos)));
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<NodeConfig> BuildThreeNodeConfigs(const std::filesystem::path& data_root,
                                              int base_port) {
  NodeConfig n1;
  n1.node_id = 1;
  n1.address = "127.0.0.1:" + std::to_string(base_port + 1);
  n1.peers = {
      PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
      PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
  };
  n1.election_timeout_min = std::chrono::milliseconds(250);
  n1.election_timeout_max = std::chrono::milliseconds(500);
  n1.heartbeat_interval = std::chrono::milliseconds(80);
  n1.rpc_deadline = std::chrono::milliseconds(300);
  n1.data_dir = (data_root / "node_1").string();

  NodeConfig n2;
  n2.node_id = 2;
  n2.address = "127.0.0.1:" + std::to_string(base_port + 2);
  n2.peers = {
      PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
      PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
  };
  n2.election_timeout_min = std::chrono::milliseconds(250);
  n2.election_timeout_max = std::chrono::milliseconds(500);
  n2.heartbeat_interval = std::chrono::milliseconds(80);
  n2.rpc_deadline = std::chrono::milliseconds(300);
  n2.data_dir = (data_root / "node_2").string();

  NodeConfig n3;
  n3.node_id = 3;
  n3.address = "127.0.0.1:" + std::to_string(base_port + 3);
  n3.peers = {
      PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
      PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
  };
  n3.election_timeout_min = std::chrono::milliseconds(250);
  n3.election_timeout_max = std::chrono::milliseconds(500);
  n3.heartbeat_interval = std::chrono::milliseconds(80);
  n3.rpc_deadline = std::chrono::milliseconds(300);
  n3.data_dir = (data_root / "node_3").string();

  return {n1, n2, n3};
}

std::vector<snapshotConfig> BuildThreeSnapshotConfigs(
    const std::filesystem::path& snapshot_root,
    bool enabled,
    std::uint64_t log_threshold) {
  snapshotConfig s1;
  s1.enabled = enabled;
  s1.snapshot_dir = (snapshot_root / "node_1").string();
  s1.log_threshold = log_threshold;
  s1.snapshot_interval = std::chrono::minutes(10);
  s1.max_snapshot_count = 3;
  s1.load_on_startup = true;
  s1.file_prefix = "snapshot";

  snapshotConfig s2 = s1;
  s2.snapshot_dir = (snapshot_root / "node_2").string();

  snapshotConfig s3 = s1;
  s3.snapshot_dir = (snapshot_root / "node_3").string();

  return {s1, s2, s3};
}

class TestCluster {
 public:
  TestCluster(std::vector<NodeConfig> configs,
              std::vector<snapshotConfig> snapshot_configs)
      : configs_(std::move(configs)), snapshot_configs_(std::move(snapshot_configs)) {}

  ~TestCluster() { StopAll(); }

  void Start() {
    StopAll();
    nodes_.clear();
    wait_threads_.clear();

    for (std::size_t i = 0; i < configs_.size(); ++i) {
      nodes_.push_back(std::make_shared<RaftNode>(configs_[i], snapshot_configs_[i]));
    }
    wait_threads_.resize(nodes_.size());

    for (const auto& node : nodes_) {
      node->Start();
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      const auto node = nodes_[i];
      wait_threads_[i] = std::thread([node]() { node->Wait(); });
    }
  }

  void StopAll() {
    for (const auto& node : nodes_) {
      if (node) {
        node->Stop();
      }
    }
    for (auto& thread : wait_threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    wait_threads_.clear();
  }

  void StopNode(std::size_t index) {
    if (index >= nodes_.size() || !nodes_[index]) {
      return;
    }
    nodes_[index]->Stop();
    if (index < wait_threads_.size() && wait_threads_[index].joinable()) {
      wait_threads_[index].join();
    }
  }

  void RestartNode(std::size_t index) {
    ASSERT_LT(index, configs_.size());
    StopNode(index);

    if (nodes_.size() < configs_.size()) {
      nodes_.resize(configs_.size());
    }
    if (wait_threads_.size() < configs_.size()) {
      wait_threads_.resize(configs_.size());
    }

    nodes_[index] = std::make_shared<RaftNode>(configs_[index], snapshot_configs_[index]);
    nodes_[index]->Start();
    const auto node = nodes_[index];
    wait_threads_[index] = std::thread([node]() { node->Wait(); });
  }

  const std::vector<std::shared_ptr<RaftNode>>& Nodes() const { return nodes_; }

 private:
  std::vector<NodeConfig> configs_;
  std::vector<snapshotConfig> snapshot_configs_;
  std::vector<std::shared_ptr<RaftNode>> nodes_;
  std::vector<std::thread> wait_threads_;
};

bool IsExcluded(std::size_t index, const std::vector<std::size_t>& excluded) {
  for (std::size_t excluded_index : excluded) {
    if (index == excluded_index) {
      return true;
    }
  }
  return false;
}

std::shared_ptr<RaftNode> WaitForSingleLeader(
    const std::vector<std::shared_ptr<RaftNode>>& nodes,
    std::chrono::milliseconds timeout,
    const std::vector<std::size_t>& excluded = {}) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    std::shared_ptr<RaftNode> leader;
    int leader_count = 0;

    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (IsExcluded(i, excluded) || !nodes[i]) {
        continue;
      }
      if (IsLeaderNode(nodes[i])) {
        leader = nodes[i];
        ++leader_count;
      }
    }

    if (leader_count == 1) {
      return leader;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return nullptr;
}

std::size_t FindNodeIndex(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                          const std::shared_ptr<RaftNode>& target) {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i] == target) {
      return i;
    }
  }
  return nodes.size();
}

std::size_t PickFollowerIndex(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                              const std::shared_ptr<RaftNode>& leader) {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i] && nodes[i] != leader) {
      return i;
    }
  }
  return nodes.size();
}

bool WaitForValueOnNode(const std::shared_ptr<RaftNode>& node,
                        const std::string& key,
                        const std::string& expected_value,
                        std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (node) {
      std::string value;
      if (node->DebugGetValue(key, &value) && value == expected_value) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

bool WaitForValueOnAll(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                       const std::string& key,
                       const std::string& expected_value,
                       std::chrono::milliseconds timeout,
                       const std::vector<std::size_t>& excluded = {}) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    bool all_match = true;

    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (IsExcluded(i, excluded) || !nodes[i]) {
        continue;
      }

      std::string value;
      if (!nodes[i]->DebugGetValue(key, &value) || value != expected_value) {
        all_match = false;
        break;
      }
    }

    if (all_match) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

bool WaitForNodeFieldAtLeast(const std::shared_ptr<RaftNode>& node,
                             const std::string& field_name,
                             std::uint64_t minimum,
                             std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (node) {
      const auto value = ExtractUintField(node->Describe(), field_name);
      if (value.has_value() && *value >= minimum) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool ProposeWithRetry(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                      const Command& command,
                      std::chrono::milliseconds timeout,
                      ProposeResult* final_result,
                      const std::vector<std::size_t>& excluded = {}) {
  const auto deadline = Clock::now() + timeout;
  ProposeResult last_result;

  while (Clock::now() < deadline) {
    auto leader = WaitForSingleLeader(nodes, std::chrono::milliseconds(1500), excluded);
    if (!leader) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    last_result = leader->Propose(command);
    if (last_result.Ok()) {
      if (final_result != nullptr) {
        *final_result = last_result;
      }
      return true;
    }

    if (last_result.status == ProposeStatus::kInvalidCommand ||
        last_result.status == ProposeStatus::kApplyFailed ||
        last_result.status == ProposeStatus::kCommitFailed) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (final_result != nullptr) {
    *final_result = last_result;
  }
  return false;
}

void WriteManyValues(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                     const std::string& prefix,
                     int count,
                     const std::vector<std::size_t>& excluded = {}) {
  ProposeResult result;
  for (int i = 0; i < count; ++i) {
    SCOPED_TRACE(prefix + " write " + std::to_string(i));
    ASSERT_TRUE(ProposeWithRetry(nodes,
                                 SetCommand(prefix + "_" + std::to_string(i),
                                            "value_" + std::to_string(i)),
                                 std::chrono::seconds(10),
                                 &result,
                                 excluded))
        << "write failed, status=" << ProposeStatusName(result.status)
        << ", message=" << result.message;
  }
}

class RaftSnapshotRestartTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    test_name_ = std::string(test_info->test_suite_name()) + "." + test_info->name();

    root_ = MakeTestRoot(test_name_);
    data_root_ = root_ / "raft_data";
    snapshot_root_ = root_ / "raft_snapshots";
    base_port_ = PickBasePort(test_name_);

    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
    std::filesystem::create_directories(data_root_, ec);
    ASSERT_FALSE(ec) << "failed to create data root: " << ec.message();

    std::filesystem::create_directories(snapshot_root_, ec);
    ASSERT_FALSE(ec) << "failed to create snapshot root: " << ec.message();

    RecordProperty("test_root", root_.string());
    RecordProperty("base_port", std::to_string(base_port_));
  }

  void TearDown() override {
    std::error_code ec;
    if (!HasFailure()) {
      std::filesystem::remove_all(root_, ec);
    } else {
      std::cout << "preserved test root: " << root_.string() << "\n";
    }
  }

  TestCluster MakeCluster(const std::string& case_name,
                          bool snapshot_enabled,
                          std::uint64_t snapshot_log_threshold) const {
    return TestCluster(BuildThreeNodeConfigs(data_root_ / case_name, base_port_),
                       BuildThreeSnapshotConfigs(snapshot_root_ / case_name,
                                                 snapshot_enabled,
                                                 snapshot_log_threshold));
  }

  std::string test_name_;
  std::filesystem::path root_;
  std::filesystem::path data_root_;
  std::filesystem::path snapshot_root_;
  int base_port_{0};
};

TEST_F(RaftSnapshotRestartTest, FollowerKeepsStateAfterInstallSnapshotAndRestart) {
  auto cluster = MakeCluster("follower_snapshot_restart", true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  const std::size_t stopped_follower = PickFollowerIndex(cluster.Nodes(), leader);
  ASSERT_LT(stopped_follower, cluster.Nodes().size()) << "failed to pick follower";
  cluster.StopNode(stopped_follower);

  const std::vector<std::size_t> excluded{stopped_follower};
  WriteManyValues(cluster.Nodes(), "install_restart", 48, excluded);

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(20)))
      << "leader did not create snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  cluster.RestartNode(stopped_follower);

  ASSERT_TRUE(WaitForValueOnNode(cluster.Nodes()[stopped_follower],
                                 "install_restart_47", "value_47",
                                 std::chrono::seconds(30)))
      << "follower did not catch up by InstallSnapshot, describe="
      << cluster.Nodes()[stopped_follower]->Describe();

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[stopped_follower],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(10)))
      << "follower did not record installed snapshot, describe="
      << cluster.Nodes()[stopped_follower]->Describe();

  cluster.RestartNode(stopped_follower);

  ASSERT_TRUE(WaitForValueOnNode(cluster.Nodes()[stopped_follower],
                                 "install_restart_47", "value_47",
                                 std::chrono::seconds(15)))
      << "follower lost snapshot state after restart, describe="
      << cluster.Nodes()[stopped_follower]->Describe();

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[stopped_follower],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(10)))
      << "follower lost snapshot metadata after restart, describe="
      << cluster.Nodes()[stopped_follower]->Describe();
}

TEST_F(RaftSnapshotRestartTest, LeaderKeepsCompactedSnapshotStateAfterRestart) {
  auto cluster = MakeCluster("leader_compaction_restart", true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  WriteManyValues(cluster.Nodes(), "leader_restart", 32);

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader after writes";
  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(20)))
      << "leader did not compact through snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  cluster.RestartNode(leader_index);

  ASSERT_TRUE(WaitForValueOnNode(cluster.Nodes()[leader_index],
                                 "leader_restart_31", "value_31",
                                 std::chrono::seconds(15)))
      << "restarted leader node did not reload snapshot/log state, describe="
      << cluster.Nodes()[leader_index]->Describe();

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(10)))
      << "restarted leader node lost snapshot metadata, describe="
      << cluster.Nodes()[leader_index]->Describe();

  ProposeResult result;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("after_leader_restart", "ok"),
                               std::chrono::seconds(15), &result))
      << "write after leader restart failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "after_leader_restart", "ok",
                                std::chrono::seconds(20)))
      << "cluster did not continue replication after compacted leader restart";
}

TEST_F(RaftSnapshotRestartTest, FullClusterRestartsAfterSnapshotAndContinuesWriting) {
  auto cluster = MakeCluster("full_cluster_restart", true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  WriteManyValues(cluster.Nodes(), "full_restart", 40);

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "full_restart_39", "value_39",
                                std::chrono::seconds(15)))
      << "cluster did not apply baseline data before restart";

  bool any_snapshot = false;
  for (const auto& node : cluster.Nodes()) {
    if (WaitForNodeFieldAtLeast(node, "last_snapshot_index", 6,
                                std::chrono::seconds(2))) {
      any_snapshot = true;
      break;
    }
  }
  ASSERT_TRUE(any_snapshot) << "no node created snapshot before full restart";

  cluster.StopAll();
  cluster.Start();

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(10));
  ASSERT_NE(leader, nullptr) << "no leader elected after full restart";

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "full_restart_39", "value_39",
                                std::chrono::seconds(20)))
      << "cluster lost snapshot/log state after full restart";

  ProposeResult result;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("after_full_restart", "ok"),
                               std::chrono::seconds(15), &result))
      << "write after full restart failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "after_full_restart", "ok",
                                std::chrono::seconds(20)))
      << "cluster did not replicate after full restart";
}

TEST_F(RaftSnapshotRestartTest, SnapshotAndPostSnapshotLogsRecoverAfterFullRestart) {
  auto cluster = MakeCluster("snapshot_plus_tail_logs", true, 12);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  WriteManyValues(cluster.Nodes(), "snapshot_base", 36);

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader after snapshot_base writes";
  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 12,
                                      std::chrono::seconds(20)))
      << "leader did not create baseline snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  WriteManyValues(cluster.Nodes(), "tail_log", 5);

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "tail_log_4", "value_4",
                                std::chrono::seconds(15)))
      << "cluster did not apply post-snapshot tail logs";

  cluster.StopAll();
  cluster.Start();

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(10));
  ASSERT_NE(leader, nullptr) << "no leader after restarting snapshot + tail log cluster";

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "snapshot_base_35", "value_35",
                                std::chrono::seconds(20)))
      << "snapshot-covered data was not restored after restart";

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "tail_log_4", "value_4",
                                std::chrono::seconds(20)))
      << "post-snapshot log data was not restored after restart";

  ProposeResult result;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("after_tail_restart", "ok"),
                               std::chrono::seconds(15), &result))
      << "write after snapshot + tail restart failed, status="
      << ProposeStatusName(result.status) << ", message=" << result.message;

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "after_tail_restart", "ok",
                                std::chrono::seconds(20)))
      << "cluster did not continue after restoring snapshot + tail logs";
}

}  // namespace
}  // namespace raftdemo
