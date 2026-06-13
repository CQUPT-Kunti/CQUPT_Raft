#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include "raft/storage/snapshot_storage.h"
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

std::string ReadBinaryFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    return {};
  }

  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
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

NodeConfig BuildLearnerLikeConfig(const std::filesystem::path& data_root,
                                  int base_port) {
  NodeConfig config;
  config.node_id = 41;
  config.address = "127.0.0.1:" + std::to_string(base_port + 41);
  config.peers = {};
  config.election_timeout_min = std::chrono::milliseconds(250);
  config.election_timeout_max = std::chrono::milliseconds(500);
  config.heartbeat_interval = std::chrono::milliseconds(80);
  config.rpc_deadline = std::chrono::milliseconds(300);
  config.data_dir = (data_root / "learner_like_41").string();
  return config;
}

snapshotConfig BuildLearnerLikeSnapshotConfig(
    const std::filesystem::path& snapshot_root) {
  snapshotConfig config;
  config.enabled = true;
  config.snapshot_dir = (snapshot_root / "learner_like_41").string();
  config.log_threshold = 6;
  config.snapshot_interval = std::chrono::minutes(10);
  config.max_snapshot_count = 3;
  config.load_on_startup = true;
  config.file_prefix = "snapshot";
  return config;
}

void WriteLearnerIdentity(const NodeConfig& config) {
  std::error_code ec;
  std::filesystem::create_directories(config.data_dir, ec);
  ASSERT_FALSE(ec) << ec.message();

  const auto identity_path = std::filesystem::path(config.data_dir) / "node.identity";
  std::ofstream out(identity_path, std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << identity_path.string();
  out << "node_id=" << config.node_id << "\n";
  out << "address=" << config.address << "\n";
  out << "membership_state=learner\n";
  out.flush();
  ASSERT_TRUE(static_cast<bool>(out)) << identity_path.string();
}

AddLearnerProposalRequest MakeLearnerProposalRequest(const std::string& cluster_id,
                                                     const std::string& node_id,
                                                     const NodeConfig& learner_config,
                                                     int candidate_client_port) {
  AddLearnerProposalRequest request;
  request.cluster_id = cluster_id;
  request.node_id = node_id;
  request.candidate_raft_id = learner_config.node_id;
  request.candidate_client_address =
      "127.0.0.1:" + std::to_string(candidate_client_port);
  request.candidate_raft_address = learner_config.address;
  request.candidate_incarnation_id = node_id + ":boot:1710000000";
  request.candidate_sequence = 1;
  request.persistent_generation = 1;
  request.data_dir_fingerprint = "fingerprint-" + node_id;
  return request;
}

std::string DescribeLearnerEntry(const RuntimeMembershipEntry& entry) {
  return "raft_id=" + std::to_string(entry.raft_id) +
         ", pending=" + std::to_string(entry.pending) +
         ", committed=" + std::to_string(entry.committed) +
         ", match_index=" + std::to_string(entry.match_index) +
         ", next_index=" + std::to_string(entry.next_index) +
         ", last_snapshot_index=" + std::to_string(entry.last_snapshot_index) +
         ", last_snapshot_term=" + std::to_string(entry.last_snapshot_term) +
         ", last_applied_index=" + std::to_string(entry.last_applied_index) +
         ", observed_last_log_index=" + std::to_string(entry.observed_last_log_index);
}

bool WaitForLearnerSnapshotProgress(const std::shared_ptr<RaftNode>& leader,
                                    int learner_raft_id,
                                    std::uint64_t minimum_snapshot_index,
                                    std::chrono::milliseconds timeout,
                                    RuntimeMembershipEntry* learner_entry,
                                    std::string* diagnostics) {
  const auto deadline = Clock::now() + timeout;
  std::string last_diagnostics = "learner entry not observed";

  while (Clock::now() < deadline) {
    const auto summary = leader->GetRuntimeMembershipSummary();
    for (const auto& entry : summary.learner_entries) {
      if (entry.raft_id != learner_raft_id) {
        continue;
      }
      last_diagnostics = DescribeLearnerEntry(entry);
      if (entry.match_index >= minimum_snapshot_index &&
          entry.last_snapshot_index >= minimum_snapshot_index &&
          entry.last_applied_index >= minimum_snapshot_index &&
          entry.next_index >= entry.match_index) {
        if (learner_entry != nullptr) {
          *learner_entry = entry;
        }
        if (diagnostics != nullptr) {
          *diagnostics = last_diagnostics;
        }
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (diagnostics != nullptr) {
    *diagnostics = last_diagnostics;
  }
  return false;
}

SnapshotMeta LoadLatestSnapshotMetaOrDie(
    const std::filesystem::path& snapshot_root) {
  auto storage = CreateFileSnapshotStorage(snapshot_root.string(), "snapshot");
  SnapshotMeta meta;
  bool has_snapshot = false;
  std::string error;
  EXPECT_TRUE(storage->LoadLatestValidSnapshot(&meta, &has_snapshot, &error))
      << "failed to load latest snapshot meta from " << snapshot_root.string()
      << ", error=" << error;
  EXPECT_TRUE(has_snapshot)
      << "expected snapshot under " << snapshot_root.string()
      << ", error=" << error;
  return meta;
}

void ExpectCommittedThreeVoterSummary(
    const CommittedMembershipQuorumSummary& summary) {
  EXPECT_EQ(summary.voter_ids, (std::vector<int>{1, 2, 3}));
  EXPECT_TRUE(summary.learner_ids.empty());
  EXPECT_EQ(summary.voter_count, 3U);
  EXPECT_EQ(summary.learner_count, 0U);
  EXPECT_EQ(summary.quorum_size, 2U);
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

class StandaloneNodeRunner {
 public:
  explicit StandaloneNodeRunner(std::shared_ptr<RaftNode> node) : node_(std::move(node)) {}
  ~StandaloneNodeRunner() { Stop(); }

  void Start() {
    ASSERT_NE(node_, nullptr);
    node_->Start();
    wait_thread_ = std::thread([node = node_]() { node->Wait(); });
  }

  void Stop() {
    if (node_) {
      node_->Stop();
    }
    if (wait_thread_.joinable()) {
      wait_thread_.join();
    }
  }

  const std::shared_ptr<RaftNode>& Node() const { return node_; }

 private:
  std::shared_ptr<RaftNode> node_;
  std::thread wait_thread_;
};

using raftdemo::test::DeleteSyntheticObject;
using raftdemo::test::FindNodeIndex;
using raftdemo::test::PickFollowerIndex;
using raftdemo::test::ProposeWithRetry;
using raftdemo::test::SyntheticStateMatchesValue;
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

TEST_F(RaftSnapshotCatchupTest,
       LearnerLikeReceiverInstallsSnapshotWithoutChangingCommittedVoterQuorum) {
  const std::string case_name = "learner_like_install_snapshot_boundary";
  auto cluster = MakeCluster(case_name, true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  WriteSyntheticObjects(cluster.Nodes(), "learner_snapshot_gap", 48);

  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(),
                                          "learner_snapshot_gap_47",
                                          "value_47",
                                          std::chrono::seconds(10)))
      << "3-voter cluster did not apply the last learner snapshot baseline value";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index",
                                      6,
                                      std::chrono::seconds(20)))
      << "leader did not compact logs through snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(5));
  ASSERT_NE(leader, nullptr) << "no stable leader before pending learner snapshot test";

  const auto stable_leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(stable_leader_index, cluster.Nodes().size())
      << "failed to locate stable leader";

  const auto summary_before = leader->GetCommittedMembershipQuorumSummary();
  ExpectCommittedThreeVoterSummary(summary_before);

  const std::filesystem::path leader_snapshot_root =
      snapshot_root_ / case_name / ("node_" + std::to_string(stable_leader_index + 1));
  const SnapshotMeta snapshot_meta = LoadLatestSnapshotMetaOrDie(leader_snapshot_root);
  ASSERT_GE(snapshot_meta.last_included_index, 6U);
  ASSERT_FALSE(snapshot_meta.snapshot_path.empty());

  const std::string snapshot_data = ReadBinaryFile(snapshot_meta.snapshot_path);
  ASSERT_FALSE(snapshot_data.empty())
      << "leader snapshot payload is empty: " << snapshot_meta.snapshot_path;

  auto learner_like = std::make_shared<RaftNode>(
      BuildLearnerLikeConfig(data_root_ / case_name, base_port_),
      BuildLearnerLikeSnapshotConfig(snapshot_root_ / case_name));

  raft::InstallSnapshotRequest request;
  const NodeStatusSnapshot leader_status = leader->GetStatusSnapshot();
  request.set_term(leader_status.term);
  request.set_leader_id(leader_status.node_id);
  request.set_last_included_index(snapshot_meta.last_included_index);
  request.set_last_included_term(snapshot_meta.last_included_term);
  request.set_snapshot_data(snapshot_data);

  raft::InstallSnapshotResponse response;
  learner_like->OnInstallSnapshot(request, &response);

  ASSERT_TRUE(response.success()) << response.message();
  EXPECT_EQ(response.message(), "snapshot installed");
  EXPECT_EQ(response.term(), leader_status.term);
  EXPECT_GE(response.last_log_index(), snapshot_meta.last_included_index);

  const std::string learner_describe = learner_like->Describe();
  EXPECT_TRUE(Contains(learner_describe, "role=Follower")) << learner_describe;

  const auto learner_snapshot_index =
      ExtractUintField(learner_describe, "last_snapshot_index");
  ASSERT_TRUE(learner_snapshot_index.has_value()) << learner_describe;
  EXPECT_GE(*learner_snapshot_index, snapshot_meta.last_included_index)
      << learner_describe;

  const auto learner_last_applied =
      ExtractUintField(learner_describe, "last_applied");
  ASSERT_TRUE(learner_last_applied.has_value()) << learner_describe;
  EXPECT_GE(*learner_last_applied, snapshot_meta.last_included_index)
      << learner_describe;

  const MetadataStateMachine* learner_state_machine =
      learner_like->GetMetadataStateMachineV2();
  ASSERT_NE(learner_state_machine, nullptr);
  EXPECT_GE(learner_state_machine->LastAppliedIndex(),
            snapshot_meta.last_included_index);
  EXPECT_GE(learner_state_machine->LastAppliedTerm(),
            snapshot_meta.last_included_term);

  const auto summary_after = leader->GetCommittedMembershipQuorumSummary();
  ExpectCommittedThreeVoterSummary(summary_after);
  EXPECT_EQ(summary_after.voter_ids, summary_before.voter_ids);
  EXPECT_EQ(summary_after.voter_count, summary_before.voter_count);
  EXPECT_EQ(summary_after.quorum_size, summary_before.quorum_size);
  EXPECT_EQ(summary_after.learner_count, summary_before.learner_count);

  auto stable_leader_after =
      WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(3));
  ASSERT_NE(stable_leader_after, nullptr)
      << "3-voter cluster lost election availability after learner-like InstallSnapshot";
  const auto stable_summary_after =
      stable_leader_after->GetCommittedMembershipQuorumSummary();
  ExpectCommittedThreeVoterSummary(stable_summary_after);
}

TEST_F(RaftSnapshotCatchupTest,
       FailedLearnerLikeInstallSnapshotDoesNotPolluteCommittedVoterMembership) {
  const std::string case_name = "learner_like_install_snapshot_failure";
  auto cluster = MakeCluster(case_name, true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  WriteSyntheticObjects(cluster.Nodes(), "learner_snapshot_fail_gap", 24);

  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(),
                                          "learner_snapshot_fail_gap_23",
                                          "value_23",
                                          std::chrono::seconds(10)))
      << "3-voter cluster did not apply the learner snapshot failure baseline";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index",
                                      6,
                                      std::chrono::seconds(20)))
      << "leader did not create snapshot before learner failure test, describe="
      << cluster.Nodes()[leader_index]->Describe();

  const auto summary_before = leader->GetCommittedMembershipQuorumSummary();
  ExpectCommittedThreeVoterSummary(summary_before);

  const std::filesystem::path leader_snapshot_root =
      snapshot_root_ / case_name / ("node_" + std::to_string(leader_index + 1));
  const SnapshotMeta snapshot_meta = LoadLatestSnapshotMetaOrDie(leader_snapshot_root);
  ASSERT_FALSE(snapshot_meta.snapshot_path.empty());

  std::string corrupted_snapshot = ReadBinaryFile(snapshot_meta.snapshot_path);
  ASSERT_FALSE(corrupted_snapshot.empty())
      << "leader snapshot payload is empty: " << snapshot_meta.snapshot_path;
  corrupted_snapshot.resize(std::min<std::size_t>(corrupted_snapshot.size(), 32U));
  corrupted_snapshot.append("corrupted-trailer");

  auto learner_like = std::make_shared<RaftNode>(
      BuildLearnerLikeConfig(data_root_ / case_name, base_port_ + 200),
      BuildLearnerLikeSnapshotConfig(snapshot_root_ / case_name));

  raft::InstallSnapshotRequest request;
  const NodeStatusSnapshot leader_status = leader->GetStatusSnapshot();
  request.set_term(leader_status.term);
  request.set_leader_id(leader_status.node_id);
  request.set_last_included_index(snapshot_meta.last_included_index);
  request.set_last_included_term(snapshot_meta.last_included_term);
  request.set_snapshot_data(corrupted_snapshot);

  raft::InstallSnapshotResponse response;
  learner_like->OnInstallSnapshot(request, &response);

  EXPECT_FALSE(response.success());
  EXPECT_TRUE(Contains(response.message(), "load installed snapshot failed") ||
              Contains(response.message(),
                       "load installed snapshot boundary check failed"))
      << response.message();

  const std::string learner_describe = learner_like->Describe();
  const auto learner_snapshot_index =
      ExtractUintField(learner_describe, "last_snapshot_index");
  ASSERT_TRUE(learner_snapshot_index.has_value()) << learner_describe;
  EXPECT_EQ(*learner_snapshot_index, 0U) << learner_describe;

  const auto learner_last_applied =
      ExtractUintField(learner_describe, "last_applied");
  ASSERT_TRUE(learner_last_applied.has_value()) << learner_describe;
  EXPECT_EQ(*learner_last_applied, 0U) << learner_describe;

  const MetadataStateMachine* learner_state_machine =
      learner_like->GetMetadataStateMachineV2();
  ASSERT_NE(learner_state_machine, nullptr);
  EXPECT_EQ(learner_state_machine->LastAppliedIndex(), 0U);
  EXPECT_FALSE(SyntheticStateMatchesValue(*learner_state_machine,
                                          "learner_snapshot_fail_gap_23",
                                          "value_23"));

  const auto summary_after = leader->GetCommittedMembershipQuorumSummary();
  ExpectCommittedThreeVoterSummary(summary_after);
  EXPECT_EQ(summary_after.voter_ids, summary_before.voter_ids);
  EXPECT_EQ(summary_after.voter_count, summary_before.voter_count);
  EXPECT_EQ(summary_after.quorum_size, summary_before.quorum_size);
  EXPECT_EQ(summary_after.learner_count, summary_before.learner_count);
}

