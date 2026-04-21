#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
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
namespace fs = std::filesystem;

struct RunningCluster {
  std::vector<std::shared_ptr<RaftNode>> nodes;
  std::vector<std::thread> threads;
};

class ScopedDataDir {
 public:
  explicit ScopedDataDir(std::string name) {
    std::error_code ec;
    root_ = fs::temp_directory_path(ec);
    if (ec || root_.empty()) {
      root_ = fs::current_path();
    }
    root_ /= "raftdemo_tests";
    root_ /= std::move(name);
    fs::remove_all(root_, ec);
    ec.clear();
    fs::create_directories(root_, ec);
  }

  ~ScopedDataDir() {
    std::error_code ec;
    fs::remove_all(root_, ec);
  }

  const fs::path& path() const { return root_; }

 private:
  fs::path root_;
};

bool IsLeaderNode(const std::shared_ptr<RaftNode>& node) {
  return node != nullptr && node->Describe().find("role=Leader") != std::string::npos;
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

std::vector<NodeConfig> BuildThreeNodeConfigs(const fs::path& data_root, int base_port) {
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
  for (auto& node : cluster->nodes) {
    if (node) {
      node->Stop();
    }
  }
  for (auto& t : cluster->threads) {
    if (t.joinable()) {
      t.join();
    }
  }
}

bool ProposeSet(const std::shared_ptr<RaftNode>& leader, const std::string& key,
                const std::string& value) {
  if (leader == nullptr) {
    return false;
  }
  Command cmd;
  cmd.type = CommandType::kSet;
  cmd.key = key;
  cmd.value = value;
  const ProposeResult result = leader->Propose(cmd);
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

TEST(PersistenceTest, FullClusterRestartRecovery) {
  ScopedDataDir scoped_dir("test_full_restart");
  auto configs = BuildThreeNodeConfigs(scoped_dir.path(), 53050);

  auto cluster = StartCluster(configs);
  auto leader = WaitForLeader(cluster.nodes, 6s);
  ASSERT_NE(leader, nullptr);

  ASSERT_TRUE(ProposeSet(leader, "alpha", "1"));
  ASSERT_TRUE(ProposeSet(leader, "beta", "2"));
  ASSERT_TRUE(WaitUntilValue(cluster.nodes, "alpha", "1", 5s));
  ASSERT_TRUE(WaitUntilValue(cluster.nodes, "beta", "2", 5s));

  StopCluster(&cluster);
  std::this_thread::sleep_for(1s);

  cluster = StartCluster(configs);
  leader = WaitForLeader(cluster.nodes, 6s);
  ASSERT_NE(leader, nullptr);

  EXPECT_TRUE(WaitUntilValue(cluster.nodes, "alpha", "1", 8s));
  EXPECT_TRUE(WaitUntilValue(cluster.nodes, "beta", "2", 8s));

  StopCluster(&cluster);
}

TEST(PersistenceTest, RestartedFollowerCatchesUp) {
  ScopedDataDir scoped_dir("test_follower_restart");
  auto configs = BuildThreeNodeConfigs(scoped_dir.path(), 53150);

  auto cluster = StartCluster(configs);
  auto leader = WaitForLeader(cluster.nodes, 6s);
  ASSERT_NE(leader, nullptr);

  std::shared_ptr<RaftNode> follower;
  for (const auto& node : cluster.nodes) {
    if (node != leader) {
      follower = node;
      break;
    }
  }
  ASSERT_NE(follower, nullptr);

  ASSERT_TRUE(ProposeSet(leader, "first", "100"));
  ASSERT_TRUE(WaitUntilValue(cluster.nodes, "first", "100", 5s));

  follower->Stop();
  std::this_thread::sleep_for(1s);

  ASSERT_TRUE(ProposeSet(leader, "second", "200"));

  std::vector<std::shared_ptr<RaftNode>> alive_nodes;
  for (const auto& node : cluster.nodes) {
    if (node != follower) {
      alive_nodes.push_back(node);
    }
  }
  ASSERT_TRUE(WaitUntilValue(alive_nodes, "second", "200", 5s));

  const auto follower_it = std::find(cluster.nodes.begin(), cluster.nodes.end(), follower);
  ASSERT_NE(follower_it, cluster.nodes.end());

  const std::size_t follower_index =
      static_cast<std::size_t>(std::distance(cluster.nodes.begin(), follower_it));

  if (cluster.threads[follower_index].joinable()) {
    cluster.threads[follower_index].join();
  }

  auto restarted_follower = std::make_shared<RaftNode>(configs[follower_index]);
  cluster.nodes[follower_index] = restarted_follower;
  cluster.threads[follower_index] = std::thread([restarted_follower]() {
    restarted_follower->Start();
    restarted_follower->Wait();
  });

  EXPECT_TRUE(WaitUntilValue(cluster.nodes, "first", "100", 8s));
  EXPECT_TRUE(WaitUntilValue(cluster.nodes, "second", "200", 8s));

  StopCluster(&cluster);
}

}  // namespace
}  // namespace raftdemo
