#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
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
      return TestBinaryDir() / "raft_test_data" / "log_replication" /
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
      return 42000 + static_cast<int>(rd() % 13000);
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

    NodeConfig BuildDetachedLearnerLikeConfig(int base_port, const fs::path &root)
    {
      NodeConfig learner;
      learner.node_id = 4;
      learner.address = "127.0.0.1:" + std::to_string(base_port + 4);
      learner.election_timeout_min = std::chrono::milliseconds(300);
      learner.election_timeout_max = std::chrono::milliseconds(600);
      learner.heartbeat_interval = std::chrono::milliseconds(80);
      learner.rpc_deadline = std::chrono::milliseconds(500);
      learner.data_dir = (root / "raft_data" / "node_4_learner_like").string();
      return learner;
    }

    AddLearnerProposalRequest MakePendingLearnerProposalRequest(
        const std::string &cluster_id,
        const std::string &node_id,
        std::int32_t candidate_raft_id,
        std::uint16_t candidate_client_port,
        std::uint16_t candidate_raft_port)
    {
      AddLearnerProposalRequest request;
      request.cluster_id = cluster_id;
      request.node_id = node_id;
      request.candidate_raft_id = candidate_raft_id;
      request.candidate_client_address =
          "127.0.0.1:" + std::to_string(candidate_client_port);
      request.candidate_raft_address =
          "127.0.0.1:" + std::to_string(candidate_raft_port);
      request.candidate_incarnation_id = node_id + ":boot:1710000000";
      request.candidate_sequence = 1;
      request.persistent_generation = 1;
      request.data_dir_fingerprint = "fingerprint-" + node_id;
      return request;
    }

    void ExpectCommittedVoterSummary(const RaftNode &node,
                                     const std::vector<int> &expected_voters,
                                     const std::size_t expected_quorum)
    {
      const auto summary = node.GetCommittedMembershipQuorumSummary();
      EXPECT_EQ(summary.voter_ids, expected_voters);
      EXPECT_TRUE(summary.learner_ids.empty());
      EXPECT_EQ(summary.voter_count, expected_voters.size());
      EXPECT_EQ(summary.learner_count, 0U);
      EXPECT_EQ(summary.quorum_size, expected_quorum);
    }

    std::string DescribeRuntimeMembershipSummary(const RuntimeMembershipSummary &summary)
    {
      std::ostringstream oss;
      oss << "commit_index=" << summary.committed_log_index
          << ", term=" << summary.committed_term
          << ", voters=[";
      for (std::size_t index = 0; index < summary.voter_ids.size(); ++index)
      {
        if (index != 0)
        {
          oss << ",";
        }
        oss << summary.voter_ids[index];
      }
      oss << "], learners=[";
      for (std::size_t index = 0; index < summary.learner_ids.size(); ++index)
      {
        if (index != 0)
        {
          oss << ",";
        }
        oss << summary.learner_ids[index];
      }
      oss << "], voter_count=" << summary.voter_count
          << ", learner_count=" << summary.learner_count
          << ", committed_voter_quorum=" << summary.committed_voter_quorum_size
          << ", local_role=" << static_cast<int>(summary.local_role);
      for (const auto &entry : summary.learner_entries)
      {
        oss << " | learner_entry{id=" << entry.raft_id
            << ", committed=" << entry.committed
            << ", pending=" << entry.pending
            << ", match=" << entry.match_index
            << ", next=" << entry.next_index
            << "}";
      }
      return oss.str();
    }

    void WriteStructuredLearnerIdentity(const NodeConfig &learner)
    {
      std::error_code ec;
      fs::create_directories(learner.data_dir, ec);
      ASSERT_FALSE(ec) << ec.message();

      std::ofstream out(fs::path(learner.data_dir) / "node.identity",
                        std::ios::trunc);
      ASSERT_TRUE(out.is_open());
      out << "membership_state=learner\n";
      out.flush();
      ASSERT_TRUE(static_cast<bool>(out));
    }

    bool WaitForLearnerProgress(const std::shared_ptr<RaftNode> &leader,
                                const int learner_raft_id,
                                const std::uint64_t minimum_match_index,
                                const std::chrono::milliseconds timeout,
                                RuntimeMembershipEntry *learner_entry,
                                std::string *diagnostics)
    {
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      std::string last_snapshot;

      while (std::chrono::steady_clock::now() < deadline)
      {
        const auto summary = leader->GetRuntimeMembershipSummary();
        last_snapshot = DescribeRuntimeMembershipSummary(summary);
        for (const auto &entry : summary.learner_entries)
        {
          if (entry.raft_id != learner_raft_id)
          {
            continue;
          }
          if (entry.match_index >= minimum_match_index &&
              entry.next_index >= entry.match_index)
          {
            if (learner_entry != nullptr)
            {
              *learner_entry = entry;
            }
            if (diagnostics != nullptr)
            {
              *diagnostics = last_snapshot;
            }
            return true;
          }
        }
        std::this_thread::sleep_for(50ms);
      }

      if (diagnostics != nullptr)
      {
        *diagnostics = last_snapshot;
      }
      return false;
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

    class StandaloneNodeRunner
    {
    public:
      explicit StandaloneNodeRunner(std::shared_ptr<RaftNode> node)
          : node_(std::move(node))
      {
      }

      ~StandaloneNodeRunner()
      {
        Stop();
      }

      void Start()
      {
        if (!node_ || thread_.joinable())
        {
          return;
        }
        thread_ = std::thread([node = node_]()
                              {
          node->Start();
          node->Wait(); });
      }

      void Stop()
      {
        if (node_)
        {
          node_->Stop();
        }
        if (thread_.joinable())
        {
          thread_.join();
        }
      }

      const std::shared_ptr<RaftNode> &Node() const
      {
        return node_;
      }

    private:
      std::shared_ptr<RaftNode> node_;
      std::thread thread_;
    };

    TEST(RaftLogReplicationTest, LeaderProposeReplicatesLogToAllNodes)
    {
      ClusterRunner cluster(PickBasePort());
      cluster.Start();

      auto leader = cluster.WaitForLeader(5s);
      ASSERT_NE(leader, nullptr);

      const ProposeResult bucket_result = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCreateBucketCommand("replication-bucket",
                                                          "replication-bucket-create-1"));
      ASSERT_EQ(bucket_result.status, ProposeStatus::kOk) << bucket_result.message;

      const ProposeResult create_result = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCreateObjectCommand("replication-bucket",
                                                          "logs/object-a",
                                                          "obj-repl-a",
                                                          "replication-object-create-1"));
      ASSERT_EQ(create_result.status, ProposeStatus::kOk) << create_result.message;

      const ProposeResult commit_result = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCommitObjectCommand("replication-bucket",
                                                          "logs/object-a",
                                                          "obj-repl-a",
                                                          "replication-object-commit-1"));
      ASSERT_EQ(commit_result.status, ProposeStatus::kOk) << commit_result.message;
      ASSERT_GT(commit_result.log_index, 0u);

      const std::string index = std::to_string(commit_result.log_index);
      ASSERT_TRUE(cluster.WaitUntilAll({"last_log_index=" + index,
                                        "commit_index=" + index,
                                        "last_applied=" + index},
                                       5s));
      ASSERT_TRUE(raftdemo::test::WaitUntilAllCommittedObject(
          cluster.Nodes(),
          "replication-bucket",
          "logs/object-a",
          "obj-repl-a",
          2,
          commit_result.log_index,
          5s));
    }

    TEST(RaftLogReplicationTest, MultipleSequentialEntriesStayConsistentAcrossCluster)
    {
      ClusterRunner cluster(PickBasePort());
      cluster.Start();

      auto leader = cluster.WaitForLeader(5s);
      ASSERT_NE(leader, nullptr);

      const ProposeResult r1 = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCreateBucketCommand("replication-bucket-b",
                                                          "replication-bucket-create-2"));
      ASSERT_EQ(r1.status, ProposeStatus::kOk) << r1.message;

      const ProposeResult r2 = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCreateObjectCommand("replication-bucket-b",
                                                          "logs/object-a",
                                                          "obj-repl-b-a",
                                                          "replication-object-create-2"));
      ASSERT_EQ(r2.status, ProposeStatus::kOk) << r2.message;

      const ProposeResult r3 = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCommitObjectCommand("replication-bucket-b",
                                                          "logs/object-a",
                                                          "obj-repl-b-a",
                                                          "replication-object-commit-2"));
      ASSERT_EQ(r3.status, ProposeStatus::kOk) << r3.message;

      const ProposeResult r4 = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCreateObjectCommand("replication-bucket-b",
                                                          "logs/object-b",
                                                          "obj-repl-b-b",
                                                          "replication-object-create-3"));
      ASSERT_EQ(r4.status, ProposeStatus::kOk) << r4.message;

      const ProposeResult r5 = raftdemo::test::ProposeMetadataCommand(
          leader, raftdemo::test::MakeCommitObjectCommand("replication-bucket-b",
                                                          "logs/object-b",
                                                          "obj-repl-b-b",
                                                          "replication-object-commit-3"));
      ASSERT_EQ(r5.status, ProposeStatus::kOk) << r5.message;

      const std::string index = std::to_string(r5.log_index);
      ASSERT_TRUE(cluster.WaitUntilAll({"last_log_index=" + index,
                                        "commit_index=" + index,
                                        "last_applied=" + index},
                                       5s));
      ASSERT_TRUE(raftdemo::test::WaitUntilAllCommittedObject(
          cluster.Nodes(),
          "replication-bucket-b",
          "logs/object-a",
          "obj-repl-b-a",
          2,
          r5.log_index,
          5s));
      ASSERT_TRUE(raftdemo::test::WaitUntilAllCommittedObject(
          cluster.Nodes(),
          "replication-bucket-b",
          "logs/object-b",
          "obj-repl-b-b",
          2,
          r5.log_index,
          5s));
      ASSERT_TRUE(raftdemo::test::WaitUntilAllListObjectsMatch(
          cluster.Nodes(),
          "replication-bucket-b",
          "logs/",
          {"logs/object-a", "logs/object-b"},
          r5.log_index,
          5s));
    }

    TEST(RaftLogReplicationTest,
         LearnerLikeAppendEntriesCatchUpDoesNotAffectCommittedVoterQuorum)
    {
      constexpr const char *kBucket = "learner-appendentries-bucket";
      constexpr const char *kObjectKey = "logs/learner-object-a";
      constexpr const char *kObjectId = "obj-learner-a";
      constexpr const char *kNoOpCommand = "__raft_internal_noop__";

      const int base_port = PickBasePort();
      ClusterRunner cluster(base_port);
      cluster.Start();

      auto leader = cluster.WaitForLeader(5s);
      ASSERT_NE(leader, nullptr);

      const ProposeResult bucket_result = raftdemo::test::ProposeMetadataCommand(
          leader,
          raftdemo::test::MakeCreateBucketCommand(
              kBucket,
              "learner-appendentries-create-bucket"));
      ASSERT_EQ(bucket_result.status, ProposeStatus::kOk) << bucket_result.message;

      const ProposeResult create_result = raftdemo::test::ProposeMetadataCommand(
          leader,
          raftdemo::test::MakeCreateObjectCommand(
              kBucket,
              kObjectKey,
              kObjectId,
              "learner-appendentries-create-object"));
      ASSERT_EQ(create_result.status, ProposeStatus::kOk) << create_result.message;
      ASSERT_GT(bucket_result.log_index, 0U);
      ASSERT_GT(create_result.log_index, bucket_result.log_index);

      const NodeStatusSnapshot leader_status = leader->GetStatusSnapshot();
      ASSERT_GT(leader_status.term, 0U);
      ExpectCommittedVoterSummary(*leader, {1, 2, 3}, 2U);

      const fs::path learner_root = MakeTestRoot();
      std::error_code ec;
      fs::remove_all(learner_root, ec);
      fs::create_directories(learner_root, ec);
      snapshotConfig learner_snapshot_config;
      learner_snapshot_config.enabled = false;
      learner_snapshot_config.snapshot_dir =
          (learner_root / "raft_snapshots").string();
      auto learner =
          std::make_shared<RaftNode>(BuildDetachedLearnerLikeConfig(base_port,
                                                                    learner_root),
                                     learner_snapshot_config);

      raft::AppendEntriesRequest catch_up_request;
      catch_up_request.set_term(leader_status.term);
      catch_up_request.set_leader_id(leader_status.node_id);
      catch_up_request.set_prev_log_index(0);
      catch_up_request.set_prev_log_term(0);
      catch_up_request.set_leader_commit(bucket_result.log_index);
      for (std::uint64_t index = 1; index <= create_result.log_index; ++index)
      {
        auto *entry = catch_up_request.add_entries();
        entry->set_index(index);
        entry->set_term(leader_status.term);
        if (index == bucket_result.log_index)
        {
          entry->set_command(SerializeMetadataCommand(
              raftdemo::test::MakeCreateBucketCommand(
                  kBucket,
                  "learner-appendentries-create-bucket")));
        }
        else if (index == create_result.log_index)
        {
          entry->set_command(SerializeMetadataCommand(
              raftdemo::test::MakeCreateObjectCommand(
                  kBucket,
                  kObjectKey,
                  kObjectId,
                  "learner-appendentries-create-object")));
        }
        else
        {
          entry->set_command(kNoOpCommand);
        }
      }

      raft::AppendEntriesResponse catch_up_response;
      learner->OnAppendEntries(catch_up_request, &catch_up_response);
      ASSERT_TRUE(catch_up_response.success());
      EXPECT_EQ(catch_up_response.match_index(), create_result.log_index);

      NodeStatusSnapshot learner_status = learner->GetStatusSnapshot();
      EXPECT_EQ(learner_status.last_log_index, create_result.log_index);
      EXPECT_EQ(learner_status.commit_index, bucket_result.log_index);
      EXPECT_EQ(learner_status.last_applied, bucket_result.log_index);

      const MetadataStateMachine *learner_state_machine =
          learner->GetMetadataStateMachineV2();
      ASSERT_NE(learner_state_machine, nullptr);
      EXPECT_TRUE(learner_state_machine->FindBucket(kBucket).has_value());
      EXPECT_FALSE(
          learner_state_machine->FindObject(kBucket, kObjectKey).has_value());

      raft::AppendEntriesResponse duplicate_response;
      learner->OnAppendEntries(catch_up_request, &duplicate_response);
      ASSERT_TRUE(duplicate_response.success());
      learner_status = learner->GetStatusSnapshot();
      EXPECT_EQ(learner_status.last_log_index, create_result.log_index);
      EXPECT_EQ(learner_status.commit_index, bucket_result.log_index);

      raft::AppendEntriesRequest failing_request;
      failing_request.set_term(leader_status.term);
      failing_request.set_leader_id(leader_status.node_id);
      failing_request.set_prev_log_index(create_result.log_index + 1);
      failing_request.set_prev_log_term(leader_status.term);
      failing_request.set_leader_commit(create_result.log_index);
      raft::AppendEntriesResponse failing_response;
      learner->OnAppendEntries(failing_request, &failing_response);
      EXPECT_FALSE(failing_response.success());
      EXPECT_EQ(failing_response.last_log_index(), create_result.log_index);

      const ProposeResult commit_result = raftdemo::test::ProposeMetadataCommand(
          leader,
          raftdemo::test::MakeCommitObjectCommand(
              kBucket,
              kObjectKey,
              kObjectId,
              "learner-appendentries-commit-object"));
      ASSERT_EQ(commit_result.status, ProposeStatus::kOk) << commit_result.message;
      ASSERT_GT(commit_result.log_index, create_result.log_index);
      ExpectCommittedVoterSummary(*leader, {1, 2, 3}, 2U);

      raft::AppendEntriesRequest finish_catch_up_request;
      finish_catch_up_request.set_term(leader_status.term);
      finish_catch_up_request.set_leader_id(leader_status.node_id);
      finish_catch_up_request.set_prev_log_index(create_result.log_index);
      finish_catch_up_request.set_prev_log_term(leader_status.term);
      finish_catch_up_request.set_leader_commit(commit_result.log_index);
      auto *commit_entry = finish_catch_up_request.add_entries();
      commit_entry->set_index(commit_result.log_index);
      commit_entry->set_term(leader_status.term);
      commit_entry->set_command(SerializeMetadataCommand(
          raftdemo::test::MakeCommitObjectCommand(
              kBucket,
              kObjectKey,
              kObjectId,
              "learner-appendentries-commit-object")));

      raft::AppendEntriesResponse finish_catch_up_response;
      learner->OnAppendEntries(finish_catch_up_request,
                               &finish_catch_up_response);
      ASSERT_TRUE(finish_catch_up_response.success());
      EXPECT_EQ(finish_catch_up_response.match_index(), commit_result.log_index);

      learner_status = learner->GetStatusSnapshot();
      EXPECT_EQ(learner_status.last_log_index, commit_result.log_index);
      EXPECT_EQ(learner_status.commit_index, commit_result.log_index);
      EXPECT_EQ(learner_status.last_applied, commit_result.log_index);

      const auto learner_object =
          learner_state_machine->FindObject(kBucket, kObjectKey);
      ASSERT_TRUE(learner_object.has_value());
      EXPECT_TRUE(learner_object->IsCommitted());

      ExpectCommittedVoterSummary(*leader, {1, 2, 3}, 2U);
    }

    TEST(RaftLogReplicationTest,
         PendingLearnerReplicationProgressAdvancesWithoutChangingCommittedVoterQuorum)
    {
      constexpr const char *kClusterId = "cluster-log-repl-t074";
      constexpr const char *kBucket = "learner-progress-bucket";
      constexpr const char *kObjectKey = "logs/learner-progress-object-a";
      constexpr const char *kObjectId = "obj-learner-progress-a";
      constexpr std::int32_t kLearnerRaftId = 4;

      const int base_port = PickBasePort();
      ClusterRunner cluster(base_port);
      cluster.Start();

      auto leader = cluster.WaitForLeader(5s);
      ASSERT_NE(leader, nullptr);

      const ProposeResult bucket_result = raftdemo::test::ProposeMetadataCommand(
          leader,
          raftdemo::test::MakeCreateBucketCommand(
              kBucket,
              "learner-progress-create-bucket"));
      ASSERT_EQ(bucket_result.status, ProposeStatus::kOk) << bucket_result.message;

      const ProposeResult create_result = raftdemo::test::ProposeMetadataCommand(
          leader,
          raftdemo::test::MakeCreateObjectCommand(
              kBucket,
              kObjectKey,
              kObjectId,
              "learner-progress-create-object"));
      ASSERT_EQ(create_result.status, ProposeStatus::kOk) << create_result.message;
      ASSERT_GT(create_result.log_index, bucket_result.log_index);

      ExpectCommittedVoterSummary(*leader, {1, 2, 3}, 2U);

      const fs::path learner_root = MakeTestRoot();
      std::error_code ec;
      fs::remove_all(learner_root, ec);
      fs::create_directories(learner_root, ec);
      ASSERT_FALSE(ec) << ec.message();

      NodeConfig learner_config = BuildDetachedLearnerLikeConfig(base_port, learner_root);
      WriteStructuredLearnerIdentity(learner_config);

      snapshotConfig learner_snapshot_config;
      learner_snapshot_config.enabled = false;
      learner_snapshot_config.snapshot_dir =
          (learner_root / "raft_snapshots").string();

      StandaloneNodeRunner learner_runner(
          std::make_shared<RaftNode>(learner_config, learner_snapshot_config));
      learner_runner.Start();

      const auto add_learner_result = leader->ProposeAddLearner(
          MakePendingLearnerProposalRequest(kClusterId,
                                            "meta-learner-progress",
                                            kLearnerRaftId,
                                            static_cast<std::uint16_t>(base_port + 104),
                                            static_cast<std::uint16_t>(base_port + 4)));
      ASSERT_EQ(add_learner_result.status,
                AddLearnerProposalStatus::kAcceptedPendingCommit)
          << add_learner_result.message;
      EXPECT_FALSE(add_learner_result.committed_membership_changed);
      EXPECT_EQ(add_learner_result.assigned_raft_id, kLearnerRaftId);

      const auto initial_runtime_summary = leader->GetRuntimeMembershipSummary();
      ASSERT_EQ(initial_runtime_summary.learner_ids, std::vector<int>{kLearnerRaftId})
          << DescribeRuntimeMembershipSummary(initial_runtime_summary);
      ASSERT_EQ(initial_runtime_summary.learner_entries.size(), 1U);
      EXPECT_TRUE(initial_runtime_summary.learner_entries.front().pending);
      EXPECT_FALSE(initial_runtime_summary.learner_entries.front().committed);
      EXPECT_EQ(initial_runtime_summary.committed_voter_quorum_size, 2U)
          << DescribeRuntimeMembershipSummary(initial_runtime_summary);

      RuntimeMembershipEntry learner_progress_after_create;
      std::string learner_progress_diagnostics;
      ASSERT_TRUE(WaitForLearnerProgress(leader,
                                         kLearnerRaftId,
                                         create_result.log_index,
                                         5s,
                                         &learner_progress_after_create,
                                         &learner_progress_diagnostics))
          << "pending learner did not advance AppendEntries replication progress after "
             "leader accepted AddLearner; summary="
          << learner_progress_diagnostics;
      EXPECT_TRUE(learner_progress_after_create.pending);
      EXPECT_FALSE(learner_progress_after_create.committed);
      EXPECT_GE(learner_progress_after_create.next_index,
                learner_progress_after_create.match_index);

      const ProposeResult commit_result = raftdemo::test::ProposeMetadataCommand(
          leader,
          raftdemo::test::MakeCommitObjectCommand(
              kBucket,
              kObjectKey,
              kObjectId,
              "learner-progress-commit-object"));
      ASSERT_EQ(commit_result.status, ProposeStatus::kOk) << commit_result.message;
      ASSERT_GT(commit_result.log_index, create_result.log_index);

      RuntimeMembershipEntry learner_progress_after_commit;
      ASSERT_TRUE(WaitForLearnerProgress(leader,
                                         kLearnerRaftId,
                                         commit_result.log_index,
                                         5s,
                                         &learner_progress_after_commit,
                                         &learner_progress_diagnostics))
          << "pending learner did not advance replication progress after committed voter "
             "majority accepted a new log entry; summary="
          << learner_progress_diagnostics;
      EXPECT_TRUE(learner_progress_after_commit.pending);
      EXPECT_FALSE(learner_progress_after_commit.committed);
      EXPECT_GE(learner_progress_after_commit.match_index, commit_result.log_index);
      EXPECT_GE(learner_progress_after_commit.next_index,
                learner_progress_after_commit.match_index);

      ASSERT_TRUE(raftdemo::test::WaitUntilAllCommittedObject(
          {learner_runner.Node()},
          kBucket,
          kObjectKey,
          kObjectId,
          2,
          commit_result.log_index,
          5s));

      const auto final_runtime_summary = leader->GetRuntimeMembershipSummary();
      EXPECT_EQ(final_runtime_summary.voter_ids, std::vector<int>({1, 2, 3}))
          << DescribeRuntimeMembershipSummary(final_runtime_summary);
      EXPECT_EQ(final_runtime_summary.learner_ids, std::vector<int>({kLearnerRaftId}))
          << DescribeRuntimeMembershipSummary(final_runtime_summary);
      EXPECT_EQ(final_runtime_summary.voter_count, 3U)
          << DescribeRuntimeMembershipSummary(final_runtime_summary);
      EXPECT_EQ(final_runtime_summary.learner_count, 1U)
          << DescribeRuntimeMembershipSummary(final_runtime_summary);
      EXPECT_EQ(final_runtime_summary.committed_voter_quorum_size, 2U)
          << DescribeRuntimeMembershipSummary(final_runtime_summary);

      ExpectCommittedVoterSummary(*leader, {1, 2, 3}, 2U);
    }

  } // namespace
} // namespace raftdemo
