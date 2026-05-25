#include <gtest/gtest.h>

#include <algorithm>
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

#include "raft/common/config.h"
#include "raft/common/metadata_command.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "metadata_raft_test_utils.h"
#include "support/raft_snapshot_restart_test_utils.h"

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

std::uint64_t NowForPath() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

int PickBasePort(const std::string& test_name) {
  const int name_offset = static_cast<int>(std::hash<std::string>{}(test_name) % 1800);

  if (const char* env = std::getenv("RAFT_TEST_BASE_PORT")) {
    try {
      return std::stoi(env) + name_offset;
    } catch (...) {
      // Fall through to a generated port range.
    }
  }

  // Keep each test in its own small port window so adjacent cases do not
  // randomly collide with sockets that are still draining on Windows.
  return 36000 + name_offset * 12;
}

std::filesystem::path MakeTestRoot(const std::string& test_name) {
  std::random_device rd;
  std::string safe_name = test_name;
  for (char& ch : safe_name) {
    if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ') {
      ch = '_';
    }
  }

#ifdef _WIN32
  const std::string name = "sc_" + std::to_string(NowForPath()) + "_" +
                           std::to_string(rd());
  return std::filesystem::temp_directory_path() / "rq_sc" / name;
#else
  const std::string name = "raft_snapshot_catchup_" + safe_name + "_" +
                           std::to_string(NowForPath()) + "_" +
                           std::to_string(rd());
  std::filesystem::path base_dir;
  if (const char* env = std::getenv("RAFT_TEST_OUTPUT_DIR")) {
    base_dir = env;
  } else {
    // CTest/gtest_discover_tests runs test executables from the build/tests
    // directory by default, so this keeps raft_data and raft_snapshots under build.
    base_dir = std::filesystem::current_path() / "raft_test_data";
  }

  return base_dir / name;
#endif
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

using raftdemo::test::DeleteSyntheticObject;
using raftdemo::test::FindNodeIndex;
using raftdemo::test::PickFollowerIndex;
using raftdemo::test::ProposeWithRetry;
using raftdemo::test::WaitForNodeFieldAtLeast;
using raftdemo::test::WaitForSingleLeader;
using raftdemo::test::WaitForSyntheticObjectMissingOnAll;
using raftdemo::test::WaitForSyntheticObjectOnAll;
using raftdemo::test::WaitForSyntheticObjectOnNode;
using raftdemo::test::WriteSyntheticObject;
using raftdemo::test::WriteSyntheticObjects;

class RaftSnapshotCatchupTest : public ::testing::Test {
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
    const bool keep_data = std::getenv("RAFT_TEST_KEEP_DATA") != nullptr;
    if (!HasFailure() && !keep_data) {
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

TEST_F(RaftSnapshotCatchupTest, RestartedFollowerCatchesUpLargeGapWithBatchedAppendEntries) {
  auto cluster = MakeCluster("batched_append_entries", false, 1000000);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  const std::size_t stopped_follower = PickFollowerIndex(cluster.Nodes(), leader);
  ASSERT_LT(stopped_follower, cluster.Nodes().size()) << "failed to pick follower";
  cluster.StopNode(stopped_follower);

  const std::vector<std::size_t> excluded{stopped_follower};
  WriteSyntheticObjects(cluster.Nodes(), "batch_gap", 64, excluded);

  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(), "batch_gap_63", "value_63",
                                          std::chrono::seconds(10), excluded))
      << "surviving majority did not apply the last batch value";

  cluster.RestartNode(stopped_follower);

  ASSERT_TRUE(WaitForSyntheticObjectOnNode(cluster.Nodes()[stopped_follower],
                                           "batch_gap_63", "value_63",
                                           std::chrono::seconds(30)))
      << "restarted follower did not catch up through batched AppendEntries, describe="
      << cluster.Nodes()[stopped_follower]->Describe();

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[stopped_follower],
                                      "last_applied", 120,
                                      std::chrono::seconds(10)))
      << "restarted follower last_applied did not advance enough, describe="
      << cluster.Nodes()[stopped_follower]->Describe();
}

