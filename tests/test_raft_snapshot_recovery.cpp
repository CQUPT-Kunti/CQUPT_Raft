#include "metadata_raft_test_utils.h"
#include "support/raft_snapshot_restart_test_utils.h"
#include "raft/state_machine/metadata_state_machine.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace raftdemo {
namespace {

using raftdemo::test::BuildThreeNodeConfigs;
using raftdemo::test::BuildThreeSnapshotConfigs;
using raftdemo::test::CopyDirectoryRecursively;
using raftdemo::test::CopyFile;
using raftdemo::test::DescribeCluster;
using raftdemo::test::ExpectedLinuxSpecificMarker;
using raftdemo::test::FormatSnapshotIndex;
using raftdemo::test::JoinIssueReasons;
using raftdemo::test::kSnapshotStorageFailpointEnv;
using raftdemo::test::ListSnapshotDirs;
using raftdemo::test::PickFollowerIndex;
using raftdemo::test::ProposeWithRetry;
using raftdemo::test::ScopedEnvVar;
using raftdemo::test::SetCommand;
using raftdemo::test::DeleteCommand;
using raftdemo::test::SnapshotIndexFromDir;
using raftdemo::test::SnapshotRestartTestBase;
using raftdemo::test::WaitForMissingOnAll;
using raftdemo::test::WaitForNodeFieldAtLeast;
using raftdemo::test::WaitForOrderedCommitApplyAtLeast;
using raftdemo::test::WaitForStableLeader;
using raftdemo::test::WaitForValueOnAll;
using raftdemo::test::WaitForValueOnNode;
using raftdemo::test::WriteManyValues;
using raftdemo::test::ExtractUintField;
using raftdemo::test::WriteTextFile;

class RaftSnapshotRecoveryTest : public SnapshotRestartTestBase {};

TEST_F(RaftSnapshotRecoveryTest,
       FullRestartReplaysSnapshotTailWithoutLosingDeletesOrOverwrites) {
  auto cluster = MakeCluster("snapshot_tail_replay_consistency", true, 12);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before replay consistency test, cluster="
      << DescribeCluster(cluster.Nodes());

  ProposeResult result;
  std::string propose_diagnostics;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("snapshot_only", "base"),
                               std::chrono::seconds(10), &result, {}, &propose_diagnostics))
      << "snapshot_only baseline write failed, diagnostics=" << propose_diagnostics;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("tail_delete", "present"),
                               std::chrono::seconds(10), &result, {}, &propose_diagnostics))
      << "tail_delete baseline write failed, diagnostics=" << propose_diagnostics;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("tail_overwrite", "before"),
                               std::chrono::seconds(10), &result, {}, &propose_diagnostics))
      << "tail_overwrite baseline write failed, diagnostics=" << propose_diagnostics;

  WriteManyValues(cluster.Nodes(), "replay_base", 24);

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize after baseline writes, cluster="
      << DescribeCluster(cluster.Nodes());
  const std::size_t leader_index = stable_leader->leader_index;

  std::string snapshot_diagnostics;
  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index",
                                      12,
                                      std::chrono::seconds(20),
                                      &snapshot_diagnostics))
      << "leader did not create trusted snapshot before tail replay test, diagnostics="
      << snapshot_diagnostics;

  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), DeleteCommand("tail_delete"),
                               std::chrono::seconds(10), &result, {}, &propose_diagnostics))
      << "tail_delete replay failed, diagnostics=" << propose_diagnostics;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("tail_overwrite", "after"),
                               std::chrono::seconds(10), &result, {}, &propose_diagnostics))
      << "tail_overwrite replay failed, diagnostics=" << propose_diagnostics;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("tail_only", "replayed"),
                               std::chrono::seconds(10), &result, {}, &propose_diagnostics))
      << "tail_only replay failed, diagnostics=" << propose_diagnostics;

  const std::uint64_t expected_tail_index = result.log_index;

  std::string cluster_diagnostics;
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "snapshot_only", "base",
                                std::chrono::seconds(15), {}, &cluster_diagnostics))
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "tail_overwrite", "after",
                                std::chrono::seconds(15), {}, &cluster_diagnostics))
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "tail_only", "replayed",
                                std::chrono::seconds(15), {}, &cluster_diagnostics))
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForMissingOnAll(cluster.Nodes(), "tail_delete",
                                  std::chrono::seconds(15), {}, &cluster_diagnostics))
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(), expected_tail_index,
                                               std::chrono::seconds(15),
                                               &cluster_diagnostics))
      << cluster_diagnostics;

  cluster.StopAll();
  cluster.Start();

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(10));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize after full restart, cluster="
      << DescribeCluster(cluster.Nodes());

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "snapshot_only", "base",
                                std::chrono::seconds(20), {}, &cluster_diagnostics))
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "tail_overwrite", "after",
                                std::chrono::seconds(20), {}, &cluster_diagnostics))
      << "snapshot load plus tail replay lost overwrite semantics, diagnostics="
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "tail_only", "replayed",
                                std::chrono::seconds(20), {}, &cluster_diagnostics))
      << "snapshot load plus tail replay lost tail-only value, diagnostics="
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForMissingOnAll(cluster.Nodes(), "tail_delete",
                                  std::chrono::seconds(20), {}, &cluster_diagnostics))
      << "snapshot load plus tail replay lost delete semantics, diagnostics="
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(), expected_tail_index,
                                               std::chrono::seconds(20),
                                               &cluster_diagnostics))
      << "restart did not advance apply frontier to committed tail boundary, diagnostics="
      << cluster_diagnostics;
}

