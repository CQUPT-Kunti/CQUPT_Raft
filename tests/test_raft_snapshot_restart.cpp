#include "metadata_raft_test_utils.h"
#include "support/raft_snapshot_restart_test_utils.h"
#include "raft/common/metadata_command.h"
#include "raft/state_machine/metadata_state_machine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "metadata.grpc.pb.h"

namespace raftdemo {
namespace {

using raftdemo::test::DescribeCluster;
using raftdemo::test::FindNodeIndex;
using raftdemo::test::PickFollowerIndex;
using raftdemo::test::ProposeStatusName;
using raftdemo::test::ProposeWithRetry;
using raftdemo::test::SnapshotRestartTestBase;
using raftdemo::test::TestCluster;
using raftdemo::test::WaitForNodeFieldAtLeast;
using raftdemo::test::WaitForSyntheticObjectOnAll;
using raftdemo::test::WaitForSyntheticObjectOnNode;
using raftdemo::test::WaitForSingleLeader;
using raftdemo::test::WaitForStableLeader;
using raftdemo::test::WriteSyntheticObject;
using raftdemo::test::WriteSyntheticObjects;

std::optional<std::uint64_t> ExtractUnsignedDiagnosticValue(
    const std::string &text,
    const std::string &key) {
  const std::size_t begin = text.find(key);
  if (begin == std::string::npos) {
    return std::nullopt;
  }

  const std::size_t value_begin = begin + key.size();
  std::size_t value_end = value_begin;
  while (value_end < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[value_end])) != 0) {
    ++value_end;
  }
  if (value_end == value_begin) {
    return std::nullopt;
  }

