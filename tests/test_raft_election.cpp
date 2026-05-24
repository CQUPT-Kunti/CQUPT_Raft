#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
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
#include "support/metadata_test_utils.h"

namespace raftdemo {
namespace {

using namespace std::chrono_literals;

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

bool IsLeaderSnapshot(const std::string& snapshot) {
  return snapshot.find("role=Leader") != std::string::npos;
}

std::optional<int> ExtractIntField(const std::string& describe,
                                   const std::string& field_name) {
  const std::string prefix = field_name + "=";
  const std::size_t begin = describe.find(prefix);
  if (begin == std::string::npos) {
    return std::nullopt;
  }

  std::size_t pos = begin + prefix.size();
  std::size_t end = pos;
  if (end < describe.size() && describe[end] == '-') {
    ++end;
  }
  while (end < describe.size() && describe[end] >= '0' && describe[end] <= '9') {
    ++end;
  }
  if (end == pos || (end == pos + 1 && describe[pos] == '-')) {
    return std::nullopt;
  }

  try {
    return std::stoi(describe.substr(pos, end - pos));
  } catch (...) {
    return std::nullopt;
  }
}

bool ContainsAll(const std::string& snapshot, const std::vector<std::string>& parts) {
  for (const auto& part : parts) {
    if (snapshot.find(part) == std::string::npos) {
      return false;
    }
  }
  return true;
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
  n1.election_timeout_min = std::chrono::milliseconds(300);
  n1.election_timeout_max = std::chrono::milliseconds(600);
  n1.heartbeat_interval = std::chrono::milliseconds(80);
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
  n2.heartbeat_interval = std::chrono::milliseconds(80);
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
  n3.heartbeat_interval = std::chrono::milliseconds(80);
  n3.rpc_deadline = std::chrono::milliseconds(500);
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

class ClusterRunner {
 public:
  explicit ClusterRunner(int base_port) {
    std::random_device rd;
#ifdef _WIN32
    root_ = std::filesystem::temp_directory_path() / "rq_re" /
            ("rq_re_" + std::to_string(NowForPath()) + "_" + std::to_string(rd()));
#else
    root_ = TestBinaryDir() / "raft_test_data" / "election" /
            ("raft_election_" + std::to_string(NowForPath()) + "_" +
             std::to_string(rd()));
#endif
    const auto configs = BuildThreeNodeConfigs(root_ / "raft_data", base_port);
    const auto snapshot_configs = BuildDisabledSnapshotConfigs(root_ / "raft_snapshots");
    nodes_.reserve(configs.size());
    for (std::size_t i = 0; i < configs.size(); ++i) {
      nodes_.push_back(std::make_shared<RaftNode>(configs[i], snapshot_configs[i]));
    }
  }

  ~ClusterRunner() {
    Stop();
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  void Start() {
    threads_.reserve(nodes_.size());
    for (const auto& node : nodes_) {
      threads_.emplace_back([node]() {
        node->Start();
        node->Wait();
      });
    }
  }

  void Stop() {
    for (auto& node : nodes_) {
      if (node) {
        node->Stop();
      }
    }
    for (auto& t : threads_) {
      if (t.joinable()) {
        t.join();
      }
    }
    threads_.clear();
  }

  std::shared_ptr<RaftNode> WaitForLeader(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      for (const auto& node : nodes_) {
        if (IsLeaderSnapshot(node->Describe())) {
          return node;
        }
      }
      std::this_thread::sleep_for(50ms);
    }
    return nullptr;
  }

  bool WaitUntilSingleLeader(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int leader_count = 0;
      for (const auto& node : nodes_) {
        if (IsLeaderSnapshot(node->Describe())) {
          ++leader_count;
        }
      }
      if (leader_count == 1) {
        return true;
      }
      std::this_thread::sleep_for(50ms);
    }
    return false;
  }

  bool WaitForFollowerRedirectReady(const std::shared_ptr<RaftNode>& leader,
                                    const std::shared_ptr<RaftNode>& follower,
                                    std::chrono::milliseconds timeout) const {
    if (!leader || !follower) {
      return false;
    }

    const auto expected_leader_id =
        ExtractIntField(leader->Describe(), "node").value_or(-1);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const std::string leader_snapshot = leader->Describe();
      const std::string follower_snapshot = follower->Describe();
      if (IsLeaderSnapshot(leader_snapshot) && !IsLeaderSnapshot(follower_snapshot)) {
        const auto follower_leader_id = ExtractIntField(follower_snapshot, "leader");
        if (follower_leader_id.has_value() && *follower_leader_id == expected_leader_id) {
          return true;
        }
      }
      std::this_thread::sleep_for(50ms);
    }
    return false;
  }

  const std::vector<std::shared_ptr<RaftNode>>& nodes() const { return nodes_; }

 private:
  std::filesystem::path root_;
  std::vector<std::shared_ptr<RaftNode>> nodes_;
  std::vector<std::thread> threads_;
};

TEST(RaftElectionTest, ThreeNodeClusterElectsExactlyOneLeader) {
  ClusterRunner cluster(54050);
  cluster.Start();

  auto leader = cluster.WaitForLeader(5s);
  ASSERT_NE(leader, nullptr);
  EXPECT_TRUE(cluster.WaitUntilSingleLeader(2s));
  for (const auto& node : cluster.nodes()) {
    if (node != leader) {
      ASSERT_TRUE(cluster.WaitForFollowerRedirectReady(leader, node, 2s))
          << "leader became observable, but cluster followers did not converge";
    }
  }

  int leader_count = 0;
  for (const auto& node : cluster.nodes()) {
    const std::string snapshot = node->Describe();
    if (IsLeaderSnapshot(snapshot)) {
      ++leader_count;
      EXPECT_TRUE(ContainsAll(snapshot, {"role=Leader", "leader="}));
    } else {
      EXPECT_TRUE(ContainsAll(snapshot, {"role=Follower", "leader="}));
    }
  }
  EXPECT_EQ(leader_count, 1);
}

TEST(RaftElectionTest, FollowerRejectsClientProposeAfterLeaderIsElected) {
  ClusterRunner cluster(54150);
  cluster.Start();

  auto leader = cluster.WaitForLeader(5s);
  ASSERT_NE(leader, nullptr);

  std::shared_ptr<RaftNode> follower;
  for (const auto& node : cluster.nodes()) {
    if (node != leader) {
      follower = node;
      break;
    }
  }
  ASSERT_NE(follower, nullptr);
  ASSERT_TRUE(cluster.WaitForFollowerRedirectReady(leader, follower, 2s))
      << "leader became observable, but follower redirect information was not ready";

  Command cmd;
  cmd.type = CommandType::kMetadata;
  cmd.metadata_payload = SerializeMetadataCommand(
      test::MakeCreateBucketCommand("from-follower-bucket",
                                    "from-follower-create-bucket-1"));

  const ProposeResult result = follower->Propose(cmd);
  EXPECT_EQ(result.status, ProposeStatus::kNotLeader);
  EXPECT_NE(result.leader_id, -1);
}

}  // namespace
}  // namespace raftdemo
