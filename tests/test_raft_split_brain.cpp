#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "metadata_raft_test_utils.h"
#include "raft/common/config.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"

namespace raftdemo {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

fs::path TestBinaryDir() {
#ifdef RAFT_TEST_BINARY_DIR
  return fs::path(RAFT_TEST_BINARY_DIR);
#else
  return fs::current_path();
#endif
}

std::uint64_t NowForPath() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string SafeTestName() {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name = std::string(info->test_suite_name()) + "." + info->name();
  for (char& ch : name) {
    if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ') {
      ch = '_';
    }
  }
  return name;
}

fs::path MakeTestRoot() {
  std::random_device rd;
  return TestBinaryDir() / "raft_test_data" / "split_brain" /
         (SafeTestName() + "_" + std::to_string(NowForPath()) + "_" +
          std::to_string(rd()));
}

int PickBasePort() {
  if (const char* env = std::getenv("RAFT_TEST_BASE_PORT")) {
    try {
      return std::stoi(env);
    } catch (...) {
    }
  }

  std::random_device rd;
  return 43000 + static_cast<int>(rd() % 12000);
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

bool IsLeaderSnapshot(const std::string& snapshot) {
  return Contains(snapshot, "role=Leader");
}

constexpr const char* kSplitBrainBucket = "split-brain-bucket";

std::optional<std::uint64_t> ParseUintField(const std::string& text,
                                            const std::string& field) {
  const auto pos = text.find(field);
  if (pos == std::string::npos) {
    return std::nullopt;
  }

  std::size_t begin = pos + field.size();
  std::size_t end = begin;
  while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
    ++end;
  }
  if (end == begin) {
    return std::nullopt;
  }

  try {
    return static_cast<std::uint64_t>(std::stoull(text.substr(begin, end - begin)));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<int> ParseIntField(const std::string& text, const std::string& field) {
  const auto pos = text.find(field);
  if (pos == std::string::npos) {
    return std::nullopt;
  }

  std::size_t begin = pos + field.size();
  std::size_t end = begin;
  if (end < text.size() && text[end] == '-') {
    ++end;
  }
  while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
    ++end;
  }
  if (end == begin || (end == begin + 1 && text[begin] == '-')) {
    return std::nullopt;
  }

  try {
    return std::stoi(text.substr(begin, end - begin));
  } catch (...) {
    return std::nullopt;
  }
}

std::string SplitBrainObjectId(const std::string& object_key) {
  return "split-brain-object-" + object_key;
}

ProposeResult ProposeMetadataCommand(const std::shared_ptr<RaftNode>& node,
                                     const MetadataCommand& command) {
  return node->ProposeMetadata(SerializeMetadataCommand(command));
}

MetadataCommand MakeLeaderReadyProbeCommand(std::size_t follower_index,
                                            int leader_id) {
  return test::MakeCreateBucketCommand(
      "__leader_ready_probe_bucket_" + std::to_string(leader_id) + "_" +
          std::to_string(follower_index),
      "__leader_ready_probe_request_" + std::to_string(leader_id) + "_" +
          std::to_string(follower_index));
}

std::vector<NodeConfig> BuildThreeNodeConfigs(int base_port, const fs::path& root) {
  std::vector<NodeConfig> configs;
  configs.reserve(3);

  for (int id = 1; id <= 3; ++id) {
    NodeConfig cfg;
    cfg.node_id = id;
    cfg.address = "127.0.0.1:" + std::to_string(base_port + id);
    cfg.election_timeout_min = 300ms;
    cfg.election_timeout_max = 600ms;
    cfg.heartbeat_interval = 80ms;
    cfg.rpc_deadline = 500ms;
    cfg.data_dir = (root / "raft_data" / ("node_" + std::to_string(id))).string();

    for (int peer_id = 1; peer_id <= 3; ++peer_id) {
      if (peer_id == id) {
        continue;
      }
      cfg.peers.push_back(PeerConfig{
          peer_id,
          "127.0.0.1:" + std::to_string(base_port + peer_id),
      });
    }

    configs.push_back(std::move(cfg));
  }

  return configs;
}

NodeConfig BuildSingleNodeConfig(const fs::path& root) {
  NodeConfig cfg;
  cfg.node_id = 1;
  cfg.address = "127.0.0.1:0";
  cfg.election_timeout_min = 1h;
  cfg.election_timeout_max = 1h;
  cfg.heartbeat_interval = 1h;
  cfg.rpc_deadline = 50ms;
  cfg.data_dir = (root / "raft_data" / "node_1").string();
  return cfg;
}

snapshotConfig DisabledSnapshotConfig(const fs::path& root) {
  snapshotConfig cfg;
  cfg.enabled = false;
  cfg.snapshot_dir = (root / "raft_snapshots").string();
  return cfg;
}

class SplitBrainCluster {
 public:
  explicit SplitBrainCluster(int base_port) : root_(MakeTestRoot()) {
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::create_directories(root_, ec);

    snapshot_config_ = DisabledSnapshotConfig(root_);

    const auto configs = BuildThreeNodeConfigs(base_port, root_);
    nodes_.reserve(configs.size());
    for (const auto& cfg : configs) {
      NodeRuntime runtime;
      runtime.node_id = cfg.node_id;
      runtime.node = std::make_shared<RaftNode>(cfg, snapshot_config_);
      nodes_.push_back(std::move(runtime));
    }
  }

  ~SplitBrainCluster() { StopAll(); }

  void StartAll() {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      StartNode(i);
    }
  }

  void StopAll() {
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      StopNode(i);
    }
  }

  void StartNode(std::size_t index) {
    auto& runtime = nodes_.at(index);
    if (runtime.running) {
      return;
    }

    runtime.running = true;
    auto node = runtime.node;
    runtime.thread = std::thread([node]() {
      node->Start();
      node->Wait();
    });
  }

  void StopNode(std::size_t index) {
    auto& runtime = nodes_.at(index);
    if (!runtime.running) {
      return;
    }

    runtime.node->Stop();
    if (runtime.thread.joinable()) {
      runtime.thread.join();
    }
    runtime.running = false;
  }

  std::optional<std::size_t> WaitForLeader(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (!nodes_[i].running) {
          continue;
        }
        if (IsLeaderSnapshot(nodes_[i].node->Describe())) {
          return i;
        }
      }
      std::this_thread::sleep_for(50ms);
    }
    return std::nullopt;
  }

  std::string DescribeCluster() const {
    std::ostringstream oss;
    bool first = true;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      if (!first) {
        oss << " | ";
      }
      first = false;
      oss << "node[" << i << "] id=" << nodes_[i].node_id
          << " running=" << (nodes_[i].running ? "true" : "false");
      if (nodes_[i].running) {
        oss << " " << nodes_[i].node->Describe();
      }
    }
    return oss.str();
  }

  std::string DescribeMetadataOnAllNodes(const std::string& bucket,
                                         const std::string& object_key) const {
    std::ostringstream oss;
    bool first = true;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      if (!first) {
        oss << " | ";
      }
      first = false;
      oss << "node[" << i << "] id=" << nodes_[i].node_id
          << " running=" << (nodes_[i].running ? "true" : "false");
      if (!nodes_[i].running) {
        continue;
      }

      const MetadataStateMachine* state_machine =
          nodes_[i].node->GetMetadataStateMachineV2();
      if (state_machine == nullptr) {
        oss << " metadata_sm=<null>";
      } else {
        const auto response = state_machine->HeadObject(
            {.bucket = bucket, .object_key = object_key});
        const auto internal_record = state_machine->FindObject(bucket, object_key);
        const auto indexed_object_id =
            state_machine->FindIndexedObjectId(bucket, object_key);
        const auto chunk_refs = state_machine->FindChunkRefs(bucket, object_key);
        oss << " head=" << static_cast<int>(response.result.code);
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
        oss << " requests=" << state_machine->RequestCount()
            << " tombstones=" << state_machine->TombstoneCount()
            << " last_applied_index=" << state_machine->LastAppliedIndex()
            << " last_applied_term=" << state_machine->LastAppliedTerm();
      }
    }
    return oss.str();
  }

  bool WaitUntilCommittedObjectOnAllRunning(const std::string& bucket,
                                            const std::string& object_key,
                                            const std::string& object_id,
                                            std::uint64_t expected_last_applied_index,
                                            std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      bool ok = true;
      for (const auto& runtime : nodes_) {
        if (!runtime.running) {
          continue;
        }
        const MetadataStateMachine* state_machine =
            runtime.node->GetMetadataStateMachineV2();
        if (state_machine == nullptr) {
          ok = false;
          break;
        }
        const auto response = state_machine->HeadObject(
            {.bucket = bucket, .object_key = object_key});
        if (!response.result.Ok() || !response.record.has_value() ||
            !response.record->IsCommitted() ||
            response.record->object_id != object_id) {
          ok = false;
          break;
        }
        const auto indexed_object_id =
            state_machine->FindIndexedObjectId(bucket, object_key);
        if (!indexed_object_id.has_value() || *indexed_object_id != object_id) {
          ok = false;
          break;
        }
        const auto chunk_refs = state_machine->FindChunkRefs(bucket, object_key);
        if (!chunk_refs.has_value() || chunk_refs->size() != 2 ||
            state_machine->LastAppliedIndex() < expected_last_applied_index) {
          ok = false;
          break;
        }
      }
      if (ok) {
        return true;
      }
      std::this_thread::sleep_for(50ms);
    }
    return false;
  }

  bool WaitUntilSingleLeader(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int leader_count = 0;
      for (const auto& runtime : nodes_) {
        if (!runtime.running) {
          continue;
        }
        if (IsLeaderSnapshot(runtime.node->Describe())) {
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

  std::shared_ptr<RaftNode> Node(std::size_t index) const {
    return nodes_.at(index).node;
  }

  std::size_t Size() const { return nodes_.size(); }

  bool IsRunning(std::size_t index) const { return nodes_.at(index).running; }

  int NodeId(std::size_t index) const {
    return nodes_.at(index).node_id;
  }

  std::vector<std::size_t> OtherIndexes(std::size_t index) const {
    std::vector<std::size_t> result;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      if (i != index) {
        result.push_back(i);
      }
    }
    return result;
  }

 private:
  struct NodeRuntime {
    int node_id{0};
    std::shared_ptr<RaftNode> node;
    std::thread thread;
    bool running{false};
  };

  fs::path root_;
  snapshotConfig snapshot_config_;
  std::vector<NodeRuntime> nodes_;
};

std::optional<std::size_t> FindLeaderOnce(const SplitBrainCluster& cluster,
                                          std::string* leader_snapshot) {
  for (std::size_t i = 0; i < cluster.Size(); ++i) {
    if (!cluster.IsRunning(i)) {
      continue;
    }
    const std::string snapshot = cluster.Node(i)->Describe();
    if (IsLeaderSnapshot(snapshot)) {
      if (leader_snapshot != nullptr) {
        *leader_snapshot = snapshot;
      }
      return i;
    }
  }
  if (leader_snapshot != nullptr) {
    leader_snapshot->clear();
  }
  return std::nullopt;
}

std::optional<int> FindMajorityVotedCandidate(const SplitBrainCluster& cluster,
                                              std::string* vote_summary) {
  std::vector<std::pair<int, int>> vote_counts;
  std::ostringstream summary;
  bool first = true;

  for (std::size_t i = 0; i < cluster.Size(); ++i) {
    if (!cluster.IsRunning(i)) {
      continue;
    }

    const std::string snapshot = cluster.Node(i)->Describe();
    const std::optional<int> voted_for = ParseIntField(snapshot, "voted_for=");
    if (!first) {
      summary << " | ";
    }
    first = false;
    summary << "node[" << i << "] voted_for=";
    if (voted_for.has_value()) {
      summary << *voted_for;
    } else {
      summary << "<unknown>";
    }

    if (!voted_for.has_value() || *voted_for <= 0) {
      continue;
    }

    bool matched = false;
    for (auto& [candidate_id, count] : vote_counts) {
      if (candidate_id == *voted_for) {
        ++count;
        matched = true;
        break;
      }
    }
    if (!matched) {
      vote_counts.emplace_back(*voted_for, 1);
    }
  }

  if (vote_summary != nullptr) {
    *vote_summary = summary.str();
  }

  for (const auto& [candidate_id, count] : vote_counts) {
    if (count >= 2) {
      return candidate_id;
    }
  }
  return std::nullopt;
}

std::string FormatLeaderObservation(std::optional<std::size_t> leader_index,
                                    int consecutive_observations,
                                    const std::string& cluster_snapshot) {
  std::ostringstream oss;
  oss << "leader=";
  if (leader_index.has_value()) {
    oss << *leader_index;
  } else {
    oss << "<none>";
  }
  oss << ", consecutive_observations=" << consecutive_observations
      << ", cluster=" << cluster_snapshot;
  return oss.str();
}

std::optional<std::size_t> WaitForStableLeader(const SplitBrainCluster& cluster,
                                               std::chrono::milliseconds timeout,
                                               int required_consecutive_observations,
                                               std::string* diagnostics) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::optional<std::chrono::steady_clock::time_point> vote_grace_deadline;
  std::optional<std::size_t> last_leader;
  int consecutive = 0;
  std::string last_cluster_snapshot = cluster.DescribeCluster();
  std::string last_vote_summary;
  std::optional<int> last_majority_voted_candidate;
  std::string last_leader_snapshot;

  while (true) {
    const auto now = std::chrono::steady_clock::now();
    const std::optional<std::size_t> current_leader =
        FindLeaderOnce(cluster, &last_leader_snapshot);
    last_cluster_snapshot = cluster.DescribeCluster();
    last_majority_voted_candidate =
        FindMajorityVotedCandidate(cluster, &last_vote_summary);

    if (current_leader.has_value()) {
      if (last_leader == current_leader) {
        ++consecutive;
      } else {
        last_leader = current_leader;
        consecutive = 1;
      }

      if (consecutive >= required_consecutive_observations) {
        if (diagnostics != nullptr) {
          *diagnostics = "stable leader observed: " +
                         FormatLeaderObservation(last_leader, consecutive,
                                                 last_cluster_snapshot) +
                         ", leader_snapshot=" + last_leader_snapshot;
        }
        return current_leader;
      }
    } else {
      last_leader.reset();
      consecutive = 0;

      if (now >= deadline && last_majority_voted_candidate.has_value() &&
          !vote_grace_deadline.has_value()) {
        vote_grace_deadline = now + 600ms;
      }
    }

    if (now >= deadline &&
        (!vote_grace_deadline.has_value() || now >= *vote_grace_deadline)) {
      break;
    }

    std::this_thread::sleep_for(50ms);
  }

  if (diagnostics != nullptr) {
    *diagnostics = "election did not converge to a stable leader within timeout; " +
                   FormatLeaderObservation(last_leader, consecutive,
                                           last_cluster_snapshot) +
                   ", majority_voted_candidate=" +
                   (last_majority_voted_candidate.has_value()
                        ? std::to_string(*last_majority_voted_candidate)
                        : std::string("<none>")) +
                   ", votes=" + last_vote_summary +
                   (vote_grace_deadline.has_value()
                        ? ", vote_progress_grace=used"
                        : ", vote_progress_grace=unused");
  }
  return std::nullopt;
}