  try {
    return static_cast<std::uint64_t>(
        std::stoull(text.substr(value_begin, value_end - value_begin)));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::size_t> ExtractBracketListEntryCount(
    const std::string &text,
    const std::string &key) {
  const std::size_t begin = text.find(key);
  if (begin == std::string::npos) {
    return std::nullopt;
  }

  const std::size_t list_begin = begin + key.size();
  const std::size_t list_end = text.find(']', list_begin);
  if (list_end == std::string::npos) {
    return std::nullopt;
  }
  if (list_end == list_begin) {
    return 0U;
  }

  std::size_t count = 1U;
  for (std::size_t index = list_begin; index < list_end; ++index) {
    if (text[index] == ',') {
      ++count;
    }
  }
  return count;
}

NodeConfig BuildDetachedLearnerLikeConfig(const std::filesystem::path &root,
                                          const int learner_id,
                                          const int learner_port) {
  NodeConfig learner;
  learner.node_id = learner_id;
  learner.address = "127.0.0.1:" + std::to_string(learner_port);
  learner.election_timeout_min = std::chrono::milliseconds(300);
  learner.election_timeout_max = std::chrono::milliseconds(600);
  learner.heartbeat_interval = std::chrono::milliseconds(80);
  learner.rpc_deadline = std::chrono::milliseconds(500);
  learner.data_dir = (root / ("learner_" + std::to_string(learner_id))).string();
  return learner;
}

snapshotConfig BuildDetachedLearnerSnapshotConfig(const std::filesystem::path &root,
                                                  const int learner_id) {
  snapshotConfig cfg;
  cfg.snapshot_dir =
      (root / "raft_snapshots" / ("learner_" + std::to_string(learner_id))).string();
  return cfg;
}

void WriteStructuredLearnerIdentity(const NodeConfig &learner) {
  std::error_code ec;
  std::filesystem::create_directories(learner.data_dir, ec);
  ASSERT_FALSE(ec) << ec.message();

  std::ofstream out(std::filesystem::path(learner.data_dir) / "node.identity",
                    std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out << "membership_state=learner\n";
  out.flush();
  ASSERT_TRUE(static_cast<bool>(out));
}

std::string DescribeCommittedMembershipSummary(
    const CommittedMembershipQuorumSummary &summary) {
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

std::string DescribeRuntimeMembershipSummary(const RuntimeMembershipSummary &summary) {
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

void ExpectNoCommittedFourVoterDiagnostic(const std::string &diagnostic,
                                          const std::string &context) {
  if (diagnostic.empty()) {
    return;
  }

  if (const auto voter_count = ExtractUnsignedDiagnosticValue(
          diagnostic,
          "committed_voter_count=");
      voter_count.has_value()) {
    EXPECT_NE(*voter_count, 4U)
        << context << "; diagnostic=" << diagnostic;
  }

  if (const auto voter_ids = ExtractBracketListEntryCount(
          diagnostic,
          "committed_voter_ids=[");
      voter_ids.has_value()) {
    EXPECT_NE(*voter_ids, 4U)
        << context << "; diagnostic=" << diagnostic;
  }
}

void ExpectCommittedThreeVoterBoundary(const std::shared_ptr<RaftNode> &node,
                                       const std::string &context) {
  ASSERT_NE(node, nullptr);
  const auto summary = node->GetCommittedMembershipQuorumSummary();
  EXPECT_EQ(summary.voter_ids, std::vector<int>({1, 2, 3}))
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
  EXPECT_TRUE(summary.learner_ids.empty())
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
  EXPECT_EQ(summary.voter_count, 3U)
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
  EXPECT_EQ(summary.learner_count, 0U)
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
  EXPECT_EQ(summary.quorum_size, 2U)
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
  EXPECT_NE(summary.voter_count, 4U)
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
}

void ExpectCommittedFiveVoterBoundary(const std::shared_ptr<RaftNode> &node,
                                      const std::vector<int> &expected_voter_ids,
                                      const std::string &context) {
  ASSERT_NE(node, nullptr);
  const auto summary = node->GetCommittedMembershipQuorumSummary();
  EXPECT_EQ(summary.voter_ids, expected_voter_ids)
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
  EXPECT_TRUE(summary.learner_ids.empty())
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
  EXPECT_EQ(summary.voter_count, expected_voter_ids.size())
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
  EXPECT_EQ(summary.learner_count, 0U)
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
  EXPECT_EQ(summary.quorum_size, 3U)
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
  EXPECT_NE(summary.voter_count, 4U)
      << context << "; summary=" << DescribeCommittedMembershipSummary(summary);
}

bool WaitForCommittedMembershipOnNodes(
    const std::vector<std::shared_ptr<RaftNode>> &nodes,
    const std::vector<int> &expected_voter_ids,
    const std::size_t expected_quorum_size,
    const std::chrono::milliseconds timeout,
    std::string *diagnostics) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string last_snapshot;
  while (std::chrono::steady_clock::now() < deadline) {
    bool matched = true;
    std::ostringstream oss;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
      const auto &node = nodes[index];
      if (node == nullptr) {
        matched = false;
        oss << "node[" << index << "]=null; ";
        continue;
      }
      const auto summary = node->GetCommittedMembershipQuorumSummary();
      if (summary.voter_ids != expected_voter_ids ||
          !summary.learner_ids.empty() ||
          summary.voter_count != expected_voter_ids.size() ||
          summary.learner_count != 0U ||
          summary.quorum_size != expected_quorum_size) {
        matched = false;
      }
      oss << "node[" << index << "]="
          << DescribeCommittedMembershipSummary(summary) << "; ";
    }
    last_snapshot = oss.str();
    if (matched) {
      if (diagnostics != nullptr) {
        *diagnostics = last_snapshot;
      }
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (diagnostics != nullptr) {
    *diagnostics = last_snapshot;
  }
  return false;
}

void ExpectRuntimeStillTreatsLearnersAsNonVoters(
    const RuntimeMembershipSummary &summary,
    const std::vector<int> &candidate_learner_ids,
    const std::string &context) {
  EXPECT_EQ(summary.voter_ids, std::vector<int>({1, 2, 3}))
      << context << "; runtime=" << DescribeRuntimeMembershipSummary(summary);
  EXPECT_EQ(summary.voter_count, 3U)
      << context << "; runtime=" << DescribeRuntimeMembershipSummary(summary);
  EXPECT_EQ(summary.committed_voter_quorum_size, 2U)
      << context << "; runtime=" << DescribeRuntimeMembershipSummary(summary);
  EXPECT_NE(summary.voter_count, 4U)
      << context << "; runtime=" << DescribeRuntimeMembershipSummary(summary);
  for (const int learner_id : candidate_learner_ids) {
    EXPECT_TRUE(std::find(summary.voter_ids.begin(),
                          summary.voter_ids.end(),
                          learner_id) == summary.voter_ids.end())
        << context << "; learner_id=" << learner_id
        << " must not be restored as voter; runtime="
        << DescribeRuntimeMembershipSummary(summary);
  }
}

raft::JoinMetadataClusterRequest MakeJoinMetadataClusterRequest(
    const std::string &request_id,
    const std::string &cluster_id,
    const std::string &node_id,
    const std::int32_t candidate_raft_id,
    const std::uint16_t candidate_client_port,
    const std::uint16_t candidate_raft_port) {
  raft::JoinMetadataClusterRequest request;
  request.set_request_id(request_id);
  request.set_cluster_id(cluster_id);
  request.set_node_id(node_id);
  request.set_candidate_raft_id(candidate_raft_id);
  request.set_candidate_client_address("127.0.0.1:" +
                                       std::to_string(candidate_client_port));
  request.set_candidate_raft_address("127.0.0.1:" +
                                     std::to_string(candidate_raft_port));
  request.set_candidate_incarnation_id(node_id + ":boot:1710000000");
  request.set_candidate_sequence(1);
  request.set_persistent_generation(1);
  request.set_data_dir_fingerprint("fingerprint-" + node_id);
  request.set_local_state_hint(raft::JOIN_METADATA_CANDIDATE_STATE_HINT_CANDIDATE);
  request.set_observed_view_node_id("view-1");
  request.set_observed_time_unix_ms(1710000000123ULL);
  request.set_observed_metadata_endpoint("127.0.0.1:" +
                                         std::to_string(candidate_client_port));
  return request;
}

grpc::Status JoinMetadataClusterViaAddress(
    const std::string &address,
    const raft::JoinMetadataClusterRequest &request,
    raft::JoinMetadataClusterResponse *response) {
  auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
  auto stub = raft::MetadataService::NewStub(channel);
  grpc::ClientContext context;
  return stub->JoinMetadataCluster(&context, request, response);
}

class StandaloneNodeRunner {
 public:
  explicit StandaloneNodeRunner(std::shared_ptr<RaftNode> node)
      : node_(std::move(node)) {}

  ~StandaloneNodeRunner() { Stop(); }

  void Start() {
    if (!node_ || thread_.joinable()) {
      return;
    }
    thread_ = std::thread([node = node_]() {
      node->Start();
      node->Wait();
    });
  }

  void Stop() {
    if (node_) {
      node_->Stop();
    }
    if (thread_.joinable()) {
      thread_.join();
    }
  }

 private:
  std::shared_ptr<RaftNode> node_;
  std::thread thread_;
};

bool WaitForLearnerReplicationProgress(const std::shared_ptr<RaftNode> &leader,
                                       const int learner_raft_id,
                                       const std::uint64_t minimum_match_index,
                                       const std::chrono::milliseconds timeout,
                                       RuntimeMembershipEntry *learner_entry,
                                       std::string *diagnostics) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string last_snapshot;

  while (std::chrono::steady_clock::now() < deadline) {
    const auto summary =
        leader != nullptr ? leader->GetRuntimeMembershipSummary()
                          : RuntimeMembershipSummary{};
    last_snapshot = DescribeRuntimeMembershipSummary(summary);
    for (const auto &entry : summary.learner_entries) {
      if (entry.raft_id != learner_raft_id) {
        continue;
      }
      if (entry.match_index >= minimum_match_index &&
          entry.next_index >= entry.match_index) {
        if (learner_entry != nullptr) {
          *learner_entry = entry;
        }
        if (diagnostics != nullptr) {
          *diagnostics = last_snapshot;
        }
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (diagnostics != nullptr) {
    *diagnostics = last_snapshot;
  }
  return false;
}

class RaftSnapshotRestartTest : public SnapshotRestartTestBase {};

TEST_F(RaftSnapshotRestartTest, FollowerKeepsStateAfterInstallSnapshotAndRestart) {
  auto cluster = MakeCluster("follower_snapshot_restart", true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  const std::size_t stopped_follower = PickFollowerIndex(cluster.Nodes(), leader);
  ASSERT_LT(stopped_follower, cluster.Nodes().size()) << "failed to pick follower";
  cluster.StopNode(stopped_follower);

  const std::vector<std::size_t> excluded{stopped_follower};
  WriteSyntheticObjects(cluster.Nodes(), "install_restart", 48, excluded);

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(20)))
      << "leader did not create snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  cluster.RestartNode(stopped_follower);

  ASSERT_TRUE(WaitForSyntheticObjectOnNode(cluster.Nodes()[stopped_follower],
                                           "install_restart_47", "value_47",
                                           std::chrono::seconds(30)))
      << "follower did not catch up by InstallSnapshot, describe="
      << cluster.Nodes()[stopped_follower]->Describe();

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[stopped_follower],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(10)))
      << "follower did not record installed snapshot, describe="
      << cluster.Nodes()[stopped_follower]->Describe();

  cluster.RestartNode(stopped_follower);

  ASSERT_TRUE(WaitForSyntheticObjectOnNode(cluster.Nodes()[stopped_follower],
                                           "install_restart_47", "value_47",
                                           std::chrono::seconds(15)))
      << "follower lost snapshot state after restart, describe="
      << cluster.Nodes()[stopped_follower]->Describe();

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[stopped_follower],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(10)))
      << "follower lost snapshot metadata after restart, describe="
      << cluster.Nodes()[stopped_follower]->Describe();
}

TEST_F(RaftSnapshotRestartTest, LeaderKeepsCompactedSnapshotStateAfterRestart) {
  auto cluster = MakeCluster("leader_compaction_restart", true, 6);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before baseline writes, cluster="
      << DescribeCluster(cluster.Nodes());
  auto leader = stable_leader->leader;

  WriteSyntheticObjects(cluster.Nodes(), "leader_restart", 32);

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize after baseline writes, cluster="
      << DescribeCluster(cluster.Nodes());
  leader = stable_leader->leader;
  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  std::string snapshot_diagnostics;
  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(20),
                                      &snapshot_diagnostics))
      << "leader did not compact through snapshot, diagnostics="
      << snapshot_diagnostics << ", cluster=" << DescribeCluster(cluster.Nodes());