TEST_F(RaftSnapshotRecoveryTest,
       RestartedFollowerAppliesCommittedTailExactlyOnceAfterSnapshotLoad) {
  auto cluster = MakeCluster("follower_restart_apply_consistency", true, 6);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before follower replay test, cluster="
      << DescribeCluster(cluster.Nodes());

  const std::size_t leader_index = stable_leader->leader_index;
  const std::size_t stopped_follower = PickFollowerIndex(cluster.Nodes(), stable_leader->leader);
  ASSERT_LT(stopped_follower, cluster.Nodes().size()) << "failed to pick follower";
  cluster.StopNode(stopped_follower);

  const std::vector<std::size_t> excluded{stopped_follower};
  ProposeResult result;
  std::string propose_diagnostics;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("apply_anchor", "snapshot"),
                               std::chrono::seconds(10), &result, excluded,
                               &propose_diagnostics))
      << "apply_anchor baseline write failed, diagnostics=" << propose_diagnostics;

  WriteManyValues(cluster.Nodes(), "apply_gap", 24, excluded);

  std::string cluster_diagnostics;
  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index",
                                      6,
                                      std::chrono::seconds(20),
                                      &cluster_diagnostics))
      << "leader did not create snapshot before follower restart apply test, diagnostics="
      << cluster_diagnostics;

  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("apply_view", "before_delete"),
                               std::chrono::seconds(10), &result, excluded,
                               &propose_diagnostics))
      << "apply_view initial write failed, diagnostics=" << propose_diagnostics;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), DeleteCommand("apply_view"),
                               std::chrono::seconds(10), &result, excluded,
                               &propose_diagnostics))
      << "apply_view delete failed, diagnostics=" << propose_diagnostics;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("apply_view", "after_replay"),
                               std::chrono::seconds(10), &result, excluded,
                               &propose_diagnostics))
      << "apply_view overwrite failed, diagnostics=" << propose_diagnostics;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("apply_tail", "committed"),
                               std::chrono::seconds(10), &result, excluded,
                               &propose_diagnostics))
      << "apply_tail write failed, diagnostics=" << propose_diagnostics;

  const std::uint64_t expected_tail_index = result.log_index;

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "apply_view", "after_replay",
                                std::chrono::seconds(15), excluded, &cluster_diagnostics))
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "apply_tail", "committed",
                                std::chrono::seconds(15), excluded, &cluster_diagnostics))
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(), expected_tail_index,
                                               std::chrono::seconds(15),
                                               &cluster_diagnostics,
                                               excluded))
      << cluster_diagnostics;

  cluster.RestartNode(stopped_follower);

  ASSERT_TRUE(WaitForValueOnNode(cluster.Nodes()[stopped_follower],
                                 "apply_anchor", "snapshot",
                                 std::chrono::seconds(20),
                                 &cluster_diagnostics))
      << "restarted follower lost snapshot-covered state, diagnostics="
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForValueOnNode(cluster.Nodes()[stopped_follower],
                                 "apply_view", "after_replay",
                                 std::chrono::seconds(20),
                                 &cluster_diagnostics))
      << "restarted follower did not replay committed overwrite after snapshot load, diagnostics="
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForValueOnNode(cluster.Nodes()[stopped_follower],
                                 "apply_tail", "committed",
                                 std::chrono::seconds(20),
                                 &cluster_diagnostics))
      << "restarted follower missed committed tail apply after snapshot load, diagnostics="
      << cluster_diagnostics;
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(), expected_tail_index,
                                               std::chrono::seconds(20),
                                               &cluster_diagnostics))
      << "cluster did not preserve ordered commit/apply frontier after follower restart, "
      << "diagnostics=" << cluster_diagnostics;
}