TEST_F(RaftSnapshotCatchupTest,
       LaggingFollowerReplaysLiveLogWithoutBreakingCommittedDeleteOrdering) {
  auto cluster = MakeCluster("live_log_replay_ordering", false, 1000000);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  const std::size_t stopped_follower = PickFollowerIndex(cluster.Nodes(), leader);
  ASSERT_LT(stopped_follower, cluster.Nodes().size()) << "failed to pick follower";
  cluster.StopNode(stopped_follower);

  const std::vector<std::size_t> excluded{stopped_follower};
  WriteSyntheticObjects(cluster.Nodes(), "live_gap", 96, excluded);

  ProposeResult result;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(),
                               WriteSyntheticObject("live_ordering", "phase_1"),
                               std::chrono::seconds(10), &result, excluded))
      << "live ordering phase_1 failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(),
                               WriteSyntheticObject("live_ordering", "phase_2"),
                               std::chrono::seconds(10), &result, excluded))
      << "live ordering phase_2 failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(),
                               DeleteSyntheticObject("live_ordering"),
                               std::chrono::seconds(10), &result, excluded))
      << "live ordering delete failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(),
                               WriteSyntheticObject("live_tail", "committed"),
                               std::chrono::seconds(10), &result, excluded))
      << "live tail write failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;

  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(), "live_gap_95", "value_95",
                                          std::chrono::seconds(10), excluded))
      << "surviving majority did not apply the last live gap value";
  ASSERT_TRUE(WaitForSyntheticObjectMissingOnAll(cluster.Nodes(), "live_ordering",
                                                 std::chrono::seconds(10), excluded))
      << "surviving majority did not preserve committed delete ordering";
  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(), "live_tail", "committed",
                                          std::chrono::seconds(10), excluded))
      << "surviving majority did not apply tail value after delete";

  cluster.RestartNode(stopped_follower);

  ASSERT_TRUE(WaitForSyntheticObjectOnNode(cluster.Nodes()[stopped_follower],
                                           "live_gap_95", "value_95",
                                           std::chrono::seconds(30)))
      << "lagging follower did not replay live log gap, describe="
      << cluster.Nodes()[stopped_follower]->Describe();
  ASSERT_TRUE(WaitForSyntheticObjectMissingOnAll(cluster.Nodes(), "live_ordering",
                                                 std::chrono::seconds(20)))
      << "cluster did not preserve committed delete ordering after live log catch-up";
  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(), "live_tail", "committed",
                                          std::chrono::seconds(20)))
      << "cluster did not converge on committed tail value after live log catch-up";

  const auto follower_snapshot_index =
      ExtractUintField(cluster.Nodes()[stopped_follower]->Describe(), "last_snapshot_index");
  EXPECT_TRUE(!follower_snapshot_index.has_value() || *follower_snapshot_index == 0)
      << "live log catch-up unexpectedly required snapshot handoff, describe="
      << cluster.Nodes()[stopped_follower]->Describe();
}

TEST_F(RaftSnapshotCatchupTest, RestartedFollowerInstallsSnapshotWhenLeaderCompactedLogs) {
  auto cluster = MakeCluster("install_snapshot", true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  const std::size_t stopped_follower = PickFollowerIndex(cluster.Nodes(), leader);
  ASSERT_LT(stopped_follower, cluster.Nodes().size()) << "failed to pick follower";
  cluster.StopNode(stopped_follower);

  const std::vector<std::size_t> excluded{stopped_follower};
  WriteSyntheticObjects(cluster.Nodes(), "snapshot_gap", 48, excluded);

  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(), "snapshot_gap_47", "value_47",
                                          std::chrono::seconds(10), excluded))
      << "surviving majority did not apply the last snapshot_gap value";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(20)))
      << "leader did not compact logs through snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  cluster.RestartNode(stopped_follower);

  ASSERT_TRUE(WaitForSyntheticObjectOnNode(cluster.Nodes()[stopped_follower],
                                           "snapshot_gap_47", "value_47",
                                           std::chrono::seconds(30)))
      << "restarted follower did not install snapshot and catch up, describe="
      << cluster.Nodes()[stopped_follower]->Describe();

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[stopped_follower],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(10)))
      << "restarted follower did not record installed snapshot index, describe="
      << cluster.Nodes()[stopped_follower]->Describe();
}

