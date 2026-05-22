#include <gtest/gtest.h>

#include <atomic>
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
    using namespace std::chrono_literals;
    namespace fs = std::filesystem;

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
      case ProposeStatus::kOverloaded:
        return "Overloaded";
      }
      return "Unknown";
    }

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
      return TestBinaryDir() / "raft_test_data" / "metadata_concurrency_stress" /
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
      return 47000 + static_cast<int>(rd() % 12000);
    }

    bool IsLeaderSnapshot(const std::string &snapshot)
    {
      return snapshot.find("role=Leader") != std::string::npos;
    }

    std::vector<NodeConfig> BuildThreeNodeConfigs(int base_port,
                                                  const fs::path &root,
                                                  std::chrono::milliseconds rpc_deadline)
    {
      NodeConfig n1;
      n1.node_id = 1;
      n1.address = "127.0.0.1:" + std::to_string(base_port + 1);
      n1.peers = {
          PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
          PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
      };
      n1.election_timeout_min = 300ms;
      n1.election_timeout_max = 600ms;
      n1.heartbeat_interval = 80ms;
      n1.rpc_deadline = rpc_deadline;
      n1.data_dir = (root / "raft_data" / "node_1").string();

      NodeConfig n2 = n1;
      n2.node_id = 2;
      n2.address = "127.0.0.1:" + std::to_string(base_port + 2);
      n2.peers = {
          PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
          PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
      };
      n2.data_dir = (root / "raft_data" / "node_2").string();

      NodeConfig n3 = n1;
      n3.node_id = 3;
      n3.address = "127.0.0.1:" + std::to_string(base_port + 3);
      n3.peers = {
          PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
          PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
      };
      n3.data_dir = (root / "raft_data" / "node_3").string();

      return {n1, n2, n3};
    }

    class ClusterRunner
    {
    public:
      ClusterRunner(int base_port, std::chrono::milliseconds rpc_deadline)
          : root_(MakeTestRoot()),
            configs_(BuildThreeNodeConfigs(base_port, root_, rpc_deadline))
      {
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_, ec);

        snapshot_config_.enabled = false;
        snapshot_config_.snapshot_dir = (root_ / "raft_snapshots").string();
      }

      ~ClusterRunner() { StopAll(); }

      void Start()
      {
        StopAll();
        nodes_.clear();
        threads_.clear();

        for (const auto &cfg : configs_)
        {
          nodes_.push_back(std::make_shared<RaftNode>(cfg, snapshot_config_));
        }

        for (const auto &node : nodes_)
        {
          node->Start();
          threads_.emplace_back([node]()
                                { node->Wait(); });
        }
      }

      void StopAll()
      {
        for (auto &node : nodes_)
        {
          if (node)
          {
            node->Stop();
          }
        }
        for (auto &thread : threads_)
        {
          if (thread.joinable())
          {
            thread.join();
          }
        }
        threads_.clear();
      }

      void StopNode(std::size_t index)
      {
        ASSERT_LT(index, nodes_.size());
        if (nodes_[index])
        {
          nodes_[index]->Stop();
        }
        if (index < threads_.size() && threads_[index].joinable())
        {
          threads_[index].join();
        }
      }

      void RestartNode(std::size_t index)
      {
        ASSERT_LT(index, configs_.size());
        StopNode(index);
        nodes_[index] = std::make_shared<RaftNode>(configs_[index], snapshot_config_);
        nodes_[index]->Start();
        if (index >= threads_.size())
        {
          threads_.resize(index + 1);
        }
        threads_[index] = std::thread([node = nodes_[index]]()
                                      { node->Wait(); });
      }

      std::shared_ptr<RaftNode> WaitForLeader(std::chrono::milliseconds timeout) const
      {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
          for (const auto &node : nodes_)
          {
            if (node && IsLeaderSnapshot(node->Describe()))
            {
              return node;
            }
          }
          std::this_thread::sleep_for(50ms);
        }
        return nullptr;
      }

      std::size_t FindNodeIndex(const std::shared_ptr<RaftNode> &target) const
      {
        for (std::size_t i = 0; i < nodes_.size(); ++i)
        {
          if (nodes_[i] == target)
          {
            return i;
          }
        }
        return nodes_.size();
      }

      std::vector<std::size_t> FollowerIndexes(const std::shared_ptr<RaftNode> &leader) const
      {
        std::vector<std::size_t> indexes;
        for (std::size_t i = 0; i < nodes_.size(); ++i)
        {
          if (nodes_[i] && nodes_[i] != leader)
          {
            indexes.push_back(i);
          }
        }
        return indexes;
      }

      const std::vector<std::shared_ptr<RaftNode>> &Nodes() const
      {
        return nodes_;
      }

    private:
      fs::path root_;
      std::vector<NodeConfig> configs_;
      snapshotConfig snapshot_config_;
      std::vector<std::shared_ptr<RaftNode>> nodes_;
      std::vector<std::thread> threads_;
    };

    TEST(MetadataConcurrencyStressTest, AdmissionRejectsWhenInflightLimitIsReached)
    {
      ClusterRunner cluster(PickBasePort(), 400ms);
      cluster.Start();

      auto leader = cluster.WaitForLeader(5s);
      ASSERT_NE(leader, nullptr);

      ProposeResult bucket_result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
          cluster.Nodes(),
          raftdemo::test::MakeCreateBucketCommand(
              "stress-overload-bucket", "stress-overload-create-bucket"),
          5s, &bucket_result));
      ASSERT_EQ(bucket_result.status, ProposeStatus::kOk) << bucket_result.message;

      for (const std::size_t index : cluster.FollowerIndexes(leader))
      {
        cluster.StopNode(index);
      }

      constexpr int kInflightLimit = 4;
      std::atomic<bool> start{false};
      std::vector<ProposeResult> results(kInflightLimit);
      std::vector<std::thread> threads;
      threads.reserve(kInflightLimit);
      for (int i = 0; i < kInflightLimit; ++i)
      {
        threads.emplace_back(
            [&, i]()
            {
              while (!start.load(std::memory_order_acquire))
              {
              }
              results[static_cast<std::size_t>(i)] = leader->ProposeMetadata(
                  SerializeMetadataCommand(
                      raftdemo::test::MakeCreateObjectCommand(
                          "stress-overload-bucket",
                          "objects/" + std::to_string(i),
                          "obj-overload-" + std::to_string(i),
                          "stress-overload-create-" + std::to_string(i))));
            });
      }

      start.store(true, std::memory_order_release);
      std::this_thread::sleep_for(20ms);

      const ProposeResult overload = leader->ProposeMetadata(
          SerializeMetadataCommand(
              raftdemo::test::MakeCreateObjectCommand(
                  "stress-overload-bucket", "objects/overflow", "obj-overflow",
                  "stress-overload-overflow")));

      for (auto &thread : threads)
      {
        thread.join();
      }

      EXPECT_EQ(overload.status, ProposeStatus::kOverloaded)
          << overload.message;
      EXPECT_NE(overload.message.find("in-flight limit reached"), std::string::npos)
          << overload.message;
      for (const auto &result : results)
      {
        EXPECT_NE(result.status, ProposeStatus::kOk)
            << "isolated leader should not commit while majority is down";
      }

      const auto *state_machine = leader->GetMetadataStateMachineV2();
      ASSERT_NE(state_machine, nullptr);
      EXPECT_EQ(state_machine->RequestCount(), 1U);
      EXPECT_GE(state_machine->LastAppliedIndex(), bucket_result.log_index);

      const auto listed = state_machine->ListObjects(
          {.bucket = "stress-overload-bucket",
           .prefix = "objects/",
           .limit = std::nullopt,
           .continuation_token = ""});
      ASSERT_TRUE(listed.result.Ok()) << listed.result.summary.message;
      EXPECT_TRUE(listed.records.empty());
      EXPECT_FALSE(state_machine->FindObject("stress-overload-bucket",
                                             "objects/overflow")
                       .has_value());
    }

    TEST(MetadataConcurrencyStressTest,
         TimeoutReturnsWithoutBlockingAndRetryUsesSameInflightProposal)
    {
      ClusterRunner cluster(PickBasePort(), 400ms);
      cluster.Start();

      auto leader = cluster.WaitForLeader(5s);
      ASSERT_NE(leader, nullptr);

      const std::string bucket = "stress-timeout-bucket";
      const std::string object_key = "objects/slow";
      const std::string object_id = "obj-slow";
      const std::string create_request_id = "stress-timeout-create";

      ProposeResult bucket_result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
          cluster.Nodes(),
          raftdemo::test::MakeCreateBucketCommand(
              bucket, "stress-timeout-create-bucket"),
          5s, &bucket_result));
      ASSERT_EQ(bucket_result.status, ProposeStatus::kOk) << bucket_result.message;

      const auto follower_indexes = cluster.FollowerIndexes(leader);
      ASSERT_EQ(follower_indexes.size(), 2U);
      cluster.StopNode(follower_indexes[0]);
      cluster.StopNode(follower_indexes[1]);

      ProposeResult first_result;
      const auto started_at = std::chrono::steady_clock::now();
      std::thread first_call(
          [&]()
          {
            first_result = leader->ProposeMetadata(
                SerializeMetadataCommand(
                    raftdemo::test::MakeCreateObjectCommand(
                        bucket, object_key, object_id, create_request_id)));
          });
      first_call.join();
      const auto elapsed =
          std::chrono::steady_clock::now() - started_at;

      EXPECT_EQ(first_result.status, ProposeStatus::kTimeout)
          << first_result.message;
      EXPECT_LT(elapsed, 1200ms);

      cluster.RestartNode(follower_indexes[0]);
      cluster.RestartNode(follower_indexes[1]);

      ProposeResult retry_result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
          cluster.Nodes(),
          raftdemo::test::MakeCreateObjectCommand(
              bucket, object_key, object_id, create_request_id),
          5s, &retry_result));
      ASSERT_EQ(retry_result.status, ProposeStatus::kOk)
          << retry_result.message;

      ProposeResult commit_result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
          cluster.Nodes(),
          raftdemo::test::MakeCommitObjectCommand(
              bucket, object_key, object_id, "stress-timeout-commit"),
          5s, &commit_result));
      ASSERT_EQ(commit_result.status, ProposeStatus::kOk)
          << commit_result.message;

      ASSERT_TRUE(raftdemo::test::WaitUntilAllCommittedObject(
          cluster.Nodes(), bucket, object_key, object_id, 2U,
          commit_result.log_index, 10s));

      for (const auto &node : cluster.Nodes())
      {
        const auto *state_machine = node->GetMetadataStateMachineV2();
        ASSERT_NE(state_machine, nullptr);
        EXPECT_EQ(state_machine->RequestCount(), 3U);
      }
    }

    TEST(MetadataConcurrencyStressTest,
         ConcurrentDuplicateRequestIdProposalsShareOneLogEntryAndOneApply)
    {
      ClusterRunner cluster(PickBasePort(), 1200ms);
      cluster.Start();

      auto leader = cluster.WaitForLeader(5s);
      ASSERT_NE(leader, nullptr);

      const std::string bucket = "stress-duplicate-bucket";
      const std::string object_key = "objects/shared";
      const std::string object_id = "obj-shared";

      ProposeResult bucket_result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
          cluster.Nodes(),
          raftdemo::test::MakeCreateBucketCommand(
              bucket, "stress-duplicate-create-bucket"),
          5s, &bucket_result));
      ASSERT_EQ(bucket_result.status, ProposeStatus::kOk) << bucket_result.message;

      const std::string create_payload = SerializeMetadataCommand(
          raftdemo::test::MakeCreateObjectCommand(
              bucket, object_key, object_id, "stress-duplicate-create"));
      const std::string commit_payload = SerializeMetadataCommand(
          raftdemo::test::MakeCommitObjectCommand(
              bucket, object_key, object_id, "stress-duplicate-commit"));

      constexpr int kConcurrency = 6;
      auto run_wave =
          [&](const std::string &payload)
      {
        std::atomic<bool> start{false};
        std::vector<ProposeResult> wave_results(kConcurrency);
        std::vector<std::thread> wave_threads;
        wave_threads.reserve(kConcurrency);
        for (int i = 0; i < kConcurrency; ++i)
        {
          wave_threads.emplace_back(
              [&, i]()
              {
                while (!start.load(std::memory_order_acquire))
                {
                }
                wave_results[static_cast<std::size_t>(i)] =
                    leader->ProposeMetadata(payload);
              });
        }
        start.store(true, std::memory_order_release);
        for (auto &thread : wave_threads)
        {
          thread.join();
        }
        return wave_results;
      };

      const auto create_results = run_wave(create_payload);
      const auto commit_results = run_wave(commit_payload);

      const std::uint64_t create_log_index = create_results.front().log_index;
      const std::uint64_t commit_log_index = commit_results.front().log_index;
      for (const auto &result : create_results)
      {
        ASSERT_EQ(result.status, ProposeStatus::kOk)
            << ProposeStatusName(result.status) << ": " << result.message;
        EXPECT_EQ(result.log_index, create_log_index);
      }
      for (const auto &result : commit_results)
      {
        ASSERT_EQ(result.status, ProposeStatus::kOk)
            << ProposeStatusName(result.status) << ": " << result.message;
        EXPECT_EQ(result.log_index, commit_log_index);
      }

      ASSERT_TRUE(raftdemo::test::WaitUntilAllCommittedObject(
          cluster.Nodes(), bucket, object_key, object_id, 2U,
          commit_log_index, 10s));

      raftdemo::test::MetadataRecoveryExpectation expectation;
      expectation.bucket = bucket;
      expectation.objects = {{
          object_key,
          object_id,
          2U,
          false,
      }};
      expectation.visible_keys = {object_key};
      expectation.expected_request_count = 3U;
      expectation.expected_tombstone_count = 0U;
      expectation.expected_last_applied_index = commit_log_index;
      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
          cluster.Nodes(), expectation, 10s));
    }

    TEST(MetadataConcurrencyStressTest,
         ConcurrentDuplicateDeleteRequestsShareOneLogEntryAndKeepDeletionFactsConsistent)
    {
      ClusterRunner cluster(PickBasePort(), 1200ms);
      cluster.Start();

      auto leader = cluster.WaitForLeader(5s);
      ASSERT_NE(leader, nullptr);

      const std::string bucket = "stress-delete-bucket";
      const std::string object_key = "objects/deleted";
      const std::string object_id = "obj-deleted";

      ProposeResult bucket_result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
          cluster.Nodes(),
          raftdemo::test::MakeCreateBucketCommand(
              bucket, "stress-delete-create-bucket"),
          5s, &bucket_result));
      ASSERT_EQ(bucket_result.status, ProposeStatus::kOk) << bucket_result.message;

      ProposeResult create_result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
          cluster.Nodes(),
          raftdemo::test::MakeCreateObjectCommand(
              bucket, object_key, object_id, "stress-delete-create"),
          5s, &create_result));
      ASSERT_EQ(create_result.status, ProposeStatus::kOk) << create_result.message;

      ProposeResult commit_result;
      ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
          cluster.Nodes(),
          raftdemo::test::MakeCommitObjectCommand(
              bucket, object_key, object_id, "stress-delete-commit"),
          5s, &commit_result));
      ASSERT_EQ(commit_result.status, ProposeStatus::kOk) << commit_result.message;

      const std::string delete_payload = SerializeMetadataCommand(
          raftdemo::test::MakeDeleteObjectCommand(
              bucket, object_key, object_id, "stress-delete-request"));

      constexpr int kConcurrency = 6;
      std::atomic<bool> start{false};
      std::vector<ProposeResult> delete_results(kConcurrency);
      std::vector<std::thread> delete_threads;
      delete_threads.reserve(kConcurrency);
      for (int i = 0; i < kConcurrency; ++i)
      {
        delete_threads.emplace_back(
            [&, i]()
            {
              while (!start.load(std::memory_order_acquire))
              {
              }
              delete_results[static_cast<std::size_t>(i)] =
                  leader->ProposeMetadata(delete_payload);
            });
      }

      start.store(true, std::memory_order_release);
      for (auto &thread : delete_threads)
      {
        thread.join();
      }

      const std::uint64_t delete_log_index = delete_results.front().log_index;
      for (const auto &result : delete_results)
      {
        ASSERT_EQ(result.status, ProposeStatus::kOk)
            << ProposeStatusName(result.status) << ": " << result.message;
        EXPECT_EQ(result.log_index, delete_log_index);
      }

      raftdemo::test::MetadataRecoveryExpectation expectation;
      expectation.bucket = bucket;
      expectation.objects = {{
          object_key,
          object_id,
          2U,
          true,
      }};
      expectation.visible_keys = {};
      expectation.expected_request_count = 4U;
      expectation.expected_tombstone_count = 1U;
      expectation.expected_last_applied_index = delete_log_index;
      ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
          cluster.Nodes(), expectation, 10s));
    }

  } // namespace
} // namespace raftdemo