  cluster.RestartNode(leader_index);

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(10));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize after leader restart, cluster="
      << DescribeCluster(cluster.Nodes());

  std::string restore_diagnostics;
  ASSERT_TRUE(WaitForSyntheticObjectOnNode(cluster.Nodes()[leader_index],
                                           "leader_restart_31", "value_31",
                                           std::chrono::seconds(15),
                                           &restore_diagnostics))
      << "restarted leader node did not reload snapshot/log state, diagnostics="
      << restore_diagnostics << ", cluster=" << DescribeCluster(cluster.Nodes());

  std::string snapshot_meta_diagnostics;
  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(10),
                                      &snapshot_meta_diagnostics))
      << "restarted leader node lost snapshot metadata, diagnostics="
      << snapshot_meta_diagnostics
      << ", cluster=" << DescribeCluster(cluster.Nodes());

  ProposeResult result;
  std::string propose_diagnostics;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(),
                               WriteSyntheticObject("after_leader_restart", "ok"),
                               std::chrono::seconds(15), &result, {}, &propose_diagnostics))
      << "write after leader restart failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message
      << ", diagnostics=" << propose_diagnostics;

  std::string replication_diagnostics;
  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(), "after_leader_restart", "ok",
                                          std::chrono::seconds(20), {},
                                          &replication_diagnostics))
      << "cluster did not continue replication after compacted leader restart, diagnostics="
      << replication_diagnostics;
}

