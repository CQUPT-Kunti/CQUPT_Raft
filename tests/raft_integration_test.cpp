#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
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

    std::optional<std::uint64_t> ExtractUintField(const std::string &describe,
                                                  const std::string &field_name)
    {
      const std::string prefix = field_name + "=";
      const std::size_t begin = describe.find(prefix);
      if (begin == std::string::npos)
      {
        return std::nullopt;
      }

      std::size_t pos = begin + prefix.size();
      std::size_t end = pos;
      while (end < describe.size() && describe[end] >= '0' && describe[end] <= '9')
      {
        ++end;
      }

      if (end == pos)
      {
        return std::nullopt;
      }

      try
      {
        return static_cast<std::uint64_t>(std::stoull(describe.substr(pos, end - pos)));
      }
      catch (...)
      {
        return std::nullopt;
      }
    }

    bool IsLeaderNode(const std::shared_ptr<RaftNode> &node)
    {
      return node && Contains(node->Describe(), "role=Leader");
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

    std::filesystem::path TestBinaryDir()
    {
#ifdef RAFT_TEST_BINARY_DIR
      return std::filesystem::path(RAFT_TEST_BINARY_DIR);
#else
      return std::filesystem::current_path();
#endif
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

#ifdef _WIN32
      // Keep Windows integration roots short so publish/staging artifacts stay
      // below common path-length limits during cluster-style test runs.
      const std::string name = "ri_" + std::to_string(NowForPath()) + "_" +
                               std::to_string(rd());
      return std::filesystem::temp_directory_path() / "rq_ri" / name;
#else
      const std::string name = "raft_kv_gtest_" + safe_name + "_" +
                               std::to_string(NowForPath()) + "_" +
                               std::to_string(rd());
      return TestBinaryDir() / "raft_test_data" / "integration" / name;
#endif
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

      void RestartNode(std::size_t index)
      {
        ASSERT_LT(index, configs_.size());
        StopNode(index);

        if (nodes_.size() < configs_.size())
        {
          nodes_.resize(configs_.size());
        }
        if (wait_threads_.size() < configs_.size())
        {
          wait_threads_.resize(configs_.size());
        }

        nodes_[index] = std::make_shared<RaftNode>(configs_[index], snapshot_configs_[index]);
        nodes_[index]->Start();
        const auto node = nodes_[index];
        wait_threads_[index] = std::thread([node]()
                                           { node->Wait(); });
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

    bool WaitForNodeFieldAtLeast(const std::shared_ptr<RaftNode> &node,
                                 const std::string &field_name,
                                 std::uint64_t minimum,
                                 std::chrono::milliseconds timeout)
    {
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        if (node)
        {
          const auto value = ExtractUintField(node->Describe(), field_name);
          if (value.has_value() && *value >= minimum)
          {
            return true;
          }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
        if (!entry.is_regular_file(ec))
        {
          continue;
        }
        const auto name = entry.path().filename().string();
        if (name == "__raft_snapshot_meta")
        {
          const auto snapshot_dir = entry.path().parent_path();
          if (std::filesystem::exists(snapshot_dir / "data.bin", ec))
          {
            return true;
          }
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

      const std::string bucket = "integration-replication-bucket";
      const auto create_bucket = raftdemo::test::MakeCreateBucketCommand(
          bucket, "integration-replication-create-bucket-1");
      const auto create_x_v1 = raftdemo::test::MakeCreateObjectCommand(
          bucket, "x", "obj-x-v1", "integration-replication-create-x-v1");
      const auto commit_x_v1 = raftdemo::test::MakeCommitObjectCommand(
          bucket, "x", "obj-x-v1", "integration-replication-commit-x-v1");
      const auto create_y = raftdemo::test::MakeCreateObjectCommand(
          bucket, "y", "obj-y-v1", "integration-replication-create-y-v1");
      const auto commit_y = raftdemo::test::MakeCommitObjectCommand(
          bucket, "y", "obj-y-v1", "integration-replication-commit-y-v1");
      const auto delete_y = raftdemo::test::MakeDeleteObjectCommand(
          bucket, "y", "obj-y-v1", "integration-replication-delete-y-v1");

      ProposeResult result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(), create_bucket, std::chrono::seconds(8), &result))
          << "CreateBucket failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(), create_x_v1, std::chrono::seconds(8), &result))
          << "CreateObject x failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(), commit_x_v1, std::chrono::seconds(8), &result))
          << "CommitObject x failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(), create_y, std::chrono::seconds(8), &result))
          << "CreateObject y failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(), commit_y, std::chrono::seconds(8), &result))
          << "CommitObject y failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(), delete_y, std::chrono::seconds(8), &result))
          << "DeleteObject y failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;

      ASSERT_TRUE(raftdemo::test::WaitUntilAllCommittedObject(
                      cluster.Nodes(), bucket, "x", "obj-x-v1", 2U, result.log_index,
                      std::chrono::seconds(8)))
          << "not all nodes committed metadata object x";
      ASSERT_TRUE(raftdemo::test::WaitUntilAllDeletedObjectHidden(
                      cluster.Nodes(), bucket, "y", "obj-y-v1", result.log_index,
                      std::chrono::seconds(8)))
          << "not all nodes preserved deleted metadata object y";
      ASSERT_TRUE(raftdemo::test::WaitUntilAllListObjectsMatch(
                      cluster.Nodes(), bucket, "", {"x"}, result.log_index,
                      std::chrono::seconds(8)))
          << "object_index/ListObjects did not converge to committed metadata view";
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

      const std::string bucket = "integration-failover-bucket";
      const auto create_bucket = raftdemo::test::MakeCreateBucketCommand(
          bucket, "integration-failover-create-bucket-1");
      const auto create_after_failover = raftdemo::test::MakeCreateObjectCommand(
          bucket, "after_failover", "obj-after-failover",
          "integration-failover-create-object-1");
      const auto commit_after_failover = raftdemo::test::MakeCommitObjectCommand(
          bucket, "after_failover", "obj-after-failover",
          "integration-failover-commit-object-1");

      ProposeResult result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(), create_bucket, std::chrono::seconds(10), &result, excluded))
          << "CreateBucket after failover failed, status="
          << ProposeStatusName(result.status)
          << ", message=" << result.message;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(), create_after_failover, std::chrono::seconds(10), &result,
                      excluded))
          << "CreateObject after failover failed, status="
          << ProposeStatusName(result.status)
          << ", message=" << result.message;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(), commit_after_failover, std::chrono::seconds(10), &result,
                      excluded))
          << "CommitObject after failover failed, status="
          << ProposeStatusName(result.status)
          << ", message=" << result.message;

      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                      cluster.Nodes(),
                      {.bucket = bucket,
                       .objects = {{"after_failover", "obj-after-failover", 2U, false}},
                       .visible_keys = {"after_failover"},
                       .expected_request_count = 3U,
                       .expected_tombstone_count = 0U,
                       .expected_last_applied_index = result.log_index},
                      std::chrono::seconds(8),
                      excluded))
          << "surviving nodes did not converge on metadata object after_failover";
    }

    TEST_F(RaftIntegrationTest, GeneratesSnapshotMetaFileAfterEnoughAppliedLogs)
    {
      auto cluster = MakeCluster("snapshot", "snapshot");
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
      ASSERT_NE(leader, nullptr) << "no single leader elected within timeout";

      const std::string bucket = "integration-snapshot-bucket";
      ProposeResult result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(),
                      raftdemo::test::MakeCreateBucketCommand(
                          bucket, "integration-snapshot-create-bucket-1"),
                      std::chrono::seconds(8), &result))
          << "CreateBucket for snapshot test failed, status="
          << ProposeStatusName(result.status) << ", message=" << result.message;

      std::vector<std::string> expected_keys;
      for (int i = 0; i < 8; ++i)
      {
        SCOPED_TRACE("snapshot write " + std::to_string(i));
        const std::string object_key = "snap_" + std::to_string(i);
        expected_keys.push_back(object_key);
        ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                        cluster.Nodes(),
                        raftdemo::test::MakeCreateObjectCommand(
                            bucket, object_key, "obj-" + object_key,
                            "integration-snapshot-create-" + std::to_string(i)),
                        std::chrono::seconds(8), &result))
            << "snapshot create failed, status=" << ProposeStatusName(result.status)
            << ", message=" << result.message;
        ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                        cluster.Nodes(),
                        raftdemo::test::MakeCommitObjectCommand(
                            bucket, object_key, "obj-" + object_key,
                            "integration-snapshot-commit-" + std::to_string(i)),
                        std::chrono::seconds(8), &result))
            << "snapshot commit failed, status=" << ProposeStatusName(result.status)
            << ", message=" << result.message;
      }

      ASSERT_TRUE(raftdemo::test::WaitUntilAllListObjectsMatch(
                      cluster.Nodes(), bucket, "snap_", expected_keys, result.log_index,
                      std::chrono::seconds(10)))
          << "metadata object_index/ListObjects did not converge before snapshot generation";

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

    TEST_F(RaftIntegrationTest,
           LaggingFollowerInstallsSnapshotAndReplaysTailDeleteAcrossCompactionBoundary)
    {
      auto cluster = MakeCluster("snapshot_boundary", "snapshot_boundary");
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
      ASSERT_NE(leader, nullptr) << "no leader elected";

      std::size_t leader_index = cluster.Nodes().size();
      for (std::size_t i = 0; i < cluster.Nodes().size(); ++i)
      {
        if (cluster.Nodes()[i] == leader)
        {
          leader_index = i;
          break;
        }
      }
      ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader index";

      std::size_t stopped_follower = cluster.Nodes().size();
      for (std::size_t i = 0; i < cluster.Nodes().size(); ++i)
      {
        if (i != leader_index && cluster.Nodes()[i])
        {
          stopped_follower = i;
          break;
        }
      }
      ASSERT_LT(stopped_follower, cluster.Nodes().size()) << "failed to pick follower";
      cluster.StopNode(stopped_follower);

      const std::vector<std::size_t> excluded{stopped_follower};
      ProposeResult result;
      const std::string bucket = "integration-boundary-bucket";
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(),
                      raftdemo::test::MakeCreateBucketCommand(
                          bucket, "integration-boundary-create-bucket-1"),
                      std::chrono::seconds(10), &result, excluded))
          << "boundary CreateBucket failed, status="
          << ProposeStatusName(result.status)
          << ", message=" << result.message;

      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(),
                      raftdemo::test::MakeCreateObjectCommand(
                          bucket, "boundary_key", "obj-boundary-key",
                          "integration-boundary-create-seed"),
                      std::chrono::seconds(10), &result, excluded))
          << "boundary seed create failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(),
                      raftdemo::test::MakeCommitObjectCommand(
                          bucket, "boundary_key", "obj-boundary-key",
                          "integration-boundary-commit-seed"),
                      std::chrono::seconds(10), &result, excluded))
          << "boundary seed commit failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;

      std::vector<std::string> visible_keys;
      for (int i = 0; i < 8; ++i)
      {
        SCOPED_TRACE("snapshot boundary fill " + std::to_string(i));
        const std::string object_key = "boundary_fill_" + std::to_string(i);
        visible_keys.push_back(object_key);
        ASSERT_TRUE(
            raftdemo::test::ProposeMetadataCommandWithRetry(
                cluster.Nodes(),
                raftdemo::test::MakeCreateObjectCommand(
                    bucket, object_key, "obj-" + object_key,
                    "integration-boundary-create-fill-" + std::to_string(i)),
                std::chrono::seconds(10), &result, excluded))
            << "boundary fill create failed, status=" << ProposeStatusName(result.status)
            << ", message=" << result.message;
        ASSERT_TRUE(
            raftdemo::test::ProposeMetadataCommandWithRetry(
                cluster.Nodes(),
                raftdemo::test::MakeCommitObjectCommand(
                    bucket, object_key, "obj-" + object_key,
                    "integration-boundary-commit-fill-" + std::to_string(i)),
                std::chrono::seconds(10), &result, excluded))
            << "boundary fill commit failed, status=" << ProposeStatusName(result.status)
            << ", message=" << result.message;
      }

      ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                          "last_snapshot_index", 4,
                                          std::chrono::seconds(20)))
          << "leader did not compact logs before follower restart, describe="
          << cluster.Nodes()[leader_index]->Describe();

      const auto leader_snapshot_index =
          ExtractUintField(cluster.Nodes()[leader_index]->Describe(), "last_snapshot_index");
      ASSERT_TRUE(leader_snapshot_index.has_value())
          << "leader snapshot index missing from describe output";

      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(),
                      raftdemo::test::MakeDeleteObjectCommand(
                          bucket, "boundary_key", "obj-boundary-key",
                          "integration-boundary-delete-seed"),
                      std::chrono::seconds(10), &result, excluded))
          << "boundary delete failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(),
                      raftdemo::test::MakeCreateObjectCommand(
                          bucket, "boundary_tail", "obj-boundary-tail",
                          "integration-boundary-create-tail"),
                      std::chrono::seconds(10), &result, excluded))
          << "boundary tail create failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
                      cluster.Nodes(),
                      raftdemo::test::MakeCommitObjectCommand(
                          bucket, "boundary_tail", "obj-boundary-tail",
                          "integration-boundary-commit-tail"),
                      std::chrono::seconds(10), &result, excluded))
          << "boundary tail commit failed, status=" << ProposeStatusName(result.status)
          << ", message=" << result.message;

      visible_keys.push_back("boundary_tail");
      const raftdemo::test::MetadataRecoveryExpectation expected_state{
          .bucket = bucket,
          .objects = {
              {"boundary_key", "obj-boundary-key", 0U, true},
              {"boundary_fill_0", "obj-boundary_fill_0", 2U, false},
              {"boundary_fill_1", "obj-boundary_fill_1", 2U, false},
              {"boundary_fill_2", "obj-boundary_fill_2", 2U, false},
              {"boundary_fill_3", "obj-boundary_fill_3", 2U, false},
              {"boundary_fill_4", "obj-boundary_fill_4", 2U, false},
              {"boundary_fill_5", "obj-boundary_fill_5", 2U, false},
              {"boundary_fill_6", "obj-boundary_fill_6", 2U, false},
              {"boundary_fill_7", "obj-boundary_fill_7", 2U, false},
              {"boundary_tail", "obj-boundary-tail", 2U, false},
          },
          .visible_keys = visible_keys,
          .expected_request_count = 22U,
          .expected_tombstone_count = 1U,
          .expected_last_applied_index = result.log_index,
          .expected_min_last_applied_term = result.term,
      };

      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                      cluster.Nodes(), expected_state, std::chrono::seconds(10), excluded))
          << "surviving majority did not preserve metadata ordering after compaction";

      cluster.RestartNode(stopped_follower);

      ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[stopped_follower],
                                          "last_snapshot_index",
                                          *leader_snapshot_index,
                                          std::chrono::seconds(30)))
          << "lagging follower did not install retained snapshot before tail replay, describe="
          << cluster.Nodes()[stopped_follower]->Describe();
      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                      cluster.Nodes(), expected_state, std::chrono::seconds(20)))
          << "cluster did not converge on metadata snapshot + tail replay state";

      const MetadataStateMachine *restarted_state_machine =
          cluster.Nodes()[stopped_follower]->GetMetadataStateMachineV2();
      ASSERT_NE(restarted_state_machine, nullptr);
      EXPECT_EQ(restarted_state_machine->RequestCount(), 22U);
      EXPECT_EQ(restarted_state_machine->TombstoneCount(), 1U);
      EXPECT_GE(restarted_state_machine->LastAppliedIndex(), result.log_index);
      EXPECT_GE(restarted_state_machine->LastAppliedTerm(), result.term);
    }

  } // namespace
} // namespace raftdemo
