#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
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

namespace raftdemo
{
  namespace
  {

    using Clock = std::chrono::steady_clock;

    std::string ProposeStatusName(ProposeStatus status)
    {
      switch (status)
      {
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

    bool Contains(const std::string &text, const std::string &needle)
    {
      return text.find(needle) != std::string::npos;
    }

    bool IsLeaderNode(const std::shared_ptr<RaftNode> &node)
    {
      return node && Contains(node->Describe(), "role=Leader");
    }

    Command SetCommand(const std::string &key, const std::string &value)
    {
      Command command;
      command.type = CommandType::kSet;
      command.key = key;
      command.value = value;
      return command;
    }

    Command DeleteCommand(const std::string &key)
    {
      Command command;
      command.type = CommandType::kDelete;
      command.key = key;
      return command;
    }

    std::uint64_t NowForPath()
    {
      return static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
    }

    int PickBasePort()
    {
      if (const char *env = std::getenv("RAFT_TEST_BASE_PORT"))
      {
        try
        {
          return std::stoi(env);
        }
        catch (...)
        {
          // Fall through to a generated port range.
        }
      }

      std::random_device rd;
      const int jitter = static_cast<int>(rd() % 1000);
      const auto tick = static_cast<int>(Clock::now().time_since_epoch().count() % 1000);
      return 35000 + jitter + tick;
    }

    std::filesystem::path MakeTestRoot(const std::string &test_name)
    {
      std::random_device rd;
      std::string safe_name = test_name;
      for (char &ch : safe_name)
      {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ')
        {
          ch = '_';
        }
      }

      const std::string name = "raft_kv_gtest_" + safe_name + "_" +
                               std::to_string(NowForPath()) + "_" +
                               std::to_string(rd());
      return std::filesystem::temp_directory_path() / name;
    }

    std::vector<NodeConfig> BuildThreeNodeConfigs(const std::filesystem::path &data_root,
                                                  int base_port)
    {
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
      n1.rpc_deadline = std::chrono::milliseconds(250);
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
      n2.rpc_deadline = std::chrono::milliseconds(250);
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
      n3.rpc_deadline = std::chrono::milliseconds(250);
      n3.data_dir = (data_root / "node_3").string();

      return {n1, n2, n3};
    }

    std::vector<snapshotConfig> BuildThreeSnapshotConfigs(
        const std::filesystem::path &snapshot_root)
    {
      snapshotConfig s1;
      s1.enabled = true;
      s1.snapshot_dir = (snapshot_root / "node_1").string();
      s1.log_threshold = 4;
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

    class TestCluster
    {
    public:
      TestCluster(std::vector<NodeConfig> configs,
                  std::vector<snapshotConfig> snapshot_configs)
          : configs_(std::move(configs)), snapshot_configs_(std::move(snapshot_configs)) {}

      ~TestCluster() { StopAll(); }

      void Start()
      {
        StopAll();
        nodes_.clear();
        wait_threads_.clear();

        for (std::size_t i = 0; i < configs_.size(); ++i)
        {
          nodes_.push_back(std::make_shared<RaftNode>(configs_[i], snapshot_configs_[i]));
        }
        for (const auto &node : nodes_)
        {
          node->Start();
        }
        for (const auto &node : nodes_)
        {
          wait_threads_.emplace_back([node]()
                                     { node->Wait(); });
        }
      }

      void StopAll()
      {
        for (const auto &node : nodes_)
        {
          if (node)
          {
            node->Stop();
          }
        }
        for (auto &thread : wait_threads_)
        {
          if (thread.joinable())
          {
            thread.join();
          }
        }
        wait_threads_.clear();
      }

      void StopNode(std::size_t index)
      {
        if (index >= nodes_.size() || !nodes_[index])
        {
          return;
        }
        nodes_[index]->Stop();
        if (index < wait_threads_.size() && wait_threads_[index].joinable())
        {
          wait_threads_[index].join();
        }
      }

      const std::vector<std::shared_ptr<RaftNode>> &Nodes() const { return nodes_; }

    private:
      std::vector<NodeConfig> configs_;
      std::vector<snapshotConfig> snapshot_configs_;
      std::vector<std::shared_ptr<RaftNode>> nodes_;
      std::vector<std::thread> wait_threads_;
    };

    bool IsExcluded(std::size_t index, const std::vector<std::size_t> &excluded)
    {
      for (std::size_t excluded_index : excluded)
      {
        if (index == excluded_index)
        {
          return true;
        }
      }
      return false;
    }

    std::shared_ptr<RaftNode> WaitForSingleLeader(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        std::chrono::milliseconds timeout,
        const std::vector<std::size_t> &excluded = {})
    {
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        std::shared_ptr<RaftNode> leader;
        int leader_count = 0;

        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
          if (IsExcluded(i, excluded) || !nodes[i])
          {
            continue;
          }
          if (IsLeaderNode(nodes[i]))
          {
            leader = nodes[i];
            ++leader_count;
          }
        }

        if (leader_count == 1)
        {
          return leader;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      return nullptr;
    }

    bool WaitForValueOnAll(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                           const std::string &key,
                           const std::string &expected_value,
                           std::chrono::milliseconds timeout,
                           const std::vector<std::size_t> &excluded = {})
    {
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        bool all_match = true;

        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
          if (IsExcluded(i, excluded) || !nodes[i])
          {
            continue;
          }

          std::string value;
          if (!nodes[i]->DebugGetValue(key, &value) || value != expected_value)
          {
            all_match = false;
            break;
          }
        }

        if (all_match)
        {
          return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      return false;
    }

    bool WaitForMissingOnAll(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                             const std::string &key,
                             std::chrono::milliseconds timeout,
                             const std::vector<std::size_t> &excluded = {})
    {
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        bool all_missing = true;

        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
          if (IsExcluded(i, excluded) || !nodes[i])
          {
            continue;
          }

          std::string value;
          if (nodes[i]->DebugGetValue(key, &value))
          {
            all_missing = false;
            break;
          }
        }

        if (all_missing)
        {
          return true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      return false;
    }

    bool ProposeWithRetry(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                          Command command,
                          std::chrono::milliseconds timeout,
                          ProposeResult *final_result,
                          const std::vector<std::size_t> &excluded = {})
    {
      const auto deadline = Clock::now() + timeout;
      ProposeResult last_result;

      while (Clock::now() < deadline)
      {
        auto leader = WaitForSingleLeader(nodes, std::chrono::milliseconds(1500), excluded);
        if (!leader)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }

        last_result = leader->Propose(command);
        if (last_result.Ok())
        {
          if (final_result != nullptr)
          {
            *final_result = last_result;
          }
          return true;
        }

        if (last_result.status == ProposeStatus::kInvalidCommand ||
            last_result.status == ProposeStatus::kApplyFailed ||
            last_result.status == ProposeStatus::kCommitFailed)
        {
          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      if (final_result != nullptr)
      {
        *final_result = last_result;
      }
      return false;
    }

    bool HasSnapshotMetaFile(const std::filesystem::path &snapshot_root)
    {
      std::error_code ec;
      if (!std::filesystem::exists(snapshot_root, ec))
      {
        return false;
      }
      for (const auto &entry : std::filesystem::recursive_directory_iterator(snapshot_root, ec))
      {
        if (ec)
        {
          return false;
        }
        if (entry.is_regular_file(ec) && entry.path().extension() == ".meta")
        {
          return true;
        }
      }
      return false;
    }

    class RaftIntegrationTest : public ::testing::Test
    {
    protected:
      void SetUp() override
      {
        const auto *test_info = ::testing::UnitTest::GetInstance()->current_test_info();
        const std::string test_name = std::string(test_info->test_suite_name()) + "." +
                                      test_info->name();

        root_ = MakeTestRoot(test_name);
        data_root_ = root_ / "raft_data";
        snapshot_root_ = root_ / "raft_snapshots";
        base_port_ = PickBasePort() + static_cast<int>(port_offset_);
        port_offset_ += 50;

        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
        std::filesystem::create_directories(data_root_, ec);
        ASSERT_FALSE(ec) << "failed to create data root: " << ec.message();

        std::filesystem::create_directories(snapshot_root_, ec);
        ASSERT_FALSE(ec) << "failed to create snapshot root: " << ec.message();

        RecordProperty("test_root", root_.string());
        RecordProperty("base_port", base_port_);
      }

      void TearDown() override
      {
        std::error_code ec;
        if (!HasFailure())
        {
          std::filesystem::remove_all(root_, ec);
        }
        else
        {
          std::cout << "preserved test root: " << root_.string() << "\n";
        }
      }

      TestCluster MakeCluster(const std::filesystem::path &data_case,
                              const std::filesystem::path &snapshot_case,
                              int port_offset = 0) const
      {
        return TestCluster(BuildThreeNodeConfigs(data_root_ / data_case,
                                                 base_port_ + port_offset),
                           BuildThreeSnapshotConfigs(snapshot_root_ / snapshot_case));
      }

      std::filesystem::path root_;
      std::filesystem::path data_root_;
      std::filesystem::path snapshot_root_;
      int base_port_{0};

    private:
      static int port_offset_;
    };

    int RaftIntegrationTest::port_offset_ = 0;

    TEST_F(RaftIntegrationTest, ElectsSingleLeaderInThreeNodeCluster)
    {
      auto cluster = MakeCluster("election", "election");
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
      ASSERT_NE(leader, nullptr) << "no single leader elected within timeout";
    }

    TEST_F(RaftIntegrationTest, ReplicatesSetAndDeleteCommandsToAllNodes)
    {
      auto cluster = MakeCluster("replication", "replication");
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
      ASSERT_NE(leader, nullptr) << "no single leader elected within timeout";

      ProposeResult result;
      ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("x", "1"),
                                   std::chrono::seconds(8), &result))
          << "SET x 1 failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      EXPECT_TRUE(WaitForValueOnAll(cluster.Nodes(), "x", "1", std::chrono::seconds(8)))
          << "not all nodes applied x=1";

      ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("y", "2"),
                                   std::chrono::seconds(8), &result))
          << "SET y 2 failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      EXPECT_TRUE(WaitForValueOnAll(cluster.Nodes(), "y", "2", std::chrono::seconds(8)))
          << "not all nodes applied y=2";

      ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("x", "3"),
                                   std::chrono::seconds(8), &result))
          << "SET x 3 failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      EXPECT_TRUE(WaitForValueOnAll(cluster.Nodes(), "x", "3", std::chrono::seconds(8)))
          << "not all nodes applied x=3";

      ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), DeleteCommand("y"),
                                   std::chrono::seconds(8), &result))
          << "DEL y failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      EXPECT_TRUE(WaitForMissingOnAll(cluster.Nodes(), "y", std::chrono::seconds(8)))
          << "not all nodes applied delete y";
    }

    TEST_F(RaftIntegrationTest, ElectsNewLeaderAfterCurrentLeaderStops)
    {
      auto cluster = MakeCluster("failover", "failover");
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
      ASSERT_NE(leader, nullptr) << "no leader before failover test";

      std::size_t old_leader_index = cluster.Nodes().size();
      for (std::size_t i = 0; i < cluster.Nodes().size(); ++i)
      {
        if (cluster.Nodes()[i] == leader)
        {
          old_leader_index = i;
          break;
        }
      }
      ASSERT_LT(old_leader_index, cluster.Nodes().size()) << "failed to locate leader index";

      cluster.StopNode(old_leader_index);

      const std::vector<std::size_t> excluded{old_leader_index};
      auto new_leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(10), excluded);
      ASSERT_NE(new_leader, nullptr) << "no new leader after stopping old leader";

      ProposeResult result;
      ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("after_failover", "ok"),
                                   std::chrono::seconds(10), &result, excluded))
          << "SET after_failover ok failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;

      EXPECT_TRUE(WaitForValueOnAll(cluster.Nodes(), "after_failover", "ok",
                                    std::chrono::seconds(8), excluded))
          << "surviving nodes did not apply after_failover=ok";
    }

    TEST_F(RaftIntegrationTest, GeneratesSnapshotMetaFileAfterEnoughAppliedLogs)
    {
      auto cluster = MakeCluster("snapshot", "snapshot");
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
      ASSERT_NE(leader, nullptr) << "no single leader elected within timeout";

      ProposeResult result;
      for (int i = 0; i < 8; ++i)
      {
        SCOPED_TRACE("snapshot write " + std::to_string(i));
        ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("snap_" + std::to_string(i), "value_" + std::to_string(i)),
                                     std::chrono::seconds(8), &result))
            << "snapshot write failed, status=" << ProposeStatusName(result.status)
            << ", message=" << result.message;
      }

      const auto deadline = Clock::now() + std::chrono::seconds(10);
      while (Clock::now() < deadline)
      {
        if (HasSnapshotMetaFile(snapshot_root_ / "snapshot"))
        {
          SUCCEED();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      FAIL() << "no snapshot meta file generated within timeout";
    }

  } // namespace
} // namespace raftdemo