TEST_F(RaftSnapshotRestartTest, FullClusterRestartsAfterSnapshotAndContinuesWriting) {
  auto cluster = MakeCluster("full_cluster_restart", true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  WriteSyntheticObjects(cluster.Nodes(), "full_restart", 40);

  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(), "full_restart_39", "value_39",
                                          std::chrono::seconds(15)))
      << "cluster did not apply baseline data before restart";

  bool any_snapshot = false;
  for (const auto& node : cluster.Nodes()) {
    if (WaitForNodeFieldAtLeast(node, "last_snapshot_index", 6,
                                std::chrono::seconds(2))) {
      any_snapshot = true;
      break;
    }
  }
  ASSERT_TRUE(any_snapshot) << "no node created snapshot before full restart";

  cluster.StopAll();
  cluster.Start();

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(10));
  ASSERT_NE(leader, nullptr) << "no leader elected after full restart";

  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(), "full_restart_39", "value_39",
                                          std::chrono::seconds(20)))
      << "cluster lost snapshot/log state after full restart";

  ProposeResult result;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(),
                               WriteSyntheticObject("after_full_restart", "ok"),
                               std::chrono::seconds(15), &result))
      << "write after full restart failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;

  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(), "after_full_restart", "ok",
                                          std::chrono::seconds(20)))
      << "cluster did not replicate after full restart";
}