bool WaitForMetadataToRemainAbsent(const SplitBrainCluster& cluster,
                                   std::size_t node_index,
                                   const std::string& bucket,
                                   const std::string& object_key,
                                   std::size_t expected_request_count,
                                   std::uint64_t expected_last_applied_index,
                                   std::chrono::milliseconds timeout,
                                   std::string* diagnostics) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::string last_value_summary =
      cluster.DescribeMetadataOnAllNodes(bucket, object_key);

  while (std::chrono::steady_clock::now() < deadline) {
    const MetadataStateMachine* state_machine =
        cluster.Node(node_index)->GetMetadataStateMachineV2();
    if (state_machine == nullptr) {
      if (diagnostics != nullptr) {
        *diagnostics = "metadata state machine unavailable; cluster=" +
                       cluster.DescribeCluster();
      }
      return false;
    }
    const auto response = state_machine->HeadObject(
        {.bucket = bucket, .object_key = object_key});
    const auto internal_record = state_machine->FindObject(bucket, object_key);
    if (response.result.code != MetadataStatusCode::kNotFound ||
        response.record.has_value() || internal_record.has_value() ||
        state_machine->FindIndexedObjectId(bucket, object_key).has_value() ||
        state_machine->FindChunkRefs(bucket, object_key).has_value() ||
        state_machine->RequestCount() != expected_request_count ||
        state_machine->LastAppliedIndex() != expected_last_applied_index) {
      if (diagnostics != nullptr) {
        *diagnostics =
            "uncommitted metadata command polluted minority node state; values=" +
            cluster.DescribeMetadataOnAllNodes(bucket, object_key) +
            ", cluster=" + cluster.DescribeCluster();
      }
      return false;
    }

    last_value_summary = cluster.DescribeMetadataOnAllNodes(bucket, object_key);
    std::this_thread::sleep_for(50ms);
  }

  if (diagnostics != nullptr) {
    *diagnostics = "metadata remained absent during observation window; values=" +
                   last_value_summary + ", cluster=" + cluster.DescribeCluster();
  }
  return true;
}

