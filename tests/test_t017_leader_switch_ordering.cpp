// T017：验证 leader 切换、follower 追赶与新 proposal 交织时 committed state 保持一致，且 commit/apply 不逆序。
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "raft/common/command.h"
#include "raft/common/config.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"

namespace raftdemo {
namespace {

using Clock = std::chrono::steady_clock;

std::filesystem::path TestBinaryDir() {
#ifdef RAFT_TEST_BINARY_DIR
  return std::filesystem::path(RAFT_TEST_BINARY_DIR);
#else
  return std::filesystem::current_path();
#endif
}

std::uint64_t NowForPath() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string SafeTestName(const ::testing::TestInfo* info) {
  std::string name = std::string(info->test_suite_name()) + "." + info->name();
  for (char& ch : name) {
    if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ') {
      ch = '_';
    }
  }
  return name;
}

std::filesystem::path MakeTestRoot(const ::testing::TestInfo* info) {
  std::random_device rd;
#ifdef _WIN32
  const std::string name = "rq_ls_" + std::to_string(NowForPath()) + "_" +
                           std::to_string(rd());
  return std::filesystem::temp_directory_path() / "rq_ls" / name;
#else
  return TestBinaryDir() / "raft_test_data" / "t017_leader_switch_ordering" /
         (SafeTestName(info) + "_" + std::to_string(NowForPath()) + "_" +
          std::to_string(rd()));
#endif
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

bool IsLeaderNode(const std::shared_ptr<RaftNode>& node) {
  return node != nullptr && Contains(node->Describe(), "role=Leader");
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

Command SetCommand(const std::string& key, const std::string& value) {
  Command command;
  command.type = CommandType::kSet;
  command.key = key;
  command.value = value;
  return command;
}

Command DeleteCommand(const std::string& key) {
  Command command;
  command.type = CommandType::kDelete;
  command.key = key;
  return command;
}

int PickBasePort(const std::string& test_name) {
  const int name_offset = static_cast<int>(std::hash<std::string>{}(test_name) % 2000);
  if (const char* env = std::getenv("RAFT_TEST_BASE_PORT")) {
    try {
      return std::stoi(env) + name_offset;
    } catch (...) {
      // Fall through to generated port range.
    }
  }

  std::random_device rd;
  return 47000 + name_offset + static_cast<int>(rd() % 6000);
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
  n1.election_timeout_min = std::chrono::milliseconds(260);
  n1.election_timeout_max = std::chrono::milliseconds(520);
  n1.heartbeat_interval = std::chrono::milliseconds(70);
  n1.rpc_deadline = std::chrono::milliseconds(300);
  n1.data_dir = (data_root / "node_1").string();

  NodeConfig n2 = n1;
  n2.node_id = 2;
  n2.address = "127.0.0.1:" + std::to_string(base_port + 2);
  n2.peers = {
      PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
      PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
  };
  n2.data_dir = (data_root / "node_2").string();

  NodeConfig n3 = n1;
  n3.node_id = 3;
  n3.address = "127.0.0.1:" + std::to_string(base_port + 3);
  n3.peers = {
      PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
      PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
  };
  n3.data_dir = (data_root / "node_3").string();

  return {n1, n2, n3};
}

std::vector<snapshotConfig> BuildDisabledSnapshotConfigs(
    const std::filesystem::path& snapshot_root) {
  snapshotConfig config;
  config.enabled = false;
  config.snapshot_interval = std::chrono::minutes(10);
  config.load_on_startup = true;
  config.file_prefix = "snapshot";

  snapshotConfig s1 = config;
  s1.snapshot_dir = (snapshot_root / "node_1").string();
  snapshotConfig s2 = config;
  s2.snapshot_dir = (snapshot_root / "node_2").string();
  snapshotConfig s3 = config;
  s3.snapshot_dir = (snapshot_root / "node_3").string();
  return {s1, s2, s3};
}

class RestartableCluster {
 public:
  RestartableCluster(std::vector<NodeConfig> configs,
                     std::vector<snapshotConfig> snapshot_configs)
      : configs_(std::move(configs)), snapshot_configs_(std::move(snapshot_configs)) {}

  ~RestartableCluster() { StopAll(); }

  void StartAll() {
    StopAll();
    nodes_.assign(configs_.size(), nullptr);
    wait_threads_.clear();
    wait_threads_.resize(configs_.size());
    for (std::size_t i = 0; i < configs_.size(); ++i) {
      StartNode(i);
    }
  }

  void StartNode(std::size_t index) {
    ASSERT_LT(index, configs_.size());
    StopNode(index);

    nodes_[index] = std::make_shared<RaftNode>(configs_[index], snapshot_configs_[index]);
    nodes_[index]->Start();
    const auto node = nodes_[index];
    wait_threads_[index] = std::thread([node]() { node->Wait(); });
  }

  void StopNode(std::size_t index) {
    if (index >= nodes_.size()) {
      return;
    }
    if (nodes_[index]) {
      nodes_[index]->Stop();
    }
    if (index < wait_threads_.size() && wait_threads_[index].joinable()) {
      wait_threads_[index].join();
    }
    nodes_[index].reset();
  }

  void RestartNode(std::size_t index) { StartNode(index); }

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
    for (auto& node : nodes_) {
      node.reset();
    }
    wait_threads_.clear();
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

bool WaitForMissingOnAll(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                         const std::string& key,
                         std::chrono::milliseconds timeout,
                         const std::vector<std::size_t>& excluded = {}) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    bool all_missing = true;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (IsExcluded(i, excluded) || !nodes[i]) {
        continue;
      }

      std::string value;
      if (nodes[i]->DebugGetValue(key, &value)) {
        all_missing = false;
        break;
      }
    }

    if (all_missing) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

bool WaitForOrderedCommitApplyAtLeast(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                      std::uint64_t minimum_index,
                                      std::chrono::milliseconds timeout,
                                      std::string* failure_detail,
                                      const std::vector<std::size_t>& excluded = {}) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    bool all_ready = true;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (IsExcluded(i, excluded) || !nodes[i]) {
        continue;
      }

      const std::string describe = nodes[i]->Describe();
      const auto commit_index = ExtractUintField(describe, "commit_index");
      const auto last_applied = ExtractUintField(describe, "last_applied");
      if (!commit_index.has_value() || !last_applied.has_value()) {
        all_ready = false;
        continue;
      }
      if (*last_applied > *commit_index) {
        if (failure_detail != nullptr) {
          *failure_detail = "node " + std::to_string(i) +
                            " observed last_applied > commit_index: " + describe;
        }
        return false;
      }
      if (*commit_index < minimum_index || *last_applied < minimum_index) {
        all_ready = false;
      }
    }

    if (all_ready) {
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (failure_detail != nullptr) {
    *failure_detail = "timed out waiting for ordered commit/apply to reach index " +
                      std::to_string(minimum_index);
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

class RaftLeaderSwitchOrderingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    test_name_ = SafeTestName(test_info);
    root_ = MakeTestRoot(test_info);
    data_root_ = root_ / "raft_data";
    snapshot_root_ = root_ / "raft_snapshots";
    base_port_ = PickBasePort(test_name_);

    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
    std::filesystem::create_directories(data_root_, ec);
    ASSERT_FALSE(ec) << "failed to create data root: " << ec.message();
    std::filesystem::create_directories(snapshot_root_, ec);
    ASSERT_FALSE(ec) << "failed to create snapshot root: " << ec.message();
  }

  void TearDown() override {
    std::error_code ec;
    const bool keep_data = std::getenv("RAFT_TEST_KEEP_DATA") != nullptr;
    if (!HasFailure() && !keep_data) {
      std::filesystem::remove_all(root_, ec);
    } else {
      std::cout << "preserved test root: " << root_.string() << "\n";
    }
  }

  RestartableCluster MakeCluster(const std::string& case_name) const {
    return RestartableCluster(BuildThreeNodeConfigs(data_root_ / case_name, base_port_),
                              BuildDisabledSnapshotConfigs(snapshot_root_ / case_name));
  }

  std::string test_name_;
  std::filesystem::path root_;
  std::filesystem::path data_root_;
  std::filesystem::path snapshot_root_;
  int base_port_{0};
};

TEST_F(RaftLeaderSwitchOrderingTest,
       CommittedStateSurvivesLeaderSwitchAndNewLeaderContinuesReplication) {
  auto cluster = MakeCluster("committed_state_survives_switch");
  cluster.StartAll();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no initial leader elected";

  const std::size_t old_leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(old_leader_index, cluster.Nodes().size()) << "failed to locate old leader";

  ProposeResult result;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("stable_before_switch", "v1"),
                               std::chrono::seconds(10), &result))
      << "stable_before_switch failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("switch_anchor", "kept"),
                               std::chrono::seconds(10), &result))
      << "switch_anchor failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "stable_before_switch", "v1",
                                std::chrono::seconds(10)))
      << "cluster did not apply committed state before leader switch";
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "switch_anchor", "kept",
                                std::chrono::seconds(10)))
      << "cluster did not apply switch anchor before leader switch";

  std::string ordering_failure;
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(), result.log_index,
                                               std::chrono::seconds(10),
                                               &ordering_failure))
      << ordering_failure;

  cluster.StopNode(old_leader_index);
  const std::vector<std::size_t> excluded_old_leader{old_leader_index};

  auto new_leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(10),
                                        excluded_old_leader);
  ASSERT_NE(new_leader, nullptr) << "no replacement leader elected";

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "stable_before_switch", "v1",
                                std::chrono::seconds(10), excluded_old_leader))
      << "surviving quorum lost committed value after leader switch";
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "switch_anchor", "kept",
                                std::chrono::seconds(10), excluded_old_leader))
      << "surviving quorum lost committed anchor after leader switch";

  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("after_switch", "v2"),
                               std::chrono::seconds(10), &result, excluded_old_leader))
      << "after_switch failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "after_switch", "v2",
                                std::chrono::seconds(10), excluded_old_leader))
      << "surviving quorum did not advance new committed log after leader switch";
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(), result.log_index,
                                               std::chrono::seconds(10),
                                               &ordering_failure,
                                               excluded_old_leader))
      << ordering_failure;

  cluster.RestartNode(old_leader_index);

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "stable_before_switch", "v1",
                                std::chrono::seconds(20)))
      << "restarted old leader did not preserve prior committed state";
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "switch_anchor", "kept",
                                std::chrono::seconds(20)))
      << "restarted old leader did not preserve switch anchor";
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "after_switch", "v2",
                                std::chrono::seconds(20)))
      << "restarted old leader did not catch up to post-switch committed log";
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(), result.log_index,
                                               std::chrono::seconds(20),
                                               &ordering_failure))
      << ordering_failure;
}

