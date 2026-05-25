#include "metadata_raft_test_utils.h"
#include "support/raft_snapshot_restart_test_utils.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "raft/common/metadata_command.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"
#include "raft/state_machine/metadata_state_machine.h"

namespace raftdemo {
namespace {

using namespace std::chrono_literals;

using raftdemo::test::DescribeCluster;
using raftdemo::test::FindNodeIndex;
using raftdemo::test::MetadataRecoveryExpectation;
using raftdemo::test::PickFollowerIndex;
using raftdemo::test::ProposeMetadataCommandWithRetry;
using raftdemo::test::ProposeStatusName;
using raftdemo::test::SnapshotRestartTestBase;
using raftdemo::test::TestCluster;
using raftdemo::test::WaitForNodeFieldAtLeast;
using raftdemo::test::WaitForOrderedCommitApplyAtLeast;
using raftdemo::test::WaitForSingleLeader;
using raftdemo::test::WaitForStableLeader;
using raftdemo::test::WaitUntilAllMetadataRecoveryMatches;

class MetadataRecoveryStressTest : public SnapshotRestartTestBase {};

struct MetadataStressWorkload {
  std::string bucket;
  std::string live_prefix;
  std::string deleted_prefix;
  int live_count{0};
  int deleted_count{0};
  std::string request_prefix;
};

struct StressExecutionState {
  std::atomic<std::uint64_t> max_log_index{0};
  std::atomic<std::uint64_t> max_term{0};
  std::atomic<int> failures{0};
  std::mutex mu;
  std::vector<std::string> messages;
};

void RecordAppliedResult(StressExecutionState* state, const ProposeResult& result) {
  auto max_index = state->max_log_index.load(std::memory_order_relaxed);
  while (max_index < result.log_index &&
         !state->max_log_index.compare_exchange_weak(
             max_index, result.log_index, std::memory_order_relaxed)) {
  }

  auto max_term = state->max_term.load(std::memory_order_relaxed);
  while (max_term < result.term &&
         !state->max_term.compare_exchange_weak(
             max_term, result.term, std::memory_order_relaxed)) {
  }
}

void RecordFailure(StressExecutionState* state,
                   const std::string& category,
                   const std::string& detail) {
  state->failures.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(state->mu);
  state->messages.push_back(category + ": " + detail);
}

std::string JoinMessages(StressExecutionState& state) {
  std::lock_guard<std::mutex> lock(state.mu);
  std::ostringstream oss;
  for (std::size_t i = 0; i < state.messages.size(); ++i) {
    if (i != 0) {
      oss << " | ";
    }
    oss << state.messages[i];
  }
  return oss.str();
}

std::string FormatIndex(int index) {
  return index < 10 ? "0" + std::to_string(index) : std::to_string(index);
}

std::string ObjectKey(const std::string& prefix, int index) {
  return prefix + "/" + FormatIndex(index);
}

std::string ObjectId(const std::string& prefix, int index) {
  return "obj-" + prefix + "-" + FormatIndex(index);
}

std::string RequestId(const std::string& prefix,
                      const std::string& op,
                      const std::string& key) {
  return prefix + "-" + op + "-" + key;
}

bool IsReplayOrNoopMessage(const std::string& message) {
  return message.find("idempotent replay") != std::string::npos ||
         message.find("nothing to apply") != std::string::npos;
}

bool RunMetadataCommand(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                        const MetadataCommand& command,
                        const std::vector<std::size_t>& excluded,
                        StressExecutionState* state) {
  ProposeResult result;
  if (!ProposeMetadataCommandWithRetry(nodes, command, 10s, &result, excluded)) {
    RecordFailure(state,
                  "proposal",
                  "request_id=" + command.request_id + ", status=" +
                      ProposeStatusName(result.status) + ", message=" + result.message);
    return false;
  }

  RecordAppliedResult(state, result);
  return true;
}

void RunConcurrentWriteWorkload(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                const MetadataStressWorkload& workload,
                                const std::vector<std::size_t>& excluded,
                                StressExecutionState* state) {
  std::thread live_writer([&]() {
    for (int i = 0; i < workload.live_count; ++i) {
      const std::string key = ObjectKey(workload.live_prefix, i);
      const std::string object_id = ObjectId(workload.live_prefix, i);

      if (!RunMetadataCommand(nodes,
                              raftdemo::test::MakeCreateObjectCommand(
                                  workload.bucket, key, object_id,
                                  RequestId(workload.request_prefix, "create", key)),
                              excluded,
                              state)) {
        return;
      }
      std::this_thread::sleep_for(15ms);

      if (!RunMetadataCommand(nodes,
                              raftdemo::test::MakeCommitObjectCommand(
                                  workload.bucket, key, object_id,
                                  RequestId(workload.request_prefix, "commit", key)),
                              excluded,
                              state)) {
        return;
      }
      std::this_thread::sleep_for(15ms);
    }
  });

  std::thread deleted_writer([&]() {
    for (int i = 0; i < workload.deleted_count; ++i) {
      const std::string key = ObjectKey(workload.deleted_prefix, i);
      const std::string object_id = ObjectId(workload.deleted_prefix, i);

      if (!RunMetadataCommand(nodes,
                              raftdemo::test::MakeCreateObjectCommand(
                                  workload.bucket, key, object_id,
                                  RequestId(workload.request_prefix, "create", key)),
                              excluded,
                              state)) {
        return;
      }
      std::this_thread::sleep_for(20ms);

      if (!RunMetadataCommand(nodes,
                              raftdemo::test::MakeCommitObjectCommand(
                                  workload.bucket, key, object_id,
                                  RequestId(workload.request_prefix, "commit", key)),
                              excluded,
                              state)) {
        return;
      }
      std::this_thread::sleep_for(20ms);

      if (!RunMetadataCommand(nodes,
                              raftdemo::test::MakeDeleteObjectCommand(
                                  workload.bucket, key, object_id,
                                  RequestId(workload.request_prefix, "delete", key)),
                              excluded,
                              state)) {
        return;
      }
      std::this_thread::sleep_for(20ms);
    }
  });

  live_writer.join();
  deleted_writer.join();
}

bool ObserveConsistentReadSnapshot(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                   const std::string& bucket,
                                   const std::vector<std::string>& observed_keys,
                                   std::string* detail) {
  auto leader = WaitForSingleLeader(nodes, 1200ms);
  if (leader == nullptr) {
    return true;
  }

  const MetadataStateMachine* state_machine = leader->GetMetadataStateMachineV2();
  if (state_machine == nullptr) {
    if (detail != nullptr) {
      *detail = "leader metadata state machine unavailable";
    }
    return false;
  }

  const auto listed = state_machine->ListObjects(
      {.bucket = bucket, .prefix = "", .limit = std::nullopt, .continuation_token = ""});
  if (!listed.result.Ok()) {
    if (detail != nullptr) {
      *detail = "ListObjects failed: " + listed.result.summary.message;
    }
    return false;
  }

  for (const auto& record : listed.records) {
    if (!record.IsCommitted()) {
      if (detail != nullptr) {
        *detail = "ListObjects exposed non-committed object: " + record.object_key;
      }
      return false;
    }
  }

  for (const auto& key : observed_keys) {
    const auto head = state_machine->HeadObject({.bucket = bucket, .object_key = key});
    if (head.result.code == MetadataStatusCode::kOk) {
      if (!head.record.has_value() || !head.record->IsCommitted()) {
        if (detail != nullptr) {
          *detail = "HeadObject returned visible but non-committed object: " + key;
        }
        return false;
      }
    } else if (head.result.code != MetadataStatusCode::kNotFound) {
      if (detail != nullptr) {
        *detail = "HeadObject returned unexpected status for " + key + ": " +
                  head.result.summary.message;
      }
      return false;
    }
  }

  return true;
}

MetadataRecoveryExpectation BuildExpectation(const MetadataStressWorkload& workload,
                                            std::uint64_t expected_last_applied_index,
                                            std::optional<std::uint64_t> expected_min_term) {
  MetadataRecoveryExpectation expectation;
  expectation.bucket = workload.bucket;
  expectation.expected_request_count =
      1U + static_cast<std::size_t>(workload.live_count * 2 + workload.deleted_count * 3);
  expectation.expected_tombstone_count =
      static_cast<std::size_t>(workload.deleted_count);
  expectation.expected_last_applied_index = expected_last_applied_index;
  expectation.expected_min_last_applied_term = expected_min_term;

  for (int i = 0; i < workload.live_count; ++i) {
    const std::string key = ObjectKey(workload.live_prefix, i);
    expectation.objects.push_back(
        {key, ObjectId(workload.live_prefix, i), 2U, false});
    expectation.visible_keys.push_back(key);
  }
  for (int i = 0; i < workload.deleted_count; ++i) {
    const std::string key = ObjectKey(workload.deleted_prefix, i);
    expectation.objects.push_back(
        {key, ObjectId(workload.deleted_prefix, i), 2U, true});
  }
  return expectation;
}

void ExpectRequestCountsUnchanged(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                  const MetadataRecoveryExpectation& expectation) {
  for (const auto& node : nodes) {
    ASSERT_NE(node, nullptr);
    const MetadataStateMachine* state_machine = node->GetMetadataStateMachineV2();
    ASSERT_NE(state_machine, nullptr) << node->Describe();
    EXPECT_EQ(state_machine->RequestCount(), expectation.expected_request_count)
        << node->Describe();
    EXPECT_EQ(state_machine->TombstoneCount(), expectation.expected_tombstone_count)
        << node->Describe();
  }
}

TEST_F(MetadataRecoveryStressTest,
       ConcurrentApplyAndQueryTriggerSnapshotWithoutBreakingMetadataFacts) {
  const MetadataStressWorkload workload{
      .bucket = "stress-snapshot-bucket",
      .live_prefix = "live",
      .deleted_prefix = "deleted",
      .live_count = 10,
      .deleted_count = 4,
      .request_prefix = "stress-snapshot",
  };

  auto cluster = MakeCluster("snapshot_during_concurrency", true, 6);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), 8s);
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize, cluster=" << DescribeCluster(cluster.Nodes());