TEST_F(RaftSnapshotRestartTest, SnapshotAndPostSnapshotLogsRecoverAfterFullRestart) {
  auto cluster = MakeCluster("snapshot_plus_tail_logs", true, 12);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  const std::string bucket = "snapshot-plus-tail-bucket";
  ProposeResult result;
  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCreateBucketCommand(
          bucket, "snapshot-plus-tail-create-bucket-1"),
      std::chrono::seconds(10),
      &result));

  std::vector<raftdemo::test::ExpectedRecoveredMetadataObject> expected_objects;
  std::vector<std::string> visible_keys;
  for (int i = 0; i < 18; ++i) {
    const std::string suffix = (i < 10 ? "0" : "") + std::to_string(i);
    const std::string key = "snapshot_base_" + suffix;
    const std::string object_id = "obj-" + key;
    ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
        cluster.Nodes(),
        raftdemo::test::MakeCreateObjectCommand(
            bucket, key, object_id,
            "snapshot-plus-tail-create-base-" + suffix),
        std::chrono::seconds(10),
        &result));
    ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
        cluster.Nodes(),
        raftdemo::test::MakeCommitObjectCommand(
            bucket, key, object_id,
            "snapshot-plus-tail-commit-base-" + suffix),
        std::chrono::seconds(10),
        &result));
    expected_objects.push_back({key, object_id, 2U, false});
    visible_keys.push_back(key);
  }

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader after snapshot_base writes";
  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 12,
                                      std::chrono::seconds(20)))
      << "leader did not create baseline snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCreateObjectCommand(
          bucket, "tail_only", "obj-tail-only",
          "snapshot-plus-tail-create-tail-only"),
      std::chrono::seconds(10),
      &result));
  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCommitObjectCommand(
          bucket, "tail_only", "obj-tail-only",
          "snapshot-plus-tail-commit-tail-only"),
      std::chrono::seconds(10),
      &result));
  expected_objects.push_back({"tail_only", "obj-tail-only", 2U, false});
  visible_keys.push_back("tail_only");

  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCreateObjectCommand(
          bucket, "tail_delete", "obj-tail-delete",
          "snapshot-plus-tail-create-tail-delete"),
      std::chrono::seconds(10),
      &result));
  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCommitObjectCommand(
          bucket, "tail_delete", "obj-tail-delete",
          "snapshot-plus-tail-commit-tail-delete"),
      std::chrono::seconds(10),
      &result));
  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeDeleteObjectCommand(
          bucket, "tail_delete", "obj-tail-delete",
          "snapshot-plus-tail-delete-tail-delete"),
      std::chrono::seconds(10),
      &result));
  expected_objects.push_back({"tail_delete", "obj-tail-delete", 0U, true});

  std::sort(visible_keys.begin(), visible_keys.end());
  const raftdemo::test::MetadataRecoveryExpectation expected_before_restart{
      .bucket = bucket,
      .objects = expected_objects,
      .visible_keys = visible_keys,
      .expected_request_count = 42U,
      .expected_tombstone_count = 1U,
      .expected_last_applied_index = result.log_index,
      .expected_min_last_applied_term = result.term,
  };
  ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expected_before_restart, std::chrono::seconds(15)))
      << "cluster did not apply post-snapshot metadata tail logs";

  cluster.StopAll();
  cluster.Start();

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(10));
  ASSERT_NE(leader, nullptr) << "no leader after restarting snapshot + tail log cluster";

  ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expected_before_restart, std::chrono::seconds(20)))
      << "metadata snapshot-covered state plus tail logs were not restored after restart";
  for (const auto& node : cluster.Nodes()) {
    const MetadataStateMachine* state_machine = node->GetMetadataStateMachineV2();
    ASSERT_NE(state_machine, nullptr);
    EXPECT_EQ(state_machine->RequestCount(), 42U);
    EXPECT_EQ(state_machine->TombstoneCount(), 1U);
    EXPECT_GE(state_machine->LastAppliedTerm(), result.term);
    EXPECT_GE(state_machine->LastAppliedIndex(),
              expected_before_restart.expected_last_applied_index);
  }

  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCreateObjectCommand(
          bucket, "after_tail_restart", "obj-after-tail-restart",
          "snapshot-plus-tail-create-after-restart"),
      std::chrono::seconds(15),
      &result));
  ASSERT_TRUE(raftdemo::test::ProposeMetadataCommandWithRetry(
      cluster.Nodes(),
      raftdemo::test::MakeCommitObjectCommand(
          bucket, "after_tail_restart", "obj-after-tail-restart",
          "snapshot-plus-tail-commit-after-restart"),
      std::chrono::seconds(15),
      &result));

  auto expected_after_restart = expected_before_restart;
  expected_after_restart.objects.push_back(
      {"after_tail_restart", "obj-after-tail-restart", 2U, false});
  expected_after_restart.visible_keys.push_back("after_tail_restart");
  std::sort(expected_after_restart.visible_keys.begin(),
            expected_after_restart.visible_keys.end());
  expected_after_restart.expected_request_count = 44U;
  expected_after_restart.expected_last_applied_index = result.log_index;
  expected_after_restart.expected_min_last_applied_term = result.term;

  ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expected_after_restart, std::chrono::seconds(20)))
      << "cluster did not continue after restoring metadata snapshot + tail logs";
}