TEST_F(RaftSnapshotCatchupTest, FollowerContinuesReplicatingLogsAfterInstallingSnapshot) {
  auto cluster = MakeCluster("snapshot_then_logs", true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  const std::size_t stopped_follower = PickFollowerIndex(cluster.Nodes(), leader);
  ASSERT_LT(stopped_follower, cluster.Nodes().size()) << "failed to pick follower";
  cluster.StopNode(stopped_follower);

  const std::vector<std::size_t> excluded{stopped_follower};
  const std::string bucket = "snapshot-then-logs-bucket";
  ProposeResult result;
  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCreateBucketCommand(
          bucket, "snapshot-then-logs-create-bucket-1"),
      std::chrono::seconds(10),
      &result,
      excluded));

  std::vector<raftdemo::test::ExpectedRecoveredMetadataObject> expected_objects;
  std::vector<std::string> visible_keys;
  for (int i = 0; i < 20; ++i) {
    const std::string suffix = (i < 10 ? "0" : "") + std::to_string(i);
    const std::string key = "install_first_" + suffix;
    const std::string object_id = "obj-" + key;
    ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
        cluster.Nodes(),
        raftdemo::test::MakeCreateObjectCommand(
            bucket, key, object_id,
            "snapshot-then-logs-create-" + suffix),
        std::chrono::seconds(10),
        &result,
        excluded));
    ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
        cluster.Nodes(),
        raftdemo::test::MakeCommitObjectCommand(
            bucket, key, object_id,
            "snapshot-then-logs-commit-" + suffix),
        std::chrono::seconds(10),
        &result,
        excluded));
    expected_objects.push_back({key, object_id, 2U, false});
    visible_keys.push_back(key);
  }

  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCreateObjectCommand(
          bucket, "deleted_anchor", "obj-deleted-anchor",
          "snapshot-then-logs-create-deleted-anchor"),
      std::chrono::seconds(10),
      &result,
      excluded));
  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCommitObjectCommand(
          bucket, "deleted_anchor", "obj-deleted-anchor",
          "snapshot-then-logs-commit-deleted-anchor"),
      std::chrono::seconds(10),
      &result,
      excluded));
  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeDeleteObjectCommand(
          bucket, "deleted_anchor", "obj-deleted-anchor",
          "snapshot-then-logs-delete-deleted-anchor"),
      std::chrono::seconds(10),
      &result,
      excluded));

  expected_objects.push_back({"deleted_anchor", "obj-deleted-anchor", 0U, true});
  std::sort(visible_keys.begin(), visible_keys.end());
  const raftdemo::test::MetadataRecoveryExpectation expected_after_install{
      .bucket = bucket,
      .objects = expected_objects,
      .visible_keys = visible_keys,
      .expected_request_count = 44U,
      .expected_tombstone_count = 1U,
      .expected_last_applied_index = result.log_index,
      .expected_min_last_applied_term = result.term,
  };

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(20)))
      << "leader did not create a usable snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  cluster.RestartNode(stopped_follower);

  ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
      {cluster.Nodes()[stopped_follower]}, expected_after_install,
      std::chrono::seconds(30)))
      << "restarted follower did not catch up to metadata snapshot baseline, describe="
      << cluster.Nodes()[stopped_follower]->Describe();

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[stopped_follower],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(10)))
      << "restarted follower did not install snapshot, describe="
      << cluster.Nodes()[stopped_follower]->Describe();

  const MetadataStateMachine* restarted_state_machine =
      cluster.Nodes()[stopped_follower]->GetMetadataStateMachineV2();
  ASSERT_NE(restarted_state_machine, nullptr);
  EXPECT_EQ(restarted_state_machine->RequestCount(), 44U);
  EXPECT_EQ(restarted_state_machine->TombstoneCount(), 1U);
  EXPECT_GE(restarted_state_machine->LastAppliedTerm(), result.term);

  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCreateObjectCommand(
          bucket, "after_snapshot", "obj-after-snapshot",
          "snapshot-then-logs-create-after-snapshot"),
      std::chrono::seconds(10),
      &result));
  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCommitObjectCommand(
          bucket, "after_snapshot", "obj-after-snapshot",
          "snapshot-then-logs-commit-after-snapshot"),
      std::chrono::seconds(10),
      &result));

  auto expected_after_tail = expected_after_install;
  expected_after_tail.objects.push_back({"after_snapshot", "obj-after-snapshot", 2U, false});
  expected_after_tail.visible_keys.push_back("after_snapshot");
  std::sort(expected_after_tail.visible_keys.begin(), expected_after_tail.visible_keys.end());
  expected_after_tail.expected_request_count = 46U;
  expected_after_tail.expected_last_applied_index = result.log_index;
  expected_after_tail.expected_min_last_applied_term = result.term;

  ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expected_after_tail, std::chrono::seconds(15)))
      << "not all nodes replicated metadata logs after snapshot installation";
}

}  // namespace
}  // namespace raftdemo
