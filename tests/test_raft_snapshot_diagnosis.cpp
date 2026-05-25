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

#include "metadata_raft_test_utils.h"
#include "raft/common/config.h"
#include "raft/common/propose.h"
#include "raft/node/raft_node.h"

namespace raftdemo {
namespace {

using Clock = std::chrono::steady_clock;
using test::ExpectedRecoveredMetadataObject;
using test::MetadataRecoveryExpectation;

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

constexpr const char* kDiagnosisBucket = "snapshot-diagnosis-bucket";

std::string ObjectKey(const std::string& prefix, const int index) {
  return prefix + "_" + std::to_string(index);
}

std::string ObjectId(const std::string& prefix, const int index) {
  return prefix + "-object-" + std::to_string(index);
}

ExpectedRecoveredMetadataObject MakeCommittedObjectExpectation(
    const std::string& prefix,
    const int index) {
  return ExpectedRecoveredMetadataObject{
      ObjectKey(prefix, index),
      ObjectId(prefix, index),
      2,
      false,
  };
}

ExpectedRecoveredMetadataObject MakeDeletedObjectExpectation(
    const std::string& object_key,
    const std::string& object_id) {
  return ExpectedRecoveredMetadataObject{
      object_key,
      object_id,
      0,
      true,
  };
}

MetadataRecoveryExpectation MakeBaseExpectation() {
  MetadataRecoveryExpectation expectation;
  expectation.bucket = kDiagnosisBucket;
  expectation.expected_request_count = 1;
  expectation.expected_last_applied_index = 1;
  expectation.expected_min_last_applied_term = 1;
  return expectation;
}

void AddCommittedObjects(MetadataRecoveryExpectation* expectation,
                         const std::string& prefix,
                         const int count) {
  ASSERT_NE(expectation, nullptr);
  for (int i = 0; i < count; ++i) {
    expectation->objects.push_back(MakeCommittedObjectExpectation(prefix, i));
    expectation->visible_keys.push_back(ObjectKey(prefix, i));
  }
  expectation->expected_request_count += static_cast<std::size_t>(count) * 2;
  expectation->expected_last_applied_index += static_cast<std::uint64_t>(count) * 2;
  std::sort(expectation->visible_keys.begin(), expectation->visible_keys.end());
}

void AddDeletedObject(MetadataRecoveryExpectation* expectation,
                      const std::string& object_key,
                      const std::string& object_id) {
  ASSERT_NE(expectation, nullptr);
  expectation->objects.push_back(MakeDeletedObjectExpectation(object_key, object_id));
  expectation->expected_request_count += 3;
  expectation->expected_tombstone_count += 1;
  expectation->expected_last_applied_index += 3;
}

std::string DescribeMetadataOnAllNodes(
    const std::vector<std::shared_ptr<RaftNode>>& nodes,
    const MetadataRecoveryExpectation& expectation) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    oss << "node[" << i << "] ";
    if (!nodes[i]) {
      oss << "<not running>\n";
      continue;
    }

    oss << nodes[i]->Describe();
    const MetadataStateMachine* state_machine = nodes[i]->GetMetadataStateMachineV2();
    if (state_machine == nullptr) {
      oss << " | metadata_sm=<null>\n";
      continue;
    }

    oss << " | requests=" << state_machine->RequestCount()
        << " tombstones=" << state_machine->TombstoneCount()
        << " last_applied_index=" << state_machine->LastAppliedIndex()
        << " last_applied_term=" << state_machine->LastAppliedTerm();

    const auto listed = state_machine->ListObjects(
        {.bucket = expectation.bucket, .prefix = "", .limit = std::nullopt, .continuation_token = ""});
    oss << " | visible=";
    if (!listed.result.Ok()) {
      oss << "<list-failed:" << static_cast<int>(listed.result.code) << ">";
    } else {
      oss << "[";
      for (std::size_t key_index = 0; key_index < listed.records.size(); ++key_index) {
        if (key_index > 0) {
          oss << ",";
        }
        oss << listed.records[key_index].object_key;
      }
      oss << "]";
    }

