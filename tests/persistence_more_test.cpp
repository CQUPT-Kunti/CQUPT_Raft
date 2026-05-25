#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "metadata_raft_test_utils.h"
#include "raft/common/config.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"
#include "raft/runtime/logging.h"

namespace raftdemo {
namespace {

using namespace std::chrono_literals;
using test::ExpectedRecoveredMetadataObject;
using test::MetadataRecoveryExpectation;

constexpr const char* kManualBucket = "manual-restart-bucket";

struct RunningCluster {
  std::vector<std::shared_ptr<RaftNode>> nodes;
  std::vector<std::thread> threads;
};

const std::vector<std::string>& Phase1CommittedKeys() {
  static const std::vector<std::string> keys = {
      "alpha",
      "beta",
      "gamma",
      "persist_marker",
  };
  return keys;
}

std::string DeletedObjectKey() {
  return "deleted_marker";
}

std::string RecoveryProbeKey() {
  return "recovery_probe";
}

std::string ObjectIdForKey(const std::string& key) {
  return "manual-object-" + key;
}

ExpectedRecoveredMetadataObject MakeCommittedExpectation(const std::string& key) {
  return ExpectedRecoveredMetadataObject{key, ObjectIdForKey(key), 2, false};
}

ExpectedRecoveredMetadataObject MakeDeletedExpectation(const std::string& key) {
  return ExpectedRecoveredMetadataObject{key, ObjectIdForKey(key), 0, true};
}

MetadataRecoveryExpectation BuildBaseExpectation() {
  MetadataRecoveryExpectation expectation;
  expectation.bucket = kManualBucket;
  expectation.expected_request_count = 1;
  expectation.expected_last_applied_index = 1;
  expectation.expected_min_last_applied_term = 1;
  return expectation;
}

MetadataRecoveryExpectation BuildPhase1Expectation() {
  MetadataRecoveryExpectation expectation = BuildBaseExpectation();
  for (const auto& key : Phase1CommittedKeys()) {
    expectation.objects.push_back(MakeCommittedExpectation(key));
    expectation.visible_keys.push_back(key);
  }
  expectation.objects.push_back(MakeDeletedExpectation(DeletedObjectKey()));
  expectation.expected_request_count += Phase1CommittedKeys().size() * 2 + 3;
  expectation.expected_tombstone_count = 1;
  expectation.expected_last_applied_index +=
      static_cast<std::uint64_t>(Phase1CommittedKeys().size() * 2 + 3);
  std::sort(expectation.visible_keys.begin(), expectation.visible_keys.end());
  return expectation;
}

MetadataRecoveryExpectation BuildPhase2Expectation() {
  MetadataRecoveryExpectation expectation = BuildPhase1Expectation();
  expectation.objects.push_back(MakeCommittedExpectation(RecoveryProbeKey()));
  expectation.visible_keys.push_back(RecoveryProbeKey());
  std::sort(expectation.visible_keys.begin(), expectation.visible_keys.end());
  expectation.expected_request_count += 2;
  expectation.expected_last_applied_index += 2;
  return expectation;
}

const char* ProposeStatusName(ProposeStatus status) {
  switch (status) {
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

bool IsLeaderNode(const std::shared_ptr<RaftNode>& node) {
  return node != nullptr &&
         node->Describe().find("role=Leader") != std::string::npos;
}

std::shared_ptr<RaftNode> WaitForLeader(
    const std::vector<std::shared_ptr<RaftNode>>& nodes,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto& node : nodes) {
      if (node != nullptr && IsLeaderNode(node)) {
        return node;
      }
    }
    std::this_thread::sleep_for(100ms);
  }
  return nullptr;
}

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
  n1.heartbeat_interval = std::chrono::milliseconds(100);
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
  n2.heartbeat_interval = std::chrono::milliseconds(100);
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
  n3.heartbeat_interval = std::chrono::milliseconds(100);
  n3.rpc_deadline = std::chrono::milliseconds(500);
  n3.data_dir = (data_root / "node_3").string();