  ProposeResult bucket_result;
  ASSERT_TRUE(raftdemo::test::ProposeCreateBucketWithRetry(
      cluster.Nodes(), workload.bucket, "stress-snapshot-create-bucket", 10s, &bucket_result));

  StressExecutionState execution;
  RecordAppliedResult(&execution, bucket_result);

  std::vector<std::string> observed_keys;
  observed_keys.reserve(
      static_cast<std::size_t>(workload.live_count + workload.deleted_count));
  for (int i = 0; i < workload.live_count; ++i) {
    observed_keys.push_back(ObjectKey(workload.live_prefix, i));
  }
  for (int i = 0; i < workload.deleted_count; ++i) {
    observed_keys.push_back(ObjectKey(workload.deleted_prefix, i));
  }

  std::atomic<bool> stop_readers{false};
  std::atomic<int> reader_violations{0};
  std::mutex reader_mu;
  std::vector<std::string> reader_failures;
  std::vector<std::thread> readers;
  readers.reserve(4);
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&]() {
      while (!stop_readers.load(std::memory_order_acquire)) {
        std::string detail;
        if (!ObserveConsistentReadSnapshot(cluster.Nodes(), workload.bucket, observed_keys,
                                           &detail)) {
          reader_violations.fetch_add(1, std::memory_order_relaxed);
          std::lock_guard<std::mutex> lock(reader_mu);
          reader_failures.push_back(detail);
          return;
        }
        std::this_thread::sleep_for(10ms);
      }
    });
  }

  std::atomic<bool> writers_finished{false};
  std::thread writers([&]() {
    RunConcurrentWriteWorkload(cluster.Nodes(), workload, {}, &execution);
    writers_finished.store(true, std::memory_order_release);
  });

  std::string snapshot_diagnostics;
  ASSERT_TRUE(WaitForNodeFieldAtLeast(stable_leader->leader, "last_snapshot_index", 6, 20s,
                                      &snapshot_diagnostics))
      << "leader did not create snapshot during concurrent workload, diagnostics="
      << snapshot_diagnostics << ", cluster=" << DescribeCluster(cluster.Nodes());
  EXPECT_FALSE(writers_finished.load(std::memory_order_acquire))
      << "snapshot was expected while writers were still active";

  writers.join();
  stop_readers.store(true, std::memory_order_release);
  for (auto& reader : readers) {
    reader.join();
  }

  ASSERT_EQ(execution.failures.load(std::memory_order_relaxed), 0)
      << JoinMessages(execution);
  ASSERT_EQ(reader_violations.load(std::memory_order_relaxed), 0)
      << (reader_failures.empty() ? std::string("no reader detail")
                                  : reader_failures.front());

  const auto expected = BuildExpectation(
      workload,
      execution.max_log_index.load(std::memory_order_relaxed),
      execution.max_term.load(std::memory_order_relaxed));
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(
      cluster.Nodes(), expected.expected_last_applied_index, 15s));
  ASSERT_TRUE(WaitUntilAllMetadataRecoveryMatches(cluster.Nodes(), expected, 15s))
      << "snapshot stress workload lost metadata facts, cluster="
      << DescribeCluster(cluster.Nodes());
}

