#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "raft/common/config.h"
#include "raft/common/metadata_command.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"
#include "metadata_raft_test_utils.h"

namespace raftdemo
{
  namespace
  {

    using namespace std::chrono_literals;
    namespace fs = std::filesystem;

    fs::path TestBinaryDir()
    {
#ifdef RAFT_TEST_BINARY_DIR
      return fs::path(RAFT_TEST_BINARY_DIR);
#else
      return fs::current_path();
#endif
    }

    std::uint64_t NowForPath()
    {
      return static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
    }

    std::string SafeTestName()
    {
      const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
      std::string name = std::string(info->test_suite_name()) + "." + info->name();
      for (char &ch : name)
      {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ')
        {
          ch = '_';
        }
      }
      return name;
    }

    fs::path MakeTestRoot()
    {
      std::random_device rd;
      return TestBinaryDir() / "raft_test_data" / "commit_apply" /
             (SafeTestName() + "_" + std::to_string(NowForPath()) + "_" +
              std::to_string(rd()));
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
        }
      }
      std::random_device rd;
      return 40000 + static_cast<int>(rd() % 15000);
    }

    bool IsLeaderSnapshot(const std::string &snapshot)
    {
      return snapshot.find("role=Leader") != std::string::npos;
    }

    bool ContainsAll(const std::string &snapshot, const std::vector<std::string> &parts)
    {
      for (const auto &part : parts)
      {
        if (snapshot.find(part) == std::string::npos)
        {
          return false;
        }
      }
      return true;
    }

    std::vector<NodeConfig> BuildThreeNodeConfigs(int base_port, const fs::path &root)
    {
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
      n1.data_dir = (root / "raft_data" / "node_1").string();

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
      n2.data_dir = (root / "raft_data" / "node_2").string();

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
      n3.data_dir = (root / "raft_data" / "node_3").string();

      return {n1, n2, n3};
    }

    class ClusterRunner
    {
    public:
      explicit ClusterRunner(int base_port) : root_(MakeTestRoot())
      {
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);

        const auto configs = BuildThreeNodeConfigs(base_port, root_);
        snapshot_config_.enabled = false;
        snapshot_config_.snapshot_dir = (root_ / "raft_snapshots").string();

        nodes_.reserve(configs.size());
        for (const auto &cfg : configs)
        {
          nodes_.push_back(std::make_shared<RaftNode>(cfg, snapshot_config_));
        }
      }

      ~ClusterRunner() { Stop(); }

      void Start()
      {
        threads_.reserve(nodes_.size());
        for (const auto &node : nodes_)
        {
          threads_.emplace_back([node]()
                                {
        node->Start();
        node->Wait(); });
        }
      }

      void Stop()
      {
        for (auto &node : nodes_)
        {
          if (node)
          {
            node->Stop();
          }
        }
        for (auto &t : threads_)
        {
          if (t.joinable())
          {
            t.join();
          }
        }
        threads_.clear();
      }

      std::shared_ptr<RaftNode> WaitForLeader(std::chrono::milliseconds timeout) const
      {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
          for (const auto &node : nodes_)
          {
            if (IsLeaderSnapshot(node->Describe()))
            {
              return node;
            }
          }
          std::this_thread::sleep_for(50ms);
        }
        return nullptr;
      }

      bool WaitUntilAll(const std::vector<std::string> &required_parts,
                        std::chrono::milliseconds timeout) const
      {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
          bool ok = true;
          for (const auto &node : nodes_)
          {
            if (!ContainsAll(node->Describe(), required_parts))
            {
              ok = false;
              break;
            }
          }
          if (ok)
          {
            return true;
          }
          std::this_thread::sleep_for(50ms);
        }
        return false;
      }

      const std::vector<std::shared_ptr<RaftNode>> &Nodes() const
      {
        return nodes_;
      }

    private:
      fs::path root_;
      snapshotConfig snapshot_config_;
      std::vector<std::shared_ptr<RaftNode>> nodes_;
      std::vector<std::thread> threads_;
    };

    TEST(RaftCommitApplyTest, CommitAndApplyIndexesAdvanceAfterSuccessfulPropose)
    {
      ClusterRunner cluster(PickBasePort());
      cluster.Start();

      auto leader = cluster.WaitForLeader(5s);
      ASSERT_NE(leader, nullptr);

      const ProposeResult bucket_result = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCreateBucketCommand("commit-apply-bucket",
                                                          "commit-apply-bucket-create-1"));
      ASSERT_EQ(bucket_result.status, ProposeStatus::kOk) << bucket_result.message;

      const ProposeResult create_result = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCreateObjectCommand("commit-apply-bucket",
                                                          "logs/apply-object",
                                                          "obj-apply-1",
                                                          "commit-apply-object-create-1"));
      ASSERT_EQ(create_result.status, ProposeStatus::kOk) << create_result.message;

      const ProposeResult result = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCommitObjectCommand("commit-apply-bucket",
                                                          "logs/apply-object",
                                                          "obj-apply-1",
                                                          "commit-apply-object-commit-1"));
      ASSERT_EQ(result.status, ProposeStatus::kOk) << result.message;
      ASSERT_GT(result.log_index, 0u);

      const std::string index = std::to_string(result.log_index);
      ASSERT_TRUE(cluster.WaitUntilAll(
          {"last_log_index=" + index,
           "commit_index=" + index,
           "last_applied=" + index},
          5s));
      ASSERT_TRUE(raftdemo::test::WaitUntilAllCommittedObject(
          cluster.Nodes(),
          "commit-apply-bucket",
          "logs/apply-object",
          "obj-apply-1",
          2,
          result.log_index,
          5s));
    }

    TEST(RaftCommitApplyTest, DeleteCommandIsAppliedToAllNodes)
    {
      ClusterRunner cluster(PickBasePort());
      cluster.Start();

      auto leader = cluster.WaitForLeader(5s);
      ASSERT_NE(leader, nullptr);

      const ProposeResult bucket_result = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCreateBucketCommand("commit-delete-bucket",
                                                          "commit-delete-bucket-create-1"));
      ASSERT_EQ(bucket_result.status, ProposeStatus::kOk) << bucket_result.message;

      const ProposeResult create_result = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCreateObjectCommand("commit-delete-bucket",
                                                          "logs/delete-object",
                                                          "obj-delete-1",
                                                          "commit-delete-object-create-1"));
      ASSERT_EQ(create_result.status, ProposeStatus::kOk) << create_result.message;

      const ProposeResult commit_result = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCommitObjectCommand("commit-delete-bucket",
                                                          "logs/delete-object",
                                                          "obj-delete-1",
                                                          "commit-delete-object-commit-1"));
      ASSERT_EQ(commit_result.status, ProposeStatus::kOk) << commit_result.message;
      ASSERT_TRUE(raftdemo::test::WaitUntilAllCommittedObject(
          cluster.Nodes(),
          "commit-delete-bucket",
          "logs/delete-object",
          "obj-delete-1",
          2,
          commit_result.log_index,
          5s));

      const ProposeResult del_result = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeDeleteObjectCommand("commit-delete-bucket",
                                                          "logs/delete-object",
                                                          "obj-delete-1",
                                                          "commit-delete-object-delete-1"));
      ASSERT_EQ(del_result.status, ProposeStatus::kOk) << del_result.message;

      const std::string index = std::to_string(del_result.log_index);
      ASSERT_TRUE(cluster.WaitUntilAll({"last_log_index=" + index,
                                        "commit_index=" + index,
                                        "last_applied=" + index},
                                       5s));
      ASSERT_TRUE(raftdemo::test::WaitUntilAllDeletedObjectHidden(
          cluster.Nodes(),
          "commit-delete-bucket",
          "logs/delete-object",
          "obj-delete-1",
          del_result.log_index,
          5s));
    }

  } // namespace
} // namespace raftdemo