    for (const auto& object : expectation.objects) {
      const auto response = state_machine->HeadObject(
          {.bucket = expectation.bucket, .object_key = object.object_key});
      const auto internal_record =
          state_machine->FindObject(expectation.bucket, object.object_key);
      const auto indexed_object_id =
          state_machine->FindIndexedObjectId(expectation.bucket, object.object_key);
      const auto chunk_refs =
          state_machine->FindChunkRefs(expectation.bucket, object.object_key);
      oss << " | object=" << object.object_key << ":head="
          << static_cast<int>(response.result.code);
      if (response.record.has_value()) {
        oss << "/" << response.record->object_id;
      }
      if (internal_record.has_value()) {
        oss << ",state=" << static_cast<int>(internal_record->state)
            << ",internal_id=" << internal_record->object_id;
      } else {
        oss << ",state=<none>";
      }
      oss << ",indexed=";
      if (indexed_object_id.has_value()) {
        oss << *indexed_object_id;
      } else {
        oss << "<none>";
      }
      oss << ",chunks=";
      if (chunk_refs.has_value()) {
        oss << chunk_refs->size();
      } else {
        oss << "<none>";
      }
    }
    oss << '\n';
  }
  return oss.str();
}

void AssertExactMetadataFacts(const std::shared_ptr<RaftNode>& node,
                              const MetadataRecoveryExpectation& expectation) {
  ASSERT_NE(node, nullptr);
  const MetadataStateMachine* state_machine = node->GetMetadataStateMachineV2();
  ASSERT_NE(state_machine, nullptr) << node->Describe();
  EXPECT_EQ(state_machine->RequestCount(), expectation.expected_request_count)
      << node->Describe();
  EXPECT_EQ(state_machine->TombstoneCount(), expectation.expected_tombstone_count)
      << node->Describe();
  EXPECT_GE(state_machine->LastAppliedIndex(), expectation.expected_last_applied_index)
      << node->Describe();
  ASSERT_TRUE(expectation.expected_min_last_applied_term.has_value());
  EXPECT_GE(state_machine->LastAppliedTerm(),
            *expectation.expected_min_last_applied_term)
      << node->Describe();
  const auto parse_uint_field = [](const std::string& text,
                                   const std::string& field_name)
      -> std::optional<std::uint64_t> {
    const std::string prefix = field_name;
    const std::size_t begin = text.find(prefix);
    if (begin == std::string::npos) {
      return std::nullopt;
    }
    std::size_t pos = begin + prefix.size();
    std::size_t end = pos;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
      ++end;
    }
    if (end == pos) {
      return std::nullopt;
    }
    try {
      return static_cast<std::uint64_t>(std::stoull(text.substr(pos, end - pos)));
    } catch (...) {
      return std::nullopt;
    }
  };
  const auto node_last_applied =
      parse_uint_field(node->Describe(), "last_applied=");
  ASSERT_TRUE(node_last_applied.has_value()) << node->Describe();
  EXPECT_EQ(*node_last_applied, state_machine->LastAppliedIndex())
      << node->Describe();
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
  const std::string name = "sd_" + std::to_string(NowForPath()) + "_" +
                           std::to_string(rd());
  return std::filesystem::temp_directory_path() / "rq_sd" / name;
#else
  const std::string name = "raft_snapshot_diagnosis_" + safe_name + "_" +
                           std::to_string(NowForPath()) + "_" +
                           std::to_string(rd());
  std::filesystem::path base_dir;
  if (const char* env = std::getenv("RAFT_TEST_OUTPUT_DIR")) {
    base_dir = env;
  } else {
    // CTest/gtest_discover_tests runs test executables from the build/tests
    // directory by default, so this keeps raft_data and raft_snapshots under build.
    base_dir = std::filesystem::current_path() / "raft_test_data";
  }

  return base_dir / name;
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

