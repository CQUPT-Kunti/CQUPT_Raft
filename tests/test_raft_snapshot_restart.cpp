#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "raft/common/command.h"
#include "raft/common/config.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"
#include "raft/storage/snapshot_storage.h"

namespace raftdemo {
namespace {

using Clock = std::chrono::steady_clock;
constexpr const char* kSnapshotStorageFailpointEnv = "RAFT_TEST_SNAPSHOT_STORAGE_FAILPOINT";

std::string ProposeStatusName(ProposeStatus status) {
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

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

bool IsLeaderNode(const std::shared_ptr<RaftNode>& node) {
  return node && Contains(node->Describe(), "role=Leader");
}

std::string DescribeCluster(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                            const std::vector<std::size_t>& excluded = {}) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    bool excluded_node = false;
    for (std::size_t excluded_index : excluded) {
      if (excluded_index == i) {
        excluded_node = true;
        break;
      }
    }
    if (i != 0) {
      oss << " | ";
    }
    oss << "node[" << i << "]";
    if (excluded_node) {
      oss << "(excluded)";
    }
    oss << "=";
    if (!nodes[i]) {
      oss << "null";
    } else {
      oss << nodes[i]->Describe();
    }
  }
  return oss.str();
}

Command SetCommand(const std::string& key, const std::string& value) {
  Command command;
  command.type = CommandType::kSet;
  command.key = key;
  command.value = value;
  return command;
}

Command DeleteCommand(const std::string& key) {
  Command command;
  command.type = CommandType::kDelete;
  command.key = key;
  return command;
}

std::uint64_t NowForPath() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

int PickBasePort(const std::string& test_name) {
  const int name_offset = static_cast<int>(std::hash<std::string>{}(test_name) % 1800);

  if (const char* env = std::getenv("RAFT_TEST_BASE_PORT")) {
    try {
      return std::stoi(env) + name_offset;
    } catch (...) {
      // Fall through to a generated port range.
    }
  }

  // Keep each test in its own small port window so adjacent cases do not
  // randomly collide with sockets that are still draining on Windows.
  return 36000 + name_offset * 12;
}

std::filesystem::path MakeTestRoot(const std::string& test_name) {
  std::random_device rd;
  std::string safe_name = test_name;
  for (char& ch : safe_name) {
    if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ') {
      ch = '_';
    }
  }

#ifdef _WIN32
  const std::string name = "sr_" + std::to_string(NowForPath()) + "_" +
                           std::to_string(rd());
  return std::filesystem::temp_directory_path() / "rq_sr" / name;
#else
  const std::string name = "raft_snapshot_restart_" + safe_name + "_" +
                           std::to_string(NowForPath()) + "_" +
                           std::to_string(rd());
  return std::filesystem::temp_directory_path() / name;
#endif
}

void WriteTextFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << path.string();
  out << content;
  out.flush();
  ASSERT_TRUE(static_cast<bool>(out)) << path.string();
}

void CopyFile(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code ec;
  std::filesystem::create_directories(to.parent_path(), ec);
  ASSERT_FALSE(ec) << ec.message();
  std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
  ASSERT_FALSE(ec) << "copy snapshot input failed: from=" << from.string()
                   << ", to=" << to.string() << ", error=" << ec.message();
}

void CopyDirectoryRecursively(const std::filesystem::path& from,
                              const std::filesystem::path& to) {
  std::error_code ec;
  std::filesystem::create_directories(to.parent_path(), ec);
  ASSERT_FALSE(ec) << ec.message();
  std::filesystem::copy(from,
                        to,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing,
                        ec);
  ASSERT_FALSE(ec) << "copy snapshot directory failed: from=" << from.string()
                   << ", to=" << to.string() << ", error=" << ec.message();
}

std::string JoinIssueReasons(const std::vector<SnapshotValidationIssue>& issues) {
  std::ostringstream oss;
  for (const auto& issue : issues) {
    oss << issue.path << ": " << issue.reason << "\n";
  }
  return oss.str();
}

std::string FormatSnapshotIndex(std::uint64_t index) {
  std::ostringstream oss;
  oss << std::setw(20) << std::setfill('0') << index;
  return oss.str();
}

