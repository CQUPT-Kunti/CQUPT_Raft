#include <gtest/gtest.h>

#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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

std::optional<std::size_t> FindLeaderIndex(
    const std::vector<std::shared_ptr<RaftNode>>& nodes) {
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    if (nodes[index] && IsLeaderSnapshot(nodes[index]->Describe())) {
      return index;
    }
  }
  return std::nullopt;
}

std::shared_ptr<RaftNode> WaitForSingleLeaderAmong(
    const std::vector<std::shared_ptr<RaftNode>>& nodes,
    const std::vector<std::size_t>& indexes,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::shared_ptr<RaftNode> leader;
    int leader_count = 0;
    for (const std::size_t index : indexes) {
      if (index >= nodes.size() || !nodes[index]) {
        continue;
      }
      if (IsLeaderSnapshot(nodes[index]->Describe())) {
        leader = nodes[index];
        ++leader_count;
      }
    }
    if (leader_count == 1) {
      return leader;
    }
    std::this_thread::sleep_for(50ms);
  }
  return nullptr;
}

std::vector<int> ExtractNodeIds(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                const std::vector<std::size_t>& indexes) {
  std::vector<int> ids;
  ids.reserve(indexes.size());
  for (const auto index : indexes) {
    if (index >= nodes.size() || !nodes[index]) {
      continue;
    }
    const auto node_id = ExtractIntField(nodes[index]->Describe(), "node");
    if (node_id.has_value()) {
      ids.push_back(*node_id);
    }
  }
  return ids;
}

std::string DescribeCommittedMembershipSummary(
    const CommittedMembershipQuorumSummary& summary) {
  std::ostringstream oss;
  oss << "commit_index=" << summary.committed_log_index
      << ", term=" << summary.committed_term
      << ", voters=[";
  for (std::size_t index = 0; index < summary.voter_ids.size(); ++index) {
    if (index != 0) {
      oss << ",";
    }
    oss << summary.voter_ids[index];
  }
  oss << "], learners=[";
  for (std::size_t index = 0; index < summary.learner_ids.size(); ++index) {
    if (index != 0) {
      oss << ",";
    }
    oss << summary.learner_ids[index];
  }
  oss << "], voter_count=" << summary.voter_count
      << ", learner_count=" << summary.learner_count
      << ", quorum=" << summary.quorum_size
      << ", local_role=" << static_cast<int>(summary.local_role);
  return oss.str();
}

std::string DescribeRuntimeMembershipSummary(
    const RuntimeMembershipSummary& summary) {
  std::ostringstream oss;
  oss << "commit_index=" << summary.committed_log_index
      << ", term=" << summary.committed_term
      << ", voters=[";
  for (std::size_t index = 0; index < summary.voter_ids.size(); ++index) {
    if (index != 0) {
      oss << ",";
    }
    oss << summary.voter_ids[index];
  }
  oss << "], learners=[";
  for (std::size_t index = 0; index < summary.learner_ids.size(); ++index) {
    if (index != 0) {
      oss << ",";
    }
    oss << summary.learner_ids[index];
  }
  oss << "], voter_count=" << summary.voter_count
      << ", learner_count=" << summary.learner_count
      << ", committed_voter_quorum=" << summary.committed_voter_quorum_size
      << ", local_role=" << static_cast<int>(summary.local_role);
  return oss.str();
}