  return {n1, n2, n3};
}

RunningCluster StartCluster(const std::vector<NodeConfig>& configs) {
  RunningCluster cluster;
  cluster.nodes.reserve(configs.size());
  for (const auto& cfg : configs) {
    cluster.nodes.push_back(std::make_shared<RaftNode>(cfg));
  }

  cluster.threads.reserve(cluster.nodes.size());
  for (const auto& node : cluster.nodes) {
    cluster.threads.emplace_back([node]() {
      node->Start();
      node->Wait();
    });
  }
  return cluster;
}

void StopCluster(RunningCluster* cluster) {
  if (cluster == nullptr) {
    return;
  }
  Log("manual-test", "stopping cluster");
  for (auto& node : cluster->nodes) {
    if (node != nullptr) {
      node->Stop();
    }
  }
  for (auto& t : cluster->threads) {
    if (t.joinable()) {
      t.join();
    }
  }
  Log("manual-test", "cluster stopped");
}

void LogClusterSnapshot(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                        const std::string& title) {
  Log("manual-test", "========== ", title, " ==========");
  for (const auto& node : nodes) {
    if (node != nullptr) {
      Log("manual-test", node->Describe());
    }
  }
}

std::string BuildMetadataSection(const std::shared_ptr<RaftNode>& node) {
  std::ostringstream oss;
  const MetadataStateMachine* state_machine = node->GetMetadataStateMachineV2();
  if (state_machine == nullptr) {
    oss << "metadata_state_machine=<null>\n";
    return oss.str();
  }

  oss << "bucket=" << kManualBucket << "\n";
  oss << "request_count=" << state_machine->RequestCount() << "\n";
  oss << "tombstone_count=" << state_machine->TombstoneCount() << "\n";
  oss << "last_applied_index=" << state_machine->LastAppliedIndex() << "\n";
  oss << "last_applied_term=" << state_machine->LastAppliedTerm() << "\n";

  const auto list = state_machine->ListObjects(
      {.bucket = kManualBucket, .prefix = "", .limit = std::nullopt, .continuation_token = ""});
  oss << "visible_objects=";
  if (!list.result.Ok()) {
    oss << "<list-failed>\n";
  } else {
    for (std::size_t i = 0; i < list.records.size(); ++i) {
      if (i > 0) {
        oss << ",";
      }
      oss << list.records[i].object_key;
    }
    oss << "\n";
  }

  std::vector<std::string> tracked_keys = Phase1CommittedKeys();
  tracked_keys.push_back(DeletedObjectKey());
  tracked_keys.push_back(RecoveryProbeKey());
  for (const auto& key : tracked_keys) {
    const auto response = state_machine->HeadObject({.bucket = kManualBucket, .object_key = key});
    const auto internal_record = state_machine->FindObject(kManualBucket, key);
    const auto indexed_object_id = state_machine->FindIndexedObjectId(kManualBucket, key);
    const auto chunk_refs = state_machine->FindChunkRefs(kManualBucket, key);
    oss << key << ": head=" << static_cast<int>(response.result.code);
    if (response.record.has_value()) {
      oss << "/" << response.record->object_id;
    }
    if (internal_record.has_value()) {
      oss << " internal_state=" << static_cast<int>(internal_record->state)
          << " internal_id=" << internal_record->object_id;
    } else {
      oss << " internal_state=<none>";
    }
    oss << " indexed=";
    if (indexed_object_id.has_value()) {
      oss << *indexed_object_id;
    } else {
      oss << "<none>";
    }
    oss << " chunks=";
    if (chunk_refs.has_value()) {
      oss << chunk_refs->size();
    } else {
      oss << "<none>";
    }
    oss << "\n";
  }
  return oss.str();
}

void LogStateFiles(const std::filesystem::path& data_root) {
  Log("manual-test", "state files under: ", data_root.string());
  for (int id = 1; id <= 3; ++id) {
    const auto state_file = data_root / ("node_" + std::to_string(id)) / "raft_state.bin";
    std::error_code ec;
    const bool exists = std::filesystem::exists(state_file, ec);
    const auto size = exists ? std::filesystem::file_size(state_file, ec) : 0;
    Log("manual-test", "node-", id, " file=", state_file.string(), ", exists=",
        exists ? "true" : "false", ", size=",
        static_cast<unsigned long long>(size));
  }
}

void SaveTextFile(const std::filesystem::path& path, const std::string& content) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << content;
}