std::string PendingT010Message(const std::string& operation,
                               const std::filesystem::path& path,
                               const std::string& trusted_state_expectation,
                               const std::string& diagnostic_expectation) {
  std::ostringstream oss;
  oss << "TODO(T010): missing snapshot storage failure injection seam"
      << ", operation=" << operation
      << ", path=" << path.string()
      << ", linux_specific=true"
      << ", trusted_state_expectation=" << trusted_state_expectation
      << ", diagnostic_expectation=" << diagnostic_expectation;
  return oss.str();
}

const char* ExpectedLinuxSpecificMarker() {
#if defined(__linux__)
  return "linux_specific=true";
#else
  return "linux_specific=false";
#endif
}

void SetEnvVar(const char* name, const std::string& value) {
#if defined(_WIN32)
  ASSERT_EQ(_putenv_s(name, value.c_str()), 0) << name;
#else
  ASSERT_EQ(::setenv(name, value.c_str(), 1), 0) << name;
#endif
}

void UnsetEnvVar(const char* name) {
#if defined(_WIN32)
  ASSERT_EQ(_putenv_s(name, ""), 0) << name;
#else
  ASSERT_EQ(::unsetenv(name), 0) << name;
#endif
}

class ScopedEnvVar {
 public:
  ScopedEnvVar(const char* name, std::string value)
      : name_(name) {
    const char* current = std::getenv(name_);
    if (current != nullptr) {
      had_original_ = true;
      original_value_ = current;
    }
    SetEnvVar(name_, value);
  }

  ~ScopedEnvVar() {
    if (had_original_) {
      SetEnvVar(name_, original_value_);
    } else {
      UnsetEnvVar(name_);
    }
  }

 private:
  const char* name_;
  bool had_original_{false};
  std::string original_value_;
};

std::vector<std::filesystem::path> ListSnapshotDirs(const std::filesystem::path& snapshot_root) {
  std::vector<std::filesystem::path> dirs;
  std::error_code ec;
  if (!std::filesystem::exists(snapshot_root, ec)) {
    return dirs;
  }

  for (const auto& entry : std::filesystem::directory_iterator(snapshot_root, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_directory()) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    if (name.rfind("snapshot_", 0) == 0) {
      dirs.push_back(entry.path());
    }
  }

  std::sort(dirs.begin(), dirs.end());
  return dirs;
}

