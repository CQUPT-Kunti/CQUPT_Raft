#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "metadata_raft_test_utils.h"
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

std::filesystem::path MakeInspectableTestRoot(const ::testing::TestInfo* info) {
  std::random_device rd;
#ifdef _WIN32
  const std::string dir_name = "rq_rb_" + std::to_string(NowForPath()) + "_" +
                               std::to_string(rd());
  return std::filesystem::temp_directory_path() / "rq_rb" / dir_name;
#else
  const std::string dir_name = SafeTestName(info) + "_" +
                               std::to_string(NowForPath()) + "_" +
                               std::to_string(rd());
  return TestBinaryDir() / "raft_test_data" / "replicator_behavior" / dir_name;
#endif
}

bool KeepTestData() {
  const char* value = std::getenv("RAFT_TEST_KEEP_DATA");
  if (value == nullptr) {
    return true;
  }
  return std::string(value) != "0";
}

std::string DescribeDirectoryTree(const std::filesystem::path& root) {
  std::ostringstream oss;
  oss << "test root: " << root.string() << '\n';

  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    oss << "<missing>\n";
    return oss.str();
  }

  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());

  for (const auto& path : paths) {
    oss << "  " << std::filesystem::relative(path, root, ec).string();
    if (std::filesystem::is_regular_file(path, ec)) {
      oss << " (" << std::filesystem::file_size(path, ec) << " bytes)";
    }
    oss << '\n';
  }
  return oss.str();
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

bool IsLeaderNode(const std::shared_ptr<RaftNode>& node) {
  return node != nullptr && Contains(node->Describe(), "role=Leader");
}

std::string ObjectKey(const std::string& prefix, const int index) {
  return prefix + "_" + std::to_string(index);
}

std::string ObjectId(const std::string& prefix, const int index) {
  return prefix + "-object-" + std::to_string(index);
}

std::vector<std::string> SortedObjectKeys(const std::string& prefix,
                                          const int begin,
                                          const int end_exclusive) {
  std::vector<std::string> keys;
  keys.reserve(static_cast<std::size_t>(std::max(0, end_exclusive - begin)));
  for (int i = begin; i < end_exclusive; ++i) {
    keys.push_back(ObjectKey(prefix, i));
  }
  std::sort(keys.begin(), keys.end());
  return keys;
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
  return 46000 + name_offset + static_cast<int>(rd() % 7000);
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

std::vector<snapshotConfig> BuildThreeSnapshotConfigs(const std::filesystem::path& snapshot_root,
                                                      std::uint64_t log_threshold) {
  snapshotConfig s1;
  s1.enabled = true;
  s1.snapshot_dir = (snapshot_root / "node_1").string();
  s1.log_threshold = log_threshold;
  s1.snapshot_interval = std::chrono::minutes(10);
  s1.max_snapshot_count = 8;
  s1.load_on_startup = true;
  s1.file_prefix = "snapshot";

  snapshotConfig s2 = s1;
  s2.snapshot_dir = (snapshot_root / "node_2").string();

  snapshotConfig s3 = s1;
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
  }

  const std::vector<std::shared_ptr<RaftNode>>& Nodes() const { return nodes_; }

 private:
  std::vector<NodeConfig> configs_;
  std::vector<snapshotConfig> snapshot_configs_;
  std::vector<std::shared_ptr<RaftNode>> nodes_;
  std::vector<std::thread> wait_threads_;
};

std::string DescribeAllNodes(const std::vector<std::shared_ptr<RaftNode>>& nodes) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    oss << "node[" << i << "] ";
    if (nodes[i]) {
      oss << nodes[i]->Describe();
    } else {
      oss << "<not running>";
    }
    oss << '\n';
  }
  return oss.str();
}

