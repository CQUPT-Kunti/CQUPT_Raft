#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "raft/common/command.h"
#include "raft/common/config.h"
#include "raft/common/metadata_command.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"
#include "raft/storage/raft_storage.h"
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

    bool KeepTestData()
    {
      const char *value = std::getenv("RAFT_TEST_KEEP_DATA");
      return value != nullptr && std::string(value) == "1";
    }

    const char *ExpectedLinuxSpecificMarker()
    {
#if defined(__linux__)
      return "linux_specific=true";
#else
      return "linux_specific=false";
#endif
    }

    int RandomBasePort()
    {
      // Keep ports in a high range and leave enough room for +1/+2/+3.
      // This avoids collisions with the older fixed-port tests when running a large test set.
      static std::random_device rd;
      static std::mt19937 rng(rd());
      std::uniform_int_distribution<int> dist(24000, 52000);
      return dist(rng);
    }

    struct RunningCluster
    {
      RunningCluster() = default;
      RunningCluster(const RunningCluster &) = delete;
      RunningCluster &operator=(const RunningCluster &) = delete;

      RunningCluster(RunningCluster &&other) noexcept
          : nodes(std::move(other.nodes)), threads(std::move(other.threads)) {}

      RunningCluster &operator=(RunningCluster &&other) noexcept
      {
        if (this != &other)
        {
          Stop();
          nodes = std::move(other.nodes);
          threads = std::move(other.threads);
        }
        return *this;
      }

      ~RunningCluster() { Stop(); }

      void Stop()
      {
        for (auto &node : nodes)
        {
          if (node)
          {
            node->Stop();
          }
        }

        for (auto &t : threads)
        {
          if (t.joinable())
          {
            t.join();
          }
        }

        threads.clear();
      }

      std::vector<std::shared_ptr<RaftNode>> nodes;
      std::vector<std::thread> threads;
    };

    class ScopedDataDir
    {
    public:
      explicit ScopedDataDir(std::string name)
      {
        std::random_device rd;
        std::error_code ec;
        root_ = TestBinaryDir() / "raft_test_data" / "persistence" /
                (std::move(name) + "_" + std::to_string(NowForPath()) + "_" +
                 std::to_string(rd()));
        fs::remove_all(root_, ec);
        ec.clear();
        fs::create_directories(root_, ec);
      }

      ~ScopedDataDir()
      {
        if (KeepTestData())
        {
          std::cout << "preserved test root: " << root_ << std::endl;
          return;
        }

        std::error_code ec;
        fs::remove_all(root_, ec);
      }

      const fs::path &path() const { return root_; }

    private:
      fs::path root_;
    };

    std::string DescribeAllNodes(const std::vector<std::shared_ptr<RaftNode>> &nodes)
    {
      std::ostringstream oss;
      for (std::size_t i = 0; i < nodes.size(); ++i)
      {
        oss << "node[" << i << "] ";
        if (!nodes[i])
        {
          oss << "<null>\n";
        }
        else
        {
          oss << nodes[i]->Describe() << "\n";
        }
      }
      return oss.str();
    }

    bool IsLeaderNode(const std::shared_ptr<RaftNode> &node)
    {
      return node != nullptr && node->Describe().find("role=Leader") != std::string::npos;
    }

    std::shared_ptr<RaftNode> WaitForLeader(
        const std::vector<std::shared_ptr<RaftNode>> &nodes,
        std::chrono::milliseconds timeout)
    {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline)
      {
        for (const auto &node : nodes)
        {
          if (IsLeaderNode(node))
          {
            return node;
          }
        }
        std::this_thread::sleep_for(100ms);
      }
      return nullptr;
    }

    std::vector<NodeConfig> BuildThreeNodeConfigs(const fs::path &data_root, int base_port)
    {
      std::vector<NodeConfig> configs(3);

      for (int i = 0; i < 3; ++i)
      {
        const int node_id = i + 1;
        NodeConfig cfg;
        cfg.node_id = node_id;
        cfg.address = "127.0.0.1:" + std::to_string(base_port + node_id);
        cfg.election_timeout_min = std::chrono::milliseconds(700);
        cfg.election_timeout_max = std::chrono::milliseconds(1400);
        cfg.heartbeat_interval = std::chrono::milliseconds(120);
        cfg.rpc_deadline = std::chrono::milliseconds(700);
        cfg.data_dir = (data_root / ("node_" + std::to_string(node_id))).string();

        for (int peer_id = 1; peer_id <= 3; ++peer_id)
        {
          if (peer_id == node_id)
          {
            continue;
          }
          cfg.peers.push_back(
              PeerConfig{peer_id, "127.0.0.1:" + std::to_string(base_port + peer_id)});
        }

        configs[i] = std::move(cfg);
      }

      return configs;
    }

    snapshotConfig BuildPersistenceSnapshotConfig(const fs::path &root, int node_id)
    {
      snapshotConfig snapshot_config;
      snapshot_config.enabled = false;
      snapshot_config.load_on_startup = false;
      snapshot_config.snapshot_dir = (root / "snapshots" / ("node_" + std::to_string(node_id))).string();
      return snapshot_config;
    }

    constexpr const char *kPersistenceBoundaryBucket = "persistence-boundary-bucket";
    constexpr const char *kBoundaryAlphaKey = "boundary_alpha";
    constexpr const char *kBoundaryAlphaObjectId = "obj-boundary-alpha";
    constexpr const char *kBoundaryBetaKey = "boundary_beta";
    constexpr const char *kBoundaryBetaObjectId = "obj-boundary-beta";

    std::vector<LogRecord> BuildPersistenceBoundaryLog(const std::uint64_t term)
    {
      return {
          LogRecord{1,
                    term,
                    raftdemo::SerializeMetadataCommand(
                        raftdemo::test::MakeCreateBucketCommand(
                            kPersistenceBoundaryBucket,
                            "persistence-boundary-create-bucket"))},
          LogRecord{2,
                    term,
                    raftdemo::SerializeMetadataCommand(
                        raftdemo::test::MakeCreateObjectCommand(
                            kPersistenceBoundaryBucket,
                            kBoundaryAlphaKey,
                            kBoundaryAlphaObjectId,
                            "persistence-boundary-create-alpha"))},
          LogRecord{3,
                    term,
                    raftdemo::SerializeMetadataCommand(
                        raftdemo::test::MakeCommitObjectCommand(
                            kPersistenceBoundaryBucket,
                            kBoundaryAlphaKey,
                            kBoundaryAlphaObjectId,
                            "persistence-boundary-commit-alpha"))},
          LogRecord{4,
                    term,
                    raftdemo::SerializeMetadataCommand(
                        raftdemo::test::MakeCreateObjectCommand(
                            kPersistenceBoundaryBucket,
                            kBoundaryBetaKey,
                            kBoundaryBetaObjectId,
                            "persistence-boundary-create-beta"))},
          LogRecord{5,
                    term,
                    raftdemo::SerializeMetadataCommand(
                        raftdemo::test::MakeCommitObjectCommand(
                            kPersistenceBoundaryBucket,
                            kBoundaryBetaKey,
                            kBoundaryBetaObjectId,
                            "persistence-boundary-commit-beta"))},
      };
    }

    PersistentRaftState MakePersistenceState(std::uint64_t first_index, std::uint64_t last_index)
    {
      PersistentRaftState state;
      state.current_term = 5;
      state.voted_for = 1;
      state.commit_index = last_index;
      state.last_applied = last_index;

      const auto log = BuildPersistenceBoundaryLog(state.current_term);
      for (const auto &record : log)
      {
        if (record.index >= first_index && record.index <= last_index)
        {
          state.log.push_back(record);
        }
      }

      return state;
    }

    PersistentRaftState MakePersistenceStateWithHardState(std::uint64_t first_index,
                                                          std::uint64_t last_index,
                                                          std::uint64_t current_term,
                                                          std::int64_t voted_for,
                                                          std::uint64_t commit_index,
                                                          std::uint64_t last_applied)
    {
      PersistentRaftState state;
      state.current_term = current_term;
      state.voted_for = voted_for;
      state.commit_index = commit_index;
      state.last_applied = last_applied;

      const auto log = BuildPersistenceBoundaryLog(current_term);
      for (const auto &record : log)
      {
        if (record.index >= first_index && record.index <= last_index)
        {
          state.log.push_back(record);
        }
      }

      return state;
    }

    void SetEnvVar(const char *name, const char *value)
    {
#if defined(_WIN32)
      ASSERT_EQ(_putenv_s(name, value), 0);
#else
      ASSERT_EQ(setenv(name, value, 1), 0);
#endif
    }

    void UnsetEnvVar(const char *name)
    {
#if defined(_WIN32)
      ASSERT_EQ(_putenv_s(name, ""), 0);
#else
      ASSERT_EQ(unsetenv(name), 0);
#endif
    }

    class ScopedEnvVar
    {
    public:
      ScopedEnvVar(const char *name, const char *value) : name_(name)
      {
        const char *existing = std::getenv(name_);
        if (existing != nullptr)
        {
          had_old_value_ = true;
          old_value_ = existing;
        }
        SetEnvVar(name_, value);
      }

      ~ScopedEnvVar()
      {
        if (had_old_value_)
        {
          SetEnvVar(name_, old_value_.c_str());
        }
        else
        {
          UnsetEnvVar(name_);
        }
      }

    private:
      const char *name_;
      bool had_old_value_{false};
      std::string old_value_;
    };

    void ExpectInjectedFailure(const std::string &error,
                               const std::string &operation,
                               const fs::path &path,
                               const std::string &failure_class,
                               const std::string &trusted_state_expectation,
                               const std::string &diagnostic_expectation)
    {
      EXPECT_NE(error.find("injected durability failure"), std::string::npos) << error;
      EXPECT_NE(error.find("operation=" + operation), std::string::npos) << error;
      EXPECT_NE(error.find("path=" + path.string()), std::string::npos) << error;
      EXPECT_NE(error.find("failure_class=" + failure_class), std::string::npos) << error;
      EXPECT_NE(error.find(ExpectedLinuxSpecificMarker()), std::string::npos) << error;
      EXPECT_NE(error.find("trusted_state_expectation=" + trusted_state_expectation), std::string::npos)
          << error;
      EXPECT_NE(error.find("recovery_expectation=" + trusted_state_expectation), std::string::npos)
          << error;
      EXPECT_NE(error.find("diagnostic_expectation=" + diagnostic_expectation), std::string::npos)
          << error;
    }

    RunningCluster StartCluster(const std::vector<NodeConfig> &configs, const fs::path &test_root)
    {
      RunningCluster cluster;
      cluster.nodes.reserve(configs.size());

      for (const auto &cfg : configs)
      {
        cluster.nodes.push_back(
            std::make_shared<RaftNode>(cfg, BuildPersistenceSnapshotConfig(test_root, cfg.node_id)));
      }

      cluster.threads.reserve(cluster.nodes.size());
      for (const auto &node : cluster.nodes)
      {
        cluster.threads.emplace_back([node]()
                                     {
      node->Start();
      node->Wait(); });
      }

      return cluster;
    }

    void StopCluster(RunningCluster *cluster)
    {
      if (cluster != nullptr)
      {
        cluster->Stop();
      }
    }

    bool ProposeSetToLeader(const std::shared_ptr<RaftNode> &leader,
                            const std::string &key,
                            const std::string &value)
    {
      if (leader == nullptr)
      {
        return false;
      }

      Command cmd;
      cmd.type = CommandType::kSet;
      cmd.key = key;
      cmd.value = value;

      const ProposeResult result = leader->Propose(cmd);
      return result.Ok();
    }

    bool ProposeSetWithRetry(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                             const std::string &key,
                             const std::string &value,
                             std::chrono::milliseconds timeout)
    {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline)
      {
        auto leader = WaitForLeader(nodes, 500ms);
        if (leader != nullptr && ProposeSetToLeader(leader, key, value))
        {
          return true;
        }
        std::this_thread::sleep_for(100ms);
      }
      return false;
    }

    bool WaitUntilValue(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                        const std::string &key,
                        const std::string &expected_value,
                        std::chrono::milliseconds timeout)
    {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline)
      {
        bool all_ok = true;
        for (const auto &node : nodes)
        {
          if (node == nullptr)
          {
            all_ok = false;
            break;
          }

          std::string actual;
          if (!node->DebugGetValue(key, &actual) || actual != expected_value)
          {
            all_ok = false;
            break;
          }
        }

        if (all_ok)
        {
          return true;
        }

        std::this_thread::sleep_for(100ms);
      }

      return false;
    }

    bool ProposeMetadataWithRetry(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                  const MetadataCommand &command,
                                  std::chrono::milliseconds timeout,
                                  ProposeResult *last_result = nullptr)
    {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline)
      {
        auto leader = WaitForLeader(nodes, 500ms);
        if (leader != nullptr)
        {
          const ProposeResult result =
              raftdemo::test::ProposeMetadataCommand(leader, command);
          if (last_result != nullptr)
          {
            *last_result = result;
          }
          if (result.status == ProposeStatus::kOk)
          {
            return true;
          }
        }

        std::this_thread::sleep_for(100ms);
      }

      return false;
    }

    bool ExtractIntField(const std::string &description,
                         const std::string &field_name,
                         int *value)
    {
      if (value == nullptr)
      {
        return false;
      }

      const std::string needle = field_name + "=";
      const std::size_t start = description.find(needle);
      if (start == std::string::npos)
      {
        return false;
      }

      std::size_t pos = start + needle.size();
      if (pos < description.size() && description[pos] == '-')
      {
        ++pos;
      }

      std::size_t end = pos;
      while (end < description.size() &&
             description[end] >= '0' &&
             description[end] <= '9')
      {
        ++end;
      }
      if (end == pos)
      {
        return false;
      }

      try
      {
        *value = std::stoi(description.substr(start + needle.size(),
                                              end - (start + needle.size())));
        return true;
      }
      catch (...)
      {
        return false;
      }
    }

    void ExpectBoundaryObjectState(const MetadataStateMachine &state_machine,
                                   const std::string &object_key,
                                   const std::string &object_id,
                                   const bool expected_visible)
    {
      const auto response = state_machine.HeadObject(
          {.bucket = kPersistenceBoundaryBucket, .object_key = object_key});
      const auto indexed_object_id =
          state_machine.FindIndexedObjectId(kPersistenceBoundaryBucket, object_key);
      const auto chunk_refs =
          state_machine.FindChunkRefs(kPersistenceBoundaryBucket, object_key);
      const auto internal_object =
          state_machine.FindObject(kPersistenceBoundaryBucket, object_key);

      if (expected_visible)
      {
        ASSERT_TRUE(response.result.Ok());
        ASSERT_TRUE(response.record.has_value());
        EXPECT_TRUE(response.record->IsCommitted());
        EXPECT_EQ(response.record->object_id, object_id);
        EXPECT_TRUE(indexed_object_id.has_value());
        EXPECT_EQ(*indexed_object_id, object_id);
        EXPECT_TRUE(chunk_refs.has_value());
        EXPECT_EQ(chunk_refs->size(), 2U);
        EXPECT_TRUE(internal_object.has_value());
        EXPECT_TRUE(internal_object->IsCommitted());
      }
      else
      {
        EXPECT_EQ(response.result.code, MetadataStatusCode::kNotFound);
        EXPECT_FALSE(response.record.has_value());
        EXPECT_FALSE(chunk_refs.has_value());
      }
    }

    void ExpectBoundaryMetadataState(const std::shared_ptr<RaftNode> &node,
                                     const std::uint64_t expected_last_applied_index,
                                     const std::uint64_t expected_term,
                                     const bool expect_alpha_visible,
                                     const bool expect_beta_visible)
    {
      ASSERT_NE(node, nullptr);
      const MetadataStateMachine *state_machine = node->GetMetadataStateMachineV2();
      ASSERT_NE(state_machine, nullptr) << node->Describe();

      const auto bucket = state_machine->FindBucket(kPersistenceBoundaryBucket);
      ASSERT_TRUE(bucket.has_value()) << node->Describe();
      EXPECT_TRUE(bucket->IsActive()) << node->Describe();

      EXPECT_EQ(state_machine->LastAppliedIndex(), expected_last_applied_index)
          << node->Describe();
      EXPECT_EQ(state_machine->LastAppliedTerm(),
                expected_last_applied_index == 0 ? 0U : expected_term)
          << node->Describe();
      EXPECT_EQ(state_machine->RequestCount(),
                static_cast<std::size_t>(expected_last_applied_index))
          << node->Describe();
      EXPECT_EQ(state_machine->TombstoneCount(), 0U) << node->Describe();

      ExpectBoundaryObjectState(*state_machine,
                                kBoundaryAlphaKey,
                                kBoundaryAlphaObjectId,
                                expect_alpha_visible);
      ExpectBoundaryObjectState(*state_machine,
                                kBoundaryBetaKey,
                                kBoundaryBetaObjectId,
                                expect_beta_visible);
    }

    TEST(PersistenceTest, FullClusterRestartRecovery)
    {
      ScopedDataDir scoped_dir("test_full_restart");
      const int base_port = RandomBasePort();
      auto configs = BuildThreeNodeConfigs(scoped_dir.path() / "raft_data", base_port);

      auto cluster = StartCluster(configs, scoped_dir.path());
      ASSERT_NE(WaitForLeader(cluster.nodes, 10s), nullptr)
          << DescribeAllNodes(cluster.nodes);

      const std::string bucket = "restart-cluster-bucket";
      const auto create_bucket = raftdemo::test::MakeCreateBucketCommand(
          bucket, "restart-cluster-create-bucket-1");
      const auto create_alpha = raftdemo::test::MakeCreateObjectCommand(
          bucket, "alpha", "obj-alpha", "restart-cluster-create-alpha-1");
      const auto commit_alpha = raftdemo::test::MakeCommitObjectCommand(
          bucket, "alpha", "obj-alpha", "restart-cluster-commit-alpha-1");
      const auto create_gone = raftdemo::test::MakeCreateObjectCommand(
          bucket, "gone", "obj-gone", "restart-cluster-create-gone-1");
      const auto commit_gone = raftdemo::test::MakeCommitObjectCommand(
          bucket, "gone", "obj-gone", "restart-cluster-commit-gone-1");
      const auto delete_gone = raftdemo::test::MakeDeleteObjectCommand(
          bucket, "gone", "obj-gone", "restart-cluster-delete-gone-1");

      ProposeResult delete_result;
      ASSERT_TRUE(ProposeMetadataWithRetry(cluster.nodes, create_bucket, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(cluster.nodes, create_alpha, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(cluster.nodes, commit_alpha, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(cluster.nodes, create_gone, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(cluster.nodes, commit_gone, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(cluster.nodes, delete_gone, 10s, &delete_result))
          << DescribeAllNodes(cluster.nodes);

      const raftdemo::test::MetadataRecoveryExpectation expected_state{
          .bucket = bucket,
          .objects =
              {
                  {"alpha", "obj-alpha", 2U, false},
                  {"gone", "obj-gone", 0U, true},
              },
          .visible_keys = {"alpha"},
          .expected_request_count = 6U,
          .expected_tombstone_count = 1U,
          .expected_last_applied_index = delete_result.log_index,
          .expected_min_last_applied_term = delete_result.term,
      };

      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                      cluster.nodes, expected_state, 10s))
          << DescribeAllNodes(cluster.nodes);

      StopCluster(&cluster);
      std::this_thread::sleep_for(1200ms);

      cluster = StartCluster(configs, scoped_dir.path());
      ASSERT_NE(WaitForLeader(cluster.nodes, 10s), nullptr)
          << DescribeAllNodes(cluster.nodes);

      EXPECT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                      cluster.nodes, expected_state, 12s))
          << DescribeAllNodes(cluster.nodes);

      StopCluster(&cluster);
    }

    TEST(PersistenceTest, RestartedFollowerCatchesUp)
    {
      ScopedDataDir scoped_dir("test_follower_restart");
      const int base_port = RandomBasePort();
      auto configs = BuildThreeNodeConfigs(scoped_dir.path() / "raft_data", base_port);

      auto cluster = StartCluster(configs, scoped_dir.path());
      auto leader = WaitForLeader(cluster.nodes, 10s);
      ASSERT_NE(leader, nullptr) << DescribeAllNodes(cluster.nodes);

      std::shared_ptr<RaftNode> follower;
      for (const auto &node : cluster.nodes)
      {
        if (node != leader)
        {
          follower = node;
          break;
        }
      }
      ASSERT_NE(follower, nullptr) << DescribeAllNodes(cluster.nodes);

      const std::string bucket = "restart-follower-bucket";
      const auto create_bucket = raftdemo::test::MakeCreateBucketCommand(
          bucket, "restart-follower-create-bucket-1");
      const auto create_first = raftdemo::test::MakeCreateObjectCommand(
          bucket, "first", "obj-first", "restart-follower-create-first-1");
      const auto commit_first = raftdemo::test::MakeCommitObjectCommand(
          bucket, "first", "obj-first", "restart-follower-commit-first-1");
      const auto create_second = raftdemo::test::MakeCreateObjectCommand(
          bucket, "second", "obj-second", "restart-follower-create-second-1");
      const auto commit_second = raftdemo::test::MakeCommitObjectCommand(
          bucket, "second", "obj-second", "restart-follower-commit-second-1");
      const auto create_gone = raftdemo::test::MakeCreateObjectCommand(
          bucket, "gone", "obj-gone", "restart-follower-create-gone-1");
      const auto commit_gone = raftdemo::test::MakeCommitObjectCommand(
          bucket, "gone", "obj-gone", "restart-follower-commit-gone-1");
      const auto delete_gone = raftdemo::test::MakeDeleteObjectCommand(
          bucket, "gone", "obj-gone", "restart-follower-delete-gone-1");

      ProposeResult commit_first_result;
      ASSERT_TRUE(ProposeMetadataWithRetry(cluster.nodes, create_bucket, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(cluster.nodes, create_first, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(cluster.nodes, commit_first, 10s, &commit_first_result))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                      cluster.nodes,
                      {.bucket = bucket,
                       .objects = {{"first", "obj-first", 2U, false}},
                       .visible_keys = {"first"},
                       .expected_request_count = 3U,
                       .expected_tombstone_count = 0U,
                       .expected_last_applied_index = commit_first_result.log_index},
                      10s))
          << DescribeAllNodes(cluster.nodes);

      follower->Stop();
      std::this_thread::sleep_for(1200ms);

      std::vector<std::shared_ptr<RaftNode>> alive_nodes;
      for (const auto &node : cluster.nodes)
      {
        if (node != follower)
        {
          alive_nodes.push_back(node);
        }
      }

      ProposeResult delete_result;
      ASSERT_TRUE(ProposeMetadataWithRetry(alive_nodes, create_second, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(alive_nodes, commit_second, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(alive_nodes, create_gone, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(alive_nodes, commit_gone, 10s))
          << DescribeAllNodes(cluster.nodes);
      ASSERT_TRUE(ProposeMetadataWithRetry(alive_nodes, delete_gone, 10s, &delete_result))
          << DescribeAllNodes(cluster.nodes);

      const raftdemo::test::MetadataRecoveryExpectation expected_state{
          .bucket = bucket,
          .objects =
              {
                  {"first", "obj-first", 2U, false},
                  {"second", "obj-second", 2U, false},
                  {"gone", "obj-gone", 0U, true},
              },
          .visible_keys = {"first", "second"},
          .expected_request_count = 8U,
          .expected_tombstone_count = 1U,
          .expected_last_applied_index = delete_result.log_index,
          .expected_min_last_applied_term = delete_result.term,
      };
      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                      alive_nodes, expected_state, 10s))
          << DescribeAllNodes(cluster.nodes);

      const auto follower_it = std::find(cluster.nodes.begin(), cluster.nodes.end(), follower);
      ASSERT_NE(follower_it, cluster.nodes.end());

      const std::size_t follower_index =
          static_cast<std::size_t>(std::distance(cluster.nodes.begin(), follower_it));

      if (cluster.threads[follower_index].joinable())
      {
        cluster.threads[follower_index].join();
      }

      auto restarted_follower = std::make_shared<RaftNode>(
          configs[follower_index],
          BuildPersistenceSnapshotConfig(scoped_dir.path(), configs[follower_index].node_id));
      cluster.nodes[follower_index] = restarted_follower;
      cluster.threads[follower_index] = std::thread([restarted_follower]()
                                                    {
    restarted_follower->Start();
    restarted_follower->Wait(); });

      EXPECT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                      cluster.nodes, expected_state, 12s))
          << DescribeAllNodes(cluster.nodes);

      const MetadataStateMachine *restarted_state_machine =
          restarted_follower->GetMetadataStateMachineV2();
      ASSERT_NE(restarted_state_machine, nullptr);
      EXPECT_GE(restarted_state_machine->LastAppliedIndex(), delete_result.log_index);
      EXPECT_GE(restarted_state_machine->LastAppliedTerm(), delete_result.term);
      EXPECT_EQ(restarted_state_machine->RequestCount(), 8U);
      EXPECT_EQ(restarted_state_machine->TombstoneCount(), 1U);

      StopCluster(&cluster);
    }

    TEST(PersistenceTest, ColdRestartPreservesPersistedHardStateBeforeStart)
    {
      ScopedDataDir scoped_dir("test_hard_state_cold_restart");

      NodeConfig config;
      config.node_id = 1;
      config.address = "127.0.0.1:" + std::to_string(RandomBasePort());
      config.election_timeout_min = std::chrono::milliseconds(250);
      config.election_timeout_max = std::chrono::milliseconds(400);
      config.heartbeat_interval = std::chrono::milliseconds(80);
      config.rpc_deadline = std::chrono::milliseconds(500);
      config.data_dir = (scoped_dir.path() / "raft_data" / "node_1").string();

      const auto snapshot_config = BuildPersistenceSnapshotConfig(scoped_dir.path(), config.node_id);
      auto node = std::make_shared<RaftNode>(config, snapshot_config);
      std::thread worker([node]()
                         {
        node->Start();
        node->Wait(); });

      ASSERT_NE(WaitForLeader({node}, 8s), nullptr) << node->Describe();

      const std::string bucket = "hard-state-bucket";
      const auto create_bucket = raftdemo::test::MakeCreateBucketCommand(
          bucket, "hard-state-create-bucket-1");
      const auto create_alpha = raftdemo::test::MakeCreateObjectCommand(
          bucket, "alpha", "obj-hard-alpha", "hard-state-create-alpha-1");
      const auto commit_alpha = raftdemo::test::MakeCommitObjectCommand(
          bucket, "alpha", "obj-hard-alpha", "hard-state-commit-alpha-1");
      const auto create_gone = raftdemo::test::MakeCreateObjectCommand(
          bucket, "gone", "obj-hard-gone", "hard-state-create-gone-1");
      const auto commit_gone = raftdemo::test::MakeCommitObjectCommand(
          bucket, "gone", "obj-hard-gone", "hard-state-commit-gone-1");
      const auto delete_gone = raftdemo::test::MakeDeleteObjectCommand(
          bucket, "gone", "obj-hard-gone", "hard-state-delete-gone-1");

      ProposeResult delete_result;
      ASSERT_TRUE(ProposeMetadataWithRetry({node}, create_bucket, 6s)) << node->Describe();
      ASSERT_TRUE(ProposeMetadataWithRetry({node}, create_alpha, 6s)) << node->Describe();
      ASSERT_TRUE(ProposeMetadataWithRetry({node}, commit_alpha, 6s)) << node->Describe();
      ASSERT_TRUE(ProposeMetadataWithRetry({node}, create_gone, 6s)) << node->Describe();
      ASSERT_TRUE(ProposeMetadataWithRetry({node}, commit_gone, 6s)) << node->Describe();
      ASSERT_TRUE(ProposeMetadataWithRetry({node}, delete_gone, 6s, &delete_result))
          << node->Describe();

      const raftdemo::test::MetadataRecoveryExpectation expected_state{
          .bucket = bucket,
          .objects =
              {
                  {"alpha", "obj-hard-alpha", 2U, false},
                  {"gone", "obj-hard-gone", 0U, true},
              },
          .visible_keys = {"alpha"},
          .expected_request_count = 6U,
          .expected_tombstone_count = 1U,
          .expected_last_applied_index = delete_result.log_index,
          .expected_min_last_applied_term = delete_result.term,
      };
      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                      {node}, expected_state, 6s))
          << node->Describe();

      const NodeStatusSnapshot before_status = node->GetStatusSnapshot();
      const std::string before_description = node->Describe();
      int before_voted_for = -1;
      ASSERT_TRUE(ExtractIntField(before_description, "voted_for", &before_voted_for))
          << before_description;
      const MetadataStateMachine *before_state_machine = node->GetMetadataStateMachineV2();
      ASSERT_NE(before_state_machine, nullptr);
      EXPECT_EQ(before_state_machine->LastAppliedIndex(), delete_result.log_index);
      EXPECT_EQ(before_state_machine->LastAppliedTerm(), delete_result.term);

      node->Stop();
      if (worker.joinable())
      {
        worker.join();
      }

      auto restarted = std::make_shared<RaftNode>(config, snapshot_config);
      const NodeStatusSnapshot after_status = restarted->GetStatusSnapshot();
      const std::string after_description = restarted->Describe();
      int after_voted_for = -1;
      ASSERT_TRUE(ExtractIntField(after_description, "voted_for", &after_voted_for))
          << after_description;
      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
                      {restarted}, expected_state, 500ms))
          << after_description;

      const MetadataStateMachine *restarted_state_machine =
          restarted->GetMetadataStateMachineV2();
      ASSERT_NE(restarted_state_machine, nullptr);

      EXPECT_EQ(after_status.term, before_status.term) << after_description;
      EXPECT_EQ(after_status.commit_index, before_status.commit_index) << after_description;
      EXPECT_EQ(after_status.last_applied, before_status.last_applied) << after_description;
      EXPECT_EQ(after_voted_for, before_voted_for) << after_description;
      EXPECT_EQ(restarted_state_machine->LastAppliedIndex(), delete_result.log_index);
      EXPECT_EQ(restarted_state_machine->LastAppliedTerm(), delete_result.term);

      const fs::path probe_dir = scoped_dir.path() / "metadata_recovery_probe";
      std::error_code ec;
      fs::create_directories(probe_dir, ec);
      ASSERT_FALSE(ec) << ec.message();

      const fs::path probe_snapshot = probe_dir / "cold-restart-recovered.snapshot";
      const SnapshotResult save_result =
          restarted_state_machine->SaveSnapshot(probe_snapshot.string());
      ASSERT_EQ(save_result.status, SnapshotStatus::kOk) << save_result.message;

      MetadataStateMachine replay_probe;
      const SnapshotResult load_result = replay_probe.LoadSnapshot(probe_snapshot.string());
      ASSERT_EQ(load_result.status, SnapshotStatus::kOk) << load_result.message;
      EXPECT_EQ(replay_probe.RequestCount(), 6U);
      EXPECT_EQ(replay_probe.TombstoneCount(), 1U);
      EXPECT_EQ(replay_probe.LastAppliedIndex(), delete_result.log_index);
      EXPECT_EQ(replay_probe.LastAppliedTerm(), delete_result.term);

      const ApplyResult delete_replay = replay_probe.Apply(
          delete_result.log_index + 1,
          SerializeMetadataCommand(delete_gone));
      EXPECT_TRUE(delete_replay.Ok);
      EXPECT_EQ(delete_replay.message, "idempotent replay");

      const ApplyResult commit_deleted_replay = replay_probe.Apply(
          delete_result.log_index + 2,
          SerializeMetadataCommand(commit_gone));
      EXPECT_TRUE(commit_deleted_replay.Ok);
      EXPECT_EQ(commit_deleted_replay.message, "idempotent replay");

      const auto gone_after_replay =
          replay_probe.HeadObject({.bucket = bucket, .object_key = "gone"});
      EXPECT_EQ(gone_after_replay.result.code, MetadataStatusCode::kNotFound);
      EXPECT_FALSE(gone_after_replay.record.has_value());
      EXPECT_FALSE(replay_probe.FindIndexedObjectId(bucket, "gone").has_value());
      EXPECT_FALSE(replay_probe.FindChunkRefs(bucket, "gone").has_value());
      EXPECT_EQ(replay_probe.RequestCount(), 6U);
      EXPECT_EQ(replay_probe.TombstoneCount(), 1U);

      const MetadataCommand conflicting_delete = raftdemo::test::MakeDeleteObjectCommand(
          bucket, "alpha", "obj-hard-alpha", "hard-state-delete-gone-1");
      const ApplyResult conflict = replay_probe.Apply(
          delete_result.log_index + 3,
          SerializeMetadataCommand(conflicting_delete));
      EXPECT_FALSE(conflict.Ok);
      EXPECT_EQ(conflict.message,
                "idempotency conflict: request_id maps to different command");
      EXPECT_EQ(replay_probe.RequestCount(), 6U);
      EXPECT_EQ(replay_probe.TombstoneCount(), 1U);

      const auto alpha_after_conflict =
          replay_probe.HeadObject({.bucket = bucket, .object_key = "alpha"});
      ASSERT_EQ(alpha_after_conflict.result.code, MetadataStatusCode::kOk);
      ASSERT_TRUE(alpha_after_conflict.record.has_value());
      EXPECT_EQ(alpha_after_conflict.record->object_id, "obj-hard-alpha");
      EXPECT_TRUE(alpha_after_conflict.record->IsCommitted());
    }

    TEST(PersistenceTest, ColdRestartClampsCommitAndApplyBoundariesToLastLogIndex)
    {
      ScopedDataDir scoped_dir("test_recovery_boundary_clamp");

      NodeConfig config;
      config.node_id = 1;
      config.address = "127.0.0.1:" + std::to_string(RandomBasePort());
      config.election_timeout_min = std::chrono::milliseconds(250);
      config.election_timeout_max = std::chrono::milliseconds(400);
      config.heartbeat_interval = std::chrono::milliseconds(80);
      config.rpc_deadline = std::chrono::milliseconds(500);
      config.data_dir = (scoped_dir.path() / "raft_data" / "node_1").string();

      const auto persisted = MakePersistenceStateWithHardState(1, 3, 3, 1, 99, 99);

      std::string error;
      auto storage = CreateFileRaftStorage(config.data_dir);
      ASSERT_TRUE(storage->Save(persisted, &error)) << error;

      const auto snapshot_config = BuildPersistenceSnapshotConfig(scoped_dir.path(), config.node_id);
      auto restarted = std::make_shared<RaftNode>(config, snapshot_config);

      const NodeStatusSnapshot status = restarted->GetStatusSnapshot();
      EXPECT_EQ(status.commit_index, 3U) << restarted->Describe();
      EXPECT_EQ(status.last_applied, 3U) << restarted->Describe();
      EXPECT_EQ(status.last_log_index, 3U) << restarted->Describe();

      ExpectBoundaryMetadataState(restarted, 3U, 3U, true, false);
    }

    TEST(PersistenceTest, ColdRestartUsesPreviouslyTrustedMetaBoundaryWhenNewLogPublishesBeforeMeta)
    {
      ScopedDataDir scoped_dir("test_restart_old_meta_new_log_boundary");

      NodeConfig config;
      config.node_id = 1;
      config.address = "127.0.0.1:" + std::to_string(RandomBasePort());
      config.election_timeout_min = std::chrono::milliseconds(250);
      config.election_timeout_max = std::chrono::milliseconds(400);
      config.heartbeat_interval = std::chrono::milliseconds(80);
      config.rpc_deadline = std::chrono::milliseconds(500);
      config.data_dir = (scoped_dir.path() / "raft_data" / "node_1").string();

      const fs::path storage_root = config.data_dir;
      const fs::path saved_root = scoped_dir.path() / "saved_publish_states";
      auto storage = CreateFileRaftStorage(config.data_dir);
      const auto old_state = MakePersistenceState(1, 3);
      const auto new_state = MakePersistenceState(1, 5);
      std::string error;

      ASSERT_TRUE(storage->Save(old_state, &error)) << error;

      std::error_code ec;
      fs::create_directories(saved_root, ec);
      ASSERT_FALSE(ec) << ec.message();
      fs::copy_file(storage_root / "meta.bin", saved_root / "old_meta.bin",
                    fs::copy_options::overwrite_existing, ec);
      ASSERT_FALSE(ec) << ec.message();

      ASSERT_TRUE(storage->Save(new_state, &error)) << error;

      // Simulate a crash after the newer log/ became visible but before the newer
      // meta.bin publish completed. Restart must stay bounded by the last trusted meta.
      fs::copy_file(saved_root / "old_meta.bin", storage_root / "meta.bin",
                    fs::copy_options::overwrite_existing, ec);
      ASSERT_FALSE(ec) << ec.message();

      const auto snapshot_config = BuildPersistenceSnapshotConfig(scoped_dir.path(), config.node_id);
      auto restarted = std::make_shared<RaftNode>(config, snapshot_config);
      const NodeStatusSnapshot status = restarted->GetStatusSnapshot();
      const std::string description = restarted->Describe();
      int voted_for = -1;
      ASSERT_TRUE(ExtractIntField(description, "voted_for", &voted_for)) << description;

      EXPECT_EQ(status.term, old_state.current_term) << description;
      EXPECT_EQ(status.commit_index, old_state.commit_index) << description;
      EXPECT_EQ(status.last_applied, old_state.last_applied) << description;
      EXPECT_EQ(status.last_log_index, old_state.log.back().index) << description;
      EXPECT_EQ(voted_for, old_state.voted_for) << description;

      ExpectBoundaryMetadataState(restarted, 3U, new_state.current_term, true, false);
    }

    TEST(PersistenceTest, ColdRestartClampsCommitIndexToLastLogAndReplaysCommittedPrefix)
    {
      ScopedDataDir scoped_dir("test_recovery_commit_missing_log");

      NodeConfig config;
      config.node_id = 1;
      config.address = "127.0.0.1:" + std::to_string(RandomBasePort());
      config.election_timeout_min = std::chrono::milliseconds(250);
      config.election_timeout_max = std::chrono::milliseconds(400);
      config.heartbeat_interval = std::chrono::milliseconds(80);
      config.rpc_deadline = std::chrono::milliseconds(500);
      config.data_dir = (scoped_dir.path() / "raft_data" / "node_1").string();

      const auto persisted = MakePersistenceStateWithHardState(1, 3, 4, 2, 99, 1);
      std::string error;
      auto storage = CreateFileRaftStorage(config.data_dir);
      ASSERT_TRUE(storage->Save(persisted, &error)) << error;

      const auto snapshot_config = BuildPersistenceSnapshotConfig(scoped_dir.path(), config.node_id);
      auto restarted = std::make_shared<RaftNode>(config, snapshot_config);
      const NodeStatusSnapshot status = restarted->GetStatusSnapshot();
      const std::string description = restarted->Describe();
      int voted_for = -1;
      ASSERT_TRUE(ExtractIntField(description, "voted_for", &voted_for)) << description;

      EXPECT_EQ(status.term, 4U) << description;
      EXPECT_EQ(status.commit_index, 3U) << description;
      EXPECT_EQ(status.last_applied, 3U) << description;
      EXPECT_EQ(status.last_log_index, 3U) << description;
      EXPECT_EQ(voted_for, 2) << description;

      ExpectBoundaryMetadataState(restarted, 3U, 4U, true, false);
    }

    TEST(PersistenceTest, ColdRestartClampsLastAppliedToCommitIndexWhenAppliedExceedsCommit)
    {
      ScopedDataDir scoped_dir("test_recovery_last_applied_gt_commit");

      NodeConfig config;
      config.node_id = 1;
      config.address = "127.0.0.1:" + std::to_string(RandomBasePort());
      config.election_timeout_min = std::chrono::milliseconds(250);
      config.election_timeout_max = std::chrono::milliseconds(400);
      config.heartbeat_interval = std::chrono::milliseconds(80);
      config.rpc_deadline = std::chrono::milliseconds(500);
      config.data_dir = (scoped_dir.path() / "raft_data" / "node_1").string();

      const auto persisted = MakePersistenceStateWithHardState(1, 3, 4, 2, 2, 3);
      std::string error;
      auto storage = CreateFileRaftStorage(config.data_dir);
      ASSERT_TRUE(storage->Save(persisted, &error)) << error;

      const auto snapshot_config = BuildPersistenceSnapshotConfig(scoped_dir.path(), config.node_id);
      auto restarted = std::make_shared<RaftNode>(config, snapshot_config);
      const NodeStatusSnapshot status = restarted->GetStatusSnapshot();
      const std::string description = restarted->Describe();

      EXPECT_EQ(status.term, 4U) << description;
      EXPECT_EQ(status.commit_index, 2U) << description;
      EXPECT_EQ(status.last_applied, 2U) << description;
      EXPECT_EQ(status.last_log_index, 3U) << description;

      ExpectBoundaryMetadataState(restarted, 2U, 4U, false, false);
    }

    TEST(PersistenceTest, ColdRestartClampsLastAppliedToTrustedLogPrefixWhenAppliedPointsPastAvailableLog)
    {
      ScopedDataDir scoped_dir("test_recovery_last_applied_missing_log");

      NodeConfig config;
      config.node_id = 1;
      config.address = "127.0.0.1:" + std::to_string(RandomBasePort());
      config.election_timeout_min = std::chrono::milliseconds(250);
      config.election_timeout_max = std::chrono::milliseconds(400);
      config.heartbeat_interval = std::chrono::milliseconds(80);
      config.rpc_deadline = std::chrono::milliseconds(500);
      config.data_dir = (scoped_dir.path() / "raft_data" / "node_1").string();

      const auto persisted = MakePersistenceStateWithHardState(1, 3, 4, 2, 3, 99);
      std::string error;
      auto storage = CreateFileRaftStorage(config.data_dir);
      ASSERT_TRUE(storage->Save(persisted, &error)) << error;

      const auto snapshot_config = BuildPersistenceSnapshotConfig(scoped_dir.path(), config.node_id);
      auto restarted = std::make_shared<RaftNode>(config, snapshot_config);
      const NodeStatusSnapshot status = restarted->GetStatusSnapshot();
      const std::string description = restarted->Describe();

      EXPECT_EQ(status.term, 4U) << description;
      EXPECT_EQ(status.commit_index, 3U) << description;
      EXPECT_EQ(status.last_applied, 3U) << description;
      EXPECT_EQ(status.last_log_index, 3U) << description;

      ExpectBoundaryMetadataState(restarted, 3U, 4U, true, false);
    }

    TEST(PersistenceTest, ColdRestartUsesOlderMetaTermAndVoteWhenNewerLogTreeIsVisible)
    {
      ScopedDataDir scoped_dir("test_restart_old_meta_new_log_term_vote_boundary");

      NodeConfig config;
      config.node_id = 1;
      config.address = "127.0.0.1:" + std::to_string(RandomBasePort());
      config.election_timeout_min = std::chrono::milliseconds(250);
      config.election_timeout_max = std::chrono::milliseconds(400);
      config.heartbeat_interval = std::chrono::milliseconds(80);
      config.rpc_deadline = std::chrono::milliseconds(500);
      config.data_dir = (scoped_dir.path() / "raft_data" / "node_1").string();

      const fs::path storage_root = config.data_dir;
      const fs::path saved_root = scoped_dir.path() / "saved_publish_states";
      auto storage = CreateFileRaftStorage(config.data_dir);
      const auto old_state = MakePersistenceStateWithHardState(1, 3, 5, 1, 3, 3);
      const auto new_state = MakePersistenceStateWithHardState(1, 5, 9, 3, 5, 5);
      std::string error;

      ASSERT_TRUE(storage->Save(old_state, &error)) << error;

      std::error_code ec;
      fs::create_directories(saved_root, ec);
      ASSERT_FALSE(ec) << ec.message();
      fs::copy_file(storage_root / "meta.bin", saved_root / "old_meta.bin",
                    fs::copy_options::overwrite_existing, ec);
      ASSERT_FALSE(ec) << ec.message();

      ASSERT_TRUE(storage->Save(new_state, &error)) << error;
      fs::copy_file(saved_root / "old_meta.bin", storage_root / "meta.bin",
                    fs::copy_options::overwrite_existing, ec);
      ASSERT_FALSE(ec) << ec.message();

      const auto snapshot_config = BuildPersistenceSnapshotConfig(scoped_dir.path(), config.node_id);
      auto restarted = std::make_shared<RaftNode>(config, snapshot_config);
      const NodeStatusSnapshot status = restarted->GetStatusSnapshot();
      const std::string description = restarted->Describe();
      int voted_for = -1;
      ASSERT_TRUE(ExtractIntField(description, "voted_for", &voted_for)) << description;

      EXPECT_EQ(status.term, old_state.current_term) << description;
      EXPECT_EQ(status.commit_index, old_state.commit_index) << description;
      EXPECT_EQ(status.last_applied, old_state.last_applied) << description;
      EXPECT_EQ(status.last_log_index, old_state.log.back().index) << description;
      EXPECT_EQ(voted_for, old_state.voted_for) << description;

      ExpectBoundaryMetadataState(restarted, 3U, new_state.current_term, true, false);
    }

    TEST(PersistenceTest, NewMetaWithOldLogBoundaryRejectsUntrustedCurrentTermAndVote)
    {
      ScopedDataDir scoped_dir("test_restart_new_meta_old_log_term_vote_boundary");

      NodeConfig config;
      config.node_id = 1;
      config.address = "127.0.0.1:" + std::to_string(RandomBasePort());
      config.election_timeout_min = std::chrono::milliseconds(250);
      config.election_timeout_max = std::chrono::milliseconds(400);
      config.heartbeat_interval = std::chrono::milliseconds(80);
      config.rpc_deadline = std::chrono::milliseconds(500);
      config.data_dir = (scoped_dir.path() / "raft_data" / "node_1").string();

      const fs::path storage_root = config.data_dir;
      const fs::path saved_root = scoped_dir.path() / "saved_publish_states";
      auto storage = CreateFileRaftStorage(config.data_dir);
      const auto old_state = MakePersistenceStateWithHardState(1, 3, 5, 1, 3, 3);
      const auto new_state = MakePersistenceStateWithHardState(1, 5, 9, 3, 5, 5);
      std::string error;

      ASSERT_TRUE(storage->Save(old_state, &error)) << error;

      std::error_code ec;
      fs::create_directories(saved_root, ec);
      ASSERT_FALSE(ec) << ec.message();
      fs::copy(storage_root / "log", saved_root / "old_log",
               fs::copy_options::recursive |
                   fs::copy_options::overwrite_existing,
               ec);
      ASSERT_FALSE(ec) << ec.message();

      ASSERT_TRUE(storage->Save(new_state, &error)) << error;

      fs::remove_all(storage_root / "log", ec);
      ASSERT_FALSE(ec) << ec.message();
      fs::copy(saved_root / "old_log", storage_root / "log",
               fs::copy_options::recursive |
                   fs::copy_options::overwrite_existing,
               ec);
      ASSERT_FALSE(ec) << ec.message();

      PersistentRaftState rejected;
      bool has_state = false;
      ASSERT_FALSE(storage->Load(&rejected, &has_state, &error));
      EXPECT_FALSE(has_state);
      EXPECT_NE(error.find("log count mismatch"), std::string::npos) << error;
    }

    TEST(PersistenceTest, MetaFileSyncFailureNeedsExactFailureInjectionSeam)
    {
      ScopedDataDir scoped_dir("test_meta_file_sync_injection_contract");
      NodeConfig config;
      config.node_id = 1;
      config.address = "127.0.0.1:" + std::to_string(RandomBasePort());
      config.election_timeout_min = std::chrono::milliseconds(250);
      config.election_timeout_max = std::chrono::milliseconds(400);
      config.heartbeat_interval = std::chrono::milliseconds(80);
      config.rpc_deadline = std::chrono::milliseconds(500);
      config.data_dir = (scoped_dir.path() / "raft_data" / "node_1").string();

      const fs::path storage_root = config.data_dir;
      const fs::path injected_meta_tmp_path = storage_root / "meta.bin.tmp";
      const std::string trusted_state_expectation =
          "restart must keep the previously durable term/vote/commit_index/last_applied and must not expose newer hard state when meta file data was not durably synced";

      auto storage = CreateFileRaftStorage(config.data_dir);
      const auto old_state = MakePersistenceState(1, 3);
      const auto new_state = MakePersistenceState(1, 5);
      std::string error;

      ASSERT_TRUE(storage->Save(old_state, &error)) << error;
      {
        ScopedEnvVar failpoint("RAFT_TEST_STORAGE_FAILPOINT", "meta_file_sync");
        ASSERT_FALSE(storage->Save(new_state, &error));
      }
      ExpectInjectedFailure(error,
                            "meta.bin file sync",
                            injected_meta_tmp_path,
                            "file sync",
                            trusted_state_expectation,
                            "error should identify that the meta temp file sync boundary failed before the new hard-state publish became durable");

      const auto snapshot_config = BuildPersistenceSnapshotConfig(scoped_dir.path(), config.node_id);
      auto restarted = std::make_shared<RaftNode>(config, snapshot_config);
      const NodeStatusSnapshot status = restarted->GetStatusSnapshot();
      const std::string description = restarted->Describe();
      int voted_for = -1;
      ASSERT_TRUE(ExtractIntField(description, "voted_for", &voted_for)) << description;

      EXPECT_EQ(status.term, old_state.current_term) << description;
      EXPECT_EQ(status.commit_index, old_state.commit_index) << description;
      EXPECT_EQ(status.last_applied, old_state.last_applied) << description;
      EXPECT_EQ(status.last_log_index, old_state.log.back().index) << description;
      EXPECT_EQ(voted_for, old_state.voted_for) << description;

      ExpectBoundaryMetadataState(restarted, 3U, old_state.current_term, true, false);
    }

    TEST(PersistenceTest, MetaDirectorySyncFailureNeedsExactFailureInjectionSeam)
    {
      ScopedDataDir scoped_dir("test_meta_directory_sync_injection_contract");
      NodeConfig config;
      config.node_id = 1;
      config.address = "127.0.0.1:" + std::to_string(RandomBasePort());
      config.election_timeout_min = std::chrono::milliseconds(250);
      config.election_timeout_max = std::chrono::milliseconds(400);
      config.heartbeat_interval = std::chrono::milliseconds(80);
      config.rpc_deadline = std::chrono::milliseconds(500);
      config.data_dir = (scoped_dir.path() / "raft_data" / "node_1").string();

      const fs::path storage_root = config.data_dir;
      const fs::path saved_root = scoped_dir.path() / "saved_publish_states";
      const fs::path meta_path = storage_root / "meta.bin";
      const std::string trusted_state_expectation =
          "restart must stay on the previously trusted hard-state boundary if the new meta publish reached rename/replace but the parent directory sync did not complete";

      auto storage = CreateFileRaftStorage(config.data_dir);
      const auto old_state = MakePersistenceState(1, 3);
      const auto new_state = MakePersistenceState(1, 5);
      std::string error;

      ASSERT_TRUE(storage->Save(old_state, &error)) << error;

      std::error_code ec;
      fs::create_directories(saved_root, ec);
      ASSERT_FALSE(ec) << ec.message();
      fs::copy_file(meta_path, saved_root / "old_meta.bin",
                    fs::copy_options::overwrite_existing, ec);
      ASSERT_FALSE(ec) << ec.message();

      {
        ScopedEnvVar failpoint("RAFT_TEST_STORAGE_FAILPOINT", "meta_directory_sync");
        ASSERT_FALSE(storage->Save(new_state, &error));
      }
      ExpectInjectedFailure(error,
                            "meta.bin parent directory sync after replace",
                            storage_root,
                            "directory sync",
                            trusted_state_expectation,
                            "error should identify that meta.bin reached rename/replace but the parent directory sync boundary did not complete");

      // Simulate restart after an untrusted meta publish by restoring the last
      // known durable meta boundary while keeping the newer log tree visible.
      fs::copy_file(saved_root / "old_meta.bin", meta_path,
                    fs::copy_options::overwrite_existing, ec);
      ASSERT_FALSE(ec) << ec.message();

      const auto snapshot_config = BuildPersistenceSnapshotConfig(scoped_dir.path(), config.node_id);
      auto restarted = std::make_shared<RaftNode>(config, snapshot_config);
      const NodeStatusSnapshot status = restarted->GetStatusSnapshot();
      const std::string description = restarted->Describe();
      int voted_for = -1;
      ASSERT_TRUE(ExtractIntField(description, "voted_for", &voted_for)) << description;

      EXPECT_EQ(status.term, old_state.current_term) << description;
      EXPECT_EQ(status.commit_index, old_state.commit_index) << description;
      EXPECT_EQ(status.last_applied, old_state.last_applied) << description;
      EXPECT_EQ(status.last_log_index, old_state.log.back().index) << description;
      EXPECT_EQ(voted_for, old_state.voted_for) << description;

      ExpectBoundaryMetadataState(restarted, 3U, old_state.current_term, true, false);
    }

  } // namespace
} // namespace raftdemo
