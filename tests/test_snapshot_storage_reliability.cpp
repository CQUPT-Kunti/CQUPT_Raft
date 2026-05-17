#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "raft/storage/snapshot_storage.h"

namespace raftdemo {
namespace {

constexpr const char* kSnapshotStorageFailpointEnv = "RAFT_TEST_SNAPSHOT_STORAGE_FAILPOINT";

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
#ifdef _WIN32
  std::random_device rd;
  const std::string name = "ssr_" + std::to_string(rd());
  return std::filesystem::temp_directory_path() / "rq_ssr" / name;
#else
  return std::filesystem::current_path() / "raft_test_data" / "snapshot_storage" / Sanitize(test_name);
#endif
}

bool KeepData() {
  const char* value = std::getenv("RAFT_TEST_KEEP_DATA");
  return value != nullptr && std::string(value) != "0";
}

const char* ExpectedLinuxSpecificMarker() {
#if defined(__linux__)
  return "linux_specific=true";
#else
  return "linux_specific=false";
#endif
}

void WriteTextFile(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  ASSERT_TRUE(out.is_open()) << path.string();
  out << content;
  out.flush();
  ASSERT_TRUE(static_cast<bool>(out));
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open()) << path.string();
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string JoinIssueReasons(const std::vector<SnapshotValidationIssue>& issues) {
  std::ostringstream oss;
  for (const auto& issue : issues) {
    oss << issue.path << ": " << issue.reason << "\n";
  }
  return oss.str();
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

TEST_F(SnapshotStorageReliabilityTest, PublishesSnapshotWithCompatibleFinalLayout) {
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
    EXPECT_NE(name.rfind(".snapshot_staging_", 0), 0U)
        << "unexpected staging snapshot dir after publish: " << entry.path();
  }

  EXPECT_TRUE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000010" / "data.bin"));
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000010" / "__raft_snapshot_meta"));
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000020" / "data.bin"));
  EXPECT_TRUE(std::filesystem::exists(snapshot_dir_ / "snapshot_00000000000000000020" / "__raft_snapshot_meta"));
}

TEST_F(SnapshotStorageReliabilityTest, IgnoresStagingAndIncompleteSnapshotDirectories) {
  SnapshotMeta old_meta = SaveSnapshot(10, 1, "valid-snapshot-10");

  WriteTextFile(snapshot_dir_ / ".snapshot_staging_00000000000000000030_1" / "data.bin",
                "staged-but-not-published");
  WriteTextFile(snapshot_dir_ / "snapshot_00000000000000000020" / "data.bin",
                "missing-meta");
  WriteTextFile(snapshot_dir_ / "snapshot_00000000000000000030" / "__raft_snapshot_meta",
                "missing-data");

  std::vector<SnapshotMeta> snapshots;
  std::string error;
  ASSERT_TRUE(storage_->ListSnapshots(&snapshots, &error)) << error;
  ASSERT_EQ(snapshots.size(), 1U);
  EXPECT_EQ(snapshots.front().last_included_index, old_meta.last_included_index);

  SnapshotMeta loaded;
  bool has_snapshot = false;
  ASSERT_TRUE(storage_->LoadLatestValidSnapshot(&loaded, &has_snapshot, &error)) << error;
  ASSERT_TRUE(has_snapshot);
  EXPECT_EQ(loaded.last_included_index, old_meta.last_included_index);
}

TEST_F(SnapshotStorageReliabilityTest, ReportsValidationIssuesForSkippedSnapshotEntries) {
  SnapshotMeta old_meta = SaveSnapshot(10, 1, "valid-snapshot-10");
  SnapshotMeta missing_data_meta = SaveSnapshot(30, 3, "valid-snapshot-30");
  SnapshotMeta corrupted_meta = SaveSnapshot(40, 4, "valid-snapshot-40");

  WriteTextFile(snapshot_dir_ / ".snapshot_staging_00000000000000000050_1" / "data.bin",
                "staged-but-not-published");
  WriteTextFile(snapshot_dir_ / "snapshot_00000000000000000020" / "data.bin",
                "missing-meta");
  ASSERT_TRUE(std::filesystem::remove(missing_data_meta.snapshot_path))
      << missing_data_meta.snapshot_path;
  WriteTextFile(corrupted_meta.snapshot_path, "corrupted-new-snapshot");

  SnapshotListResult result;
  std::string error;
  ASSERT_TRUE(storage_->ListSnapshotsWithDiagnostics(&result, &error)) << error;

  ASSERT_EQ(result.snapshots.size(), 1U);
  EXPECT_EQ(result.snapshots.front().last_included_index, old_meta.last_included_index);

  const std::string reasons = JoinIssueReasons(result.validation_issues);
  EXPECT_NE(reasons.find("staging snapshot directory ignored"), std::string::npos) << reasons;
  EXPECT_NE(reasons.find("open snapshot meta file failed"), std::string::npos) << reasons;
  EXPECT_NE(reasons.find("snapshot data file missing"), std::string::npos) << reasons;
  EXPECT_NE(reasons.find("snapshot checksum mismatch"), std::string::npos) << reasons;
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

TEST_F(SnapshotStorageReliabilityTest, AllInvalidSnapshotsReturnNoTrustedSnapshotWithDiagnostics) {
  SnapshotMeta missing_data_meta = SaveSnapshot(20, 2, "valid-snapshot-20");
  SnapshotMeta corrupted_meta = SaveSnapshot(30, 3, "valid-snapshot-30");

  WriteTextFile(snapshot_dir_ / ".snapshot_staging_00000000000000000040_1" / "data.bin",
                "staged-but-not-published");
  WriteTextFile(snapshot_dir_ / "snapshot_00000000000000000010" / "data.bin",
                "missing-meta");
  ASSERT_TRUE(std::filesystem::remove(missing_data_meta.snapshot_path))
      << missing_data_meta.snapshot_path;
  WriteTextFile(corrupted_meta.snapshot_path, "corrupted-only-snapshot");

  SnapshotListResult result;
  std::string error;
  ASSERT_TRUE(storage_->ListSnapshotsWithDiagnostics(&result, &error)) << error;
  EXPECT_TRUE(result.snapshots.empty());

  const std::string reasons = JoinIssueReasons(result.validation_issues);
  EXPECT_NE(reasons.find("staging snapshot directory ignored"), std::string::npos) << reasons;
  EXPECT_NE(reasons.find("open snapshot meta file failed"), std::string::npos) << reasons;
  EXPECT_NE(reasons.find("snapshot data file missing"), std::string::npos) << reasons;
  EXPECT_NE(reasons.find("snapshot checksum mismatch"), std::string::npos) << reasons;

  SnapshotMeta loaded;
  bool has_snapshot = true;
  ASSERT_TRUE(storage_->LoadLatestValidSnapshot(&loaded, &has_snapshot, &error)) << error;
  EXPECT_FALSE(has_snapshot);
}

TEST_F(SnapshotStorageReliabilityTest, SameIndexSameTermSaveIsIdempotent) {
  SnapshotMeta first = SaveSnapshot(20, 2, "stable-snapshot-20");
  SnapshotMeta second = SaveSnapshot(20, 2, "replacement-payload-ignored");

  EXPECT_EQ(second.last_included_index, first.last_included_index);
  EXPECT_EQ(second.last_included_term, first.last_included_term);
  EXPECT_EQ(std::filesystem::path(second.snapshot_dir), std::filesystem::path(first.snapshot_dir));
  EXPECT_EQ(ReadTextFile(second.snapshot_path), "stable-snapshot-20");

  std::vector<SnapshotMeta> snapshots;
  std::string error;
  ASSERT_TRUE(storage_->ListSnapshots(&snapshots, &error)) << error;
  ASSERT_EQ(snapshots.size(), 1U);
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

TEST_F(SnapshotStorageReliabilityTest, StagedSnapshotPublishFailureNeedsExactFailureInjectionSeam) {
  SaveSnapshot(10, 1, "stable-snapshot-10");

  const std::filesystem::path snapshot_publish_root =
      snapshot_dir_ / "snapshot_00000000000000000020";
  const std::filesystem::path input_file = input_dir_ / "snapshot_input_20.bin";
  WriteTextFile(input_file, "stable-snapshot-20");

  SnapshotMeta unused_meta;
  std::string error;
  {
    ScopedEnvVar failpoint(kSnapshotStorageFailpointEnv,
                           "snapshot_staged_publish_after_data_meta_write");
    EXPECT_FALSE(storage_->SaveSnapshotFile(input_file.string(), 20, 2, &unused_meta, &error));
  }
  EXPECT_NE(error.find("operation=snapshot staged publish after data/meta write"), std::string::npos)
      << error;
  EXPECT_NE(error.find("path=" + snapshot_publish_root.string()), std::string::npos) << error;
  EXPECT_NE(error.find("failure_class=replace/rename"), std::string::npos) << error;
  EXPECT_NE(error.find(ExpectedLinuxSpecificMarker()), std::string::npos) << error;
  EXPECT_NE(
      error.find("trusted_state_expectation=if staged snapshot publish fails before the final trusted publish point, restart must keep using the previously trusted snapshot and ignore the incomplete newer snapshot"),
      std::string::npos)
      << error;
  EXPECT_NE(
      error.find("recovery_expectation=restart should keep using the last trusted snapshot and reject the incomplete newer snapshot publish attempt"),
      std::string::npos)
      << error;
  EXPECT_NE(
      error.find("diagnostic_expectation=error should identify that the staged snapshot directory was never promoted to the trusted published snapshot boundary"),
      std::string::npos)
      << error;

  SnapshotListResult result;
  ASSERT_TRUE(storage_->ListSnapshotsWithDiagnostics(&result, &error)) << error;
  ASSERT_EQ(result.snapshots.size(), 1U);
  EXPECT_EQ(result.snapshots.front().last_included_index, 10U);
  EXPECT_NE(JoinIssueReasons(result.validation_issues).find("staging snapshot directory ignored"),
            std::string::npos);

  SnapshotMeta loaded;
  bool has_snapshot = false;
  ASSERT_TRUE(storage_->LoadLatestValidSnapshot(&loaded, &has_snapshot, &error)) << error;
  ASSERT_TRUE(has_snapshot);
  EXPECT_EQ(loaded.last_included_index, 10U);
}

TEST_F(SnapshotStorageReliabilityTest, SnapshotDirectorySyncFailureNeedsExactFailureInjectionSeam) {
  SaveSnapshot(10, 1, "stable-snapshot-10");

  const std::filesystem::path input_file = input_dir_ / "snapshot_input_20.bin";
  const std::filesystem::path final_dir =
      snapshot_dir_ / "snapshot_00000000000000000020";
  WriteTextFile(input_file, "stable-snapshot-20");

  SnapshotMeta unused_meta;
  std::string error;
  {
    ScopedEnvVar failpoint(kSnapshotStorageFailpointEnv,
                           "snapshot_parent_directory_sync_after_publish");
    EXPECT_FALSE(storage_->SaveSnapshotFile(input_file.string(), 20, 2, &unused_meta, &error));
  }
  EXPECT_NE(error.find("operation=snapshot parent directory sync after publish"), std::string::npos)
      << error;
  EXPECT_NE(error.find("path=" + snapshot_dir_.string()), std::string::npos) << error;
  EXPECT_NE(error.find("failure_class=directory sync"), std::string::npos) << error;
  EXPECT_NE(error.find(ExpectedLinuxSpecificMarker()), std::string::npos) << error;
  EXPECT_NE(
      error.find("trusted_state_expectation=if the new snapshot directory becomes visible but the parent directory sync does not complete, restart must stay on the last fully durable trusted snapshot boundary"),
      std::string::npos)
      << error;
  EXPECT_NE(
      error.find("recovery_expectation=if the new snapshot directory becomes visible but the parent directory sync does not complete, restart must stay on the last fully durable trusted snapshot boundary"),
      std::string::npos)
      << error;
  EXPECT_NE(
      error.find("diagnostic_expectation=error should identify that the snapshot directory became visible but the parent directory sync boundary did not complete"),
      std::string::npos)
      << error;

  EXPECT_TRUE(std::filesystem::exists(final_dir)) << final_dir.string();
  EXPECT_FALSE(std::filesystem::exists(final_dir / "__raft_snapshot_meta"));

  SnapshotListResult result;
  ASSERT_TRUE(storage_->ListSnapshotsWithDiagnostics(&result, &error)) << error;
  ASSERT_EQ(result.snapshots.size(), 1U);
  EXPECT_EQ(result.snapshots.front().last_included_index, 10U);
  EXPECT_NE(JoinIssueReasons(result.validation_issues).find("open snapshot meta file failed"),
            std::string::npos);

  SnapshotMeta loaded;
  bool has_snapshot = false;
  ASSERT_TRUE(storage_->LoadLatestValidSnapshot(&loaded, &has_snapshot, &error)) << error;
  ASSERT_TRUE(has_snapshot);
  EXPECT_EQ(loaded.last_included_index, 10U);
}

TEST_F(SnapshotStorageReliabilityTest, SnapshotPruneRemoveFailureNeedsExactFailureInjectionSeam) {
  SaveSnapshot(10, 1, "snapshot-10");
  SaveSnapshot(20, 2, "snapshot-20");
  SaveSnapshot(30, 3, "snapshot-30");

  const std::filesystem::path old_snapshot_dir =
      snapshot_dir_ / "snapshot_00000000000000000010";

  std::string error;
  {
    ScopedEnvVar failpoint(kSnapshotStorageFailpointEnv,
                           "snapshot_prune_remove_old_directory");
  EXPECT_FALSE(storage_->PruneSnapshots(2, &error));
  }
  EXPECT_NE(error.find("operation=snapshot prune remove old directory"), std::string::npos) << error;
  EXPECT_NE(error.find("path=" + old_snapshot_dir.string()), std::string::npos) << error;
  EXPECT_NE(error.find("failure_class=prune/remove"), std::string::npos) << error;
  EXPECT_NE(error.find(ExpectedLinuxSpecificMarker()), std::string::npos) << error;
  EXPECT_NE(
      error.find("trusted_state_expectation=if pruning old snapshots fails during remove/publish cleanup, the newest trusted snapshot must remain loadable and restart must not mis-classify prune leftovers as the chosen trusted snapshot"),
      std::string::npos)
      << error;
  EXPECT_NE(
      error.find("recovery_expectation=restart should keep the newest trusted snapshot loadable and should not let prune leftovers win trusted snapshot selection"),
      std::string::npos)
      << error;
  EXPECT_NE(
      error.find("diagnostic_expectation=error should identify that pruning the old snapshot directory failed before cleanup completed"),
      std::string::npos)
      << error;

  std::vector<SnapshotMeta> snapshots;
  ASSERT_TRUE(storage_->ListSnapshots(&snapshots, &error)) << error;
  ASSERT_EQ(snapshots.size(), 3U);
  EXPECT_EQ(snapshots.front().last_included_index, 30U);
  EXPECT_TRUE(std::filesystem::exists(old_snapshot_dir));

  SnapshotMeta loaded;
  bool has_snapshot = false;
  ASSERT_TRUE(storage_->LoadLatestValidSnapshot(&loaded, &has_snapshot, &error)) << error;
  ASSERT_TRUE(has_snapshot);
  EXPECT_EQ(loaded.last_included_index, 30U);
}

}  // namespace
}  // namespace raftdemo
