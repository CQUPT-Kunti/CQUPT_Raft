#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
#include "raft/storage/raft_storage.h"

namespace raftdemo
{
  namespace
  {

    using Clock = std::chrono::steady_clock;

    std::filesystem::path TestBinaryDir()
    {
#ifdef RAFT_TEST_BINARY_DIR
      return std::filesystem::path(RAFT_TEST_BINARY_DIR);
#else
      return std::filesystem::current_path();
#endif
    }

    std::uint64_t NowForPath()
    {
      return static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count());
    }

    std::string SafeTestName(const ::testing::TestInfo *info)
    {
      std::string name = std::string(info->test_suite_name()) + "." + info->name();
      for (char &ch : name)
      {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ')
        {
          ch = '_';
        }
      }
      return name;
    }

    std::filesystem::path MakeInspectableTestRoot(const ::testing::TestInfo *info)
    {
      std::random_device rd;
#ifdef _WIN32
      const std::string dir_name = "ss_" + std::to_string(NowForPath()) + "_" +
                                   std::to_string(rd());
      return std::filesystem::temp_directory_path() / "rq_ss" / dir_name;
#else
      const std::string dir_name = SafeTestName(info) + "_" +
                                   std::to_string(NowForPath()) + "_" +
                                   std::to_string(rd());
      return TestBinaryDir() / "raft_test_data" / "segment_storage" / dir_name;
#endif
    }

    bool KeepTestData()
    {
      const char *value = std::getenv("RAFT_TEST_KEEP_DATA");
      if (value == nullptr)
      {
        // Segment storage tests intentionally preserve data by default so the generated
        // meta.bin, segment logs and snapshots can be inspected under build/tests.
        return true;
      }
      return std::string(value) != "0";
    }

    const char *ExpectedLinuxSpecificMarker()
    {
#if defined(__linux__)
      return "linux_specific=true";
#else
      return "linux_specific=false";
#endif
    }

    std::size_t CountFilesWithExtension(const std::filesystem::path &dir,
                                        const std::string &extension)
    {
      std::error_code ec;
      if (!std::filesystem::exists(dir, ec))
      {
        return 0;
      }

      std::size_t count = 0;
      for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
      {
        if (ec)
        {
          return count;
        }
        if (entry.is_regular_file() && entry.path().extension() == extension)
        {
          ++count;
        }
      }
      return count;
    }

    std::vector<std::filesystem::path> ListFilesWithExtension(const std::filesystem::path &dir,
                                                              const std::string &extension)
    {
      std::vector<std::filesystem::path> files;
      std::error_code ec;
      if (!std::filesystem::exists(dir, ec))
      {
        return files;
      }

      for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
      {
        if (ec)
        {
          break;
        }
        if (entry.is_regular_file() && entry.path().extension() == extension)
        {
          files.push_back(entry.path());
        }
      }

      std::sort(files.begin(), files.end());
      return files;
    }

    std::size_t CountSegmentFiles(const std::filesystem::path &data_dir)
    {
      return CountFilesWithExtension(data_dir / "log", ".log");
    }

    struct SnapshotFileCounts
    {
      std::size_t data_files{0};
      std::size_t meta_files{0};
    };

    SnapshotFileCounts CountDirectorySnapshots(const std::filesystem::path &snapshot_dir)
    {
      SnapshotFileCounts counts;
      std::error_code ec;
      if (!std::filesystem::exists(snapshot_dir, ec))
      {
        return counts;
      }

      for (const auto &entry : std::filesystem::directory_iterator(snapshot_dir, ec))
      {
        if (ec)
        {
          return counts;
        }
        if (!entry.is_directory(ec))
        {
          continue;
        }
        const auto dir = entry.path();
        if (std::filesystem::exists(dir / "data.bin", ec))
        {
          ++counts.data_files;
        }
        if (std::filesystem::exists(dir / "__raft_snapshot_meta", ec))
        {
          ++counts.meta_files;
        }
      }
      return counts;
    }

    std::string DescribeDirectoryTree(const std::filesystem::path &root)
    {
      std::ostringstream oss;
      oss << "test root: " << root.string() << '\n';

      std::error_code ec;
      if (!std::filesystem::exists(root, ec))
      {
        oss << "<missing>\n";
        return oss.str();
      }

      std::vector<std::filesystem::path> paths;
      for (const auto &entry : std::filesystem::recursive_directory_iterator(root, ec))
      {
        if (ec)
        {
          break;
        }
        paths.push_back(entry.path());
      }
      std::sort(paths.begin(), paths.end());

      for (const auto &path : paths)
      {
        oss << "  " << std::filesystem::relative(path, root, ec).string();
        if (std::filesystem::is_regular_file(path, ec))
        {
          oss << " (" << std::filesystem::file_size(path, ec) << " bytes)";
        }
        oss << '\n';
      }
      return oss.str();
    }

    PersistentRaftState MakeState(std::uint64_t first_index, std::uint64_t last_index)
    {
      PersistentRaftState state;
      state.current_term = 7;
      state.voted_for = 2;
      state.commit_index = last_index;
      state.last_applied = last_index;

      for (std::uint64_t index = first_index; index <= last_index; ++index)
      {
        state.log.push_back(LogRecord{index,
                                      7,
                                      "SET|storage_key_" + std::to_string(index) +
                                          "|storage_value_" + std::to_string(index)});
      }
      return state;
    }