std::optional<std::uint64_t> SnapshotIndexFromDir(const std::filesystem::path& snapshot_dir) {
  const std::string name = snapshot_dir.filename().string();
  constexpr std::size_t kPrefixSize = 9;  // "snapshot_"
  if (name.size() <= kPrefixSize || name.rfind("snapshot_", 0) != 0) {
    return std::nullopt;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(name.substr(kPrefixSize)));
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::uint64_t> ExtractUintField(const std::string& describe,
                                              const std::string& field_name) {
  const std::string prefix = field_name + "=";
  const std::size_t begin = describe.find(prefix);
  if (begin == std::string::npos) {
    return std::nullopt;
  }

  std::size_t pos = begin + prefix.size();
  std::size_t end = pos;
  while (end < describe.size() && describe[end] >= '0' && describe[end] <= '9') {
    ++end;
  }

  if (end == pos) {
    return std::nullopt;
  }

  try {
    return static_cast<std::uint64_t>(std::stoull(describe.substr(pos, end - pos)));
  } catch (...) {
    return std::nullopt;
  }
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
  n1.election_timeout_min = std::chrono::milliseconds(250);
  n1.election_timeout_max = std::chrono::milliseconds(500);
  n1.heartbeat_interval = std::chrono::milliseconds(80);
  n1.rpc_deadline = std::chrono::milliseconds(300);
  n1.data_dir = (data_root / "node_1").string();

  NodeConfig n2;
  n2.node_id = 2;
  n2.address = "127.0.0.1:" + std::to_string(base_port + 2);
  n2.peers = {
      PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
      PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
  };
  n2.election_timeout_min = std::chrono::milliseconds(250);
  n2.election_timeout_max = std::chrono::milliseconds(500);
  n2.heartbeat_interval = std::chrono::milliseconds(80);
  n2.rpc_deadline = std::chrono::milliseconds(300);
  n2.data_dir = (data_root / "node_2").string();

  NodeConfig n3;
  n3.node_id = 3;
  n3.address = "127.0.0.1:" + std::to_string(base_port + 3);
  n3.peers = {
      PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
      PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
  };
  n3.election_timeout_min = std::chrono::milliseconds(250);
  n3.election_timeout_max = std::chrono::milliseconds(500);
  n3.heartbeat_interval = std::chrono::milliseconds(80);
  n3.rpc_deadline = std::chrono::milliseconds(300);
  n3.data_dir = (data_root / "node_3").string();

  return {n1, n2, n3};
}

std::vector<snapshotConfig> BuildThreeSnapshotConfigs(
    const std::filesystem::path& snapshot_root,
    bool enabled,
    std::uint64_t log_threshold) {
  snapshotConfig s1;
  s1.enabled = enabled;
  s1.snapshot_dir = (snapshot_root / "node_1").string();
  s1.log_threshold = log_threshold;
  s1.snapshot_interval = std::chrono::minutes(10);
  s1.max_snapshot_count = 3;
  s1.load_on_startup = true;
  s1.file_prefix = "snapshot";

  snapshotConfig s2 = s1;
  s2.snapshot_dir = (snapshot_root / "node_2").string();

  snapshotConfig s3 = s1;
  s3.snapshot_dir = (snapshot_root / "node_3").string();

  return {s1, s2, s3};
}

class TestCluster {
 public:
  TestCluster(std::vector<NodeConfig> configs,
              std::vector<snapshotConfig> snapshot_configs)
      : configs_(std::move(configs)), snapshot_configs_(std::move(snapshot_configs)) {}

  ~TestCluster() { StopAll(); }

  void Start() {
    StopAll();
    nodes_.clear();
    wait_threads_.clear();

    for (std::size_t i = 0; i < configs_.size(); ++i) {
      nodes_.push_back(std::make_shared<RaftNode>(configs_[i], snapshot_configs_[i]));
    }
    wait_threads_.resize(nodes_.size());

    for (const auto& node : nodes_) {
      node->Start();
    }
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
      const auto node = nodes_[i];
      wait_threads_[i] = std::thread([node]() { node->Wait(); });
    }
  }

  void StopAll() {
    for (const auto& node : nodes_) {
      if (node) {
        node->Stop();
      }
    }
    for (auto& thread : wait_threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    wait_threads_.clear();
  }

  void StopNode(std::size_t index) {
    if (index >= nodes_.size() || !nodes_[index]) {
      return;
    }
    nodes_[index]->Stop();
    if (index < wait_threads_.size() && wait_threads_[index].joinable()) {
      wait_threads_[index].join();
    }
  }

  void RestartNode(std::size_t index) {
    ASSERT_LT(index, configs_.size());
    StopNode(index);

    if (nodes_.size() < configs_.size()) {
      nodes_.resize(configs_.size());
    }
    if (wait_threads_.size() < configs_.size()) {
      wait_threads_.resize(configs_.size());
    }

    nodes_[index] = std::make_shared<RaftNode>(configs_[index], snapshot_configs_[index]);
    nodes_[index]->Start();
    const auto node = nodes_[index];
    wait_threads_[index] = std::thread([node]() { node->Wait(); });
  }

  const std::vector<std::shared_ptr<RaftNode>>& Nodes() const { return nodes_; }

 private:
  std::vector<NodeConfig> configs_;
  std::vector<snapshotConfig> snapshot_configs_;
  std::vector<std::shared_ptr<RaftNode>> nodes_;
  std::vector<std::thread> wait_threads_;
};

bool IsExcluded(std::size_t index, const std::vector<std::size_t>& excluded) {
  for (std::size_t excluded_index : excluded) {
    if (index == excluded_index) {
      return true;
    }
  }
  return false;
}

std::shared_ptr<RaftNode> WaitForSingleLeader(
    const std::vector<std::shared_ptr<RaftNode>>& nodes,
    std::chrono::milliseconds timeout,
    const std::vector<std::size_t>& excluded = {}) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    std::shared_ptr<RaftNode> leader;
    int leader_count = 0;

    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (IsExcluded(i, excluded) || !nodes[i]) {
        continue;
      }
      if (IsLeaderNode(nodes[i])) {
        leader = nodes[i];
        ++leader_count;
      }
    }

    if (leader_count == 1) {
      return leader;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return nullptr;
}