bool WaitForLeaderServiceReady(const SplitBrainCluster& cluster, std::size_t leader_index,
                               std::chrono::milliseconds timeout,
                               std::string* diagnostics) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto follower_indexes = cluster.OtherIndexes(leader_index);
  std::string last_cluster_snapshot = cluster.DescribeCluster();
  std::string last_reason = "leader role not yet observable";

  while (std::chrono::steady_clock::now() < deadline) {
    const std::string leader_snapshot = cluster.Node(leader_index)->Describe();
    last_cluster_snapshot = cluster.DescribeCluster();

    if (!IsLeaderSnapshot(leader_snapshot)) {
      std::string vote_summary;
      const std::optional<int> majority_voted_candidate =
          FindMajorityVotedCandidate(cluster, &vote_summary);
      last_reason =
          "majority vote formed but leader role not observable; leader_snapshot=" +
          leader_snapshot + ", majority_voted_candidate=" +
          (majority_voted_candidate.has_value()
               ? std::to_string(*majority_voted_candidate)
               : std::string("<none>")) +
          ", votes=" + vote_summary;
      std::this_thread::sleep_for(50ms);
      continue;
    }

    bool followers_ready = true;
    for (const auto follower_index : follower_indexes) {
      const std::string follower_snapshot = cluster.Node(follower_index)->Describe();
      if (IsLeaderSnapshot(follower_snapshot)) {
        followers_ready = false;
        last_reason = "multiple leaders observable while waiting for service-ready; "
                      "follower_snapshot=" + follower_snapshot;
        break;
      }
    }
    if (!followers_ready) {
      std::this_thread::sleep_for(50ms);
      continue;
    }

    for (const auto follower_index : follower_indexes) {
      const ProposeResult probe = ProposeMetadataCommand(
          cluster.Node(follower_index),
          MakeLeaderReadyProbeCommand(follower_index, cluster.NodeId(leader_index)));
      if (probe.status != ProposeStatus::kNotLeader) {
        followers_ready = false;
        last_reason = "leader observable but follower still not redirecting writes; "
                      "follower_index=" + std::to_string(follower_index) +
                      ", status=" + std::to_string(static_cast<int>(probe.status)) +
                      ", message=" + probe.message;
        break;
      }
      if (probe.leader_id != -1 && probe.leader_id != cluster.NodeId(leader_index)) {
        followers_ready = false;
        last_reason = "leader observable but follower reports different leader_id; "
                      "follower_index=" + std::to_string(follower_index) +
                      ", reported_leader_id=" + std::to_string(probe.leader_id) +
                      ", expected_leader_id=" + std::to_string(cluster.NodeId(leader_index));
        break;
      }
    }

    if (followers_ready) {
      if (diagnostics != nullptr) {
        *diagnostics = "leader service-ready observed; leader_index=" +
                       std::to_string(leader_index) + ", cluster=" +
                       last_cluster_snapshot;
      }
      return true;
    }

    std::this_thread::sleep_for(50ms);
  }

  if (diagnostics != nullptr) {
    *diagnostics = last_reason + ", cluster=" + last_cluster_snapshot;
  }
  return false;
}