AddLearnerProposalRequest MakePendingLearnerProposalRequest(
    const std::string& cluster_id,
    const std::string& node_id,
    std::int32_t candidate_raft_id,
    std::uint16_t candidate_client_port,
    std::uint16_t candidate_raft_port) {
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

void WriteLearnerMetadataIdentity(const std::filesystem::path& data_dir,
                                  const int raft_id,
                                  const std::string& cluster_node_id) {
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  ASSERT_FALSE(static_cast<bool>(ec)) << ec.message();

  const auto identity_path = data_dir / "node.identity";
  std::ofstream out(identity_path, std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << identity_path.string();
  out << "identity_version=2\n";
  out << "cluster_id=cluster-t073-election\n";
  out << "node_id=" << cluster_node_id << "\n";
  out << "node_type=metadata\n";
  out << "raft_id=" << raft_id << "\n";
  out << "membership_state=learner\n";
  out << "persistent_generation=1\n";
  out << "created_at_unix_ms=1710000000000\n";
  out << "source=config_generator\n";
  out.flush();
  ASSERT_TRUE(static_cast<bool>(out)) << identity_path.string();
}

class CountingVoteService final : public raft::RaftService::Service {
 public:
  grpc::Status RequestVote(grpc::ServerContext*,
                           const raft::VoteRequest* request,
                           raft::VoteResponse* response) override {
    request_vote_count_.fetch_add(1);
    last_candidate_id_.store(request->candidate_id());
    response->set_term(request->term());
    response->set_vote_granted(true);
    return grpc::Status::OK;
  }

  int request_vote_count() const { return request_vote_count_.load(); }
  int last_candidate_id() const { return last_candidate_id_.load(); }

 private:
  std::atomic<int> request_vote_count_{0};
  std::atomic<int> last_candidate_id_{-1};
};

class FakeLearnerVoteEndpoint {
 public:
  explicit FakeLearnerVoteEndpoint(const std::string& address) {
    grpc::ServerBuilder builder;
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service_);
    server_ = builder.BuildAndStart();
  }

  ~FakeLearnerVoteEndpoint() {
    if (server_) {
      server_->Shutdown();
    }
  }

  int request_vote_count() const { return service_.request_vote_count(); }
  int last_candidate_id() const { return service_.last_candidate_id(); }

 private:
  CountingVoteService service_;
  std::unique_ptr<grpc::Server> server_;
};

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

TEST(RaftElectionTest,
     PendingLearnerCandidateIsExcludedFromRequestVoteAndLeaderElectionQuorum) {
  constexpr const char* kClusterId = "cluster-t068-election";
  constexpr std::int32_t kLearnerRaftId = 4;
  constexpr std::uint16_t kLearnerClientPort = 54360;
  constexpr std::uint16_t kLearnerRaftPort = 54361;
  const std::vector<int> kCommittedVoters{1, 2, 3};

  ClusterRunner cluster(54250);
  cluster.Start();

  auto leader = cluster.WaitForLeader(5s);
  ASSERT_NE(leader, nullptr);
  ASSERT_TRUE(cluster.WaitUntilSingleLeader(2s));

  const auto leader_index = FindLeaderIndex(cluster.nodes());
  ASSERT_TRUE(leader_index.has_value());

  std::vector<std::size_t> follower_indexes;
  follower_indexes.reserve(cluster.nodes().size() - 1);
  for (std::size_t index = 0; index < cluster.nodes().size(); ++index) {
    if (index != *leader_index) {
      follower_indexes.push_back(index);
    }
  }
  ASSERT_EQ(follower_indexes.size(), 2U);

  const auto expected_successor_ids =
      ExtractNodeIds(cluster.nodes(), follower_indexes);
  ASSERT_EQ(expected_successor_ids.size(), 2U);

  FakeLearnerVoteEndpoint fake_learner_endpoint(
      "127.0.0.1:" + std::to_string(kLearnerRaftPort));

  const auto add_learner_result = leader->ProposeAddLearner(
      MakePendingLearnerProposalRequest(kClusterId,
                                        "meta-learner-pending",
                                        kLearnerRaftId,
                                        kLearnerClientPort,
                                        kLearnerRaftPort));
  ASSERT_EQ(add_learner_result.status,
            AddLearnerProposalStatus::kAcceptedPendingCommit)
      << add_learner_result.message;
  EXPECT_FALSE(add_learner_result.committed_membership_changed);
  EXPECT_EQ(add_learner_result.assigned_raft_id, kLearnerRaftId);
  EXPECT_TRUE(ContainsAll(add_learner_result.message,
                          {"committed membership log proposal", "promote-to-voter"}))
      << add_learner_result.message;

  const auto runtime_summary = leader->GetRuntimeMembershipSummary();
  EXPECT_EQ(runtime_summary.voter_ids, kCommittedVoters)
      << DescribeRuntimeMembershipSummary(runtime_summary);
  EXPECT_EQ(runtime_summary.learner_ids, std::vector<int>{kLearnerRaftId})
      << DescribeRuntimeMembershipSummary(runtime_summary);
  EXPECT_EQ(runtime_summary.voter_count, 3U)
      << DescribeRuntimeMembershipSummary(runtime_summary);
  EXPECT_EQ(runtime_summary.learner_count, 1U)
      << DescribeRuntimeMembershipSummary(runtime_summary);
  EXPECT_EQ(runtime_summary.committed_voter_quorum_size, 2U)
      << DescribeRuntimeMembershipSummary(runtime_summary);
  ASSERT_EQ(runtime_summary.learner_entries.size(), 1U);
  EXPECT_EQ(runtime_summary.learner_entries.front().role,
            RuntimeMembershipRole::kLearner);
  EXPECT_FALSE(runtime_summary.learner_entries.front().committed);
  EXPECT_TRUE(runtime_summary.learner_entries.front().pending);
  EXPECT_EQ(runtime_summary.learner_entries.front().raft_id, kLearnerRaftId);
  EXPECT_EQ(runtime_summary.learner_entries.front().canonical_node_id,
            "meta-learner-pending");

  for (const auto& node : cluster.nodes()) {
    const auto summary = node->GetCommittedMembershipQuorumSummary();
    EXPECT_EQ(summary.voter_ids, kCommittedVoters)
        << DescribeCommittedMembershipSummary(summary);
    EXPECT_TRUE(summary.learner_ids.empty())
        << DescribeCommittedMembershipSummary(summary);
    EXPECT_EQ(summary.voter_count, 3U)
        << DescribeCommittedMembershipSummary(summary);
    EXPECT_EQ(summary.learner_count, 0U)
        << DescribeCommittedMembershipSummary(summary);
    EXPECT_EQ(summary.quorum_size, 2U)
        << DescribeCommittedMembershipSummary(summary);
    EXPECT_EQ(summary.local_role, CommittedMembershipRole::kVoter)
        << DescribeCommittedMembershipSummary(summary);
  }

  leader->Stop();

  const auto successor =
      WaitForSingleLeaderAmong(cluster.nodes(), follower_indexes, 5s);
  ASSERT_NE(successor, nullptr);

  const auto successor_id = ExtractIntField(successor->Describe(), "node");
  ASSERT_TRUE(successor_id.has_value());
  EXPECT_TRUE(std::find(expected_successor_ids.begin(),
                        expected_successor_ids.end(),
                        *successor_id) != expected_successor_ids.end())
      << "unexpected successor=" << successor->Describe();

  std::this_thread::sleep_for(500ms);

  EXPECT_EQ(fake_learner_endpoint.request_vote_count(), 0)
      << "pending learner candidate unexpectedly received RequestVote RPCs"
      << ", last_candidate_id=" << fake_learner_endpoint.last_candidate_id();

  for (const auto index : follower_indexes) {
    const auto& node = cluster.nodes()[index];
    const auto summary = node->GetCommittedMembershipQuorumSummary();
    EXPECT_EQ(summary.voter_ids, kCommittedVoters)
        << DescribeCommittedMembershipSummary(summary);
    EXPECT_TRUE(summary.learner_ids.empty())
        << DescribeCommittedMembershipSummary(summary);
    EXPECT_EQ(summary.voter_count, 3U)
        << DescribeCommittedMembershipSummary(summary);
    EXPECT_EQ(summary.learner_count, 0U)
        << DescribeCommittedMembershipSummary(summary);
    EXPECT_EQ(summary.quorum_size, 2U)
        << DescribeCommittedMembershipSummary(summary);
    EXPECT_EQ(summary.local_role, CommittedMembershipRole::kVoter)
        << DescribeCommittedMembershipSummary(summary);
  }
}

TEST(RaftElectionTest,
     LearnerIdentityNodeRejectsVoteRequestsAndCannotSelfElectLeader) {
  std::random_device rd;
  const auto root =
      TestBinaryDir() / "raft_test_data" / "election" /
      ("raft_election_learner_identity_" + std::to_string(NowForPath()) + "_" +
       std::to_string(rd()));
  const auto data_dir = root / "raft_data" / "learner_node";
  const auto snapshot_dir = root / "raft_snapshots" / "learner_node";
  WriteLearnerMetadataIdentity(data_dir, 41, "meta-learner-local-t073");

  NodeConfig config;
  config.node_id = 41;
  config.address = "127.0.0.1:54391";
  config.peers = {};
  config.election_timeout_min = std::chrono::milliseconds(250);
  config.election_timeout_max = std::chrono::milliseconds(350);
  config.heartbeat_interval = std::chrono::milliseconds(80);
  config.rpc_deadline = std::chrono::milliseconds(300);
  config.data_dir = data_dir.string();

  snapshotConfig snapshot_config;
  snapshot_config.enabled = false;
  snapshot_config.snapshot_dir = snapshot_dir.string();

  auto learner = std::make_shared<RaftNode>(config, snapshot_config);
  const auto runtime_summary = learner->GetRuntimeMembershipSummary();
  EXPECT_EQ(runtime_summary.local_role, RuntimeMembershipRole::kLearner)
      << DescribeRuntimeMembershipSummary(runtime_summary);
  EXPECT_TRUE(runtime_summary.voter_ids.empty())
      << DescribeRuntimeMembershipSummary(runtime_summary);
  EXPECT_EQ(runtime_summary.learner_ids, std::vector<int>{41})
      << DescribeRuntimeMembershipSummary(runtime_summary);
  EXPECT_EQ(runtime_summary.voter_count, 0U)
      << DescribeRuntimeMembershipSummary(runtime_summary);
  EXPECT_EQ(runtime_summary.learner_count, 1U)
      << DescribeRuntimeMembershipSummary(runtime_summary);
  EXPECT_EQ(runtime_summary.committed_voter_quorum_size, 0U)
      << DescribeRuntimeMembershipSummary(runtime_summary);

  std::thread thread([learner]() {
    learner->Start();
    learner->Wait();
  });

  std::this_thread::sleep_for(900ms);

  const std::string before_vote_snapshot = learner->Describe();
  EXPECT_FALSE(IsLeaderSnapshot(before_vote_snapshot)) << before_vote_snapshot;
  EXPECT_EQ(ExtractIntField(before_vote_snapshot, "leader").value_or(-1), -1)
      << before_vote_snapshot;

  raft::VoteRequest request;
  request.set_term(7);
  request.set_candidate_id(1);
  request.set_last_log_index(0);
  request.set_last_log_term(0);

  raft::VoteResponse response;
  learner->OnRequestVote(request, &response);
  EXPECT_FALSE(response.vote_granted());
  EXPECT_EQ(response.term(), 7U);

  std::this_thread::sleep_for(300ms);

  const std::string after_vote_snapshot = learner->Describe();
  EXPECT_FALSE(IsLeaderSnapshot(after_vote_snapshot)) << after_vote_snapshot;
  EXPECT_FALSE(ContainsAll(after_vote_snapshot, {"role=Candidate"}))
      << after_vote_snapshot;

  learner->Stop();
  if (thread.joinable()) {
    thread.join();
  }

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

}  // namespace
}  // namespace raftdemo