TEST_F(MetadataRecoveryStressTest,
       RestartRecoveryAfterConcurrentWritesKeepsCommittedAndDeletedMetadataStable) {
  const MetadataStressWorkload workload{
      .bucket = "stress-restart-bucket",
      .live_prefix = "live",
      .deleted_prefix = "deleted",
      .live_count = 8,
      .deleted_count = 3,
      .request_prefix = "stress-restart",
  };

  auto cluster = MakeCluster("restart_after_concurrent_writes", false, 0);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), 8s);
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize, cluster=" << DescribeCluster(cluster.Nodes());

  ProposeResult bucket_result;
  ASSERT_TRUE(raftdemo::test::ProposeCreateBucketWithRetry(
      cluster.Nodes(), workload.bucket, "stress-restart-create-bucket", 10s, &bucket_result));

  StressExecutionState execution;
  RecordAppliedResult(&execution, bucket_result);
  RunConcurrentWriteWorkload(cluster.Nodes(), workload, {}, &execution);
  ASSERT_EQ(execution.failures.load(std::memory_order_relaxed), 0)
      << JoinMessages(execution);

  const auto expected = BuildExpectation(
      workload,
      execution.max_log_index.load(std::memory_order_relaxed),
      execution.max_term.load(std::memory_order_relaxed));
  ASSERT_TRUE(WaitUntilAllMetadataRecoveryMatches(cluster.Nodes(), expected, 15s))
      << "pre-restart metadata facts never converged, cluster="
      << DescribeCluster(cluster.Nodes());

  cluster.StopAll();
  cluster.Start();

  stable_leader = WaitForStableLeader(cluster.Nodes(), 10s);
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not recover after restart, cluster="
      << DescribeCluster(cluster.Nodes());

  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(
      cluster.Nodes(), expected.expected_last_applied_index, 15s));
  ASSERT_TRUE(WaitUntilAllMetadataRecoveryMatches(cluster.Nodes(), expected, 15s))
      << "restart recovery lost metadata facts, cluster="
      << DescribeCluster(cluster.Nodes());

  stable_leader = WaitForStableLeader(cluster.Nodes(), 8s);
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before replay verification, cluster="
      << DescribeCluster(cluster.Nodes());
  auto restarted_leader = stable_leader->leader;
  ASSERT_NE(restarted_leader, nullptr);

  const std::string replay_key = ObjectKey(workload.live_prefix, 0);
  const std::string replay_object_id = ObjectId(workload.live_prefix, 0);
  const std::string replay_request_id =
      RequestId(workload.request_prefix, "create", replay_key);
  const ProposeResult replay_result = restarted_leader->ProposeMetadata(
      SerializeMetadataCommand(raftdemo::test::MakeCreateObjectCommand(
          workload.bucket, replay_key, replay_object_id, replay_request_id)));
  ASSERT_EQ(replay_result.status, ProposeStatus::kOk)
      << replay_result.message;
  EXPECT_TRUE(IsReplayOrNoopMessage(replay_result.message))
      << replay_result.message;

  const std::string deleted_key = ObjectKey(workload.deleted_prefix, 0);
  const std::string deleted_object_id = ObjectId(workload.deleted_prefix, 0);
  const ProposeResult delete_replay = restarted_leader->ProposeMetadata(
      SerializeMetadataCommand(raftdemo::test::MakeDeleteObjectCommand(
          workload.bucket,
          deleted_key,
          deleted_object_id,
          RequestId(workload.request_prefix, "delete", deleted_key))));
  ASSERT_EQ(delete_replay.status, ProposeStatus::kOk)
      << delete_replay.message;
  EXPECT_TRUE(IsReplayOrNoopMessage(delete_replay.message))
      << delete_replay.message;

  stable_leader = WaitForStableLeader(cluster.Nodes(), 8s);
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before fingerprint conflict verification, cluster="
      << DescribeCluster(cluster.Nodes());
  restarted_leader = stable_leader->leader;
  ASSERT_NE(restarted_leader, nullptr);

  const ProposeResult conflict_result = restarted_leader->ProposeMetadata(
      SerializeMetadataCommand(raftdemo::test::MakeCreateObjectCommand(
          workload.bucket,
          replay_key,
          replay_object_id + "-conflict",
          replay_request_id)));
  EXPECT_EQ(conflict_result.status, ProposeStatus::kApplyFailed)
      << conflict_result.message;
  EXPECT_NE(conflict_result.message.find("idempotency conflict"), std::string::npos)
      << conflict_result.message;

  ASSERT_TRUE(WaitUntilAllMetadataRecoveryMatches(cluster.Nodes(), expected, 15s))
      << "metadata facts changed after replay/conflict checks, cluster="
      << DescribeCluster(cluster.Nodes());
  ExpectRequestCountsUnchanged(cluster.Nodes(), expected);
}

