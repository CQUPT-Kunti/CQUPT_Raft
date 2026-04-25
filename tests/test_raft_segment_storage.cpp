#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "raft/command.h"
#include "raft/config.h"
#include "raft/propose.h"
#include "raft/raft_node.h"
#include "raft/raft_storage.h"

namespace raftdemo {
namespace {

using Clock = std::chrono::steady_clock;

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

std::string SafeTestName(const ::testing::TestInfo* info) {
  std::string name = std::string(info->test_suite_name()) + "." + info->name();
  for (char& ch : name) {
    if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ') {
      ch = '_';
    }
  }
  return name;
}

std::filesystem::path MakeInspectableTestRoot(const ::testing::TestInfo* info) {
  std::random_device rd;
  const std::string dir_name = SafeTestName(info) + "_" +
                               std::to_string(NowForPath()) + "_" +
                               std::to_string(rd());
  return TestBinaryDir() / "raft_test_data" / "segment_storage" / dir_name;
}

bool KeepTestData() {
  const char* value = std::getenv("RAFT_TEST_KEEP_DATA");
  if (value == nullptr) {
    // Segment storage tests intentionally preserve data by default so the generated
    // meta.bin, segment logs and snapshots can be inspected under build/tests.
    return true;
  }
  return std::string(value) != "0";
}

std::size_t CountFilesWithExtension(const std::filesystem::path& dir,
                                    const std::string& extension) {
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    return 0;
  }

  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      return count;
    }
    if (entry.is_regular_file() && entry.path().extension() == extension) {
      ++count;
    }
  }
  return count;
}

std::vector<std::filesystem::path> ListFilesWithExtension(const std::filesystem::path& dir,
                                                          const std::string& extension) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    return files;
  }

  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    if (entry.is_regular_file() && entry.path().extension() == extension) {
      files.push_back(entry.path());
    }
  }

  std::sort(files.begin(), files.end());
  return files;
}

std::size_t CountSegmentFiles(const std::filesystem::path& data_dir) {
  return CountFilesWithExtension(data_dir / "log", ".log");
}

std::string DescribeDirectoryTree(const std::filesystem::path& root) {
  std::ostringstream oss;
  oss << "test root: " << root.string() << '\n';

  std::error_code ec;
  if (!std::filesystem::exists(root, ec)) {
    oss << "<missing>\n";
    return oss.str();
  }

  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    paths.push_back(entry.path());
  }
  std::sort(paths.begin(), paths.end());

  for (const auto& path : paths) {
    oss << "  " << std::filesystem::relative(path, root, ec).string();
    if (std::filesystem::is_regular_file(path, ec)) {
      oss << " (" << std::filesystem::file_size(path, ec) << " bytes)";
    }
    oss << '\n';
  }
  return oss.str();
}

PersistentRaftState MakeState(std::uint64_t first_index, std::uint64_t last_index) {
  PersistentRaftState state;
  state.current_term = 7;
  state.voted_for = 2;
  state.commit_index = last_index;
  state.last_applied = last_index;

  for (std::uint64_t index = first_index; index <= last_index; ++index) {
    state.log.push_back(LogRecord{index,
                                  7,
                                  "SET|storage_key_" + std::to_string(index) +
                                      "|storage_value_" + std::to_string(index)});
  }
  return state;
}

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
  return node != nullptr && Contains(node->Describe(), "role=Leader");
}

Command SetCommand(const std::string& key, const std::string& value) {
  Command command;
  command.type = CommandType::kSet;
  command.key = key;
  command.value = value;
  return command;
}