std::shared_ptr<RaftNode> WaitForSingleLeader(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                              std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    std::shared_ptr<RaftNode> leader;
    int leader_count = 0;

    for (const auto& node : nodes) {
      if (IsLeaderNode(node)) {
        leader = node;
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

std::optional<std::size_t> FindNodeIndex(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                         const std::shared_ptr<RaftNode>& target) {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i] == target) {
      return i;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> PickRunningFollowerIndex(
    const std::vector<std::shared_ptr<RaftNode>>& nodes,
    const std::shared_ptr<RaftNode>& leader) {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i] && nodes[i] != leader) {
      return i;
    }
  }
  return std::nullopt;
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

std::size_t CountNodesWithSnapshotAtLeast(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                          std::uint64_t minimum) {
  std::size_t count = 0;
  for (const auto& node : nodes) {
    if (!node) {
      continue;
    }
    const auto value = ExtractUintField(node->Describe(), "last_snapshot_index");
    if (value.has_value() && *value >= minimum) {
      ++count;
    }
  }
  return count;
}

bool WaitForSnapshotOnAtLeastNodes(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                   std::uint64_t minimum_snapshot_index,
                                   std::size_t minimum_node_count,
                                   std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (CountNodesWithSnapshotAtLeast(nodes, minimum_snapshot_index) >= minimum_node_count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

void WriteManyCommittedObjects(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                               const std::string& bucket,
                               const std::string& prefix,
                               int begin,
                               int end_exclusive,
                               std::chrono::milliseconds timeout_per_write) {
  ProposeResult result;
  for (int i = begin; i < end_exclusive; ++i) {
    SCOPED_TRACE(prefix + " write " + std::to_string(i));
    ASSERT_TRUE(test::ProposeCreateCommitObjectWithRetry(nodes,
                                                         bucket,
                                                         ObjectKey(prefix, i),
                                                         ObjectId(prefix, i),
                                                         prefix + "-create-" +
                                                             std::to_string(i),
                                                         prefix + "-commit-" +
                                                             std::to_string(i),
                                                         timeout_per_write,
                                                         &result))
        << "write failed, status=" << ProposeStatusName(result.status)
        << ", message=" << result.message
        << "\n" << DescribeAllNodes(nodes);
  }
}

void ExpectCommittedMetadataState(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                  const std::string& bucket,
                                  const std::string& prefix,
                                  const int visible_object_count,
                                  const std::size_t expected_request_count,
                                  const std::size_t expected_object_count,
                                  const std::uint64_t expected_last_applied_index) {
  const auto expected_keys = SortedObjectKeys(prefix, 0, visible_object_count);
  ASSERT_TRUE(test::WaitUntilAllListObjectsMatch(nodes,
                                                 bucket,
                                                 prefix,
                                                 expected_keys,
                                                 expected_last_applied_index,
                                                 std::chrono::seconds(20)))
      << DescribeAllNodes(nodes);

  for (const auto& node : nodes) {
    if (!node) {
      continue;
    }

    const auto* state_machine = node->GetMetadataStateMachineV2();
    ASSERT_NE(state_machine, nullptr) << node->Describe();
    EXPECT_EQ(state_machine->RequestCount(), expected_request_count) << node->Describe();
    EXPECT_EQ(state_machine->ObjectCount(), expected_object_count) << node->Describe();
    EXPECT_EQ(state_machine->TombstoneCount(), 0U) << node->Describe();
    EXPECT_GE(state_machine->LastAppliedIndex(), expected_last_applied_index)
        << node->Describe();
    EXPECT_GT(state_machine->LastAppliedTerm(), 0U) << node->Describe();
  }
}

class RaftReplicatorBehaviorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = MakeInspectableTestRoot(::testing::UnitTest::GetInstance()->current_test_info());
    data_root_ = root_ / "raft_data";
    snapshot_root_ = root_ / "raft_snapshots";
    base_port_ = PickBasePort(SafeTestName(::testing::UnitTest::GetInstance()->current_test_info()));

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
    std::cout << "replicator behavior test data: " << root_.string() << '\n';
    std::cout << DescribeDirectoryTree(root_);
    if (!KeepTestData()) {
      std::error_code ec;
      std::filesystem::remove_all(root_, ec);
    }
  }

  RestartableCluster MakeCluster(const std::string& case_name,
                                 std::uint64_t snapshot_log_threshold) const {
    return RestartableCluster(BuildThreeNodeConfigs(data_root_ / case_name, base_port_),
                              BuildThreeSnapshotConfigs(snapshot_root_ / case_name,
                                                        snapshot_log_threshold));
  }

  std::filesystem::path root_;
  std::filesystem::path data_root_;
  std::filesystem::path snapshot_root_;
  int base_port_{0};
};

TEST_F(RaftReplicatorBehaviorTest, SlowFollowerDoesNotBlockMajorityCommit) {
  const std::string bucket = "replicator-behavior-majority";
  auto cluster = MakeCluster("slow_follower_majority_commit", 8);
  cluster.StartAll();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected\n" << DescribeAllNodes(cluster.Nodes());
  ASSERT_TRUE(test::ProposeCreateBucketWithRetry(cluster.Nodes(),
                                                 bucket,
                                                 "bucket-majority",
                                                 std::chrono::seconds(6)))
      << DescribeAllNodes(cluster.Nodes());

  const auto lagging_index = PickRunningFollowerIndex(cluster.Nodes(), leader);
  ASSERT_TRUE(lagging_index.has_value()) << DescribeAllNodes(cluster.Nodes());

  cluster.StopNode(*lagging_index);

  // With one follower down, the leader must still commit with the remaining follower.
  WriteManyCommittedObjects(cluster.Nodes(),
                            bucket,
                            "majority_while_slow",
                            0,
                            64,
                            std::chrono::seconds(6));

  constexpr std::uint64_t kExpectedLastAppliedIndex = 1 + 64 * 2;
  ASSERT_TRUE(test::WaitUntilAllCommittedObject(cluster.Nodes(),
                                                bucket,
                                                ObjectKey("majority_while_slow", 63),
                                                ObjectId("majority_while_slow", 63),
                                                2,
                                                kExpectedLastAppliedIndex,
                                                std::chrono::seconds(8)))
      << "leader and the healthy follower did not apply committed logs while another follower was down\n"
      << DescribeAllNodes(cluster.Nodes());
  ExpectCommittedMetadataState(cluster.Nodes(),
                               bucket,
                               "majority_while_slow",
                               64,
                               static_cast<std::size_t>(1 + 64 * 2),
                               64U,
                               kExpectedLastAppliedIndex);
}

TEST_F(RaftReplicatorBehaviorTest, SlowFollowerCatchesUpWhileLeaderKeepsAcceptingNewLogs) {
  const std::string bucket = "replicator-behavior-catchup";
  auto cluster = MakeCluster("slow_follower_catches_up_with_live_writes", 8);
  cluster.StartAll();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected\n" << DescribeAllNodes(cluster.Nodes());
  ASSERT_TRUE(test::ProposeCreateBucketWithRetry(cluster.Nodes(),
                                                 bucket,
                                                 "bucket-catchup",
                                                 std::chrono::seconds(6)))
      << DescribeAllNodes(cluster.Nodes());

  const auto lagging_index = PickRunningFollowerIndex(cluster.Nodes(), leader);
  ASSERT_TRUE(lagging_index.has_value()) << DescribeAllNodes(cluster.Nodes());

  cluster.StopNode(*lagging_index);

  // Generate enough committed logs while the follower is down to force snapshot/log catch-up paths.
  WriteManyCommittedObjects(cluster.Nodes(),
                            bucket,
                            "lagged_before_restart",
                            0,
                            96,
                            std::chrono::seconds(6));

  constexpr std::uint64_t kExpectedIndexBeforeRestart = 1 + 96 * 2;
  ASSERT_TRUE(test::WaitUntilAllCommittedObject(cluster.Nodes(),
                                                bucket,
                                                ObjectKey("lagged_before_restart", 95),
                                                ObjectId("lagged_before_restart", 95),
                                                2,
                                                kExpectedIndexBeforeRestart,
                                                std::chrono::seconds(10)))
      << DescribeAllNodes(cluster.Nodes());

  ASSERT_TRUE(WaitForSnapshotOnAtLeastNodes(cluster.Nodes(), 32, 2, std::chrono::seconds(20)))
      << "active majority did not create snapshots while the third follower was offline\n"
      << DescribeAllNodes(cluster.Nodes());

  cluster.StartNode(*lagging_index);

  // Immediately continue writing. These proposes should not wait for the slow follower to fully catch up;
  // the current majority should keep making progress.
  WriteManyCommittedObjects(cluster.Nodes(),
                            bucket,
                            "live_during_catchup",
                            0,
                            32,
                            std::chrono::seconds(8));

  constexpr std::uint64_t kExpectedIndexAfterCatchupWrites = 1 + (96 + 32) * 2;
  ASSERT_TRUE(test::WaitUntilAllCommittedObject(cluster.Nodes(),
                                                bucket,
                                                ObjectKey("live_during_catchup", 31),
                                                ObjectId("live_during_catchup", 31),
                                                2,
                                                kExpectedIndexAfterCatchupWrites,
                                                std::chrono::seconds(8)))
      << "running majority stopped applying new logs while the lagging follower was catching up\n"
      << DescribeAllNodes(cluster.Nodes());

  ASSERT_TRUE(test::WaitUntilAllCommittedObject(cluster.Nodes(),
                                                bucket,
                                                ObjectKey("lagged_before_restart", 95),
                                                ObjectId("lagged_before_restart", 95),
                                                2,
                                                kExpectedIndexAfterCatchupWrites,
                                                std::chrono::seconds(35)))
      << "restarted follower did not catch up old logs/snapshot\n"
      << DescribeAllNodes(cluster.Nodes());

  ASSERT_TRUE(test::WaitUntilAllCommittedObject(cluster.Nodes(),
                                                bucket,
                                                ObjectKey("live_during_catchup", 31),
                                                ObjectId("live_during_catchup", 31),
                                                2,
                                                kExpectedIndexAfterCatchupWrites,
                                                std::chrono::seconds(35)))
      << "restarted follower did not catch up logs written during catch-up\n"
      << DescribeAllNodes(cluster.Nodes());
  ExpectCommittedMetadataState(cluster.Nodes(),
                               bucket,
                               "lagged_before_restart",
                               96,
                               static_cast<std::size_t>(1 + 128 * 2),
                               128U,
                               kExpectedIndexAfterCatchupWrites);
  const auto expected_live_keys = SortedObjectKeys("live_during_catchup", 0, 32);
  ASSERT_TRUE(test::WaitUntilAllListObjectsMatch(cluster.Nodes(),
                                                 bucket,
                                                 "live_during_catchup",
                                                 expected_live_keys,
                                                 kExpectedIndexAfterCatchupWrites,
                                                 std::chrono::seconds(20)))
      << DescribeAllNodes(cluster.Nodes());
  for (const auto& node : cluster.Nodes()) {
    ASSERT_NE(node, nullptr);
    const auto* state_machine = node->GetMetadataStateMachineV2();
    ASSERT_NE(state_machine, nullptr) << node->Describe();
    EXPECT_EQ(state_machine->RequestCount(), static_cast<std::size_t>(1 + 128 * 2))
        << node->Describe();
    EXPECT_EQ(state_machine->ObjectCount(), 128U) << node->Describe();
    EXPECT_EQ(state_machine->TombstoneCount(), 0U) << node->Describe();
    EXPECT_GT(state_machine->LastAppliedTerm(), 0U) << node->Describe();
  }
}

}  // namespace
}  // namespace raftdemo
