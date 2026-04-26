#include "raft/snapshot_storage.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace raftdemo {
namespace {

constexpr std::uint32_t kMetaMagic = 0x53504D32U;  // "SPM2"
constexpr std::uint32_t kMetaVersion = 2U;
constexpr std::uint32_t kLegacyMetaMagic = 0x53504D31U;  // "SPM1"
constexpr std::uint32_t kLegacyMetaVersion = 1U;
constexpr const char* kSnapshotDataFileName = "data.bin";
constexpr const char* kSnapshotMetaFileName = "__raft_snapshot_meta";
constexpr const char* kSnapshotPrefix = "snapshot_";

std::uint64_t NowUnixMillis() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string FormatIndex(std::uint64_t index) {
  std::ostringstream oss;
  oss << std::setw(20) << std::setfill('0') << index;
  return oss.str();
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

template <typename T>
bool WritePod(std::ofstream& out, const T& value, std::string* error) {
  static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
  out.write(reinterpret_cast<const char*>(&value), sizeof(T));
  if (!out) {
    if (error != nullptr) {
      *error = "write binary field failed";
    }
    return false;
  }
  return true;
}

template <typename T>
bool ReadPod(std::ifstream& in, T* value, std::string* error) {
  static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
  if (value == nullptr) {
    if (error != nullptr) {
      *error = "ReadPod target must not be null";
    }
    return false;
  }
  in.read(reinterpret_cast<char*>(value), sizeof(T));
  if (!in) {
    if (error != nullptr) {
      *error = in.eof() ? "unexpected EOF" : "read binary field failed";
    }
    return false;
  }
  return true;
}

bool WriteString(std::ofstream& out, const std::string& value, std::string* error) {
  const std::uint64_t size = static_cast<std::uint64_t>(value.size());
  if (!WritePod(out, size, error)) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  out.write(value.data(), static_cast<std::streamsize>(size));
  if (!out) {
    if (error != nullptr) {
      *error = "write string failed";
    }
    return false;
  }
  return true;
}

bool ReadString(std::ifstream& in, std::string* value, std::string* error) {
  if (value == nullptr) {
    if (error != nullptr) {
      *error = "ReadString target must not be null";
    }
    return false;
  }
  std::uint64_t size = 0;
  if (!ReadPod(in, &size, error)) {
    return false;
  }
  // Defensive bound. Snapshot metadata should contain only a tiny filename.
  if (size > 1024 * 1024) {
    if (error != nullptr) {
      *error = "string field is too large";
    }
    return false;
  }
  value->clear();
  if (size == 0) {
    return true;
  }
  value->resize(static_cast<std::size_t>(size));
  in.read(value->data(), static_cast<std::streamsize>(size));
  if (!in) {
    if (error != nullptr) {
      *error = "read string failed";
    }
    return false;
  }
  return true;
}

std::uint32_t UpdateChecksum(std::uint32_t checksum, const char* data, std::size_t size) {
  // FNV-1a 32-bit. It is not cryptographic, but it is portable and sufficient
  // for detecting torn/corrupted snapshot files in this project.
  constexpr std::uint32_t kFnvPrime = 16777619U;
  for (std::size_t i = 0; i < size; ++i) {
    checksum ^= static_cast<unsigned char>(data[i]);
    checksum *= kFnvPrime;
  }
  return checksum;
}

bool ComputeFileChecksum(const std::filesystem::path& file_path,
                         std::uint32_t* out_checksum,
                         std::string* error) {
  if (out_checksum == nullptr) {
    if (error != nullptr) {
      *error = "checksum output must not be null";
    }
    return false;
  }

  std::ifstream in(file_path, std::ios::binary);
  if (!in.is_open()) {
    if (error != nullptr) {
      *error = "open file for checksum failed: " + file_path.string();
    }
    return false;
  }

  std::uint32_t checksum = 2166136261U;
  char buffer[64 * 1024];
  while (in) {
    in.read(buffer, sizeof(buffer));
    const std::streamsize got = in.gcount();
    if (got > 0) {
      checksum = UpdateChecksum(checksum, buffer, static_cast<std::size_t>(got));
    }
  }
  if (!in.eof()) {
    if (error != nullptr) {
      *error = "read file for checksum failed: " + file_path.string();
    }
    return false;
  }
  *out_checksum = checksum;
  return true;
}

bool CopyFilePortable(const std::filesystem::path& from,
                      const std::filesystem::path& to,
                      std::string* error) {
  std::ifstream in(from, std::ios::binary);
  if (!in.is_open()) {
    if (error != nullptr) {
      *error = "open source snapshot failed: " + from.string();
    }
    return false;
  }
  std::ofstream out(to, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    if (error != nullptr) {
      *error = "open target snapshot failed: " + to.string();
    }
    return false;
  }
  out << in.rdbuf();
  out.flush();
  if (!out) {
    if (error != nullptr) {
      *error = "copy snapshot data failed: " + to.string();
    }
    return false;
  }
  return true;
}

class FileSnapshotStorage final : public ISnapshotStorage {
 public:
  FileSnapshotStorage(std::string snapshot_dir, std::string file_prefix)
      : snapshot_dir_(std::move(snapshot_dir)),
        file_prefix_(std::move(file_prefix)) {}

  bool SaveSnapshotFile(const std::string& input_snapshot_file,
                        std::uint64_t last_included_index,
                        std::uint64_t last_included_term,
                        SnapshotMeta* out_meta,
                        std::string* error) override {
    if (input_snapshot_file.empty()) {
      if (error != nullptr) {
        *error = "input snapshot file path is empty";
      }
      return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(snapshot_dir_, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "create snapshot dir failed: " + ec.message();
      }
      return false;
    }

    const std::uint64_t now_ms = NowUnixMillis();
    const std::filesystem::path root(snapshot_dir_);
    const std::string formatted_index = FormatIndex(last_included_index);
    const std::filesystem::path final_dir =
        root / (std::string(kSnapshotPrefix) + formatted_index);
    const std::filesystem::path data_path = final_dir / kSnapshotDataFileName;
    const std::filesystem::path meta_path = final_dir / kSnapshotMetaFileName;

    // No temp snapshot directory is created. The final snapshot directory is
    // written directly under the configured snapshot_dir so test/debug files
    // are visible immediately inside build/tests/raft_test_data.
    ec.clear();
    std::filesystem::remove_all(final_dir, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "remove old snapshot dir failed: " + ec.message();
      }
      return false;
    }

    ec.clear();
    std::filesystem::create_directories(final_dir, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "create snapshot dir failed: " + ec.message();
      }
      return false;
    }

    if (!CopyFilePortable(input_snapshot_file, data_path, error)) {
      std::filesystem::remove_all(final_dir, ec);
      return false;
    }

    std::uint32_t checksum = 0;
    if (!ComputeFileChecksum(data_path, &checksum, error)) {
      std::filesystem::remove_all(final_dir, ec);
      return false;
    }

    if (!WriteMeta(meta_path, last_included_index, last_included_term, now_ms,
                   kSnapshotDataFileName, checksum, error)) {
      std::filesystem::remove_all(final_dir, ec);
      return false;
    }

    ec.clear();
    std::filesystem::remove(input_snapshot_file, ec);

    if (out_meta != nullptr) {
      out_meta->snapshot_dir = final_dir.string();
      out_meta->snapshot_path = data_path.string();
      out_meta->meta_path = meta_path.string();
      out_meta->last_included_index = last_included_index;
      out_meta->last_included_term = last_included_term;
      out_meta->created_unix_ms = now_ms;
      out_meta->data_checksum = checksum;
    }
    return true;
  }