TEST_F(RaftSnapshotRecoveryTest, StandaloneRestartFallsBackToOlderTrustedSnapshotWhenNewestSnapshotIsCorrupted) {
  constexpr std::uint64_t kSnapshotThreshold = 4;
  const std::string case_name = "restart_trusted_snapshot_fallback";
  auto cluster = MakeCluster(case_name, true, kSnapshotThreshold);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before snapshot fallback test, cluster="
      << DescribeCluster(cluster.Nodes());

  WriteManyValues(cluster.Nodes(), "restart_fallback", 48);

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize after baseline writes, cluster="
      << DescribeCluster(cluster.Nodes());
  const std::size_t leader_index = stable_leader->leader_index;

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 8,
                                      std::chrono::seconds(20)))
      << "leader did not create snapshots before restart fallback test, describe="
      << cluster.Nodes()[leader_index]->Describe();

  cluster.StopAll();

  const std::filesystem::path node_snapshot_root =
      snapshot_root_ / case_name / ("node_" + std::to_string(leader_index + 1));
  const auto snapshot_dirs = ListSnapshotDirs(node_snapshot_root);
  ASSERT_GE(snapshot_dirs.size(), 2U) << "need at least two published snapshots under "
                                      << node_snapshot_root.string();

  const auto older_snapshot_dir = snapshot_dirs[snapshot_dirs.size() - 2];
  const auto latest_snapshot_dir = snapshot_dirs.back();
  const auto older_snapshot_index = SnapshotIndexFromDir(older_snapshot_dir);
  const auto latest_snapshot_index = SnapshotIndexFromDir(latest_snapshot_dir);
  ASSERT_TRUE(older_snapshot_index.has_value()) << older_snapshot_dir.string();
  ASSERT_TRUE(latest_snapshot_index.has_value()) << latest_snapshot_dir.string();
  ASSERT_GT(*latest_snapshot_index, *older_snapshot_index);

  WriteTextFile(latest_snapshot_dir / "data.bin", "corrupted-newest-snapshot");

  auto configs = BuildThreeNodeConfigs(data_root_ / case_name, base_port_);
  auto snapshot_configs =
      BuildThreeSnapshotConfigs(snapshot_root_ / case_name, true, kSnapshotThreshold);
  auto restarted = std::make_shared<RaftNode>(configs[leader_index], snapshot_configs[leader_index]);

  std::string actual;
  ASSERT_TRUE(restarted->DebugGetValue("restart_fallback_40", &actual))
      << "restart did not retain data from a previously trusted snapshot after rejecting the corrupted newest snapshot, describe="
      << restarted->Describe();
  EXPECT_EQ(actual, "value_40");

  const std::string description = restarted->Describe();
  const auto restored_snapshot_index = ExtractUintField(description, "last_snapshot_index");
  ASSERT_TRUE(restored_snapshot_index.has_value()) << description;
  EXPECT_GE(*restored_snapshot_index, *older_snapshot_index)
      << "restart should still recover from a trusted snapshot boundary after rejecting the corrupted newest snapshot, describe="
      << description;
}