int PickBasePort(const std::string& test_name) {
  const int name_offset = static_cast<int>(std::hash<std::string>{}(test_name) % 2000);
  if (const char* env = std::getenv("RAFT_TEST_BASE_PORT")) {
    try {
      return std::stoi(env) + name_offset;
    } catch (...) {
      // Fall through to generated port range.
    }
  }

  std::random_device rd;
  return 45000 + name_offset + static_cast<int>(rd() % 8000);
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
  n1.rpc_deadline = std::chrono::milliseconds(350);
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

std::vector<snapshotConfig> BuildThreeSnapshotConfigs(const std::filesystem::path& snapshot_root,
                                                      std::uint64_t log_threshold) {
  snapshotConfig s1;
  s1.enabled = true;
  s1.snapshot_dir = (snapshot_root / "node_1").string();
  s1.log_threshold = log_threshold;
  s1.snapshot_interval = std::chrono::minutes(10);
  // Keep several snapshots in tests so the generated files are easy to inspect.
  s1.max_snapshot_count = 8;
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

std::shared_ptr<RaftNode> WaitForSingleLeader(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                              std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    std::shared_ptr<RaftNode> leader;
    int leader_count = 0;

    for (const auto& node : nodes) {
      if (IsLeaderNode(node)) {
        leader = node;
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

bool WaitForValueOnAll(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                       const std::string& key,
                       const std::string& expected_value,
                       std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    bool all_match = true;
    for (const auto& node : nodes) {
      if (!node) {
        all_match = false;
        break;
      }
      std::string value;
      if (!node->DebugGetValue(key, &value) || value != expected_value) {
        all_match = false;
        break;
      }
    }
    if (all_match) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
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

std::size_t CountNodesWithSnapshotAtLeast(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                              std::uint64_t minimum) {
  std::size_t count = 0;
  for (const auto& node : nodes) {
    if (!node) {
      continue;
    }
    const auto value = ExtractUintField(node->Describe(), "last_snapshot_index");
    if (value.has_value() && *value >= minimum) {
      ++count;
    }
  }
  return count;
}

bool WaitForSnapshotOnAtLeastNodes(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                                   std::uint64_t minimum_snapshot_index,
                                   std::size_t minimum_node_count,
                                   std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    if (CountNodesWithSnapshotAtLeast(nodes, minimum_snapshot_index) >= minimum_node_count) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool ProposeWithRetry(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                      const Command& command,
                      std::chrono::milliseconds timeout,
                      ProposeResult* final_result) {
  const auto deadline = Clock::now() + timeout;
  ProposeResult last_result;

  while (Clock::now() < deadline) {
    auto leader = WaitForSingleLeader(nodes, std::chrono::milliseconds(1500));
    if (!leader) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    last_result = leader->Propose(command);
    if (last_result.Ok()) {
      if (final_result != nullptr) {
        *final_result = last_result;
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
  return false;
}

void WriteManyValues(const std::vector<std::shared_ptr<RaftNode>>& nodes,
                     const std::string& prefix,
                     int count) {
  ProposeResult result;
  for (int i = 0; i < count; ++i) {
    SCOPED_TRACE(prefix + " write " + std::to_string(i));
    ASSERT_TRUE(ProposeWithRetry(nodes,
                                 SetCommand(prefix + "_" + std::to_string(i),
                                            "value_" + std::to_string(i)),
                                 std::chrono::seconds(10),
                                 &result))
        << "write failed, status=" << ProposeStatusName(result.status)
        << ", message=" << result.message
        << "\n" << DescribeAllNodes(nodes);
  }
}

class RaftSegmentStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = MakeInspectableTestRoot(::testing::UnitTest::GetInstance()->current_test_info());
    data_root_ = root_ / "raft_data";
    snapshot_root_ = root_ / "raft_snapshots";
    base_port_ = PickBasePort(SafeTestName(::testing::UnitTest::GetInstance()->current_test_info()));

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
    std::cout << "segment storage test data: " << root_.string() << '\n';
    std::cout << DescribeDirectoryTree(root_);
    if (!KeepTestData()) {
      std::error_code ec;
      std::filesystem::remove_all(root_, ec);
    }
  }

  TestCluster MakeCluster(const std::string& case_name,
                          std::uint64_t snapshot_log_threshold) const {
    return TestCluster(BuildThreeNodeConfigs(data_root_ / case_name, base_port_),
                       BuildThreeSnapshotConfigs(snapshot_root_ / case_name,
                                                 snapshot_log_threshold));
  }

  std::filesystem::path root_;
  std::filesystem::path data_root_;
  std::filesystem::path snapshot_root_;
  int base_port_{0};
};

TEST_F(RaftSegmentStorageTest, WritesMultipleSegmentFilesUnderBuildDirectory) {
  const auto storage_root = data_root_ / "direct_storage_multiple_segments";
  auto storage = CreateFileRaftStorage(storage_root.string());
  auto state = MakeState(1, 1300);

  std::string error;
  ASSERT_TRUE(storage->Save(state, &error)) << error;

  EXPECT_TRUE(std::filesystem::exists(storage_root / "meta.bin"));
  EXPECT_TRUE(std::filesystem::exists(storage_root / "log"));
  EXPECT_FALSE(std::filesystem::exists(storage_root / "raft_state.bin"));
  EXPECT_GE(CountSegmentFiles(storage_root), 3U)
      << DescribeDirectoryTree(root_);

  PersistentRaftState loaded;
  bool has_state = false;
  ASSERT_TRUE(storage->Load(&loaded, &has_state, &error)) << error;
  ASSERT_TRUE(has_state);
  EXPECT_EQ(loaded.current_term, state.current_term);
  EXPECT_EQ(loaded.voted_for, state.voted_for);
  EXPECT_EQ(loaded.commit_index, state.commit_index);
  EXPECT_EQ(loaded.last_applied, state.last_applied);
  ASSERT_EQ(loaded.log.size(), state.log.size());
  EXPECT_EQ(loaded.log.front().index, 1U);
  EXPECT_EQ(loaded.log.back().index, 1300U);
  EXPECT_EQ(loaded.log[1023].command, "SET|storage_key_1024|storage_value_1024");
}

TEST_F(RaftSegmentStorageTest, AutomaticallyDeletesObsoleteSegmentsAfterCompactionSave) {
  const auto storage_root = data_root_ / "direct_storage_auto_delete";
  auto storage = CreateFileRaftStorage(storage_root.string());
  std::string error;

  ASSERT_TRUE(storage->Save(MakeState(1, 1300), &error)) << error;
  const std::size_t original_segments = CountSegmentFiles(storage_root);
  ASSERT_GE(original_segments, 3U) << DescribeDirectoryTree(root_);

  // Simulate RaftNode after snapshot compaction. Only records still present in
  // PersistentRaftState::log are allowed to remain on disk.
  auto compacted = MakeState(1000, 1300);
  compacted.log.front().command = "snapshot-boundary-entry";
  ASSERT_TRUE(storage->Save(compacted, &error)) << error;

  EXPECT_LT(CountSegmentFiles(storage_root), original_segments)
      << DescribeDirectoryTree(root_);
  EXPECT_FALSE(std::filesystem::exists(storage_root / "log" /
                                       "segment_00000000000000000001.log"))
      << DescribeDirectoryTree(root_);

  PersistentRaftState loaded;
  bool has_state = false;
  ASSERT_TRUE(storage->Load(&loaded, &has_state, &error)) << error;
  ASSERT_TRUE(has_state);
  ASSERT_FALSE(loaded.log.empty());
  EXPECT_EQ(loaded.log.front().index, 1000U);
  EXPECT_EQ(loaded.log.front().command, "snapshot-boundary-entry");
  EXPECT_EQ(loaded.log.back().index, 1300U);
  EXPECT_EQ(loaded.commit_index, 1300U);
  EXPECT_EQ(loaded.last_applied, 1300U);
}

TEST_F(RaftSegmentStorageTest, RaftClusterGeneratesManySnapshotsAndSegmentLogsUnderBuildDirectory) {
  // Use a deliberately small snapshot threshold so the test quickly produces
  // multiple snapshot files that can be inspected under build/tests/raft_test_data.
  constexpr std::uint64_t kSnapshotThreshold = 4;
  constexpr int kWriteCount = 160;
  constexpr std::uint64_t kExpectedSnapshotIndex = 80;

  auto cluster = MakeCluster("cluster_generated_many_snapshots_and_logs", kSnapshotThreshold);
  cluster.Start();

  auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
  ASSERT_NE(leader, nullptr) << "no leader elected\n" << DescribeAllNodes(cluster.Nodes());

  WriteManyValues(cluster.Nodes(), "segment_cluster_many", kWriteCount);

  ASSERT_TRUE(WaitForValueOnAll(cluster.Nodes(),
                                "segment_cluster_many_159",
                                "value_159",
                                std::chrono::seconds(20)))
      << DescribeAllNodes(cluster.Nodes());

  ASSERT_TRUE(WaitForSnapshotOnAtLeastNodes(cluster.Nodes(),
                                            kExpectedSnapshotIndex,
                                            2,
                                            std::chrono::seconds(25)))
      << "snapshot threshold was small but fewer than two nodes created high-index snapshots\n"
      << DescribeAllNodes(cluster.Nodes());

  cluster.StopAll();

  std::size_t nodes_with_meta = 0;
  std::size_t nodes_with_segment = 0;
  std::size_t nodes_with_snapshot_bin = 0;
  std::size_t nodes_with_snapshot_meta = 0;
  std::size_t total_segments = 0;
  std::size_t total_snapshot_bins = 0;
  std::size_t total_snapshot_metas = 0;

  for (int node_id = 1; node_id <= 3; ++node_id) {
    const auto data_dir = data_root_ / "cluster_generated_many_snapshots_and_logs" /
                          ("node_" + std::to_string(node_id));
    const auto snapshot_dir = snapshot_root_ / "cluster_generated_many_snapshots_and_logs" /
                              ("node_" + std::to_string(node_id));

    if (std::filesystem::exists(data_dir / "meta.bin")) {
      ++nodes_with_meta;
    }

    const std::size_t segment_count = CountSegmentFiles(data_dir);
    total_segments += segment_count;
    if (segment_count > 0) {
      ++nodes_with_segment;
    }

    const std::size_t snapshot_bin_count = CountFilesWithExtension(snapshot_dir, ".bin");
    const std::size_t snapshot_meta_count = CountFilesWithExtension(snapshot_dir, ".meta");
    total_snapshot_bins += snapshot_bin_count;
    total_snapshot_metas += snapshot_meta_count;
    if (snapshot_bin_count > 0) {
      ++nodes_with_snapshot_bin;
    }
    if (snapshot_meta_count > 0) {
      ++nodes_with_snapshot_meta;
    }
  }

  EXPECT_GE(nodes_with_meta, 2U) << DescribeDirectoryTree(root_);
  EXPECT_GE(nodes_with_segment, 2U) << DescribeDirectoryTree(root_);
  EXPECT_GE(nodes_with_snapshot_bin, 2U) << DescribeDirectoryTree(root_);
  EXPECT_GE(nodes_with_snapshot_meta, 2U) << DescribeDirectoryTree(root_);
  EXPECT_GE(total_segments, 2U) << DescribeDirectoryTree(root_);
  EXPECT_GE(total_snapshot_bins, 4U) << DescribeDirectoryTree(root_);
  EXPECT_GE(total_snapshot_metas, 4U) << DescribeDirectoryTree(root_);

  std::cout << "Generated raft data for inspection at: " << root_.string() << '\n';
  std::cout << DescribeDirectoryTree(root_);
}

}  // namespace
}  // namespace raftdemo