  bool ListSnapshots(std::vector<SnapshotMeta>* out,
                     std::string* error) override {
    if (out == nullptr) {
      if (error != nullptr) {
        *error = "output snapshot list must not be null";
      }
      return false;
    }
    out->clear();

    std::error_code ec;
    if (!std::filesystem::exists(snapshot_dir_, ec)) {
      return true;
    }

    for (const auto& entry : std::filesystem::directory_iterator(snapshot_dir_, ec)) {
      if (ec) {
        if (error != nullptr) {
          *error = "iterate snapshot dir failed: " + ec.message();
        }
        return false;
      }

      SnapshotMeta meta;
      std::string local_error;
      if (entry.is_directory()) {
        const std::string name = entry.path().filename().string();
        if (!StartsWith(name, kSnapshotPrefix)) {
          continue;
        }
        if (!ReadDirectorySnapshot(entry.path(), &meta, &local_error)) {
          continue;
        }
        out->push_back(std::move(meta));
        continue;
      }

      // Backward compatibility with the old flat snapshot_<...>.bin/.meta
      // layout. These snapshots do not have checksums, but can still be loaded.
      if (entry.is_regular_file() && entry.path().extension() == ".meta") {
        if (!ReadLegacyFlatSnapshot(entry.path(), &meta, &local_error)) {
          continue;
        }
        out->push_back(std::move(meta));
      }
    }

    std::sort(out->begin(), out->end(), [](const SnapshotMeta& a, const SnapshotMeta& b) {
      if (a.last_included_index != b.last_included_index) {
        return a.last_included_index > b.last_included_index;
      }
      return a.created_unix_ms > b.created_unix_ms;
    });
    return true;
  }