std::size_t FindNodeIndex(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                          const std::shared_ptr<RaftNode>& target) {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i] == target) {
      return i;
    }
  }
  return nodes.size();
}

std::size_t PickFollowerIndex(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                              const std::shared_ptr<RaftNode>& leader) {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i] && nodes[i] != leader) {
      return i;
    }
  }
  return nodes.size();
}

struct StableLeaderObservation {
  std::shared_ptr<RaftNode> leader;
  std::size_t leader_index{0};
  int stable_observations{0};
  std::string diagnostics;
};

std::optional<StableLeaderObservation> WaitForStableLeader(
    const std::vector<std::shared_ptr<RaftNode>>& nodes,
    std::chrono::milliseconds timeout,
    const std::vector<std::size_t>& excluded = {},
    int required_observations = 3) {
  const auto deadline = Clock::now() + timeout;
  std::size_t last_leader_index = nodes.size();
  int stable_observations = 0;
  int last_leader_count = 0;
  std::string last_cluster_state;

  while (Clock::now() < deadline) {
    std::shared_ptr<RaftNode> leader;
    std::size_t leader_index = nodes.size();
    int leader_count = 0;

    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (IsExcluded(i, excluded) || !nodes[i]) {
        continue;
      }
      if (IsLeaderNode(nodes[i])) {
        leader = nodes[i];
        leader_index = i;
        ++leader_count;
      }
    }

    last_leader_count = leader_count;
    last_cluster_state = DescribeCluster(nodes, excluded);

    if (leader_count == 1) {
      if (leader_index == last_leader_index) {
        ++stable_observations;
      } else {
        last_leader_index = leader_index;
        stable_observations = 1;
      }

      if (stable_observations >= required_observations) {
        StableLeaderObservation observation;
        observation.leader = leader;
        observation.leader_index = leader_index;
        observation.stable_observations = stable_observations;
        observation.diagnostics =
            "stable leader_index=" + std::to_string(leader_index) +
            ", observations=" + std::to_string(stable_observations) +
            ", leader=" + leader->Describe() +
            ", cluster=" + last_cluster_state;
        return observation;
      }
    } else {
      last_leader_index = nodes.size();
      stable_observations = 0;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  StableLeaderObservation observation;
  observation.leader_index = last_leader_index;
  observation.stable_observations = stable_observations;
  observation.diagnostics =
      "leader did not stabilize, last_leader_count=" +
      std::to_string(last_leader_count) +
      ", last_leader_index=" + std::to_string(last_leader_index) +
      ", stable_observations=" + std::to_string(stable_observations) +
      ", cluster=" + last_cluster_state;
  return std::nullopt;
}

bool WaitForValueOnNode(const std::shared_ptr<RaftNode>& node,
                        const std::string& key,
                        const std::string& expected_value,
                        std::chrono::milliseconds timeout,
                        std::string* diagnostics = nullptr) {
  const auto deadline = Clock::now() + timeout;
  std::string last_value;
  std::string last_describe = node ? node->Describe() : "null";
  bool saw_value = false;
  while (Clock::now() < deadline) {
    if (node) {
      std::string value;
      if (node->DebugGetValue(key, &value) && value == expected_value) {
        if (diagnostics != nullptr) {
          *diagnostics = "value observed, key=" + key + ", value=" + value +
                         ", describe=" + node->Describe();
        }
        return true;
      }
      if (node->DebugGetValue(key, &value)) {
        saw_value = true;
        last_value = value;
      } else {
        saw_value = false;
        last_value.clear();
      }
      last_describe = node->Describe();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (diagnostics != nullptr) {
    *diagnostics = "value not observed, key=" + key +
                   ", expected=" + expected_value +
                   ", observed=" + (saw_value ? last_value : "<missing>") +
                   ", describe=" + last_describe;
  }
  return false;
}

bool WaitForValueOnAll(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                       const std::string& key,
                       const std::string& expected_value,
                       std::chrono::milliseconds timeout,
                       const std::vector<std::size_t>& excluded = {},
                       std::string* diagnostics = nullptr) {
  const auto deadline = Clock::now() + timeout;
  std::string last_cluster_state;
  while (Clock::now() < deadline) {
    bool all_match = true;
    std::ostringstream cluster_values;

    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (IsExcluded(i, excluded) || !nodes[i]) {
        continue;
      }

      std::string value;
      if (cluster_values.tellp() > 0) {
        cluster_values << " | ";
      }
      cluster_values << "node[" << i << "]=";
      if (!nodes[i]->DebugGetValue(key, &value) || value != expected_value) {
        if (nodes[i]->DebugGetValue(key, &value)) {
          cluster_values << value;
        } else {
          cluster_values << "<missing>";
        }
        all_match = false;
        break;
      }
      cluster_values << value;
    }

    last_cluster_state = cluster_values.str();

    if (all_match) {
      if (diagnostics != nullptr) {
        *diagnostics = "value observed on all nodes, key=" + key +
                       ", value=" + expected_value +
                       ", cluster=" + DescribeCluster(nodes, excluded);
      }
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (diagnostics != nullptr) {
    *diagnostics = "value not observed on all nodes, key=" + key +
                   ", expected=" + expected_value +
                   ", cluster_values=" + last_cluster_state +
                   ", cluster=" + DescribeCluster(nodes, excluded);
  }
  return false;
}

bool WaitForMissingOnAll(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                         const std::string& key,
                         std::chrono::milliseconds timeout,
                         const std::vector<std::size_t>& excluded = {},
                         std::string* diagnostics = nullptr) {
  const auto deadline = Clock::now() + timeout;
  std::string last_cluster_state;
  while (Clock::now() < deadline) {
    bool all_missing = true;
    std::ostringstream cluster_values;

    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (IsExcluded(i, excluded) || !nodes[i]) {
        continue;
      }

      std::string value;
      if (cluster_values.tellp() > 0) {
        cluster_values << " | ";
      }
      cluster_values << "node[" << i << "]=";
      if (nodes[i]->DebugGetValue(key, &value)) {
        cluster_values << value;
        all_missing = false;
        break;
      }
      cluster_values << "<missing>";
    }

    last_cluster_state = cluster_values.str();

    if (all_missing) {
      if (diagnostics != nullptr) {
        *diagnostics = "value missing on all nodes, key=" + key +
                       ", cluster=" + DescribeCluster(nodes, excluded);
      }
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (diagnostics != nullptr) {
    *diagnostics = "value still present on some node, key=" + key +
                   ", cluster_values=" + last_cluster_state +
                   ", cluster=" + DescribeCluster(nodes, excluded);
  }
  return false;
}

bool WaitForNodeFieldAtLeast(const std::shared_ptr<RaftNode>& node,
                             const std::string& field_name,
                             std::uint64_t minimum,
                             std::chrono::milliseconds timeout,
                             std::string* diagnostics = nullptr) {
  const auto deadline = Clock::now() + timeout;
  std::optional<std::uint64_t> last_value;
  std::string last_describe = node ? node->Describe() : "null";
  while (Clock::now() < deadline) {
    if (node) {
      last_describe = node->Describe();
      const auto value = ExtractUintField(last_describe, field_name);
      if (value.has_value() && *value >= minimum) {
        if (diagnostics != nullptr) {
          *diagnostics = "field reached threshold, field=" + field_name +
                         ", value=" + std::to_string(*value) +
                         ", minimum=" + std::to_string(minimum) +
                         ", describe=" + last_describe;
        }
        return true;
      }
      last_value = value;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (diagnostics != nullptr) {
    *diagnostics = "field did not reach threshold, field=" + field_name +
                   ", observed=" +
                   (last_value.has_value() ? std::to_string(*last_value)
                                           : std::string("<missing>")) +
                   ", minimum=" + std::to_string(minimum) +
                   ", describe=" + last_describe;
  }
  return false;
}

bool WaitForOrderedCommitApplyAtLeast(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                      std::uint64_t minimum_index,
                                      std::chrono::milliseconds timeout,
                                      std::string* diagnostics = nullptr,
                                      const std::vector<std::size_t>& excluded = {}) {
  const auto deadline = Clock::now() + timeout;
  std::string last_detail;

  while (Clock::now() < deadline) {
    bool all_ready = true;
    last_detail.clear();

    for (std::size_t i = 0; i < nodes.size(); ++i) {
      if (IsExcluded(i, excluded) || !nodes[i]) {
        continue;
      }

      const std::string describe = nodes[i]->Describe();
      const auto commit_index = ExtractUintField(describe, "commit_index");
      const auto last_applied = ExtractUintField(describe, "last_applied");
      if (!commit_index.has_value() || !last_applied.has_value()) {
        all_ready = false;
        last_detail = "missing commit/apply fields on node[" + std::to_string(i) +
                      "], describe=" + describe;
        continue;
      }
      if (*last_applied > *commit_index) {
        if (diagnostics != nullptr) {
          *diagnostics = "last_applied exceeded commit_index on node[" +
                         std::to_string(i) + "], describe=" + describe;
        }
        return false;
      }
      if (*commit_index < minimum_index || *last_applied < minimum_index) {
        all_ready = false;
        last_detail = "node[" + std::to_string(i) + "] not at replay frontier, describe=" +
                      describe;
      }
    }

    if (all_ready) {
      if (diagnostics != nullptr) {
        *diagnostics = "commit/apply reached replay frontier on all nodes, minimum_index=" +
                       std::to_string(minimum_index) +
                       ", cluster=" + DescribeCluster(nodes, excluded);
      }
      return true;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (diagnostics != nullptr) {
    *diagnostics = "timed out waiting for ordered commit/apply frontier, minimum_index=" +
                   std::to_string(minimum_index) +
                   ", detail=" + last_detail +
                   ", cluster=" + DescribeCluster(nodes, excluded);
  }
  return false;
}

bool ProposeWithRetry(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                      const Command& command,
                      std::chrono::milliseconds timeout,
                      ProposeResult* final_result,
                      const std::vector<std::size_t>& excluded = {},
                      std::string* diagnostics = nullptr) {
  const auto deadline = Clock::now() + timeout;
  ProposeResult last_result;
  std::size_t last_leader_index = nodes.size();
  std::string last_leader_describe = "none";
  std::string last_cluster_state = DescribeCluster(nodes, excluded);
  int attempts = 0;
  int no_stable_leader_rounds = 0;

  while (Clock::now() < deadline) {
    auto stable_leader =
        WaitForStableLeader(nodes, std::chrono::milliseconds(1500), excluded);
    if (!stable_leader.has_value()) {
      ++no_stable_leader_rounds;
      last_cluster_state = DescribeCluster(nodes, excluded);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    ++attempts;
    last_leader_index = stable_leader->leader_index;
    last_leader_describe = stable_leader->leader->Describe();
    last_cluster_state = DescribeCluster(nodes, excluded);

    last_result = stable_leader->leader->Propose(command);
    if (last_result.Ok()) {
      if (final_result != nullptr) {
        *final_result = last_result;
      }
      if (diagnostics != nullptr) {
        *diagnostics = "proposal committed, attempts=" + std::to_string(attempts) +
                       ", leader_index=" + std::to_string(last_leader_index) +
                       ", leader=" + last_leader_describe +
                       ", cluster=" + last_cluster_state;
      }
      return true;
    }

    if (last_result.status == ProposeStatus::kInvalidCommand ||
        last_result.status == ProposeStatus::kApplyFailed ||
        last_result.status == ProposeStatus::kCommitFailed) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (final_result != nullptr) {
    *final_result = last_result;
  }
  if (diagnostics != nullptr) {
    std::string category = "proposal_failure";
    if (no_stable_leader_rounds > 0 && attempts == 0) {
      category = "leader_not_stable_before_propose";
    } else if (last_result.status == ProposeStatus::kNotLeader) {
      category = "leadership_churn_during_propose";
    } else if (last_result.status == ProposeStatus::kReplicationFailed ||
               last_result.status == ProposeStatus::kTimeout ||
               last_result.status == ProposeStatus::kCommitFailed) {
      category = "proposal_failed_before_majority_or_commit";
    } else if (last_result.status == ProposeStatus::kApplyFailed) {
      category = "proposal_committed_but_apply_failed";
    } else if (last_result.status == ProposeStatus::kInvalidCommand) {
      category = "invalid_command";
    } else if (last_result.status == ProposeStatus::kNodeStopping) {
      category = "node_stopping";
    }

    *diagnostics =
        "category=" + category +
        ", attempts=" + std::to_string(attempts) +
        ", no_stable_leader_rounds=" + std::to_string(no_stable_leader_rounds) +
        ", last_leader_index=" + std::to_string(last_leader_index) +
        ", last_leader=" + last_leader_describe +
        ", last_status=" + ProposeStatusName(last_result.status) +
        ", last_message=" + last_result.message +
        ", cluster=" + last_cluster_state;
  }
  return false;
}

void WriteManyValues(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                     const std::string& prefix,
                     int count,
                     const std::vector<std::size_t>& excluded = {}) {
  ProposeResult result;
  for (int i = 0; i < count; ++i) {
    std::string diagnostics;
    SCOPED_TRACE(prefix + " write " + std::to_string(i));
    ASSERT_TRUE(ProposeWithRetry(nodes,
                                 SetCommand(prefix + "_" + std::to_string(i),
                                            "value_" + std::to_string(i)),
                                 std::chrono::seconds(10),
                                 &result,
                                 excluded,
                                 &diagnostics))
        << "write failed, status=" << ProposeStatusName(result.status)
        << ", message=" << result.message
        << ", diagnostics=" << diagnostics;
  }
}

class RaftSnapshotRestartTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    test_name_ = std::string(test_info->test_suite_name()) + "." + test_info->name();

    root_ = MakeTestRoot(test_name_);
    data_root_ = root_ / "raft_data";
    snapshot_root_ = root_ / "raft_snapshots";
    base_port_ = PickBasePort(test_name_);

    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
    std::filesystem::create_directories(data_root_, ec);
    ASSERT_FALSE(ec) << "failed to create data root: " << ec.message();

    std::filesystem::create_directories(snapshot_root_, ec);
    ASSERT_FALSE(ec) << "failed to create snapshot root: " << ec.message();

    RecordProperty("test_root", root_.string());
    RecordProperty("base_port", std::to_string(base_port_));
  }

  void TearDown() override {
    std::error_code ec;
    if (!HasFailure()) {
      std::filesystem::remove_all(root_, ec);
    } else {
      std::cout << "preserved test root: " << root_.string() << "\n";
    }
  }

  TestCluster MakeCluster(const std::string& case_name,
                          bool snapshot_enabled,
                          std::uint64_t snapshot_log_threshold) const {
    return TestCluster(BuildThreeNodeConfigs(data_root_ / case_name, base_port_),
                       BuildThreeSnapshotConfigs(snapshot_root_ / case_name,
                                                 snapshot_enabled,
                                                 snapshot_log_threshold));
  }

  std::string test_name_;
  std::filesystem::path root_;
  std::filesystem::path data_root_;
  std::filesystem::path snapshot_root_;
  int base_port_{0};
};

class RaftSnapshotRecoveryTest : public RaftSnapshotRestartTest {};

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

  WriteManyValues(cluster.Nodes(), "snapshot_base", 36);

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader after snapshot_base writes";
  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index", 12,
                                      std::chrono::seconds(20)))
      << "leader did not create baseline snapshot, describe="
      << cluster.Nodes()[leader_index]->Describe();

  WriteManyValues(cluster.Nodes(), "tail_log", 5);

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "tail_log_4", "value_4",
                                std::chrono::seconds(15)))
      << "cluster did not apply post-snapshot tail logs";

  cluster.StopAll();
  cluster.Start();

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(10));
  ASSERT_NE(leader, nullptr) << "no leader after restarting snapshot + tail log cluster";

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "snapshot_base_35", "value_35",
                                std::chrono::seconds(20)))
      << "snapshot-covered data was not restored after restart";

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "tail_log_4", "value_4",
                                std::chrono::seconds(20)))
      << "post-snapshot log data was not restored after restart";

  ProposeResult result;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(), SetCommand("after_tail_restart", "ok"),
                               std::chrono::seconds(15), &result))
      << "write after snapshot + tail restart failed, status="
      << ProposeStatusName(result.status) << ", message=" << result.message;

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(), "after_tail_restart", "ok",
                                std::chrono::seconds(20)))
      << "cluster did not continue after restoring snapshot + tail logs";
}

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