TEST_F(RaftSnapshotRestartTest,
       RestartRecoveryDoesNotTreatBlockedBatchPromoteAsCommittedFiveVoterMembership) {
  constexpr const char *kClusterId = "cluster-t081-batch-membership-restart";
  constexpr const char *kFirstLearnerNodeId = "meta-restart-learner-a-t081";
  constexpr const char *kSecondLearnerNodeId = "meta-restart-learner-b-t081";
  constexpr std::int32_t kFirstLearnerRaftId = 381;
  constexpr std::int32_t kSecondLearnerRaftId = 382;

  auto cluster = MakeCluster("batch_membership_restart_recovery", true, 6);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before snapshot baseline writes, cluster="
      << DescribeCluster(cluster.Nodes());
  auto leader = stable_leader->leader;

  WriteSyntheticObjects(cluster.Nodes(), "t081_snapshot_seed", 24);

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize after snapshot baseline writes, cluster="
      << DescribeCluster(cluster.Nodes());
  leader = stable_leader->leader;
  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  std::string snapshot_diagnostics;
  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index",
                                      6,
                                      std::chrono::seconds(20),
                                      &snapshot_diagnostics))
      << "leader did not create snapshot before learner restart recovery test, diagnostics="
      << snapshot_diagnostics << ", cluster=" << DescribeCluster(cluster.Nodes());

  for (const auto &node : cluster.Nodes()) {
    ExpectCommittedThreeVoterBoundary(node, "initial restart baseline before learners");
  }

  const auto learners_root = root_ / "t081_detached_learners";
  const auto first_learner_config = BuildDetachedLearnerLikeConfig(
      learners_root, kFirstLearnerRaftId, base_port_ + 381);
  const auto second_learner_config = BuildDetachedLearnerLikeConfig(
      learners_root, kSecondLearnerRaftId, base_port_ + 382);
  WriteStructuredLearnerIdentity(first_learner_config);
  WriteStructuredLearnerIdentity(second_learner_config);

  const auto first_learner_snapshot_config =
      BuildDetachedLearnerSnapshotConfig(learners_root, kFirstLearnerRaftId);
  const auto second_learner_snapshot_config =
      BuildDetachedLearnerSnapshotConfig(learners_root, kSecondLearnerRaftId);

  StandaloneNodeRunner first_learner_runner(
      std::make_shared<RaftNode>(first_learner_config, first_learner_snapshot_config));
  StandaloneNodeRunner second_learner_runner(std::make_shared<RaftNode>(
      second_learner_config, second_learner_snapshot_config));

  const std::uint64_t restart_recovery_frontier =
      leader->GetStatusSnapshot().commit_index;
  ASSERT_GT(restart_recovery_frontier, 0U);
  const std::vector<int> promoted_voters{
      1, 2, 3, kFirstLearnerRaftId, kSecondLearnerRaftId};

  std::vector<std::string> observed_diagnostics;
  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before first learner admission, cluster="
      << DescribeCluster(cluster.Nodes());
  leader = stable_leader->leader;

  raft::JoinMetadataClusterRequest first_join_request =
      MakeJoinMetadataClusterRequest("req-join-t081-learner-a",
                                     kClusterId,
                                     kFirstLearnerNodeId,
                                     kFirstLearnerRaftId,
                                     static_cast<std::uint16_t>(base_port_ + 1381),
                                     static_cast<std::uint16_t>(base_port_ + 381));
  raft::JoinMetadataClusterResponse first_join_response;
  ASSERT_TRUE(JoinMetadataClusterViaAddress(leader->GetStatusSnapshot().address,
                                            first_join_request,
                                            &first_join_response)
                  .ok());
  ASSERT_EQ(first_join_response.summary().code(), raft::METADATA_STATUS_CODE_OK)
      << first_join_response.summary().message();
  ASSERT_EQ(first_join_response.disposition(),
            raft::JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT)
      << first_join_response.summary().message();
  ASSERT_FALSE(first_join_response.committed_membership_changed());
  EXPECT_EQ(first_join_response.requested_membership(),
            raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
  EXPECT_TRUE(first_join_response.summary().message().find("learner_status=pending") !=
              std::string::npos)
      << first_join_response.summary().message();
  observed_diagnostics.push_back(first_join_response.summary().message());
  ExpectNoCommittedFourVoterDiagnostic(first_join_response.summary().message(),
                                       "first learner accepted before restart");

  first_learner_runner.Start();

  RuntimeMembershipEntry first_learner_progress;
  std::string first_learner_progress_diagnostics;
  ASSERT_TRUE(WaitForLearnerReplicationProgress(leader,
                                                kFirstLearnerRaftId,
                                                restart_recovery_frontier,
                                                std::chrono::seconds(8),
                                                &first_learner_progress,
                                                &first_learner_progress_diagnostics))
      << "first learner did not catch up to restart frontier, diagnostics="
      << first_learner_progress_diagnostics
      << ", cluster=" << DescribeCluster(cluster.Nodes());

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before ready-to-promote re-query, cluster="
      << DescribeCluster(cluster.Nodes());
  leader = stable_leader->leader;

  raft::JoinMetadataClusterResponse first_ready_response;
  ASSERT_TRUE(JoinMetadataClusterViaAddress(leader->GetStatusSnapshot().address,
                                            first_join_request,
                                            &first_ready_response)
                  .ok());
  EXPECT_EQ(first_ready_response.summary().code(),
            raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
      << first_ready_response.summary().message();
  EXPECT_EQ(first_ready_response.disposition(),
            raft::JOIN_METADATA_CLUSTER_DISPOSITION_DUPLICATE)
      << first_ready_response.summary().message();
  EXPECT_FALSE(first_ready_response.committed_membership_changed());
  EXPECT_TRUE(first_ready_response.summary().message().find("learner_status=ready_to_promote") !=
              std::string::npos)
      << first_ready_response.summary().message();
  EXPECT_TRUE(first_ready_response.summary().message().find("promotion_status=waiting_for_pair") !=
              std::string::npos)
      << first_ready_response.summary().message();
  EXPECT_TRUE(first_ready_response.summary().message().find("promotion_block_reason=even_voter_count") !=
              std::string::npos)
      << first_ready_response.summary().message();
  EXPECT_TRUE(first_ready_response.summary().message().find("committed_quorum_size=2") !=
              std::string::npos)
      << first_ready_response.summary().message();
  observed_diagnostics.push_back(first_ready_response.summary().message());
  ExpectNoCommittedFourVoterDiagnostic(first_ready_response.summary().message(),
                                       "first learner waiting_for_pair before restart");

  const auto ready_runtime = leader->GetRuntimeMembershipSummary();
  ExpectRuntimeStillTreatsLearnersAsNonVoters(
      ready_runtime,
      {kFirstLearnerRaftId, kSecondLearnerRaftId},
      "single ready learner before restart");
  EXPECT_EQ(ready_runtime.learner_ids, std::vector<int>({kFirstLearnerRaftId}))
      << DescribeRuntimeMembershipSummary(ready_runtime);
  EXPECT_EQ(ready_runtime.learner_count, 1U)
      << DescribeRuntimeMembershipSummary(ready_runtime);

  raft::JoinMetadataClusterRequest second_join_request =
      MakeJoinMetadataClusterRequest("req-join-t081-learner-b",
                                     kClusterId,
                                     kSecondLearnerNodeId,
                                     kSecondLearnerRaftId,
                                     static_cast<std::uint16_t>(base_port_ + 1382),
                                     static_cast<std::uint16_t>(base_port_ + 382));
  raft::JoinMetadataClusterResponse second_join_response;
  ASSERT_TRUE(JoinMetadataClusterViaAddress(leader->GetStatusSnapshot().address,
                                            second_join_request,
                                            &second_join_response)
                  .ok());
  EXPECT_FALSE(second_join_response.committed_membership_changed());
  EXPECT_EQ(second_join_response.requested_membership(),
            raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
  observed_diagnostics.push_back(second_join_response.summary().message());
  ExpectNoCommittedFourVoterDiagnostic(second_join_response.summary().message(),
                                       "second learner join attempt before restart");

  second_learner_runner.Start();

  RuntimeMembershipEntry second_learner_progress;
  std::string second_learner_progress_diagnostics;
  ASSERT_TRUE(WaitForLearnerReplicationProgress(leader,
                                                kSecondLearnerRaftId,
                                                restart_recovery_frontier,
                                                std::chrono::seconds(8),
                                                &second_learner_progress,
                                                &second_learner_progress_diagnostics))
      << "second learner did not catch up to restart frontier before batch commit, diagnostics="
      << second_learner_progress_diagnostics
      << ", cluster=" << DescribeCluster(cluster.Nodes());

  std::string committed_five_diagnostics;
  ASSERT_TRUE(WaitForCommittedMembershipOnNodes(cluster.Nodes(),
                                                promoted_voters,
                                                3U,
                                                std::chrono::seconds(8),
                                                &committed_five_diagnostics))
      << "cluster did not reach committed 5-voter membership before restart, diagnostics="
      << committed_five_diagnostics << ", cluster=" << DescribeCluster(cluster.Nodes());

  cluster.StopAll();
  first_learner_runner.Stop();
  second_learner_runner.Stop();
  cluster.Start();

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(10));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize after full restart, cluster="
      << DescribeCluster(cluster.Nodes());
  leader = stable_leader->leader;

  for (const auto &node : cluster.Nodes()) {
    ExpectCommittedFiveVoterBoundary(
        node, promoted_voters, "committed boundary after snapshot restart");
  }

  const auto recovered_runtime = leader->GetRuntimeMembershipSummary();
  EXPECT_EQ(recovered_runtime.voter_ids, promoted_voters)
      << DescribeRuntimeMembershipSummary(recovered_runtime);
  EXPECT_EQ(recovered_runtime.voter_count, 5U)
      << DescribeRuntimeMembershipSummary(recovered_runtime);
  EXPECT_EQ(recovered_runtime.learner_count, 0U)
      << DescribeRuntimeMembershipSummary(recovered_runtime);
  EXPECT_EQ(recovered_runtime.committed_voter_quorum_size, 3U)
      << DescribeRuntimeMembershipSummary(recovered_runtime);

  std::string replay_diagnostics;
  ASSERT_TRUE(WaitForSyntheticObjectOnAll(cluster.Nodes(),
                                          "t081_snapshot_seed_23",
                                          "value_23",
                                          std::chrono::seconds(20),
                                          {},
                                          &replay_diagnostics))
      << "cluster lost snapshot-covered data after restart, diagnostics="
      << replay_diagnostics;

  raft::JoinMetadataClusterResponse retry_first_after_restart;
  ASSERT_TRUE(JoinMetadataClusterViaAddress(leader->GetStatusSnapshot().address,
                                            first_join_request,
                                            &retry_first_after_restart)
                  .ok());
  EXPECT_FALSE(retry_first_after_restart.committed_membership_changed());
  EXPECT_EQ(retry_first_after_restart.summary().code(),
            raft::METADATA_STATUS_CODE_INVALID_ARGUMENT)
      << retry_first_after_restart.summary().message();
  EXPECT_TRUE(retry_first_after_restart.summary().message().find(
                  "candidate_raft_id already exists in committed voter set") !=
              std::string::npos)
      << retry_first_after_restart.summary().message();
  observed_diagnostics.push_back(retry_first_after_restart.summary().message());
  ExpectNoCommittedFourVoterDiagnostic(retry_first_after_restart.summary().message(),
                                       "retry first learner after restart");

  const auto retry_runtime = leader->GetRuntimeMembershipSummary();
  EXPECT_EQ(retry_runtime.voter_ids, promoted_voters)
      << DescribeRuntimeMembershipSummary(retry_runtime);
  EXPECT_EQ(retry_runtime.voter_count, 5U)
      << DescribeRuntimeMembershipSummary(retry_runtime);
  EXPECT_EQ(retry_runtime.learner_count, 0U)
      << DescribeRuntimeMembershipSummary(retry_runtime);

  raft::JoinMetadataClusterResponse duplicate_first_after_restart;
  ASSERT_TRUE(JoinMetadataClusterViaAddress(leader->GetStatusSnapshot().address,
                                            first_join_request,
                                            &duplicate_first_after_restart)
                  .ok());
  EXPECT_FALSE(duplicate_first_after_restart.committed_membership_changed());
  EXPECT_EQ(duplicate_first_after_restart.summary().code(),
            raft::METADATA_STATUS_CODE_INVALID_ARGUMENT)
      << duplicate_first_after_restart.summary().message();
  observed_diagnostics.push_back(duplicate_first_after_restart.summary().message());
  ExpectNoCommittedFourVoterDiagnostic(duplicate_first_after_restart.summary().message(),
                                       "duplicate first learner retry after restart");

  raft::JoinMetadataClusterResponse retry_second_after_restart;
  ASSERT_TRUE(JoinMetadataClusterViaAddress(leader->GetStatusSnapshot().address,
                                            second_join_request,
                                            &retry_second_after_restart)
                  .ok());
  EXPECT_FALSE(retry_second_after_restart.committed_membership_changed());
  EXPECT_EQ(retry_second_after_restart.summary().code(),
            raft::METADATA_STATUS_CODE_INVALID_ARGUMENT)
      << retry_second_after_restart.summary().message();
  observed_diagnostics.push_back(retry_second_after_restart.summary().message());
  ExpectNoCommittedFourVoterDiagnostic(retry_second_after_restart.summary().message(),
                                       "retry second learner after restart");
}

}  // namespace
}  // namespace raftdemo