std::string FormatSnapshotIndex(std::uint64_t index) {
  std::ostringstream oss;
  oss << std::setw(20) << std::setfill('0') << index;
  return oss.str();
}

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

  NodeConfig n2 = n1;
  n2.node_id = 2;
  n2.address = "127.0.0.1:" + std::to_string(base_port + 2);
  n2.peers = {
      PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
      PeerConfig{3, "127.0.0.1:" + std::to_string(base_port + 3)},
  };
  n2.data_dir = (data_root / "node_2").string();

  NodeConfig n3 = n1;
  n3.node_id = 3;
  n3.address = "127.0.0.1:" + std::to_string(base_port + 3);
  n3.peers = {
      PeerConfig{1, "127.0.0.1:" + std::to_string(base_port + 1)},
      PeerConfig{2, "127.0.0.1:" + std::to_string(base_port + 2)},
  };
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

  void StartOnly(std::size_t index) {
    StopAll();
    nodes_.assign(configs_.size(), nullptr);
    wait_threads_.clear();
    wait_threads_.resize(configs_.size());

    ASSERT_LT(index, configs_.size());
    nodes_[index] = std::make_shared<RaftNode>(configs_[index], snapshot_configs_[index]);
    nodes_[index]->Start();
    const auto node = nodes_[index];
    wait_threads_[index] = std::thread([node]() { node->Wait(); });
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

std::string DescribeAllNodes(const std::vector<std::shared_ptr<RaftNode>>& nodes) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    oss << "node[" << i << "] ";
    if (nodes[i]) {
      oss << nodes[i]->Describe();
    } else {
      oss << "<not running>";
    }
    oss << '\n';
  }
  return oss.str();
}

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

bool WaitForNodeFieldAtLeast(const std::shared_ptr<RaftNode>& node,
                             const std::string& field_name,
                             std::uint64_t minimum,
                             std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (node) {
      const auto value = ExtractUintField(node->Describe(), field_name);
      if (value.has_value() && *value >= minimum) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool ProposeWithRetry(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                      const MetadataCommand& command,
                      std::chrono::milliseconds timeout,
                      ProposeResult* final_result,
                      const std::vector<std::size_t>& excluded = {}) {
  return test::ProposeMetadataCommandWithRetry(
      nodes,
      command,
      timeout,
      final_result,
      excluded);
}

void CreateBucketOrFail(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                        const std::string& request_id,
                        const std::vector<std::size_t>& excluded = {}) {
  ProposeResult result;
  ASSERT_TRUE(test::ProposeCreateBucketWithRetry(
      nodes,
      kDiagnosisBucket,
      request_id,
      std::chrono::seconds(10),
      &result,
      excluded))
      << "create bucket failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;
}

void WriteManyCommittedObjects(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                               const std::string& prefix,
                               int count,
                               const std::vector<std::size_t>& excluded = {}) {
  ProposeResult result;
  for (int i = 0; i < count; ++i) {
    SCOPED_TRACE(prefix + " write " + std::to_string(i));
    ASSERT_TRUE(test::ProposeCreateCommitObjectWithRetry(
        nodes,
        kDiagnosisBucket,
        ObjectKey(prefix, i),
        ObjectId(prefix, i),
        prefix + "-create-request-" + std::to_string(i),
        prefix + "-commit-request-" + std::to_string(i),
        std::chrono::seconds(10),
        &result,
        excluded))
        << "write failed, status=" << ProposeStatusName(result.status)
        << ", message=" << result.message;
  }
}

void CreateCommitDeleteObject(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                              const std::string& object_key,
                              const std::string& object_id,
                              const std::string& request_prefix,
                              const std::vector<std::size_t>& excluded = {}) {
  ProposeResult result;
  ASSERT_TRUE(test::ProposeCreateCommitObjectWithRetry(
      nodes,
      kDiagnosisBucket,
      object_key,
      object_id,
      request_prefix + "-create",
      request_prefix + "-commit",
      std::chrono::seconds(10),
      &result,
      excluded))
      << "create+commit before delete failed, status="
      << ProposeStatusName(result.status) << ", message=" << result.message;

  ASSERT_TRUE(ProposeWithRetry(
      nodes,
      test::MakeDeleteObjectCommand(
          kDiagnosisBucket,
          object_key,
          object_id,
          request_prefix + "-delete"),
      std::chrono::seconds(10),
      &result,
      excluded))
      << "delete failed, status=" << ProposeStatusName(result.status)
      << ", message=" << result.message;
}

class RaftSnapshotDiagnosisTest : public ::testing::Test {
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
    const bool keep_data = std::getenv("RAFT_TEST_KEEP_DATA") != nullptr;
    if (!HasFailure() && !keep_data) {
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

TEST_F(RaftSnapshotDiagnosisTest, RestartedSingleNodeLoadsSnapshotAndTailLogsWithoutPeers) {
  auto cluster = MakeCluster("local_recovery_snapshot_tail", true, 12);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected\n" << DescribeAllNodes(cluster.Nodes());

  CreateBucketOrFail(cluster.Nodes(), "recovery-bucket");

  MetadataRecoveryExpectation expectation = MakeBaseExpectation();
  WriteManyCommittedObjects(cluster.Nodes(), "recovery_base", 30);
  AddCommittedObjects(&expectation, "recovery_base", 30);
  CreateCommitDeleteObject(
      cluster.Nodes(),
      "recovery_deleted",
      "recovery-deleted-object",
      "recovery-deleted");
  AddDeletedObject(&expectation, "recovery_deleted", "recovery-deleted-object");

  ASSERT_TRUE(test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expectation, std::chrono::seconds(15)))
      << DescribeMetadataOnAllNodes(cluster.Nodes(), expectation);

  const std::size_t target_index = 0;
  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[target_index],
                                      "last_snapshot_index",
                                      12,
                                      std::chrono::seconds(20)))
      << "target node did not create a snapshot before tail logs\n"
      << DescribeAllNodes(cluster.Nodes());

  WriteManyCommittedObjects(cluster.Nodes(), "recovery_tail", 3);
  AddCommittedObjects(&expectation, "recovery_tail", 3);

  ASSERT_TRUE(test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expectation, std::chrono::seconds(15)))
      << DescribeMetadataOnAllNodes(cluster.Nodes(), expectation);

  cluster.StopAll();
  cluster.StartOnly(target_index);

  ASSERT_TRUE(test::WaitUntilAllMetadataRecoveryMatches(
      std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[target_index]},
      expectation,
      std::chrono::seconds(3)))
      << "snapshot-covered metadata or post-snapshot tail metadata was not restored. "
      << "Suspect startup snapshot loading or replay boundary in raft_node.cpp.\n"
      << DescribeMetadataOnAllNodes(
             std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[target_index]},
             expectation);
  AssertExactMetadataFacts(cluster.Nodes()[target_index], expectation);

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[target_index],
                                      "last_snapshot_index",
                                      12,
                                      std::chrono::seconds(3)))
      << "snapshot metadata was lost after local restart. "
      << "Suspect snapshot metadata loading in raft_node.cpp/snapshot_storage.cpp.\n"
      << DescribeAllNodes(cluster.Nodes());
}