TEST_F(RaftSnapshotRecoveryTest, RestartAfterSnapshotPublishFailureNeedsExactFailureInjectionSeam) {
  constexpr std::uint64_t kSnapshotThreshold = 4;
  const std::string case_name = "restart_publish_failure_contract";
  auto cluster = MakeCluster(case_name, true, kSnapshotThreshold);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before publish failure test, cluster="
      << DescribeCluster(cluster.Nodes());

  WriteManyValues(cluster.Nodes(), "restart_publish", 48);

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize after baseline writes, cluster="
      << DescribeCluster(cluster.Nodes());
  const std::size_t leader_index = stable_leader->leader_index;

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 8,
                                      std::chrono::seconds(20)))
      << "leader did not create snapshots before publish failure test, describe="
      << cluster.Nodes()[leader_index]->Describe();

  cluster.StopAll();

  const std::filesystem::path node_snapshot_root =
      snapshot_root_ / case_name / ("node_" + std::to_string(leader_index + 1));
  auto storage = CreateFileSnapshotStorage(node_snapshot_root.string(), "snapshot");

  std::vector<SnapshotMeta> trusted_snapshots;
  std::string error;
  ASSERT_TRUE(storage->ListSnapshots(&trusted_snapshots, &error)) << error;
  ASSERT_FALSE(trusted_snapshots.empty()) << node_snapshot_root.string();

  const SnapshotMeta trusted_before = trusted_snapshots.front();
  const std::uint64_t injected_index = trusted_before.last_included_index + 4;
  const std::filesystem::path injected_final_dir =
      node_snapshot_root / ("snapshot_" + FormatSnapshotIndex(injected_index));
  const std::filesystem::path injected_input =
      root_ / "injected_snapshot_inputs" / ("node_" + std::to_string(leader_index + 1) + ".bin");
  CopyFile(trusted_before.snapshot_path, injected_input);

  SnapshotMeta unused_meta;
  {
    ScopedEnvVar failpoint(kSnapshotStorageFailpointEnv,
                           "snapshot_publish_visible_before_trusted_directory_sync");
    EXPECT_FALSE(storage->SaveSnapshotFile(injected_input.string(),
                                          injected_index,
                                          trusted_before.last_included_term,
                                          &unused_meta,
                                          &error));
  }
  EXPECT_NE(error.find("operation=snapshot publish visible before trusted directory sync"),
            std::string::npos)
      << error;
  EXPECT_NE(error.find("path=" + injected_final_dir.string()), std::string::npos) << error;
  EXPECT_NE(error.find("failure_class=directory sync"), std::string::npos) << error;
  EXPECT_NE(error.find(ExpectedLinuxSpecificMarker()), std::string::npos) << error;
  EXPECT_NE(
      error.find("trusted_state_expectation=if restart sees a newer snapshot publish point without the required trusted publish completion, it must reject that snapshot and continue from the previous trusted snapshot plus replayable log tail"),
      std::string::npos)
      << error;
  EXPECT_NE(
      error.find("recovery_expectation=if restart sees a newer snapshot publish point without the required trusted publish completion, it must reject that snapshot and continue from the previous trusted snapshot plus replayable log tail"),
      std::string::npos)
      << error;
  EXPECT_NE(
      error.find("diagnostic_expectation=error should identify that the newer snapshot publish point became visible without a trusted parent directory sync boundary"),
      std::string::npos)
      << error;

  EXPECT_TRUE(std::filesystem::exists(injected_final_dir)) << injected_final_dir.string();
  EXPECT_FALSE(std::filesystem::exists(injected_final_dir / "__raft_snapshot_meta"));

  SnapshotMeta loaded_snapshot;
  bool has_snapshot = false;
  ASSERT_TRUE(storage->LoadLatestValidSnapshot(&loaded_snapshot, &has_snapshot, &error)) << error;
  ASSERT_TRUE(has_snapshot);
  EXPECT_EQ(loaded_snapshot.last_included_index, trusted_before.last_included_index);

  SnapshotListResult diagnostics;
  ASSERT_TRUE(storage->ListSnapshotsWithDiagnostics(&diagnostics, &error)) << error;
  EXPECT_NE(JoinIssueReasons(diagnostics.validation_issues).find("open snapshot meta file failed"),
            std::string::npos);

  auto configs = BuildThreeNodeConfigs(data_root_ / case_name, base_port_);
  auto snapshot_configs =
      BuildThreeSnapshotConfigs(snapshot_root_ / case_name, true, kSnapshotThreshold);
  auto restarted = std::make_shared<RaftNode>(configs[leader_index], snapshot_configs[leader_index]);

  std::string actual;
  ASSERT_TRUE(restarted->DebugGetValue("restart_publish_40", &actual))
      << "restart did not retain trusted snapshot state after rejecting injected publish failure, describe="
      << restarted->Describe();
  EXPECT_EQ(actual, "value_40");

  const std::string description = restarted->Describe();
  const auto restored_snapshot_index = ExtractUintField(description, "last_snapshot_index");
  ASSERT_TRUE(restored_snapshot_index.has_value()) << description;
  EXPECT_LT(*restored_snapshot_index, injected_index)
      << "restart should reject the injected untrusted snapshot publish boundary, describe="
      << description;
}

