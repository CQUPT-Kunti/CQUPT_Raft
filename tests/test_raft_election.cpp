#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "raft/command.h"
#include "raft/config.h"
#include "raft/propose.h"
#include "raft/raft_node.h"

namespace raftdemo {
namespace {

using namespace std::chrono_literals;

bool IsLeaderSnapshot(const std::string& snapshot) {
  return snapshot.find("role=Leader") != std::string::npos;
}

bool ContainsAll(const std::string& snapshot, const std::vector<std::string>& parts) {
  for (const auto& part : parts) {
    if (snapshot.find(part) == std::string::npos) {
      return false;
    }
  }
  return true;
}

std::vector<NodeConfig> BuildThreeNodeConfigs(int base_port) {
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

  return {n1, n2, n3};
}

class ClusterRunner {
 public:
  explicit ClusterRunner(int base_port) {
    const auto configs = BuildThreeNodeConfigs(base_port);
    nodes_.reserve(configs.size());
    for (const auto& cfg : configs) {
      nodes_.push_back(std::make_shared<RaftNode>(cfg));
    }
  }

  ~ClusterRunner() { Stop(); }

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

  const std::vector<std::shared_ptr<RaftNode>>& nodes() const { return nodes_; }

 private:
  std::vector<std::shared_ptr<RaftNode>> nodes_;
  std::vector<std::thread> threads_;
};

TEST(RaftElectionTest, ThreeNodeClusterElectsExactlyOneLeader) {
  ClusterRunner cluster(54050);
  cluster.Start();

  auto leader = cluster.WaitForLeader(5s);
  ASSERT_NE(leader, nullptr);
  EXPECT_TRUE(cluster.WaitUntilSingleLeader(2s));

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

  Command cmd;
  cmd.type = CommandType::kSet;
  cmd.key = "from_follower";
  cmd.value = "123";

  const ProposeResult result = follower->Propose(cmd);
  EXPECT_EQ(result.status, ProposeStatus::kNotLeader);
  EXPECT_NE(result.leader_id, -1);
}

}  // namespace
}  // namespace raftdemo
