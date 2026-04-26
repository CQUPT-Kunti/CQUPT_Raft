#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "raft/snapshot_storage.h"

namespace raftdemo {
namespace {

std::string Sanitize(const std::string& name) {
  std::string out;
  out.reserve(name.size());
  for (char ch : name) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
      out.push_back(ch);
    } else {
      out.push_back('_');
    }
  }
  return out;
}

std::filesystem::path TestRoot(const std::string& test_name) {
  return std::filesystem::current_path() / "raft_test_data" / "snapshot_storage" / Sanitize(test_name);
}

bool KeepData() {
  const char* value = std::getenv("RAFT_TEST_KEEP_DATA");
  return value != nullptr && std::string(value) != "0";
}

void WriteTextFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << path.string();
  out << content;
  out.flush();
  ASSERT_TRUE(static_cast<bool>(out));
}

std::size_t CountSnapshotDirs(const std::filesystem::path& dir) {
  std::size_t count = 0;
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec)) {
    return 0;
  }
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (ec) {
      return count;
    }
    if (entry.is_directory() && entry.path().filename().string().rfind("snapshot_", 0) == 0) {
      ++count;
    }
  }
  return count;
}

class SnapshotStorageReliabilityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    root_ = TestRoot(std::string(info->test_suite_name()) + "_" + info->name());
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
    snapshot_dir_ = root_ / "raft_snapshots" / "node_1";
    input_dir_ = root_ / "input_snapshots";
    std::filesystem::create_directories(input_dir_);
    storage_ = CreateFileSnapshotStorage(snapshot_dir_.string(), "snapshot");
  }

  void TearDown() override {
    if (KeepData() || ::testing::Test::HasFailure()) {
      std::cout << "preserved snapshot storage test root: " << root_.string() << std::endl;
      return;
    }
    std::filesystem::remove_all(root_);
  }

  SnapshotMeta SaveSnapshot(std::uint64_t index, std::uint64_t term, const std::string& payload) {
    const std::filesystem::path input_file = input_dir_ / ("snapshot_input_" + std::to_string(index) + ".bin");
    WriteTextFile(input_file, payload);
    SnapshotMeta meta;
    std::string error;
    EXPECT_TRUE(storage_->SaveSnapshotFile(input_file.string(), index, term, &meta, &error)) << error;
    return meta;
  }

  std::filesystem::path root_;
  std::filesystem::path snapshot_dir_;
  std::filesystem::path input_dir_;
  std::unique_ptr<ISnapshotStorage> storage_;
};

TEST_F(SnapshotStorageReliabilityTest, SavesSnapshotAsDirectoryWithDataAndMeta) {
  SnapshotMeta meta = SaveSnapshot(120, 7, "state-machine-data-120");

  EXPECT_FALSE(meta.snapshot_dir.empty());
  EXPECT_TRUE(std::filesystem::exists(meta.snapshot_dir));
  EXPECT_TRUE(std::filesystem::exists(meta.snapshot_path));
  EXPECT_TRUE(std::filesystem::exists(meta.meta_path));
  EXPECT_EQ(meta.last_included_index, 120U);
  EXPECT_EQ(meta.last_included_term, 7U);
  EXPECT_NE(meta.data_checksum, 0U);

  const auto snapshot_dir_name = std::filesystem::path(meta.snapshot_dir).filename().string();
  EXPECT_EQ(snapshot_dir_name, "snapshot_00000000000000000120");
  EXPECT_EQ(std::filesystem::path(meta.snapshot_path).filename().string(), "data.bin");
  EXPECT_EQ(std::filesystem::path(meta.meta_path).filename().string(), "__raft_snapshot_meta");
}

TEST_F(SnapshotStorageReliabilityTest, SavesDirectlyWithoutTempSnapshotDirectory) {
  SaveSnapshot(10, 1, "stable-snapshot-10");
  SaveSnapshot(20, 2, "stable-snapshot-20");

  std::vector<SnapshotMeta> snapshots;
  std::string error;
  ASSERT_TRUE(storage_->ListSnapshots(&snapshots, &error)) << error;
  ASSERT_EQ(snapshots.size(), 2U);

  std::error_code ec;
  ASSERT_TRUE(std::filesystem::exists(snapshot_dir_, ec));
  for (const auto& entry : std::filesystem::directory_iterator(snapshot_dir_, ec)) {
    ASSERT_FALSE(ec);
    const std::string name = entry.path().filename().string();
    EXPECT_NE(name.rfind("temp_", 0), 0U) << "unexpected temp snapshot dir: " << entry.path();
  }

  EXPECT_TRUE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000010" / "data.bin"));
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000010" / "__raft_snapshot_meta"));
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000020" / "data.bin"));
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000020" / "__raft_snapshot_meta"));
}

TEST_F(SnapshotStorageReliabilityTest, FallsBackToOlderSnapshotWhenNewestIsCorrupted) {
  SnapshotMeta old_meta = SaveSnapshot(10, 1, "valid-snapshot-10");
  SnapshotMeta new_meta = SaveSnapshot(20, 2, "valid-snapshot-20");

  WriteTextFile(new_meta.snapshot_path, "corrupted-new-snapshot");

  SnapshotMeta loaded;
  bool has_snapshot = false;
  std::string error;
  ASSERT_TRUE(storage_->LoadLatestValidSnapshot(&loaded, &has_snapshot, &error)) << error;
  ASSERT_TRUE(has_snapshot);
  EXPECT_EQ(loaded.last_included_index, old_meta.last_included_index);
  EXPECT_EQ(loaded.last_included_term, old_meta.last_included_term);
  EXPECT_EQ(std::filesystem::path(loaded.snapshot_path).filename().string(), "data.bin");
}

TEST_F(SnapshotStorageReliabilityTest, PrunesOldSnapshotDirectoriesByIndex) {
  SaveSnapshot(10, 1, "snapshot-10");
  SaveSnapshot(20, 2, "snapshot-20");
  SaveSnapshot(30, 3, "snapshot-30");
  SaveSnapshot(40, 4, "snapshot-40");

  std::string error;
  ASSERT_TRUE(storage_->PruneSnapshots(2, &error)) << error;

  EXPECT_EQ(CountSnapshotDirs(snapshot_dir_), 2U);
  EXPECT_FALSE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000010"));
  EXPECT_FALSE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000020"));
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000030"));
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000040"));

  SnapshotMeta latest;
  bool has_snapshot = false;
  ASSERT_TRUE(storage_->LoadLatestValidSnapshot(&latest, &has_snapshot, &error)) << error;
  ASSERT_TRUE(has_snapshot);
  EXPECT_EQ(latest.last_included_index, 40U);
}

}  // namespace
}  // namespace raftdemo