TEST_F(RaftSnapshotRecoveryTest,
       StandaloneRestartRejectsMetadataMismatchedVisibleSnapshotAndKeepsTrustedBoundary) {
  constexpr std::uint64_t kSnapshotThreshold = 12;
  const std::string case_name = "restart_snapshot_metadata_mismatch";
  auto cluster = MakeCluster(case_name, true, kSnapshotThreshold);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before metadata mismatch test, cluster="
      << DescribeCluster(cluster.Nodes());

  WriteManyValues(cluster.Nodes(), "metadata_mismatch", 30);

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize after metadata mismatch writes, cluster="
      << DescribeCluster(cluster.Nodes());
  const std::size_t leader_index = stable_leader->leader_index;

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index",
                                      24,
                                      std::chrono::seconds(20)))
      << "leader did not create enough snapshots before metadata mismatch test, describe="
      << cluster.Nodes()[leader_index]->Describe();

  cluster.StopAll();

  const std::filesystem::path node_snapshot_root =
      snapshot_root_ / case_name / ("node_" + std::to_string(leader_index + 1));
  const auto snapshot_dirs = ListSnapshotDirs(node_snapshot_root);
  ASSERT_GE(snapshot_dirs.size(), 2U) << "need at least two trusted snapshots under "
                                      << node_snapshot_root.string();

  const auto latest_snapshot_dir = snapshot_dirs.back();
  const auto latest_snapshot_index = SnapshotIndexFromDir(latest_snapshot_dir);
  ASSERT_TRUE(latest_snapshot_index.has_value()) << latest_snapshot_dir.string();

  const std::uint64_t mismatched_visible_index = *latest_snapshot_index + kSnapshotThreshold;
  const std::filesystem::path mismatched_visible_dir =
      node_snapshot_root / ("snapshot_" + FormatSnapshotIndex(mismatched_visible_index));
  CopyDirectoryRecursively(latest_snapshot_dir, mismatched_visible_dir);

  auto storage = CreateFileSnapshotStorage(node_snapshot_root.string(), "snapshot");
  SnapshotMeta loaded_snapshot;
  bool has_snapshot = false;
  std::string error;
  ASSERT_TRUE(storage->LoadLatestValidSnapshot(&loaded_snapshot, &has_snapshot, &error)) << error;
  ASSERT_TRUE(has_snapshot);
  EXPECT_EQ(loaded_snapshot.last_included_index, *latest_snapshot_index)
      << "metadata-mismatched visible snapshot directory must not replace the real trusted "
         "snapshot boundary";

  SnapshotListResult diagnostics;
  ASSERT_TRUE(storage->ListSnapshotsWithDiagnostics(&diagnostics, &error)) << error;
  bool saw_mismatched_dir_issue = false;
  for (const auto& issue : diagnostics.validation_issues) {
    if (issue.path.find(mismatched_visible_dir.string()) != std::string::npos) {
      saw_mismatched_dir_issue = true;
      break;
    }
  }
  EXPECT_TRUE(saw_mismatched_dir_issue)
      << "expected diagnostics for visible snapshot directory whose name does not match its "
         "metadata index: "
      << mismatched_visible_dir.string();

  auto configs = BuildThreeNodeConfigs(data_root_ / case_name, base_port_);
  auto snapshot_configs =
      BuildThreeSnapshotConfigs(snapshot_root_ / case_name, true, kSnapshotThreshold);
  auto restarted = std::make_shared<RaftNode>(configs[leader_index], snapshot_configs[leader_index]);

  std::string actual;
  ASSERT_TRUE(restarted->DebugGetValue("metadata_mismatch_29", &actual))
      << "restart did not recover data after ignoring metadata-mismatched visible snapshot, "
         "describe="
      << restarted->Describe();
  EXPECT_EQ(actual, "value_29");

  const std::string description = restarted->Describe();
  const auto restored_snapshot_index = ExtractUintField(description, "last_snapshot_index");
  ASSERT_TRUE(restored_snapshot_index.has_value()) << description;
  EXPECT_EQ(*restored_snapshot_index, *latest_snapshot_index)
      << "restart should keep the real trusted snapshot boundary when a higher-index visible "
         "snapshot directory has mismatched metadata, describe="
      << description;
  EXPECT_LT(*restored_snapshot_index, mismatched_visible_index) << description;
}