TEST_F(RaftSnapshotCatchupTest,
       PendingLearnerSnapshotInstallAdvancesAppliedProgressWithoutAffectingCommittedQuorum) {
  const std::string case_name = "pending_learner_snapshot_progress";
  constexpr const char* kClusterId = "cluster-t075-snapshot";
  constexpr const char* kLearnerNodeId = "meta-learner-snapshot";

  auto cluster = MakeCluster(case_name, true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  WriteSyntheticObjects(cluster.Nodes(), "pending_learner_snapshot_gap", 48);

  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(),
                                          "pending_learner_snapshot_gap_47",
                                          "value_47",
                                          std::chrono::seconds(10)))
      << "3-voter cluster did not apply the pending learner snapshot baseline";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index",
                                      6,
                                      std::chrono::seconds(20)))
      << "leader did not compact logs through snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  const auto summary_before = leader->GetCommittedMembershipQuorumSummary();
  ExpectCommittedThreeVoterSummary(summary_before);

  const std::filesystem::path leader_snapshot_root =
      snapshot_root_ / case_name / ("node_" + std::to_string(leader_index + 1));
  const SnapshotMeta snapshot_meta = LoadLatestSnapshotMetaOrDie(leader_snapshot_root);
  ASSERT_GE(snapshot_meta.last_included_index, 6U);

  NodeConfig learner_config = BuildLearnerLikeConfig(data_root_ / case_name, base_port_);
  WriteLearnerIdentity(learner_config);
  snapshotConfig learner_snapshot_config =
      BuildLearnerLikeSnapshotConfig(snapshot_root_ / case_name);
  auto learner = std::make_shared<RaftNode>(learner_config, learner_snapshot_config);
  StandaloneNodeRunner learner_runner(learner);
  learner_runner.Start();

  AddLearnerProposalResult add_learner_result;
  const auto add_learner_request = MakeLearnerProposalRequest(kClusterId,
                                                              kLearnerNodeId,
                                                              learner_config,
                                                              base_port_ + 141);
  const auto add_learner_deadline = Clock::now() + std::chrono::seconds(5);
  do {
    leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(2));
    if (leader == nullptr) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    add_learner_result = leader->ProposeAddLearner(add_learner_request);
    if (add_learner_result.status != AddLearnerProposalStatus::kNotLeader) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (Clock::now() < add_learner_deadline);

  ASSERT_NE(leader, nullptr) << "no leader available for AddLearner";
  ASSERT_EQ(add_learner_result.status,
            AddLearnerProposalStatus::kAcceptedPendingCommit)
      << add_learner_result.message;
  EXPECT_FALSE(add_learner_result.committed_membership_changed);
  EXPECT_EQ(add_learner_result.assigned_raft_id, learner_config.node_id);

  RuntimeMembershipEntry learner_entry;
  std::string learner_progress_diagnostics;
  ASSERT_TRUE(WaitForLearnerSnapshotProgress(leader,
                                             learner_config.node_id,
                                             snapshot_meta.last_included_index,
                                             std::chrono::seconds(30),
                                             &learner_entry,
                                             &learner_progress_diagnostics))
      << learner_progress_diagnostics;

  EXPECT_EQ(learner_entry.role, RuntimeMembershipRole::kLearner)
      << learner_progress_diagnostics;
  EXPECT_FALSE(learner_entry.committed) << learner_progress_diagnostics;
  EXPECT_TRUE(learner_entry.pending) << learner_progress_diagnostics;
  EXPECT_GE(learner_entry.match_index, snapshot_meta.last_included_index)
      << learner_progress_diagnostics;
  EXPECT_GE(learner_entry.last_snapshot_index, snapshot_meta.last_included_index)
      << learner_progress_diagnostics;
  EXPECT_GE(learner_entry.last_applied_index, snapshot_meta.last_included_index)
      << learner_progress_diagnostics;
  EXPECT_GE(learner_entry.observed_last_log_index, snapshot_meta.last_included_index)
      << learner_progress_diagnostics;

  ASSERT_TRUE(WaitForNodeFieldAtLeast(learner_runner.Node(),
                                      "last_snapshot_index",
                                      snapshot_meta.last_included_index,
                                      std::chrono::seconds(10)))
      << learner_runner.Node()->Describe();
  ASSERT_TRUE(WaitForNodeFieldAtLeast(learner_runner.Node(),
                                      "last_applied",
                                      snapshot_meta.last_included_index,
                                      std::chrono::seconds(10)))
      << learner_runner.Node()->Describe();
  ASSERT_TRUE(WaitForSyntheticObjectOnNode(learner_runner.Node(),
                                           "pending_learner_snapshot_gap_47",
                                           "value_47",
                                           std::chrono::seconds(10)))
      << learner_runner.Node()->Describe();

  const auto summary_after = leader->GetCommittedMembershipQuorumSummary();
  ExpectCommittedThreeVoterSummary(summary_after);
  EXPECT_EQ(summary_after.voter_ids, summary_before.voter_ids);
  EXPECT_EQ(summary_after.voter_count, summary_before.voter_count);
  EXPECT_EQ(summary_after.quorum_size, summary_before.quorum_size);
  EXPECT_EQ(summary_after.learner_count, summary_before.learner_count);
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