void SaveClusterSnapshotFiles(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                              const std::filesystem::path& data_root,
                              const std::string& phase_name) {
  const auto snapshot_dir = data_root / "snapshots" / phase_name;
  std::error_code ec;
  std::filesystem::create_directories(snapshot_dir, ec);

  std::ostringstream manifest;
  manifest << "snapshot_phase=" << phase_name << "\n";
  manifest << "snapshot_root=" << snapshot_dir.string() << "\n";
  manifest << "bucket=" << kManualBucket << "\n";
  manifest << "tracked_objects=";
  for (std::size_t i = 0; i < Phase1CommittedKeys().size(); ++i) {
    if (i > 0) {
      manifest << ",";
    }
    manifest << Phase1CommittedKeys()[i];
  }
  manifest << "," << DeletedObjectKey() << "," << RecoveryProbeKey() << "\n\n";

  for (std::size_t i = 0; i < nodes.size(); ++i) {
    const auto& node = nodes[i];
    const auto node_file =
        snapshot_dir / ("node_" + std::to_string(i + 1) + "_snapshot.txt");

    std::ostringstream oss;
    oss << "phase=" << phase_name << "\n";
    oss << "node_file=" << node_file.string() << "\n";
    oss << "describe=" << node->Describe() << "\n";
    oss << "metadata_begin\n";
    oss << BuildMetadataSection(node);
    oss << "metadata_end\n";

    SaveTextFile(node_file, oss.str());
    manifest << "node_" << (i + 1) << "=" << node_file.string() << "\n";
  }

  SaveTextFile(snapshot_dir / "manifest.txt", manifest.str());
  Log("manual-test", "saved manual snapshot files to: ", snapshot_dir.string());
}

bool ProposeCreateBucket(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                         const std::string& request_id) {
  ProposeResult result;
  const bool ok = test::ProposeCreateBucketWithRetry(
      nodes, kManualBucket, request_id, 8s, &result);
  Log("manual-test", "propose CREATE_BUCKET bucket=", kManualBucket, ", request_id=",
      request_id, ", status=", ProposeStatusName(result.status), ", leader_id=",
      result.leader_id, ", term=", result.term, ", log_index=", result.log_index,
      ", message=", result.message);
  return ok;
}

bool ProposeCommittedObject(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                            const std::string& key,
                            const std::string& request_prefix) {
  ProposeResult result;
  const bool ok = test::ProposeCreateCommitObjectWithRetry(
      nodes,
      kManualBucket,
      key,
      ObjectIdForKey(key),
      request_prefix + "-create",
      request_prefix + "-commit",
      8s,
      &result);
  Log("manual-test", "propose CREATE+COMMIT object=", key, ", request_prefix=",
      request_prefix, ", status=", ProposeStatusName(result.status),
      ", leader_id=", result.leader_id, ", term=", result.term, ", log_index=",
      result.log_index, ", message=", result.message);
  return ok;
}

bool ProposeDeletedObject(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                          const std::string& key,
                          const std::string& request_prefix) {
  if (!ProposeCommittedObject(nodes, key, request_prefix)) {
    return false;
  }

  ProposeResult result;
  const bool ok = test::ProposeMetadataCommandWithRetry(
      nodes,
      test::MakeDeleteObjectCommand(
          kManualBucket,
          key,
          ObjectIdForKey(key),
          request_prefix + "-delete"),
      8s,
      &result);
  Log("manual-test", "propose DELETE object=", key, ", request_prefix=",
      request_prefix, ", status=", ProposeStatusName(result.status),
      ", leader_id=", result.leader_id, ", term=", result.term, ", log_index=",
      result.log_index, ", message=", result.message);
  return ok;
}

bool WaitUntilMetadataMatches(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                              const MetadataRecoveryExpectation& expectation,
                              std::chrono::milliseconds timeout) {
  return test::WaitUntilAllMetadataRecoveryMatches(nodes, expectation, timeout);
}

void WriteMarker(const std::filesystem::path& marker_path,
                 const std::string& content) {
  std::ofstream out(marker_path, std::ios::binary | std::ios::trunc);
  out << content;
}