TEST(RaftSplitBrainTest, MinorityLeaderTimesOutAndDoesNotApplyUncommittedCommand) {
  SplitBrainCluster cluster(PickBasePort());
  cluster.StartAll();

  const auto leader_index = cluster.WaitForLeader(5s);
  ASSERT_TRUE(leader_index.has_value())
      << "election 未收敛；cluster=" << cluster.DescribeCluster();

  ASSERT_TRUE(cluster.WaitUntilSingleLeader(2s))
      << "leader 可见但未收敛到单 leader；cluster=" << cluster.DescribeCluster();

  std::string service_ready_diagnostics;
  ASSERT_TRUE(WaitForLeaderServiceReady(cluster, *leader_index, 2s, &service_ready_diagnostics))
      << "leader observable but not service-ready；" << service_ready_diagnostics;

  ProposeResult create_bucket_result;
  ASSERT_TRUE(test::ProposeCreateBucketWithRetry(
      {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
      kSplitBrainBucket,
      "split-brain-bucket-create",
      5s,
      &create_bucket_result))
      << create_bucket_result.message;

  const MetadataStateMachine* leader_state_before_partition =
      cluster.Node(*leader_index)->GetMetadataStateMachineV2();
  ASSERT_NE(leader_state_before_partition, nullptr)
      << cluster.Node(*leader_index)->Describe();
  const std::size_t expected_request_count =
      leader_state_before_partition->RequestCount();
  const std::uint64_t expected_last_applied_index =
      leader_state_before_partition->LastAppliedIndex();

  for (const auto follower_index : cluster.OtherIndexes(*leader_index)) {
    cluster.StopNode(follower_index);
  }

  const std::string isolated_snapshot = cluster.Node(*leader_index)->Describe();
  ASSERT_TRUE(IsLeaderSnapshot(isolated_snapshot))
      << "没有形成预期 minority partition：被隔离节点在 proposal 前已不是 leader; leader_index="
      << *leader_index << ", leader_before_partition=" << service_ready_diagnostics
      << ", isolated_node=" << isolated_snapshot
      << ", cluster=" << cluster.DescribeCluster();

  const ProposeResult result = ProposeMetadataCommand(
      cluster.Node(*leader_index),
      test::MakeCreateObjectCommand(
          kSplitBrainBucket,
          "minority_key",
          SplitBrainObjectId("minority_key"),
          "minority-create-request"));

  EXPECT_EQ(result.status, ProposeStatus::kTimeout)
      << "minority leader 没有按预期超时; status="
      << static_cast<int>(result.status) << ", message=" << result.message
      << ", leader_index=" << *leader_index
      << ", leader_before_partition=" << service_ready_diagnostics
      << ", isolated_node=" << cluster.Node(*leader_index)->Describe()
      << ", cluster=" << cluster.DescribeCluster();

  std::string absence_diagnostics;
  EXPECT_TRUE(WaitForMetadataToRemainAbsent(
      cluster,
      *leader_index,
      kSplitBrainBucket,
      "minority_key",
      expected_request_count,
      expected_last_applied_index,
      500ms,
      &absence_diagnostics))
      << "uncommitted command 被错误 apply; " << absence_diagnostics
      << ", propose_status=" << static_cast<int>(result.status)
      << ", propose_message=" << result.message;
}

TEST(RaftSplitBrainTest, LeaderStepsDownWhenHigherTermAppendEntriesArrives) {
  SplitBrainCluster cluster(PickBasePort());
  cluster.StartAll();

  const auto leader_index = cluster.WaitForLeader(5s);
  ASSERT_TRUE(leader_index.has_value());

  const std::string before = cluster.Node(*leader_index)->Describe();
  const auto old_term = ParseUintField(before, "term=");
  ASSERT_TRUE(old_term.has_value()) << before;

  const auto other_indexes = cluster.OtherIndexes(*leader_index);
  ASSERT_FALSE(other_indexes.empty());

  raft::AppendEntriesRequest request;
  request.set_term(*old_term + 1);
  request.set_leader_id(cluster.NodeId(other_indexes.front()));
  request.set_prev_log_index(0);
  request.set_prev_log_term(0);
  request.set_leader_commit(0);

  raft::AppendEntriesResponse response;
  cluster.Node(*leader_index)->OnAppendEntries(request, &response);

  EXPECT_TRUE(response.success());
  EXPECT_EQ(response.term(), *old_term + 1);

  const std::string after = cluster.Node(*leader_index)->Describe();
  EXPECT_TRUE(Contains(after, "role=Follower")) << after;
  EXPECT_TRUE(Contains(after, "term=" + std::to_string(*old_term + 1))) << after;

  const ProposeResult result = ProposeMetadataCommand(
      cluster.Node(*leader_index),
      test::MakeCreateBucketCommand(
          "old-leader-bucket",
          "old-leader-must-reject"));
  EXPECT_EQ(result.status, ProposeStatus::kNotLeader) << result.message;
}

TEST(RaftSplitBrainTest, StaleAppendEntriesIsRejectedAfterNodeObservesHigherTerm) {
  const fs::path root = MakeTestRoot();
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  RaftNode node(BuildSingleNodeConfig(root), DisabledSnapshotConfig(root));

  raft::VoteRequest higher_term_vote;
  higher_term_vote.set_term(5);
  higher_term_vote.set_candidate_id(10);
  higher_term_vote.set_last_log_index(0);
  higher_term_vote.set_last_log_term(0);

  raft::VoteResponse vote_response;
  node.OnRequestVote(higher_term_vote, &vote_response);
  ASSERT_TRUE(vote_response.vote_granted());
  ASSERT_EQ(vote_response.term(), 5u);

  raft::AppendEntriesRequest stale_append;
  stale_append.set_term(4);
  stale_append.set_leader_id(9);
  stale_append.set_prev_log_index(0);
  stale_append.set_prev_log_term(0);
  stale_append.set_leader_commit(0);

  raft::AppendEntriesResponse append_response;
  node.OnAppendEntries(stale_append, &append_response);

  EXPECT_FALSE(append_response.success());
  EXPECT_EQ(append_response.term(), 5u);
}

TEST(RaftSplitBrainTest, SameTermSecondCandidateVoteIsRejected) {
  const fs::path root = MakeTestRoot();
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  RaftNode node(BuildSingleNodeConfig(root), DisabledSnapshotConfig(root));

  raft::VoteRequest first;
  first.set_term(7);
  first.set_candidate_id(101);
  first.set_last_log_index(0);
  first.set_last_log_term(0);

  raft::VoteResponse first_response;
  node.OnRequestVote(first, &first_response);
  ASSERT_TRUE(first_response.vote_granted());
  ASSERT_EQ(first_response.term(), 7u);

  raft::VoteRequest second;
  second.set_term(7);
  second.set_candidate_id(102);
  second.set_last_log_index(0);
  second.set_last_log_term(0);

  raft::VoteResponse second_response;
  node.OnRequestVote(second, &second_response);
  EXPECT_FALSE(second_response.vote_granted());
  EXPECT_EQ(second_response.term(), 7u);
}

TEST(RaftSplitBrainTest, StaleCandidateVoteRequestIsRejectedEvenWithHigherTerm) {
  SplitBrainCluster cluster(PickBasePort());
  cluster.StartAll();

  const auto leader_index = cluster.WaitForLeader(5s);
  ASSERT_TRUE(leader_index.has_value());

  ProposeResult result;
  ASSERT_TRUE(test::ProposeCreateBucketWithRetry(
      {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
      kSplitBrainBucket,
      "stale-vote-bucket-create",
      5s,
      &result))
      << result.message;
  ASSERT_TRUE(test::ProposeCreateCommitObjectWithRetry(
      {cluster.Node(0), cluster.Node(1), cluster.Node(2)},
      kSplitBrainBucket,
      "committed_key",
      SplitBrainObjectId("committed_key"),
      "stale-vote-create",
      "stale-vote-commit",
      5s,
      &result))
      << result.message;
  ASSERT_TRUE(cluster.WaitUntilCommittedObjectOnAllRunning(
      kSplitBrainBucket,
      "committed_key",
      SplitBrainObjectId("committed_key"),
      result.log_index,
      5s))
      << cluster.DescribeMetadataOnAllNodes(kSplitBrainBucket, "committed_key");

  const auto follower_indexes = cluster.OtherIndexes(*leader_index);
  ASSERT_FALSE(follower_indexes.empty());
  auto follower = cluster.Node(follower_indexes.front());

  const std::string follower_state = follower->Describe();
  const auto follower_term = ParseUintField(follower_state, "term=");
  ASSERT_TRUE(follower_term.has_value()) << follower_state;

  raft::VoteRequest request;
  request.set_term(*follower_term + 1);
  request.set_candidate_id(99);
  request.set_last_log_index(0);
  request.set_last_log_term(0);

  raft::VoteResponse response;
  follower->OnRequestVote(request, &response);

  EXPECT_EQ(response.term(), *follower_term + 1);
  EXPECT_FALSE(response.vote_granted());
}

TEST(RaftSplitBrainTest, AppendEntriesRejectionIncludesFastBacktrackHint) {
  const fs::path root = MakeTestRoot();
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  RaftNode node(BuildSingleNodeConfig(root), DisabledSnapshotConfig(root));

  raft::AppendEntriesRequest append;
  append.set_term(2);
  append.set_leader_id(2);
  append.set_prev_log_index(0);
  append.set_prev_log_term(0);
  append.set_leader_commit(0);

  auto* first = append.add_entries();
  first->set_index(1);
  first->set_term(1);
  first->set_command(SerializeMetadataCommand(
      test::MakeCreateBucketCommand("fast-backtrack-bucket", "fast-backtrack-request-1")));

  auto* second = append.add_entries();
  second->set_index(2);
  second->set_term(2);
  second->set_command(SerializeMetadataCommand(
      test::MakeCreateBucketCommand("fast-backtrack-bucket-2", "fast-backtrack-request-2")));

  raft::AppendEntriesResponse append_response;
  node.OnAppendEntries(append, &append_response);
  ASSERT_TRUE(append_response.success());
  ASSERT_EQ(append_response.match_index(), 2U);

  raft::AppendEntriesRequest term_mismatch;
  term_mismatch.set_term(2);
  term_mismatch.set_leader_id(2);
  term_mismatch.set_prev_log_index(2);
  term_mismatch.set_prev_log_term(99);
  term_mismatch.set_leader_commit(0);

  raft::AppendEntriesResponse mismatch_response;
  node.OnAppendEntries(term_mismatch, &mismatch_response);
  EXPECT_FALSE(mismatch_response.success());
  EXPECT_EQ(mismatch_response.conflict_term(), 2U);
  EXPECT_EQ(mismatch_response.conflict_index(), 2U);
  EXPECT_EQ(mismatch_response.last_log_index(), 2U);

  raft::AppendEntriesRequest missing_prev;
  missing_prev.set_term(2);
  missing_prev.set_leader_id(2);
  missing_prev.set_prev_log_index(10);
  missing_prev.set_prev_log_term(2);
  missing_prev.set_leader_commit(0);

  raft::AppendEntriesResponse missing_response;
  node.OnAppendEntries(missing_prev, &missing_response);
  EXPECT_FALSE(missing_response.success());
  EXPECT_EQ(missing_response.conflict_term(), 0U);
  EXPECT_EQ(missing_response.conflict_index(), 3U);
  EXPECT_EQ(missing_response.last_log_index(), 2U);
}

TEST(RaftSplitBrainTest, InstallSnapshotDiscardsSuffixWhenBoundaryTermDiffers) {
  const fs::path root = MakeTestRoot();
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  snapshotConfig snapshot_config;
  snapshot_config.enabled = true;
  snapshot_config.snapshot_dir = (root / "raft_snapshots").string();
  snapshot_config.load_on_startup = true;

  RaftNode node(BuildSingleNodeConfig(root), snapshot_config);

  raft::AppendEntriesRequest append;
  append.set_term(2);
  append.set_leader_id(2);
  append.set_prev_log_index(0);
  append.set_prev_log_term(0);
  append.set_leader_commit(0);

  auto* first = append.add_entries();
  first->set_index(1);
  first->set_term(1);
  first->set_command(SerializeMetadataCommand(
      test::MakeCreateBucketCommand(
          "before-snapshot-bucket",
          "before-snapshot-request")));

  auto* divergent = append.add_entries();
  divergent->set_index(2);
  divergent->set_term(2);
  divergent->set_command(SerializeMetadataCommand(
      test::MakeCreateBucketCommand(
          "divergent-suffix-bucket",
          "divergent-suffix-request")));

  raft::AppendEntriesResponse append_response;
  node.OnAppendEntries(append, &append_response);
  ASSERT_TRUE(append_response.success());
  ASSERT_TRUE(Contains(node.Describe(), "last_log_index=2")) << node.Describe();

  MetadataStateMachine snapshot_state;
  ASSERT_TRUE(test::ApplyMetadataCommand(
      snapshot_state,
      1,
      test::MakeCreateBucketCommand(
          "before-snapshot-bucket",
          "before-snapshot-request"),
      9).Ok);

  const fs::path snapshot_file = root / "snapshot_payload.bin";
  const SnapshotResult save_snapshot = snapshot_state.SaveSnapshot(snapshot_file.string());
  ASSERT_TRUE(save_snapshot.Ok()) << save_snapshot.message;

  std::ifstream in(snapshot_file, std::ios::binary);
  ASSERT_TRUE(in.is_open()) << snapshot_file;
  std::string snapshot_data((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());

  raft::InstallSnapshotRequest install;
  install.set_term(3);
  install.set_leader_id(2);
  install.set_last_included_index(1);
  install.set_last_included_term(9);
  install.set_snapshot_data(snapshot_data);

  raft::InstallSnapshotResponse install_response;
  node.OnInstallSnapshot(install, &install_response);
  ASSERT_TRUE(install_response.success()) << install_response.message();

  const std::string after_install = node.Describe();
  EXPECT_TRUE(Contains(after_install, "last_snapshot_index=1")) << after_install;
  EXPECT_TRUE(Contains(after_install, "last_log_index=1")) << after_install;
  const MetadataStateMachine* state_machine = node.GetMetadataStateMachineV2();
  ASSERT_NE(state_machine, nullptr);
  const auto snapshot_bucket = state_machine->FindBucket("before-snapshot-bucket");
  ASSERT_TRUE(snapshot_bucket.has_value()) << after_install;
  EXPECT_TRUE(snapshot_bucket->IsActive()) << after_install;
  EXPECT_FALSE(state_machine->FindBucket("divergent-suffix-bucket").has_value())
      << after_install;
  EXPECT_EQ(state_machine->RequestCount(), 1u) << after_install;
  EXPECT_EQ(state_machine->LastAppliedIndex(), 1u) << after_install;
  EXPECT_EQ(state_machine->LastAppliedTerm(), 9u) << after_install;

  raft::AppendEntriesRequest replacement;
  replacement.set_term(3);
  replacement.set_leader_id(2);
  replacement.set_prev_log_index(1);
  replacement.set_prev_log_term(9);
  replacement.set_leader_commit(2);

  auto* replacement_entry = replacement.add_entries();
  replacement_entry->set_index(2);
  replacement_entry->set_term(3);
  replacement_entry->set_command(SerializeMetadataCommand(
      test::MakeCreateBucketCommand(
          "replacement-suffix-bucket",
          "replacement-suffix-request")));

  raft::AppendEntriesResponse replacement_response;
  node.OnAppendEntries(replacement, &replacement_response);
  EXPECT_TRUE(replacement_response.success());
  EXPECT_EQ(replacement_response.match_index(), 2U);
  const MetadataStateMachine* state_machine_after_replacement =
      node.GetMetadataStateMachineV2();
  ASSERT_NE(state_machine_after_replacement, nullptr);
  EXPECT_TRUE(
      state_machine_after_replacement->FindBucket("replacement-suffix-bucket").has_value())
      << node.Describe();
  EXPECT_FALSE(
      state_machine_after_replacement->FindBucket("divergent-suffix-bucket").has_value())
      << node.Describe();
  EXPECT_EQ(state_machine_after_replacement->RequestCount(), 2u) << node.Describe();
  EXPECT_EQ(state_machine_after_replacement->LastAppliedIndex(), 2u)
      << node.Describe();
  EXPECT_EQ(state_machine_after_replacement->LastAppliedTerm(), 3u)
      << node.Describe();
}

}  // namespace
}  // namespace raftdemo