TEST_F(RaftSnapshotRecoveryTest, AllPublishedSnapshotsInvalidYieldNoTrustedSnapshot) {
  constexpr std::uint64_t kSnapshotThreshold = 12;
  const std::string case_name = "restart_all_invalid_snapshots";
  auto cluster = MakeCluster(case_name, true, kSnapshotThreshold);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before all-invalid snapshot test, cluster="
      << DescribeCluster(cluster.Nodes());

  WriteManyValues(cluster.Nodes(), "all_invalid_snapshot", 30);

  stable_leader = WaitForStableLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize after all-invalid snapshot writes, cluster="
      << DescribeCluster(cluster.Nodes());
  const std::size_t leader_index = stable_leader->leader_index;

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index",
                                      24,
                                      std::chrono::seconds(20)))
      << "leader did not create enough snapshots before all-invalid snapshot test, describe="
      << cluster.Nodes()[leader_index]->Describe();

  cluster.StopAll();

  const std::filesystem::path node_snapshot_root =
      snapshot_root_ / case_name / ("node_" + std::to_string(leader_index + 1));
  const auto snapshot_dirs = ListSnapshotDirs(node_snapshot_root);
  ASSERT_GE(snapshot_dirs.size(), 2U) << "need at least two trusted snapshots under "
                                      << node_snapshot_root.string();

  for (const auto& snapshot_dir : snapshot_dirs) {
    WriteTextFile(snapshot_dir / "data.bin",
                  "corrupted-all-invalid-" + snapshot_dir.filename().string());
  }

  auto storage = CreateFileSnapshotStorage(node_snapshot_root.string(), "snapshot");
  SnapshotMeta loaded_snapshot;
  bool has_snapshot = true;
  std::string error;
  ASSERT_TRUE(storage->LoadLatestValidSnapshot(&loaded_snapshot, &has_snapshot, &error)) << error;
  EXPECT_FALSE(has_snapshot)
      << "when every published snapshot is invalid, trusted snapshot selection must report that "
         "no snapshot boundary is acceptable";

  SnapshotListResult diagnostics;
  ASSERT_TRUE(storage->ListSnapshotsWithDiagnostics(&diagnostics, &error)) << error;
  EXPECT_TRUE(diagnostics.snapshots.empty())
      << "all invalid snapshots must be excluded from the trusted snapshot list";
  EXPECT_GE(diagnostics.validation_issues.size(), snapshot_dirs.size())
      << "every invalid snapshot should contribute a validation issue";
}

}  // namespace
}  // namespace raftdemo