TEST_F(RaftSnapshotDiagnosisTest, CompactedClusterReplicatesNewLogAfterRestartedLeaderStepsDown) {
  auto cluster = MakeCluster("replication_after_compacted_restart", true, 6);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected\n" << DescribeAllNodes(cluster.Nodes());

  CreateBucketOrFail(cluster.Nodes(), "replication-bucket");
  MetadataRecoveryExpectation expectation = MakeBaseExpectation();
  WriteManyCommittedObjects(cluster.Nodes(), "replication_base", 32);
  AddCommittedObjects(&expectation, "replication_base", 32);
  CreateCommitDeleteObject(
      cluster.Nodes(),
      "replication_deleted",
      "replication-deleted-object",
      "replication-deleted");
  AddDeletedObject(&expectation, "replication_deleted", "replication-deleted-object");

  ASSERT_TRUE(test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expectation, std::chrono::seconds(15)))
      << DescribeMetadataOnAllNodes(cluster.Nodes(), expectation);

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader after writes\n" << DescribeAllNodes(cluster.Nodes());
  const std::size_t restarted_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(restarted_index, cluster.Nodes().size()) << "failed to locate leader";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[restarted_index],
                                      "last_snapshot_index",
                                      6,
                                      std::chrono::seconds(20)))
      << "leader did not compact through snapshot before restart\n"
      << DescribeAllNodes(cluster.Nodes());

  cluster.RestartNode(restarted_index);

  ASSERT_TRUE(test::WaitUntilAllMetadataRecoveryMatches(
      std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[restarted_index]},
      expectation,
      std::chrono::seconds(10)))
      << "restarted compacted node failed local recovery. "
      << "Run RestartedSingleNodeLoadsSnapshotAndTailLogsWithoutPeers first; "
      << "suspect raft_node.cpp startup recovery or raft_storage.cpp.\n"
      << DescribeMetadataOnAllNodes(
             std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[restarted_index]},
             expectation);
  AssertExactMetadataFacts(cluster.Nodes()[restarted_index], expectation);

  ProposeResult result;
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(),
                               test::MakeCreateObjectCommand(
                                   kDiagnosisBucket,
                                   "diagnosis_after_restart",
                                   "diagnosis-after-restart-object",
                                   "diagnosis-after-restart-create"),
                               std::chrono::seconds(15),
                               &result))
      << "new object-create after compacted restart failed, status="
      << ProposeStatusName(result.status) << ", message=" << result.message
      << "\n" << DescribeAllNodes(cluster.Nodes());
  ASSERT_TRUE(ProposeWithRetry(cluster.Nodes(),
                               test::MakeCommitObjectCommand(
                                   kDiagnosisBucket,
                                   "diagnosis_after_restart",
                                   "diagnosis-after-restart-object",
                                   "diagnosis-after-restart-commit"),
                               std::chrono::seconds(15),
                               &result))
      << "new object-commit after compacted restart failed, status="
      << ProposeStatusName(result.status) << ", message=" << result.message
      << "\n" << DescribeAllNodes(cluster.Nodes());
  expectation.objects.push_back(ExpectedRecoveredMetadataObject{
      "diagnosis_after_restart",
      "diagnosis-after-restart-object",
      2,
      false,
  });
  expectation.visible_keys.push_back("diagnosis_after_restart");
  std::sort(expectation.visible_keys.begin(), expectation.visible_keys.end());
  expectation.expected_request_count += 2;
  expectation.expected_last_applied_index += 2;

  ASSERT_TRUE(test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expectation, std::chrono::seconds(20)))
      << "new committed metadata did not reach/apply on every node after compacted restart. "
      << "If the previous local recovery assertion passed, suspect raft_node.cpp "
      << "replication catch-up path: next_index initialization, compacted-log boundary, "
      << "AppendEntries prev_log_index/term, or InstallSnapshot handoff.\n"
      << DescribeMetadataOnAllNodes(cluster.Nodes(), expectation);

  std::optional<std::uint64_t> applied_term;
  for (const auto& node : cluster.Nodes()) {
    AssertExactMetadataFacts(node, expectation);
    const MetadataStateMachine* state_machine = node->GetMetadataStateMachineV2();
    ASSERT_NE(state_machine, nullptr);
    if (!applied_term.has_value()) {
      applied_term = state_machine->LastAppliedTerm();
    } else {
      EXPECT_EQ(state_machine->LastAppliedTerm(), *applied_term)
          << node->Describe();
    }
  }
}