TEST_F(RaftLeaderSwitchOrderingTest,
       LaggingFollowerCatchesUpDuringLeaderSwitchWithoutCommitApplyReordering) {
  auto cluster = MakeCluster("lagging_follower_mixed_switch");
  cluster.StartAll();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no initial leader elected";

  const std::size_t old_leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(old_leader_index, cluster.Nodes().size()) << "failed to locate old leader";

  const std::size_t lagging_follower = PickFollowerIndex(cluster.Nodes(), leader);
  ASSERT_LT(lagging_follower, cluster.Nodes().size()) << "failed to pick lagging follower";
  cluster.StopNode(lagging_follower);

  const std::vector<std::size_t> excluded_lagging{lagging_follower};
  WriteManyValues(cluster.Nodes(), "mixed_gap", 32, excluded_lagging);

  ProposeResult result;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("mixed_ordering", "phase_1"),
                               std::chrono::seconds(10), &result, excluded_lagging))
      << "mixed_ordering phase_1 failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("mixed_ordering", "phase_2"),
                               std::chrono::seconds(10), &result, excluded_lagging))
      << "mixed_ordering phase_2 failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), DeleteCommand("mixed_ordering"),
                               std::chrono::seconds(10), &result, excluded_lagging))
      << "mixed_ordering delete failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;

  ASSERT_TRUE(WaitForMissingOnAll(cluster.Nodes(), "mixed_ordering",
                                  std::chrono::seconds(10), excluded_lagging))
      << "surviving quorum did not preserve committed delete before leader switch";

  std::string ordering_failure;
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(), result.log_index,
                                               std::chrono::seconds(10),
                                               &ordering_failure,
                                               excluded_lagging))
      << ordering_failure;

  cluster.StopNode(old_leader_index);
  const std::vector<std::size_t> excluded_old_leader{old_leader_index};

  cluster.RestartNode(lagging_follower);

  auto new_leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(12),
                                        excluded_old_leader);
  ASSERT_NE(new_leader, nullptr)
      << "no replacement leader elected with restarted lagging follower";

  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("mixed_after_switch", "committed"),
                               std::chrono::seconds(12), &result, excluded_old_leader))
      << "mixed_after_switch failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("mixed_tail", "final"),
                               std::chrono::seconds(12), &result, excluded_old_leader))
      << "mixed_tail failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "mixed_after_switch", "committed",
                                std::chrono::seconds(20), excluded_old_leader))
      << "surviving quorum did not converge on post-switch committed value";
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "mixed_tail", "final",
                                std::chrono::seconds(20), excluded_old_leader))
      << "surviving quorum did not converge on final committed value";
  ASSERT_TRUE(WaitForMissingOnAll(cluster.Nodes(), "mixed_ordering",
                                  std::chrono::seconds(20), excluded_old_leader))
      << "surviving quorum lost committed delete while lagging follower caught up";
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(), result.log_index,
                                               std::chrono::seconds(20),
                                               &ordering_failure,
                                               excluded_old_leader))
      << ordering_failure;

  cluster.RestartNode(old_leader_index);

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "mixed_gap_31", "value_31",
                                std::chrono::seconds(20)))
      << "cluster did not preserve pre-switch committed log after full recovery";
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "mixed_after_switch", "committed",
                                std::chrono::seconds(20)))
      << "restarted old leader did not catch up to post-switch committed value";
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "mixed_tail", "final",
                                std::chrono::seconds(20)))
      << "restarted old leader did not catch up to final committed value";
  ASSERT_TRUE(WaitForMissingOnAll(cluster.Nodes(), "mixed_ordering",
                                  std::chrono::seconds(20)))
      << "cluster did not preserve delete ordering after lagging follower catch-up";
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(), result.log_index,
                                               std::chrono::seconds(20),
                                               &ordering_failure))
      << ordering_failure;
}

}  // namespace
}  // namespace raftdemo