TEST_F(MetadataRecoveryStressTest,
       FollowerCatchUpRestoresSameMetadataFactsAsLeaderAfterLaggedWrites) {
  const MetadataStressWorkload workload{
      .bucket = "stress-catchup-bucket",
      .live_prefix = "live",
      .deleted_prefix = "deleted",
      .live_count = 12,
      .deleted_count = 4,
      .request_prefix = "stress-catchup",
  };

  auto cluster = MakeCluster("follower_catchup_consistency", true, 6);
  cluster.Start();

  auto stable_leader = WaitForStableLeader(cluster.Nodes(), 8s);
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize, cluster=" << DescribeCluster(cluster.Nodes());

  const std::size_t lagging_follower =
      PickFollowerIndex(cluster.Nodes(), stable_leader->leader);
  ASSERT_LT(lagging_follower, cluster.Nodes().size())
      << "failed to choose lagging follower";

  ProposeResult bucket_result;
  ASSERT_TRUE(raftdemo::test::ProposeCreateBucketWithRetry(
      cluster.Nodes(), workload.bucket, "stress-catchup-create-bucket", 10s, &bucket_result));

  cluster.StopNode(lagging_follower);
  const std::vector<std::size_t> excluded{lagging_follower};

  StressExecutionState execution;
  RecordAppliedResult(&execution, bucket_result);
  RunConcurrentWriteWorkload(cluster.Nodes(), workload, excluded, &execution);
  ASSERT_EQ(execution.failures.load(std::memory_order_relaxed), 0)
      << JoinMessages(execution);

  const auto expected = BuildExpectation(
      workload,
      execution.max_log_index.load(std::memory_order_relaxed),
      execution.max_term.load(std::memory_order_relaxed));
  ASSERT_TRUE(WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expected, 15s, excluded))
      << "majority lost metadata facts before follower restart, cluster="
      << DescribeCluster(cluster.Nodes(), excluded);

  stable_leader = WaitForStableLeader(cluster.Nodes(), 5s, excluded);
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stay stable during lagged writes, cluster="
      << DescribeCluster(cluster.Nodes(), excluded);

  std::string snapshot_diagnostics;
  ASSERT_TRUE(WaitForNodeFieldAtLeast(stable_leader->leader, "last_snapshot_index", 6, 20s,
                                      &snapshot_diagnostics))
      << "leader did not create snapshot before follower catch-up, diagnostics="
      << snapshot_diagnostics << ", cluster=" << DescribeCluster(cluster.Nodes(), excluded);

  cluster.RestartNode(lagging_follower);

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[lagging_follower],
                                      "last_applied",
                                      expected.expected_last_applied_index,
                                      30s))
      << "lagging follower did not replay or install enough metadata, describe="
      << cluster.Nodes()[lagging_follower]->Describe();

  ASSERT_TRUE(WaitUntilAllMetadataRecoveryMatches(cluster.Nodes(), expected, 25s))
      << "follower catch-up did not converge to leader metadata facts, cluster="
      << DescribeCluster(cluster.Nodes());

  stable_leader = WaitForStableLeader(cluster.Nodes(), 8s);
  ASSERT_TRUE(stable_leader.has_value())
      << "leader did not stabilize before final boundary comparison, cluster="
      << DescribeCluster(cluster.Nodes());

  const auto* leader_state_machine = stable_leader->leader->GetMetadataStateMachineV2();
  ASSERT_NE(leader_state_machine, nullptr);
  ASSERT_TRUE(WaitForOrderedCommitApplyAtLeast(cluster.Nodes(),
                                               leader_state_machine->LastAppliedIndex(),
                                               10s))
      << "cluster did not converge to leader boundary before final comparison, cluster="
      << DescribeCluster(cluster.Nodes());

  const auto* follower_state_machine =
      cluster.Nodes()[lagging_follower]->GetMetadataStateMachineV2();
  ASSERT_NE(follower_state_machine, nullptr);
  EXPECT_GE(follower_state_machine->LastAppliedIndex(),
            expected.expected_last_applied_index);
  EXPECT_GE(follower_state_machine->LastAppliedTerm(),
            *expected.expected_min_last_applied_term);
  EXPECT_EQ(follower_state_machine->RequestCount(), leader_state_machine->RequestCount());
  EXPECT_EQ(follower_state_machine->TombstoneCount(), leader_state_machine->TombstoneCount());
  EXPECT_EQ(follower_state_machine->ObjectCount(), leader_state_machine->ObjectCount());
  EXPECT_EQ(follower_state_machine->BucketCount(), leader_state_machine->BucketCount());
  EXPECT_EQ(follower_state_machine->LastAppliedIndex(),
            leader_state_machine->LastAppliedIndex());
  EXPECT_EQ(follower_state_machine->LastAppliedTerm(),
            leader_state_machine->LastAppliedTerm());
}

}  // namespace
}  // namespace raftdemo