TEST_F(RaftSnapshotDiagnosisTest,
       RestartedSingleNodeReplaysAppliedTailAfterRejectingCorruptedNewestSnapshot) {
  constexpr std::uint64_t kSnapshotThreshold = 20;
  const std::string case_name = "diagnosis_replay_after_corrupted_snapshot";
  auto cluster = MakeCluster(case_name, true, kSnapshotThreshold);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected\n" << DescribeAllNodes(cluster.Nodes());

  CreateBucketOrFail(cluster.Nodes(), "corrupted-bucket");
  MetadataRecoveryExpectation expectation = MakeBaseExpectation();
  WriteManyCommittedObjects(cluster.Nodes(), "corrupted_fallback", 45);
  AddCommittedObjects(&expectation, "corrupted_fallback", 45);
  CreateCommitDeleteObject(
      cluster.Nodes(),
      "corrupted_deleted",
      "corrupted-deleted-object",
      "corrupted-deleted");
  AddDeletedObject(&expectation, "corrupted_deleted", "corrupted-deleted-object");

  ASSERT_TRUE(test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expectation, std::chrono::seconds(20)))
      << DescribeMetadataOnAllNodes(cluster.Nodes(), expectation);

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader after baseline writes\n"
                             << DescribeAllNodes(cluster.Nodes());
  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index",
                                      40,
                                      std::chrono::seconds(20)))
      << "target node did not create enough snapshots before corrupt-latest restart test\n"
      << DescribeAllNodes(cluster.Nodes());

  cluster.StopAll();

  const std::filesystem::path node_snapshot_root =
      snapshot_root_ / case_name / ("node_" + std::to_string(leader_index + 1));
  const auto snapshot_dirs = ListSnapshotDirs(node_snapshot_root);
  ASSERT_GE(snapshot_dirs.size(), 2U) << "need at least two published snapshots under "
                                      << node_snapshot_root.string();

  const auto older_snapshot_index = SnapshotIndexFromDir(snapshot_dirs[snapshot_dirs.size() - 2]);
  const auto latest_snapshot_index = SnapshotIndexFromDir(snapshot_dirs.back());
  ASSERT_TRUE(older_snapshot_index.has_value());
  ASSERT_TRUE(latest_snapshot_index.has_value());
  ASSERT_GT(*latest_snapshot_index, *older_snapshot_index);

  WriteTextFile(snapshot_dirs.back() / "data.bin", "corrupted-newest-snapshot-data");

  cluster.StartOnly(leader_index);

  ASSERT_TRUE(test::WaitUntilAllCommittedObject(
      std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[leader_index]},
      kDiagnosisBucket,
      ObjectKey("corrupted_fallback", 5),
      ObjectId("corrupted_fallback", 5),
      2,
      60,
      std::chrono::seconds(5)))
      << "snapshot-covered metadata was not restored after rejecting the corrupted newest "
         "snapshot.\n"
      << DescribeMetadataOnAllNodes(
             std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[leader_index]},
             expectation);
  ASSERT_TRUE(test::WaitUntilAllCommittedObject(
      std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[leader_index]},
      kDiagnosisBucket,
      ObjectKey("corrupted_fallback", 44),
      ObjectId("corrupted_fallback", 44),
      2,
      80,
      std::chrono::seconds(5)))
      << "restart did not replay committed log entries after rejecting the corrupted newest "
         "snapshot. Suspect trusted snapshot fallback or startup log replay.\n"
      << DescribeMetadataOnAllNodes(
             std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[leader_index]},
             expectation);
  ASSERT_TRUE(test::WaitUntilAllDeletedObjectHidden(
      std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[leader_index]},
      kDiagnosisBucket,
      "corrupted_deleted",
      "corrupted-deleted-object",
      80,
      std::chrono::seconds(5)))
      << DescribeMetadataOnAllNodes(
             std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[leader_index]},
             expectation);
  const MetadataStateMachine* restarted_state_machine =
      cluster.Nodes()[leader_index]->GetMetadataStateMachineV2();
  ASSERT_NE(restarted_state_machine, nullptr)
      << cluster.Nodes()[leader_index]->Describe();
  EXPECT_GE(restarted_state_machine->RequestCount(), 74u)
      << cluster.Nodes()[leader_index]->Describe();
  EXPECT_EQ(restarted_state_machine->TombstoneCount(), 1u)
      << cluster.Nodes()[leader_index]->Describe();
  EXPECT_EQ(restarted_state_machine->LastAppliedIndex(), 95u)
      << cluster.Nodes()[leader_index]->Describe();
  EXPECT_GE(restarted_state_machine->LastAppliedTerm(), 1u)
      << cluster.Nodes()[leader_index]->Describe();

  const auto restored_snapshot_index =
      ExtractUintField(cluster.Nodes()[leader_index]->Describe(), "last_snapshot_index");
  ASSERT_TRUE(restored_snapshot_index.has_value())
      << cluster.Nodes()[leader_index]->Describe();
  EXPECT_GE(*restored_snapshot_index, *older_snapshot_index);
  EXPECT_LE(*restored_snapshot_index, *latest_snapshot_index)
      << "restart should not advance beyond the highest previously published snapshot boundary "
         "while replaying committed logs after rejecting a corrupted newest snapshot";
}

