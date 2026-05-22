#include "metadata_raft_test_utils.h"
#include "support/raft_snapshot_restart_test_utils.h"
#include "raft/common/metadata_command.h"
#include "raft/state_machine/metadata_state_machine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>

namespace raftdemo {
namespace {

using raftdemo::test::DescribeCluster;
using raftdemo::test::FindNodeIndex;
using raftdemo::test::PickFollowerIndex;
using raftdemo::test::ProposeStatusName;
using raftdemo::test::ProposeWithRetry;
using raftdemo::test::SetCommand;
using raftdemo::test::SnapshotRestartTestBase;
using raftdemo::test::TestCluster;
using raftdemo::test::WaitForNodeFieldAtLeast;
using raftdemo::test::WaitForSingleLeader;
using raftdemo::test::WaitForStableLeader;
using raftdemo::test::WaitForValueOnAll;
using raftdemo::test::WaitForValueOnNode;
using raftdemo::test::WriteManyValues;

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
  WriteManyValues(cluster.Nodes(), "install_restart", 48, excluded);

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 6,
                                      std::chrono::seconds(20)))
      << "leader did not create snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  cluster.RestartNode(stopped_follower);

  ASSERT_TRUE(WaitForValueOnNode(cluster.Nodes()[stopped_follower],
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

  ASSERT_TRUE(WaitForValueOnNode(cluster.Nodes()[stopped_follower],
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

  WriteManyValues(cluster.Nodes(), "leader_restart", 32);

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
  ASSERT_TRUE(WaitForValueOnNode(cluster.Nodes()[leader_index],
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
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("after_leader_restart", "ok"),
                               std::chrono::seconds(15), &result, {}, &propose_diagnostics))
      << "write after leader restart failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message
      << ", diagnostics=" << propose_diagnostics;

  std::string replication_diagnostics;
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "after_leader_restart", "ok",
                                std::chrono::seconds(20), {}, &replication_diagnostics))
      << "cluster did not continue replication after compacted leader restart, diagnostics="
      << replication_diagnostics;
}

TEST_F(RaftSnapshotRestartTest, FullClusterRestartsAfterSnapshotAndContinuesWriting) {
  auto cluster = MakeCluster("full_cluster_restart", true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected";

  WriteManyValues(cluster.Nodes(), "full_restart", 40);

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "full_restart_39", "value_39",
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

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "full_restart_39", "value_39",
                                std::chrono::seconds(20)))
      << "cluster lost snapshot/log state after full restart";

  ProposeResult result;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("after_full_restart", "ok"),
                               std::chrono::seconds(15), &result))
      << "write after full restart failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "after_full_restart", "ok",
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
    EXPECT_EQ(state_machine->LastAppliedTerm(), 0U);
    EXPECT_GE(state_machine->LastAppliedIndex(), expected_before_restart.expected_last_applied_index);
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

  ASSERT_TRUE(raftdemo::test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expected_after_restart, std::chrono::seconds(20)))
      << "cluster did not continue after restoring metadata snapshot + tail logs";
}

}  // namespace
}  // namespace raftdemo