  bool LoadLatestValidSnapshot(SnapshotMeta* out_meta,
                               bool* has_snapshot,
                               std::string* error) override {
    if (has_snapshot == nullptr) {
      if (error != nullptr) {
        *error = "has_snapshot must not be null";
      }
      return false;
    }
    *has_snapshot = false;

    std::vector<SnapshotMeta> snapshots;
    if (!ListSnapshots(&snapshots, error)) {
      return false;
    }
    if (snapshots.empty()) {
      return true;
    }

    if (out_meta != nullptr) {
      *out_meta = snapshots.front();
    }
    *has_snapshot = true;
    return true;
  }

  bool PruneSnapshots(std::size_t max_keep,
                      std::string* error) override {
    if (max_keep == 0) {
      max_keep = 1;
    }

    std::vector<SnapshotMeta> snapshots;
    if (!ListSnapshots(&snapshots, error)) {
      return false;
    }
    if (snapshots.size() <= max_keep) {
      return true;
    }

    std::error_code ec;
    for (std::size_t i = max_keep; i < snapshots.size(); ++i) {
      if (!snapshots[i].snapshot_dir.empty()) {
        ec.clear();
        std::filesystem::remove_all(snapshots[i].snapshot_dir, ec);
        if (ec && error != nullptr) {
          *error = "remove old snapshot dir failed: " + ec.message();
        }
      } else {
        ec.clear();
        std::filesystem::remove(snapshots[i].snapshot_path, ec);
        if (ec && error != nullptr) {
          *error = "remove old snapshot data failed: " + ec.message();
        }
        ec.clear();
        std::filesystem::remove(snapshots[i].meta_path, ec);
        if (ec && error != nullptr) {
          *error = "remove old snapshot meta failed: " + ec.message();
        }
      }
    }
    return true;
  }

  const std::string& SnapshotDir() const override {
    return snapshot_dir_;
  }

 private:
  bool WriteMeta(const std::filesystem::path& meta_path,
                 std::uint64_t last_included_index,
                 std::uint64_t last_included_term,
                 std::uint64_t created_unix_ms,
                 const std::string& data_file_name,
                 std::uint32_t checksum,
                 std::string* error) {
    std::ofstream out(meta_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      if (error != nullptr) {
        *error = "open snapshot meta file failed: " + meta_path.string();
      }
      return false;
    }

    const std::uint32_t magic = kMetaMagic;
    const std::uint32_t version = kMetaVersion;
    if (!WritePod(out, magic, error) ||
        !WritePod(out, version, error) ||
        !WritePod(out, created_unix_ms, error) ||
        !WritePod(out, last_included_index, error) ||
        !WritePod(out, last_included_term, error) ||
        !WritePod(out, checksum, error) ||
        !WriteString(out, data_file_name, error)) {
      return false;
    }
    out.flush();
    if (!out) {
      if (error != nullptr) {
        *error = "flush snapshot meta file failed";
      }
      return false;
    }
    return true;
  }