TEST_F(RaftSnapshotDiagnosisTest,
       RestartedSingleNodeSkipsMetadataMismatchedVisibleSnapshotAndReplaysCommittedTail) {
  constexpr std::uint64_t kSnapshotThreshold = 12;
  const std::string case_name = "diagnosis_replay_after_metadata_mismatch";
  auto cluster = MakeCluster(case_name, true, kSnapshotThreshold);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected\n" << DescribeAllNodes(cluster.Nodes());

  CreateBucketOrFail(cluster.Nodes(), "metadata-replay-bucket");
  MetadataRecoveryExpectation expectation = MakeBaseExpectation();
  WriteManyCommittedObjects(cluster.Nodes(), "metadata_replay", 30);
  AddCommittedObjects(&expectation, "metadata_replay", 30);
  CreateCommitDeleteObject(
      cluster.Nodes(),
      "metadata_replay_deleted",
      "metadata-replay-deleted-object",
      "metadata-replay-deleted");
  AddDeletedObject(&expectation, "metadata_replay_deleted", "metadata-replay-deleted-object");

  ASSERT_TRUE(test::WaitUntilAllMetadataRecoveryMatches(
      cluster.Nodes(), expectation, std::chrono::seconds(15)))
      << DescribeMetadataOnAllNodes(cluster.Nodes(), expectation);

  leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader after metadata replay writes\n"
                             << DescribeAllNodes(cluster.Nodes());
  const std::size_t leader_index = FindNodeIndex(cluster.Nodes(), leader);
  ASSERT_LT(leader_index, cluster.Nodes().size()) << "failed to locate leader";

  ASSERT_TRUE(WaitForNodeFieldAtLeast(cluster.Nodes()[leader_index],
                                      "last_snapshot_index",
                                      24,
                                      std::chrono::seconds(20)))
      << "target node did not create enough snapshots before metadata mismatch restart test\n"
      << DescribeAllNodes(cluster.Nodes());

  cluster.StopAll();

  const std::filesystem::path node_snapshot_root =
      snapshot_root_ / case_name / ("node_" + std::to_string(leader_index + 1));
  const auto snapshot_dirs = ListSnapshotDirs(node_snapshot_root);
  ASSERT_GE(snapshot_dirs.size(), 2U) << "need at least two published snapshots under "
                                      << node_snapshot_root.string();

  const auto latest_snapshot_index = SnapshotIndexFromDir(snapshot_dirs.back());
  ASSERT_TRUE(latest_snapshot_index.has_value()) << snapshot_dirs.back().string();
  const std::uint64_t mismatched_visible_index = *latest_snapshot_index + kSnapshotThreshold;
  const std::filesystem::path mismatched_visible_dir =
      node_snapshot_root / ("snapshot_" + FormatSnapshotIndex(mismatched_visible_index));
  CopyDirectoryRecursively(snapshot_dirs.back(), mismatched_visible_dir);

  cluster.StartOnly(leader_index);

  ASSERT_TRUE(test::WaitUntilAllMetadataRecoveryMatches(
      std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[leader_index]},
      expectation,
      std::chrono::seconds(5)))
      << "restart did not replay committed log tail after skipping metadata-mismatched visible "
         "snapshot. Suspect trusted snapshot selection or startup replay.\n"
      << DescribeMetadataOnAllNodes(
             std::vector<std::shared_ptr<RaftNode>>{cluster.Nodes()[leader_index]},
             expectation);
  AssertExactMetadataFacts(cluster.Nodes()[leader_index], expectation);

  const auto restored_snapshot_index =
      ExtractUintField(cluster.Nodes()[leader_index]->Describe(), "last_snapshot_index");
  ASSERT_TRUE(restored_snapshot_index.has_value())
      << cluster.Nodes()[leader_index]->Describe();
  EXPECT_EQ(*restored_snapshot_index, *latest_snapshot_index)
      << "restart should keep the real trusted snapshot boundary when a higher-index visible "
         "snapshot directory has mismatched metadata";
  EXPECT_LT(*restored_snapshot_index, mismatched_visible_index)
      << cluster.Nodes()[leader_index]->Describe();
}

}  // namespace
}  // namespace raftdemo