    template <typename T>
    void OverwriteBinaryField(const std::filesystem::path &path,
                              std::streamoff offset,
                              const T &value)
    {
      std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
      ASSERT_TRUE(io.is_open()) << path.string();
      io.seekp(offset);
      ASSERT_TRUE(static_cast<bool>(io)) << "failed to seek " << path.string();
      io.write(reinterpret_cast<const char *>(&value), sizeof(T));
      ASSERT_TRUE(static_cast<bool>(io)) << "failed to overwrite " << path.string();
    }

    std::string ProposeStatusName(ProposeStatus status)
    {
      switch (status)
      {
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

    bool Contains(const std::string &text, const std::string &needle)
    {
      return text.find(needle) != std::string::npos;
    }

    void WriteBinaryFile(const std::filesystem::path &path, const std::string &content)
    {
      std::error_code ec;
      std::filesystem::create_directories(path.parent_path(), ec);
      ASSERT_FALSE(ec) << "failed to create parent for " << path.string()
                       << ": " << ec.message();

      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      ASSERT_TRUE(out.is_open()) << path.string();
      out.write(content.data(), static_cast<std::streamsize>(content.size()));
      out.flush();
      ASSERT_TRUE(static_cast<bool>(out)) << "failed to write " << path.string();
    }

    void SetEnvVar(const char *name, const char *value)
    {
#if defined(_WIN32)
      ASSERT_EQ(_putenv_s(name, value), 0);
#else
      ASSERT_EQ(setenv(name, value, 1), 0);
#endif
    }

    void UnsetEnvVar(const char *name)
    {
#if defined(_WIN32)
      ASSERT_EQ(_putenv_s(name, ""), 0);
#else
      ASSERT_EQ(unsetenv(name), 0);
#endif
    }

    class ScopedEnvVar
    {
    public:
      ScopedEnvVar(const char *name, const char *value) : name_(name)
      {
        const char *existing = std::getenv(name_);
        if (existing != nullptr)
        {
          had_old_value_ = true;
          old_value_ = existing;
        }
        SetEnvVar(name_, value);
      }

      ~ScopedEnvVar()
      {
        if (had_old_value_)
        {
          SetEnvVar(name_, old_value_.c_str());
        }
        else
        {
          UnsetEnvVar(name_);
        }
      }

    private:
      const char *name_;
      bool had_old_value_{false};
      std::string old_value_;
    };

    void ExpectInjectedFailure(const std::string &error,
                               const std::string &operation,
                               const std::filesystem::path &path,
                               const std::string &failure_class,
                               const std::string &trusted_state_expectation,
                               const std::string &diagnostic_expectation)
    {
      EXPECT_NE(error.find("injected durability failure"), std::string::npos) << error;
      EXPECT_NE(error.find("operation=" + operation), std::string::npos) << error;
      EXPECT_NE(error.find("path=" + path.string()), std::string::npos) << error;
      EXPECT_NE(error.find("failure_class=" + failure_class), std::string::npos) << error;
      EXPECT_NE(error.find(ExpectedLinuxSpecificMarker()), std::string::npos) << error;
      EXPECT_NE(error.find("trusted_state_expectation=" + trusted_state_expectation), std::string::npos)
          << error;
      EXPECT_NE(error.find("recovery_expectation=" + trusted_state_expectation), std::string::npos)
          << error;
      EXPECT_NE(error.find("diagnostic_expectation=" + diagnostic_expectation), std::string::npos)
          << error;
    }

    void ExpectLoadedStateMatches(const PersistentRaftState &loaded,
                                  const PersistentRaftState &expected)
    {
      EXPECT_EQ(loaded.current_term, expected.current_term);
      EXPECT_EQ(loaded.voted_for, expected.voted_for);
      EXPECT_EQ(loaded.commit_index, expected.commit_index);
      EXPECT_EQ(loaded.last_applied, expected.last_applied);
      ASSERT_EQ(loaded.log.size(), expected.log.size());
      ASSERT_FALSE(loaded.log.empty());
      EXPECT_EQ(loaded.log.front().index, expected.log.front().index);
      EXPECT_EQ(loaded.log.back().index, expected.log.back().index);
      EXPECT_EQ(loaded.log.back().command, expected.log.back().command);
    }

    bool IsLeaderNode(const std::shared_ptr<RaftNode> &node)
    {
      return node != nullptr && Contains(node->Describe(), "role=Leader");
    }

    Command SetCommand(const std::string &key, const std::string &value)
    {
      Command command;
      command.type = CommandType::kSet;
      command.key = key;
      command.value = value;
      return command;
    }

    int PickBasePort(const std::string &test_name)
    {
      const int name_offset = static_cast<int>(std::hash<std::string>{}(test_name) % 2000);
      if (const char *env = std::getenv("RAFT_TEST_BASE_PORT"))
      {
        try
        {
          return std::stoi(env) + name_offset;
        }
        catch (...)
        {
          // Fall through to generated port range.
        }
      }

      std::random_device rd;
      return 45000 + name_offset + static_cast<int>(rd() % 8000);
    }

    std::vector<NodeConfig> BuildThreeNodeConfigs(const std::filesystem::path &data_root,
                                                  int base_port)
    {
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

    std::vector<snapshotConfig> BuildThreeSnapshotConfigs(const std::filesystem::path &snapshot_root,
                                                          std::uint64_t log_threshold)
    {
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

    class TestCluster
    {
    public:
      TestCluster(std::vector<NodeConfig> configs,
                  std::vector<snapshotConfig> snapshot_configs)
          : configs_(std::move(configs)), snapshot_configs_(std::move(snapshot_configs)) {}

      ~TestCluster() { StopAll(); }

      void Start()
      {
        StopAll();
        nodes_.clear();
        wait_threads_.clear();

        for (std::size_t i = 0; i < configs_.size(); ++i)
        {
          nodes_.push_back(std::make_shared<RaftNode>(configs_[i], snapshot_configs_[i]));
        }
        wait_threads_.resize(nodes_.size());

        for (const auto &node : nodes_)
        {
          node->Start();
        }
        for (std::size_t i = 0; i < nodes_.size(); ++i)
        {
          const auto node = nodes_[i];
          wait_threads_[i] = std::thread([node]()
                                         { node->Wait(); });
        }
      }

      void StopAll()
      {
        for (const auto &node : nodes_)
        {
          if (node)
          {
            node->Stop();
          }
        }
        for (auto &thread : wait_threads_)
        {
          if (thread.joinable())
          {
            thread.join();
          }
        }
        wait_threads_.clear();
      }

      const std::vector<std::shared_ptr<RaftNode>> &Nodes() const { return nodes_; }

    private:
      std::vector<NodeConfig> configs_;
      std::vector<snapshotConfig> snapshot_configs_;
      std::vector<std::shared_ptr<RaftNode>> nodes_;
      std::vector<std::thread> wait_threads_;
    };

    std::string DescribeAllNodes(const std::vector<std::shared_ptr<RaftNode>> &nodes)
    {
      std::ostringstream oss;
      for (std::size_t i = 0; i < nodes.size(); ++i)
      {
        oss << "node[" << i << "] ";
        if (nodes[i])
        {
          oss << nodes[i]->Describe();
        }
        else
        {
          oss << "<not running>";
        }
        oss << '\n';
      }
      return oss.str();
    }

    std::shared_ptr<RaftNode> WaitForSingleLeader(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                                  std::chrono::milliseconds timeout)
    {
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        std::shared_ptr<RaftNode> leader;
        int leader_count = 0;

        for (const auto &node : nodes)
        {
          if (IsLeaderNode(node))
          {
            leader = node;
            ++leader_count;
          }
        }

        if (leader_count == 1)
        {
          return leader;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      return nullptr;
    }

    bool WaitForValueOnAll(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                           const std::string &key,
                           const std::string &expected_value,
                           std::chrono::milliseconds timeout)
    {
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        bool all_match = true;
        for (const auto &node : nodes)
        {
          if (!node)
          {
            all_match = false;
            break;
          }
          std::string value;
          if (!node->DebugGetValue(key, &value) || value != expected_value)
          {
            all_match = false;
            break;
          }
        }
        if (all_match)
        {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      return false;
    }

    std::optional<std::uint64_t> ExtractUintField(const std::string &describe,
                                                  const std::string &field_name)
    {
      const std::string prefix = field_name + "=";
      const std::size_t begin = describe.find(prefix);
      if (begin == std::string::npos)
      {
        return std::nullopt;
      }

      std::size_t pos = begin + prefix.size();
      std::size_t end = pos;
      while (end < describe.size() && describe[end] >= '0' && describe[end] <= '9')
      {
        ++end;
      }
      if (end == pos)
      {
        return std::nullopt;
      }

      try
      {
        return static_cast<std::uint64_t>(std::stoull(describe.substr(pos, end - pos)));
      }
      catch (...)
      {
        return std::nullopt;
      }
    }

    std::size_t CountNodesWithSnapshotAtLeast(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                              std::uint64_t minimum)
    {
      std::size_t count = 0;
      for (const auto &node : nodes)
      {
        if (!node)
        {
          continue;
        }
        const auto value = ExtractUintField(node->Describe(), "last_snapshot_index");
        if (value.has_value() && *value >= minimum)
        {
          ++count;
        }
      }
      return count;
    }

    bool WaitForSnapshotOnAtLeastNodes(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                                       std::uint64_t minimum_snapshot_index,
                                       std::size_t minimum_node_count,
                                       std::chrono::milliseconds timeout)
    {
      const auto deadline = Clock::now() + timeout;
      while (Clock::now() < deadline)
      {
        if (CountNodesWithSnapshotAtLeast(nodes, minimum_snapshot_index) >= minimum_node_count)
        {
          return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      return false;
    }

    bool ProposeWithRetry(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                          const Command &command,
                          std::chrono::milliseconds timeout,
                          ProposeResult *final_result)
    {
      const auto deadline = Clock::now() + timeout;
      ProposeResult last_result;

      while (Clock::now() < deadline)
      {
        auto leader = WaitForSingleLeader(nodes, std::chrono::milliseconds(1500));
        if (!leader)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
          continue;
        }

        last_result = leader->Propose(command);
        if (last_result.Ok())
        {
          if (final_result != nullptr)
          {
            *final_result = last_result;
          }
          return true;
        }

        if (last_result.status == ProposeStatus::kInvalidCommand ||
            last_result.status == ProposeStatus::kApplyFailed ||
            last_result.status == ProposeStatus::kCommitFailed)
        {
          break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      if (final_result != nullptr)
      {
        *final_result = last_result;
      }
      return false;
    }

    void WriteManyValues(const std::vector<std::shared_ptr<RaftNode>> &nodes,
                         const std::string &prefix,
                         int count)
    {
      ProposeResult result;
      for (int i = 0; i < count; ++i)
      {
        SCOPED_TRACE(prefix + " write " + std::to_string(i));
        ASSERT_TRUE(ProposeWithRetry(nodes,
                                     SetCommand(prefix + "_" + std::to_string(i),
                                                "value_" + std::to_string(i)),
                                     std::chrono::seconds(10),
                                     &result))
            << "write failed, status=" << ProposeStatusName(result.status)
            << ", message=" << result.message
            << "\n"
            << DescribeAllNodes(nodes);
      }
    }

    class RaftSegmentStorageTest : public ::testing::Test
    {
    protected:
      void SetUp() override
      {
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

      void TearDown() override
      {
        std::cout << "segment storage test data: " << root_.string() << '\n';
        std::cout << DescribeDirectoryTree(root_);
        if (!KeepTestData())
        {
          std::error_code ec;
          std::filesystem::remove_all(root_, ec);
        }
      }

      TestCluster MakeCluster(const std::string &case_name,
                              std::uint64_t snapshot_log_threshold) const
      {
        return TestCluster(BuildThreeNodeConfigs(data_root_ / case_name, base_port_),
                           BuildThreeSnapshotConfigs(snapshot_root_ / case_name,
                                                     snapshot_log_threshold));
      }

      std::filesystem::path root_;
      std::filesystem::path data_root_;
      std::filesystem::path snapshot_root_;
      int base_port_{0};
    };

    TEST_F(RaftSegmentStorageTest, WritesMultipleSegmentFilesUnderBuildDirectory)
    {
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

    TEST_F(RaftSegmentStorageTest, AutomaticallyDeletesObsoleteSegmentsAfterCompactionSave)
    {
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

    TEST_F(RaftSegmentStorageTest, TruncatesCorruptedSegmentTailDuringRecovery)
    {
      const auto storage_root = data_root_ / "direct_storage_tail_truncate";
      auto storage = CreateFileRaftStorage(storage_root.string());
      auto state = MakeState(1, 900);

      std::string error;
      ASSERT_TRUE(storage->Save(state, &error)) << error;

      const auto segment_files = ListFilesWithExtension(storage_root / "log", ".log");
      ASSERT_GE(segment_files.size(), 2U) << DescribeDirectoryTree(root_);

      const auto tail_segment = segment_files.back();
      std::error_code ec;
      const auto original_size = std::filesystem::file_size(tail_segment, ec);
      ASSERT_FALSE(ec) << ec.message();

      {
        std::ofstream out(tail_segment, std::ios::binary | std::ios::app);
        ASSERT_TRUE(out.is_open()) << tail_segment.string();
        const std::string junk = "corrupted-tail-bytes";
        out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        out.flush();
        ASSERT_TRUE(out) << "failed to append corruption to " << tail_segment.string();
      }

      const auto corrupted_size = std::filesystem::file_size(tail_segment, ec);
      ASSERT_FALSE(ec) << ec.message();
      ASSERT_GT(corrupted_size, original_size);

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_TRUE(storage->Load(&loaded, &has_state, &error)) << error;
      ASSERT_TRUE(has_state);
      ASSERT_EQ(loaded.log.size(), state.log.size());
      EXPECT_EQ(loaded.log.front().index, state.log.front().index);
      EXPECT_EQ(loaded.log.back().index, state.log.back().index);
      EXPECT_EQ(loaded.log.back().command, state.log.back().command);

      const auto truncated_size = std::filesystem::file_size(tail_segment, ec);
      ASSERT_FALSE(ec) << ec.message();
      EXPECT_EQ(truncated_size, original_size) << DescribeDirectoryTree(root_);
    }

    TEST_F(RaftSegmentStorageTest, TruncatesPartialSegmentHeaderDuringRecovery)
    {
      const auto storage_root = data_root_ / "direct_storage_partial_tail_header";
      auto storage = CreateFileRaftStorage(storage_root.string());
      auto state = MakeState(1, 900);

      std::string error;
      ASSERT_TRUE(storage->Save(state, &error)) << error;

      const auto segment_files = ListFilesWithExtension(storage_root / "log", ".log");
      ASSERT_GE(segment_files.size(), 2U) << DescribeDirectoryTree(root_);

      const auto tail_segment = segment_files.back();
      std::error_code ec;
      const auto original_size = std::filesystem::file_size(tail_segment, ec);
      ASSERT_FALSE(ec) << ec.message();

      {
        std::ofstream out(tail_segment, std::ios::binary | std::ios::app);
        ASSERT_TRUE(out.is_open()) << tail_segment.string();
        const char partial_header = '\x42';
        out.write(&partial_header, 1);
        out.flush();
        ASSERT_TRUE(out) << "failed to append partial header to " << tail_segment.string();
      }

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_TRUE(storage->Load(&loaded, &has_state, &error)) << error;
      ASSERT_TRUE(has_state);
      ASSERT_EQ(loaded.log.size(), state.log.size());
      EXPECT_EQ(loaded.log.front().index, state.log.front().index);
      EXPECT_EQ(loaded.log.back().index, state.log.back().index);
      EXPECT_EQ(loaded.log.back().command, state.log.back().command);

      const auto truncated_size = std::filesystem::file_size(tail_segment, ec);
      ASSERT_FALSE(ec) << ec.message();
      EXPECT_EQ(truncated_size, original_size) << DescribeDirectoryTree(root_);
    }

    TEST_F(RaftSegmentStorageTest, SaveDoesNotLeaveTemporaryOrBackupLogDirectories)
    {
      const auto storage_root = data_root_ / "direct_storage_publish_cleanup";
      auto storage = CreateFileRaftStorage(storage_root.string());
      std::string error;

      ASSERT_TRUE(storage->Save(MakeState(1, 1300), &error)) << error;
      EXPECT_TRUE(std::filesystem::exists(storage_root / "log"));
      EXPECT_FALSE(std::filesystem::exists(storage_root / "log.tmp")) << DescribeDirectoryTree(root_);
      EXPECT_FALSE(std::filesystem::exists(storage_root / "log.bak")) << DescribeDirectoryTree(root_);

      auto compacted = MakeState(900, 1300);
      compacted.log.front().command = "publish-boundary-entry";
      ASSERT_TRUE(storage->Save(compacted, &error)) << error;

      EXPECT_TRUE(std::filesystem::exists(storage_root / "log"));
      EXPECT_FALSE(std::filesystem::exists(storage_root / "log.tmp")) << DescribeDirectoryTree(root_);
      EXPECT_FALSE(std::filesystem::exists(storage_root / "log.bak")) << DescribeDirectoryTree(root_);
    }

    TEST_F(RaftSegmentStorageTest, SavePublishesMetaFileWithoutLeavingMetaTempFile)
    {
      const auto storage_root = data_root_ / "direct_storage_meta_publish";
      auto storage = CreateFileRaftStorage(storage_root.string());
      std::string error;

      ASSERT_TRUE(storage->Save(MakeState(1, 256), &error)) << error;

      EXPECT_TRUE(std::filesystem::exists(storage_root / "meta.bin")) << DescribeDirectoryTree(root_);
      EXPECT_FALSE(std::filesystem::exists(storage_root / "meta.bin.tmp")) << DescribeDirectoryTree(root_);
    }

    TEST_F(RaftSegmentStorageTest, RecoveryIgnoresTemporaryPublishArtifacts)
    {
      const auto storage_root = data_root_ / "direct_storage_leftover_publish_artifacts";
      auto storage = CreateFileRaftStorage(storage_root.string());
      auto state = MakeState(1, 64);

      std::string error;
      ASSERT_TRUE(storage->Save(state, &error)) << error;

      WriteBinaryFile(storage_root / "meta.bin.tmp", "partial-meta-temp");
      WriteBinaryFile(storage_root / "log.tmp" / "segment_00000000000000000001.log",
                      "partial-log-temp");
      WriteBinaryFile(storage_root / "log.bak" / "segment_00000000000000000001.log",
                      "old-log-backup");

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_TRUE(storage->Load(&loaded, &has_state, &error)) << error;
      ASSERT_TRUE(has_state);
      EXPECT_EQ(loaded.current_term, state.current_term);
      EXPECT_EQ(loaded.voted_for, state.voted_for);
      EXPECT_EQ(loaded.commit_index, state.commit_index);
      EXPECT_EQ(loaded.last_applied, state.last_applied);
      ASSERT_EQ(loaded.log.size(), state.log.size());
      EXPECT_EQ(loaded.log.front().index, state.log.front().index);
      EXPECT_EQ(loaded.log.back().index, state.log.back().index);
      EXPECT_EQ(loaded.log.back().command, state.log.back().command);
    }

    TEST_F(RaftSegmentStorageTest, MetaAndLogPublishWindowUsesOnlyTrustedBoundary)
    {
      const auto storage_root = data_root_ / "direct_storage_meta_log_publish_window";
      const auto saved_root = root_ / "saved_publish_states";
      auto storage = CreateFileRaftStorage(storage_root.string());

      const auto old_state = MakeState(1, 16);
      const auto new_state = MakeState(1, 32);
      std::string error;

      ASSERT_TRUE(storage->Save(old_state, &error)) << error;
      std::error_code ec;
      std::filesystem::create_directories(saved_root, ec);
      ASSERT_FALSE(ec) << ec.message();
      std::filesystem::copy_file(storage_root / "meta.bin", saved_root / "old_meta.bin",
                                 std::filesystem::copy_options::overwrite_existing, ec);
      ASSERT_FALSE(ec) << ec.message();
      std::filesystem::copy(storage_root / "log", saved_root / "old_log",
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing,
                            ec);
      ASSERT_FALSE(ec) << ec.message();

      ASSERT_TRUE(storage->Save(new_state, &error)) << error;
      std::filesystem::copy_file(storage_root / "meta.bin", saved_root / "new_meta.bin",
                                 std::filesystem::copy_options::overwrite_existing, ec);
      ASSERT_FALSE(ec) << ec.message();

      // Simulate crash after publishing new log/ but before publishing new meta.bin:
      // recovery must be bounded by the old complete meta state.
      std::filesystem::copy_file(saved_root / "old_meta.bin", storage_root / "meta.bin",
                                 std::filesystem::copy_options::overwrite_existing, ec);
      ASSERT_FALSE(ec) << ec.message();

      PersistentRaftState loaded_old_boundary;
      bool has_state = false;
      ASSERT_TRUE(storage->Load(&loaded_old_boundary, &has_state, &error)) << error;
      ASSERT_TRUE(has_state);
      ASSERT_EQ(loaded_old_boundary.log.size(), old_state.log.size());
      EXPECT_EQ(loaded_old_boundary.commit_index, old_state.commit_index);
      EXPECT_EQ(loaded_old_boundary.last_applied, old_state.last_applied);
      EXPECT_EQ(loaded_old_boundary.log.back().index, old_state.log.back().index);
      EXPECT_EQ(loaded_old_boundary.log.back().command, old_state.log.back().command);

      // Simulate the inverse torn publish: new meta.bin points past old log/.
      // This is not a trusted state and must fail boundary validation.
      std::filesystem::remove_all(storage_root / "log", ec);
      ASSERT_FALSE(ec) << ec.message();
      std::filesystem::copy(saved_root / "old_log", storage_root / "log",
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing,
                            ec);
      ASSERT_FALSE(ec) << ec.message();
      std::filesystem::copy_file(saved_root / "new_meta.bin", storage_root / "meta.bin",
                                 std::filesystem::copy_options::overwrite_existing, ec);
      ASSERT_FALSE(ec) << ec.message();

      PersistentRaftState rejected;
      has_state = false;
      ASSERT_FALSE(storage->Load(&rejected, &has_state, &error));
      EXPECT_FALSE(has_state);
      EXPECT_NE(error.find("log count mismatch"), std::string::npos) << error;
    }

    TEST_F(RaftSegmentStorageTest, MissingFirstSegmentFailsBeforeTrustingPublishedBoundary)
    {
      const auto storage_root = data_root_ / "direct_storage_missing_first_segment_boundary";
      auto storage = CreateFileRaftStorage(storage_root.string());
      const auto state = MakeState(1, 700);
      std::string error;

      ASSERT_TRUE(storage->Save(state, &error)) << error;

      const auto segment_files = ListFilesWithExtension(storage_root / "log", ".log");
      ASSERT_GE(segment_files.size(), 2U) << DescribeDirectoryTree(root_);
      ASSERT_TRUE(std::filesystem::remove(segment_files.front())) << DescribeDirectoryTree(root_);

      PersistentRaftState loaded;
      bool has_state = true;
      ASSERT_FALSE(storage->Load(&loaded, &has_state, &error));
      EXPECT_FALSE(has_state);
      EXPECT_FALSE(error.empty());
    }

    TEST_F(RaftSegmentStorageTest, FinalSegmentTailTruncateKeepsTrustedLogPrefixAndClampsCommitApply)
    {
      const auto storage_root = data_root_ / "direct_storage_final_segment_trusted_prefix";
      auto storage = CreateFileRaftStorage(storage_root.string());
      const auto state = MakeState(1, 513);
      std::string error;

      ASSERT_TRUE(storage->Save(state, &error)) << error;

      const auto segment_files = ListFilesWithExtension(storage_root / "log", ".log");
      ASSERT_EQ(segment_files.size(), 2U) << DescribeDirectoryTree(root_);
      const auto final_segment = segment_files.back();

      std::error_code ec;
      const auto original_size = std::filesystem::file_size(final_segment, ec);
      ASSERT_FALSE(ec) << ec.message();
      ASSERT_GT(original_size, 1U) << final_segment.string();

      std::filesystem::resize_file(final_segment, original_size - 1, ec);
      ASSERT_FALSE(ec) << ec.message();

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_TRUE(storage->Load(&loaded, &has_state, &error)) << error;
      ASSERT_TRUE(has_state);
      ASSERT_EQ(loaded.log.size(), 512U) << DescribeDirectoryTree(root_);
      EXPECT_EQ(loaded.log.back().index, 512U);
      EXPECT_EQ(loaded.commit_index, 512U);
      EXPECT_EQ(loaded.last_applied, 512U);
      EXPECT_EQ(loaded.log.back().command, state.log[511].command);
    }

    TEST_F(RaftSegmentStorageTest, LogDirectoryReplaceFailureNeedsExactFailureInjectionSeam)
    {
      const auto storage_root = data_root_ / "direct_storage_log_replace_injection_contract";
      const auto saved_root = root_ / "saved_log_replace_states";
      auto storage = CreateFileRaftStorage(storage_root.string());
      const auto old_state = MakeState(1, 16);
      const auto new_state = MakeState(1, 32);
      const std::string trusted_state_expectation =
          "restart must keep the previously durable segment set and must not trust a partially published newer log boundary when replacing log/ fails";
      std::string error;

      ASSERT_TRUE(storage->Save(old_state, &error)) << error;

      std::error_code ec;
      std::filesystem::create_directories(saved_root, ec);
      ASSERT_FALSE(ec) << ec.message();
      std::filesystem::copy(storage_root / "log", saved_root / "old_log",
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing,
                            ec);
      ASSERT_FALSE(ec) << ec.message();

      {
        ScopedEnvVar failpoint("RAFT_TEST_STORAGE_FAILPOINT", "log_directory_replace");
        ASSERT_FALSE(storage->Save(new_state, &error));
      }
      ExpectInjectedFailure(error,
                            "log directory replace",
                            storage_root / "log",
                            "replace/rename",
                            trusted_state_expectation,
                            "error should identify that replacing log/ failed before the newer segment tree became the trusted published boundary");

      std::filesystem::remove_all(storage_root / "log", ec);
      ec.clear();
      std::filesystem::copy(saved_root / "old_log", storage_root / "log",
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing,
                            ec);
      ASSERT_FALSE(ec) << ec.message();

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_TRUE(storage->Load(&loaded, &has_state, &error)) << error;
      ASSERT_TRUE(has_state);
      ExpectLoadedStateMatches(loaded, old_state);
    }

    TEST_F(RaftSegmentStorageTest, LogDirectorySyncFailureNeedsExactFailureInjectionSeam)
    {
      const auto storage_root = data_root_ / "direct_storage_log_directory_sync_injection_contract";
      const auto saved_root = root_ / "saved_log_directory_sync_states";
      auto storage = CreateFileRaftStorage(storage_root.string());
      const auto old_state = MakeState(1, 16);
      const auto new_state = MakeState(1, 32);
      const std::string trusted_state_expectation =
          "restart must stay bounded by the last fully durable segment publish if the new log tree becomes visible but the parent directory sync does not complete";
      std::string error;

      ASSERT_TRUE(storage->Save(old_state, &error)) << error;

      std::error_code ec;
      std::filesystem::create_directories(saved_root, ec);
      ASSERT_FALSE(ec) << ec.message();
      std::filesystem::copy(storage_root / "log", saved_root / "old_log",
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing,
                            ec);
      ASSERT_FALSE(ec) << ec.message();

      {
        ScopedEnvVar failpoint("RAFT_TEST_STORAGE_FAILPOINT", "log_directory_sync");
        ASSERT_FALSE(storage->Save(new_state, &error));
      }
      ExpectInjectedFailure(error,
                            "log parent directory sync after publish",
                            storage_root,
                            "directory sync",
                            trusted_state_expectation,
                            "error should identify that the parent directory sync boundary failed after the newer log tree became visible");

      std::filesystem::remove_all(storage_root / "log", ec);
      ec.clear();
      std::filesystem::copy(saved_root / "old_log", storage_root / "log",
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing,
                            ec);
      ASSERT_FALSE(ec) << ec.message();

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_TRUE(storage->Load(&loaded, &has_state, &error)) << error;
      ASSERT_TRUE(has_state);
      ExpectLoadedStateMatches(loaded, old_state);
    }

    TEST_F(RaftSegmentStorageTest, FinalSegmentPartialWriteNeedsExactFailureInjectionSeam)
    {
      const auto storage_root = data_root_ / "direct_storage_partial_write_injection_contract";
      auto storage = CreateFileRaftStorage(storage_root.string());
      const auto old_state = MakeState(1, 16);
      const auto new_state = MakeState(1, 900);
      const std::string trusted_state_expectation =
          "save must fail before publishing the new log directory, so restart stays on the previously durable log boundary and ignores the partially written temp segment";
      std::string error;

      ASSERT_TRUE(storage->Save(old_state, &error)) << error;

      {
        ScopedEnvVar failpoint("RAFT_TEST_STORAGE_FAILPOINT", "final_segment_partial_write");
        ASSERT_FALSE(storage->Save(new_state, &error));
      }
      ExpectInjectedFailure(error,
                            "final segment partial write during save",
                            storage_root / "log.tmp" / "segment_00000000000000000513.log",
                            "partial write",
                            trusted_state_expectation,
                            "error should identify that the final temp segment write stopped at a partial-record boundary and that recovery must reject the untrusted tail");

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_TRUE(storage->Load(&loaded, &has_state, &error)) << error;
      ASSERT_TRUE(has_state);
      ExpectLoadedStateMatches(loaded, old_state);
    }

    TEST_F(RaftSegmentStorageTest, MissingMetaFileCausesLoadToReportNoPersistedState)
    {
      const auto storage_root = data_root_ / "direct_storage_missing_meta";
      auto storage = CreateFileRaftStorage(storage_root.string());
      std::string error;

      ASSERT_TRUE(storage->Save(MakeState(1, 256), &error)) << error;
      ASSERT_TRUE(std::filesystem::remove(storage_root / "meta.bin")) << DescribeDirectoryTree(root_);

      PersistentRaftState loaded;
      bool has_state = true;
      ASSERT_TRUE(storage->Load(&loaded, &has_state, &error)) << error;
      EXPECT_FALSE(has_state);
      EXPECT_TRUE(loaded.log.empty());
      EXPECT_EQ(loaded.current_term, 0U);
      EXPECT_EQ(loaded.voted_for, -1);
      EXPECT_EQ(loaded.commit_index, 0U);
      EXPECT_EQ(loaded.last_applied, 0U);
    }

    TEST_F(RaftSegmentStorageTest, CorruptedMetaFileFailsLoad)
    {
      const auto storage_root = data_root_ / "direct_storage_corrupted_meta";
      auto storage = CreateFileRaftStorage(storage_root.string());
      std::string error;

      ASSERT_TRUE(storage->Save(MakeState(1, 256), &error)) << error;

      const auto meta_path = storage_root / "meta.bin";
      {
        std::fstream io(meta_path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(io.is_open()) << meta_path.string();
        const unsigned char bad_magic = 0x00;
        io.write(reinterpret_cast<const char *>(&bad_magic), sizeof(bad_magic));
        ASSERT_TRUE(static_cast<bool>(io)) << "failed to corrupt meta file";
      }

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_FALSE(storage->Load(&loaded, &has_state, &error));
      EXPECT_FALSE(has_state);
      EXPECT_NE(error.find("invalid raft meta magic"), std::string::npos) << error;
    }

    TEST_F(RaftSegmentStorageTest, UnsupportedMetaVersionFailsLoadWithPathAndVersion)
    {
      const auto storage_root = data_root_ / "direct_storage_unsupported_meta_version";
      auto storage = CreateFileRaftStorage(storage_root.string());
      std::string error;

      ASSERT_TRUE(storage->Save(MakeState(1, 16), &error)) << error;

      const auto meta_path = storage_root / "meta.bin";
      constexpr std::streamoff kMetaVersionOffset = sizeof(std::uint32_t);
      OverwriteBinaryField<std::uint32_t>(meta_path, kMetaVersionOffset, 99U);

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_FALSE(storage->Load(&loaded, &has_state, &error));
      EXPECT_FALSE(has_state);
      EXPECT_NE(error.find("unsupported raft meta version"), std::string::npos) << error;
      EXPECT_NE(error.find(meta_path.string()), std::string::npos) << error;
      EXPECT_NE(error.find("version=99"), std::string::npos) << error;
    }

    TEST_F(RaftSegmentStorageTest, InconsistentMetaLogBoundaryFailsBeforeTrustingSegments)
    {
      const auto storage_root = data_root_ / "direct_storage_bad_meta_boundary";
      auto storage = CreateFileRaftStorage(storage_root.string());
      std::string error;

      ASSERT_TRUE(storage->Save(MakeState(1, 16), &error)) << error;

      const auto meta_path = storage_root / "meta.bin";
      constexpr std::streamoff kLogCountOffset =
          sizeof(std::uint32_t) + sizeof(std::uint32_t) + sizeof(std::uint64_t) +
          sizeof(std::int64_t) + sizeof(std::uint64_t) + sizeof(std::uint64_t) +
          sizeof(std::uint64_t) + sizeof(std::uint64_t);
      OverwriteBinaryField<std::uint64_t>(meta_path, kLogCountOffset, 99U);

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_FALSE(storage->Load(&loaded, &has_state, &error));
      EXPECT_FALSE(has_state);
      EXPECT_NE(error.find("meta log boundary invariant failed"), std::string::npos) << error;
      EXPECT_NE(error.find("log_count=99"), std::string::npos) << error;
      EXPECT_NE(error.find(meta_path.string()), std::string::npos) << error;
    }

    TEST_F(RaftSegmentStorageTest, CorruptedEarlierSegmentTailCleansLaterSegmentsAndReportsDiagnostics)
    {
      const auto storage_root = data_root_ / "direct_storage_tail_truncate_cleanup";
      auto storage = CreateFileRaftStorage(storage_root.string());
      auto state = MakeState(1, 900);

      std::string error;
      ASSERT_TRUE(storage->Save(state, &error)) << error;

      const auto segment_files = ListFilesWithExtension(storage_root / "log", ".log");
      ASSERT_GE(segment_files.size(), 2U) << DescribeDirectoryTree(root_);
      const auto first_segment = segment_files.front();
      const auto later_segment = segment_files.back();

      {
        std::ofstream out(first_segment, std::ios::binary | std::ios::app);
        ASSERT_TRUE(out.is_open()) << first_segment.string();
        const std::string junk = "corrupted-earlier-segment-tail";
        out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        out.flush();
        ASSERT_TRUE(out) << "failed to append corruption to " << first_segment.string();
      }

      PersistentRaftState loaded;
      bool has_state = false;
      ASSERT_FALSE(storage->Load(&loaded, &has_state, &error));
      EXPECT_FALSE(has_state);
      EXPECT_NE(error.find("segment tail truncated"), std::string::npos) << error;
      EXPECT_NE(error.find(first_segment.filename().string()), std::string::npos) << error;
      EXPECT_NE(error.find("removed_later_segment"), std::string::npos) << error;
      EXPECT_FALSE(std::filesystem::exists(later_segment)) << DescribeDirectoryTree(root_);
    }

    TEST_F(RaftSegmentStorageTest, RaftClusterGeneratesManySnapshotsAndSegmentLogsUnderBuildDirectory)
    {
      // Use a deliberately small snapshot threshold so the test quickly produces
      // multiple snapshot files that can be inspected under build/tests/raft_test_data.
      constexpr std::uint64_t kSnapshotThreshold = 4;
      constexpr int kWriteCount = 160;
      constexpr std::uint64_t kExpectedSnapshotIndex = 80;

      auto cluster = MakeCluster("cluster_generated_many_snapshots_and_logs", kSnapshotThreshold);
      cluster.Start();

      auto leader = WaitForSingleLeader(cluster.Nodes(), std::chrono::seconds(8));
      ASSERT_NE(leader, nullptr) << "no leader elected\n"
                                 << DescribeAllNodes(cluster.Nodes());

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

      for (int node_id = 1; node_id <= 3; ++node_id)
      {
        const auto data_dir = data_root_ / "cluster_generated_many_snapshots_and_logs" /
                              ("node_" + std::to_string(node_id));
        const auto snapshot_dir = snapshot_root_ / "cluster_generated_many_snapshots_and_logs" /
                                  ("node_" + std::to_string(node_id));

        if (std::filesystem::exists(data_dir / "meta.bin"))
        {
          ++nodes_with_meta;
        }

        const std::size_t segment_count = CountSegmentFiles(data_dir);
        total_segments += segment_count;
        if (segment_count > 0)
        {
          ++nodes_with_segment;
        }

        const SnapshotFileCounts snapshot_counts = CountDirectorySnapshots(snapshot_dir);
        total_snapshot_bins += snapshot_counts.data_files;
        total_snapshot_metas += snapshot_counts.meta_files;
        if (snapshot_counts.data_files > 0)
        {
          ++nodes_with_snapshot_bin;
        }
        if (snapshot_counts.meta_files > 0)
        {
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

  } // namespace
} // namespace raftdemo