  bool ReadDirectorySnapshot(const std::filesystem::path& snapshot_dir,
                             SnapshotMeta* out_meta,
                             std::string* error) {
    if (out_meta == nullptr) {
      if (error != nullptr) {
        *error = "snapshot meta output must not be null";
      }
      return false;
    }

    const std::filesystem::path meta_path = snapshot_dir / kSnapshotMetaFileName;
    std::ifstream in(meta_path, std::ios::binary);
    if (!in.is_open()) {
      if (error != nullptr) {
        *error = "open snapshot meta file failed: " + meta_path.string();
      }
      return false;
    }

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint64_t created_unix_ms = 0;
    std::uint64_t last_included_index = 0;
    std::uint64_t last_included_term = 0;
    std::uint32_t checksum = 0;
    std::string data_file_name;

    if (!ReadPod(in, &magic, error) ||
        !ReadPod(in, &version, error) ||
        !ReadPod(in, &created_unix_ms, error) ||
        !ReadPod(in, &last_included_index, error) ||
        !ReadPod(in, &last_included_term, error) ||
        !ReadPod(in, &checksum, error) ||
        !ReadString(in, &data_file_name, error)) {
      return false;
    }

    if (magic != kMetaMagic || version != kMetaVersion) {
      if (error != nullptr) {
        *error = "invalid snapshot meta header";
      }
      return false;
    }
    if (data_file_name.empty()) {
      if (error != nullptr) {
        *error = "snapshot data file name is empty";
      }
      return false;
    }

    const std::string name = snapshot_dir.filename().string();
    if (StartsWith(name, kSnapshotPrefix)) {
      const std::string index_part = name.substr(std::string(kSnapshotPrefix).size());
      try {
        const std::uint64_t dir_index = static_cast<std::uint64_t>(std::stoull(index_part));
        if (dir_index != last_included_index) {
          if (error != nullptr) {
            *error = "snapshot directory index does not match meta";
          }
          return false;
        }
      } catch (...) {
        if (error != nullptr) {
          *error = "invalid snapshot directory index";
        }
        return false;
      }
    }

    const std::filesystem::path data_path = snapshot_dir / data_file_name;
    std::error_code ec;
    if (!std::filesystem::exists(data_path, ec) || ec) {
      if (error != nullptr) {
        *error = "snapshot data file missing: " + data_path.string();
      }
      return false;
    }

    std::uint32_t actual_checksum = 0;
    if (!ComputeFileChecksum(data_path, &actual_checksum, error)) {
      return false;
    }
    if (actual_checksum != checksum) {
      if (error != nullptr) {
        *error = "snapshot checksum mismatch";
      }
      return false;
    }

    out_meta->snapshot_dir = snapshot_dir.string();
    out_meta->snapshot_path = data_path.string();
    out_meta->meta_path = meta_path.string();
    out_meta->last_included_index = last_included_index;
    out_meta->last_included_term = last_included_term;
    out_meta->created_unix_ms = created_unix_ms;
    out_meta->data_checksum = checksum;
    return true;
  }

  bool ReadLegacyFlatSnapshot(const std::filesystem::path& meta_path,
                              SnapshotMeta* out_meta,
                              std::string* error) {
    if (out_meta == nullptr) {
      if (error != nullptr) {
        *error = "snapshot meta output must not be null";
      }
      return false;
    }

    std::ifstream in(meta_path, std::ios::binary);
    if (!in.is_open()) {
      if (error != nullptr) {
        *error = "open legacy snapshot meta failed";
      }
      return false;
    }

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    if (!ReadPod(in, &magic, error) ||
        !ReadPod(in, &version, error) ||
        !ReadPod(in, &out_meta->created_unix_ms, error) ||
        !ReadPod(in, &out_meta->last_included_index, error) ||
        !ReadPod(in, &out_meta->last_included_term, error)) {
      return false;
    }

    if (magic != kLegacyMetaMagic || version != kLegacyMetaVersion) {
      if (error != nullptr) {
        *error = "invalid legacy snapshot meta header";
      }
      return false;
    }

    const std::filesystem::path data_path = meta_path.parent_path() / (meta_path.stem().string() + ".bin");
    std::error_code ec;
    if (!std::filesystem::exists(data_path, ec) || ec) {
      if (error != nullptr) {
        *error = "legacy snapshot data missing";
      }
      return false;
    }
    out_meta->snapshot_dir.clear();
    out_meta->snapshot_path = data_path.string();
    out_meta->meta_path = meta_path.string();
    out_meta->data_checksum = 0;
    return true;
  }



  std::string snapshot_dir_;
  std::string file_prefix_;
};

}  // namespace

std::unique_ptr<ISnapshotStorage> CreateFileSnapshotStorage(std::string snapshot_dir,
                                                            std::string file_prefix) {
  return std::make_unique<FileSnapshotStorage>(std::move(snapshot_dir), std::move(file_prefix));
}

}  // namespace raftdemo