bool FileExists(const std::filesystem::path& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

int RunPhase1(const std::filesystem::path& data_root) {
  Log("manual-test",
      "phase-1: clean start, write metadata, save manual snapshot, then exit");
  std::error_code ec;
  std::filesystem::remove_all(data_root, ec);
  std::filesystem::create_directories(data_root, ec);

  const auto configs = BuildThreeNodeConfigs(data_root, 53250);
  auto cluster = StartCluster(configs);

  auto leader = WaitForLeader(cluster.nodes, 6s);
  if (!leader) {
    Log("manual-test", "phase-1 failed: leader election timeout");
    StopCluster(&cluster);
    return 1;
  }

  LogClusterSnapshot(cluster.nodes, "phase-1 snapshot after leader election");

  bool ok = ProposeCreateBucket(cluster.nodes, "manual-phase1-bucket");
  for (const auto& key : Phase1CommittedKeys()) {
    ok = ok && ProposeCommittedObject(cluster.nodes, key, "manual-" + key);
  }
  ok = ok && ProposeDeletedObject(cluster.nodes, DeletedObjectKey(), "manual-deleted");

  if (!ok) {
    Log("manual-test", "phase-1 failed: propose error");
    StopCluster(&cluster);
    return 1;
  }

  const MetadataRecoveryExpectation expectation = BuildPhase1Expectation();
  const bool replicated =
      WaitUntilMetadataMatches(cluster.nodes, expectation, 8s);

  LogClusterSnapshot(cluster.nodes, "phase-1 snapshot before stop");
  SaveClusterSnapshotFiles(cluster.nodes, data_root, "phase1_before_stop");

  StopCluster(&cluster);
  LogStateFiles(data_root);

  if (!replicated) {
    Log("manual-test",
        "phase-1 failed: metadata facts not visible on all nodes before stop");
    return 1;
  }

  WriteMarker(data_root / "phase1.done",
              "run the same executable again to verify recovery\n");
  Log("manual-test",
      "phase-1 complete. Re-run the same executable to start phase-2 verification.");
  Log("manual-test", "manual snapshot saved under: ",
      (data_root / "snapshots" / "phase1_before_stop").string());
  Log("manual-test", "data root kept at: ", data_root.string());
  return 0;
}

int RunPhase2(const std::filesystem::path& data_root) {
  Log("manual-test",
      "phase-2: restart from existing data, verify restored metadata, save another snapshot, then end");
  LogStateFiles(data_root);

  const auto configs = BuildThreeNodeConfigs(data_root, 53250);
  auto cluster = StartCluster(configs);

  auto leader = WaitForLeader(cluster.nodes, 6s);
  if (!leader) {
    Log("manual-test", "phase-2 failed: leader election timeout");
    StopCluster(&cluster);
    return 1;
  }

  LogClusterSnapshot(cluster.nodes, "phase-2 snapshot right after restart");
  SaveClusterSnapshotFiles(cluster.nodes, data_root,
                           "phase2_after_restart_before_probe");

  const MetadataRecoveryExpectation phase1_expectation = BuildPhase1Expectation();
  if (!WaitUntilMetadataMatches(cluster.nodes, phase1_expectation, 8s)) {
    Log("manual-test",
        "phase-2 failed: phase-1 metadata was not fully restored after restart");
    StopCluster(&cluster);
    return 1;
  }

  Log("manual-test",
      "phase-2: sending one metadata probe write to advance commit/apply after restart");
  if (!ProposeCommittedObject(cluster.nodes, RecoveryProbeKey(), "manual-recovery-probe")) {
    StopCluster(&cluster);
    return 1;
  }

  const MetadataRecoveryExpectation phase2_expectation = BuildPhase2Expectation();
  const bool restored =
      WaitUntilMetadataMatches(cluster.nodes, phase2_expectation, 8s);

  LogClusterSnapshot(cluster.nodes, "phase-2 snapshot after recovery probe");
  SaveClusterSnapshotFiles(cluster.nodes, data_root, "phase2_after_recovery_probe");

  StopCluster(&cluster);
  LogStateFiles(data_root);

  if (!restored) {
    Log("manual-test",
        "phase-2 failed: persisted metadata facts were not restored to all nodes");
    return 1;
  }

  WriteMarker(data_root / "phase2.ok", "recovery verified\n");
  Log("manual-test", "phase-2 success: persistence and restart recovery verified.");
  Log("manual-test", "manual snapshot after restart is under: ",
      (data_root / "snapshots" / "phase2_after_recovery_probe").string());
  Log("manual-test", "you can inspect the files under: ", data_root.string());
  return 0;
}

}  // namespace
}  // namespace raftdemo

int main(int argc, char** argv) {
  using namespace raftdemo;

  std::filesystem::path data_root;
  if (argc >= 2 && argv[1] != nullptr && std::string(argv[1]).size() > 0) {
    data_root = std::filesystem::path(argv[1]);
  } else {
    data_root =
        std::filesystem::current_path() / "raft_data" / "manual_restart_demo";
  }

  const auto marker = data_root / "phase1.done";
  Log("manual-test",
      "executable mode = auto two-phase metadata persistence demo with manual snapshot export");
  Log("manual-test", "data root = ", data_root.string());
  Log("manual-test", "marker file = ", marker.string());

  if (!FileExists(marker)) {
    return RunPhase1(data_root);
  }
  return RunPhase2(data_root);
}
