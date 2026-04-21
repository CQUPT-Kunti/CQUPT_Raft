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

  bool WaitUntilAll(const std::vector<std::string>& required_parts,
                    std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      bool ok = true;
      for (const auto& node : nodes_) {
        if (!ContainsAll(node->Describe(), required_parts)) {
          ok = false;
          break;
        }
      }
      if (ok) {
        return true;
      }
      std::this_thread::sleep_for(50ms);
    }
    return false;
  }

 private:
  std::vector<std::shared_ptr<RaftNode>> nodes_;
  std::vector<std::thread> threads_;
};

TEST(RaftLogReplicationTest, LeaderProposeReplicatesLogToAllNodes) {
  ClusterRunner cluster(55050);
  cluster.Start();

  auto leader = cluster.WaitForLeader(5s);
  ASSERT_NE(leader, nullptr);

  Command cmd;
  cmd.type = CommandType::kSet;
  cmd.key = "x";
  cmd.value = "1";

  const ProposeResult result = leader->Propose(cmd);
  ASSERT_EQ(result.status, ProposeStatus::kOk);
  EXPECT_EQ(result.log_index, 1u);

  ASSERT_TRUE(cluster.WaitUntilAll({"last_log_index=1", "commit_index=1", "last_applied=1"},
                                   5s));
  ASSERT_TRUE(cluster.WaitUntilAll({"kv={x=1}"}, 5s));
}

TEST(RaftLogReplicationTest, MultipleSequentialEntriesStayConsistentAcrossCluster) {
  ClusterRunner cluster(55150);
  cluster.Start();

  auto leader = cluster.WaitForLeader(5s);
  ASSERT_NE(leader, nullptr);

  auto propose_set = [&](const std::string& key, const std::string& value) {
    Command cmd;
    cmd.type = CommandType::kSet;
    cmd.key = key;
    cmd.value = value;
    return leader->Propose(cmd);
  };

  ASSERT_EQ(propose_set("x", "1").status, ProposeStatus::kOk);
  ASSERT_EQ(propose_set("y", "2").status, ProposeStatus::kOk);
  ASSERT_EQ(propose_set("x", "100").status, ProposeStatus::kOk);

  ASSERT_TRUE(cluster.WaitUntilAll({"last_log_index=3", "commit_index=3", "last_applied=3"},
                                   5s));
  ASSERT_TRUE(cluster.WaitUntilAll({"kv={x=100, y=2}"}, 5s));
}

}  // namespace
}  // namespace raftdemo
