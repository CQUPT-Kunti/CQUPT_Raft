#include "raft/storage/snapshot_storage.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace raftdemo
{
  namespace
  {

    constexpr std::uint32_t kMetaMagic = 0x53504D32U; // "SPM2"
    constexpr std::uint32_t kMetaVersion = 2U;
    constexpr std::uint32_t kLegacyMetaMagic = 0x53504D31U; // "SPM1"
    constexpr std::uint32_t kLegacyMetaVersion = 1U;
    constexpr const char *kSnapshotDataFileName = "data.bin";
    constexpr const char *kSnapshotMetaFileName = "__raft_snapshot_meta";
    constexpr const char *kSnapshotPrefix = "snapshot_";
    constexpr const char *kSnapshotStagingPrefix = ".snapshot_staging_";
    constexpr const char *kSnapshotStorageFailpointEnv = "RAFT_TEST_SNAPSHOT_STORAGE_FAILPOINT";

    enum class SnapshotStorageFailPoint
    {
      kNone,
      kStagedPublishAfterDataMetaWrite,
      kParentDirectorySyncAfterPublish,
      kPublishVisibleBeforeTrustedDirectorySync,
      kPruneRemoveOldDirectory,
    };

    SnapshotStorageFailPoint ActiveSnapshotStorageFailPoint()
    {
      const char *value = std::getenv(kSnapshotStorageFailpointEnv);
      if (value == nullptr)
      {
        return SnapshotStorageFailPoint::kNone;
      }

      const std::string failpoint(value);
      if (failpoint == "snapshot_staged_publish_after_data_meta_write")
      {
        return SnapshotStorageFailPoint::kStagedPublishAfterDataMetaWrite;
      }
      if (failpoint == "snapshot_parent_directory_sync_after_publish")
      {
        return SnapshotStorageFailPoint::kParentDirectorySyncAfterPublish;
      }
      if (failpoint == "snapshot_publish_visible_before_trusted_directory_sync")
      {
        return SnapshotStorageFailPoint::kPublishVisibleBeforeTrustedDirectorySync;
      }
      if (failpoint == "snapshot_prune_remove_old_directory")
      {
        return SnapshotStorageFailPoint::kPruneRemoveOldDirectory;
      }
      return SnapshotStorageFailPoint::kNone;
    }

    bool BuildInjectedSnapshotFailure(const std::string &operation,
                                      const std::filesystem::path &path,
                                      const std::string &failure_class,
                                      const std::string &trusted_state_expectation,
                                      const std::string &recovery_expectation,
                                      const std::string &diagnostic_expectation,
                                      const std::string &detail,
                                      std::string *error)
    {
      if (error != nullptr)
      {
        std::ostringstream oss;
        oss << "snapshot storage failure injection triggered"
            << ", operation=" << operation
            << ", path=" << path.string()
            << ", failure_class=" << failure_class
#if defined(__linux__)
            << ", linux_specific=true"
#else
            << ", linux_specific=false"
#endif
            << ", trusted_state_expectation=" << trusted_state_expectation
            << ", recovery_expectation=" << recovery_expectation
            << ", diagnostic_expectation=" << diagnostic_expectation;
        if (!detail.empty())
        {
          oss << ", detail=" << detail;
        }
        *error = oss.str();
      }
      return false;
    }

    bool RemovePathIfExists(const std::filesystem::path &path, std::string *detail)
    {
      std::error_code ec;
      if (!std::filesystem::exists(path, ec))
      {
        if (ec && detail != nullptr)
        {
          *detail = "check failure artifact path failed: " + path.string() + ": " + ec.message();
        }
        return !ec;
      }

      ec.clear();
      const bool removed = std::filesystem::remove(path, ec);
      if (ec && detail != nullptr)
      {
        *detail = "remove failure artifact path failed: " + path.string() + ": " + ec.message();
      }
      if (!removed && detail != nullptr && detail->empty())
      {
        *detail = "failure artifact path remained visible: " + path.string();
      }
      return !ec;
    }

    std::uint64_t NowUnixMillis()
    {
      return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                            std::chrono::system_clock::now().time_since_epoch())
                                            .count());
    }

    std::string FormatIndex(std::uint64_t index)
    {
      std::ostringstream oss;
      oss << std::setw(20) << std::setfill('0') << index;
      return oss.str();
    }

    bool StartsWith(const std::string &value, const std::string &prefix)
    {
      return value.size() >= prefix.size() &&
             value.compare(0, prefix.size(), prefix) == 0;
    }

    template <typename T>
    bool WritePod(std::ofstream &out, const T &value, std::string *error)
    {
      static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
      out.write(reinterpret_cast<const char *>(&value), sizeof(T));
      if (!out)
      {
        if (error != nullptr)
        {
          *error = "write binary field failed";
        }
        return false;
      }
      return true;
    }

    template <typename T>
    bool ReadPod(std::ifstream &in, T *value, std::string *error)
    {
      static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
      if (value == nullptr)
      {
        if (error != nullptr)
        {
          *error = "ReadPod target must not be null";
        }
        return false;
      }
      in.read(reinterpret_cast<char *>(value), sizeof(T));
      if (!in)
      {
        if (error != nullptr)
        {
          *error = in.eof() ? "unexpected EOF" : "read binary field failed";
        }
        return false;
      }
      return true;
    }

    bool WriteString(std::ofstream &out, const std::string &value, std::string *error)
    {
      const std::uint64_t size = static_cast<std::uint64_t>(value.size());
      if (!WritePod(out, size, error))
      {
        return false;
      }
      if (size == 0)
      {
        return true;
      }
      out.write(value.data(), static_cast<std::streamsize>(size));
      if (!out)
      {
        if (error != nullptr)
        {
          *error = "write string failed";
        }
        return false;
      }
      return true;
    }

    bool ReadString(std::ifstream &in, std::string *value, std::string *error)
    {
      if (value == nullptr)
      {
        if (error != nullptr)
        {
          *error = "ReadString target must not be null";
        }
        return false;
      }
      std::uint64_t size = 0;
      if (!ReadPod(in, &size, error))
      {
        return false;
      }
      // Defensive bound. Snapshot metadata should contain only a tiny filename.
      if (size > 1024 * 1024)
      {
        if (error != nullptr)
        {
          *error = "string field is too large";
        }
        return false;
      }
      value->clear();
      if (size == 0)
      {
        return true;
      }
      value->resize(static_cast<std::size_t>(size));
      in.read(value->data(), static_cast<std::streamsize>(size));
      if (!in)
      {
        if (error != nullptr)
        {
          *error = "read string failed";
        }
        return false;
      }
      return true;
    }

    std::uint32_t UpdateChecksum(std::uint32_t checksum, const char *data, std::size_t size)
    {
      // FNV-1a 32-bit. It is not cryptographic, but it is portable and sufficient
      // for detecting torn/corrupted snapshot files in this project.
      constexpr std::uint32_t kFnvPrime = 16777619U;
      for (std::size_t i = 0; i < size; ++i)
      {
        checksum ^= static_cast<unsigned char>(data[i]);
        checksum *= kFnvPrime;
      }
      return checksum;
    }

    bool ComputeFileChecksum(const std::filesystem::path &file_path,
                             std::uint32_t *out_checksum,
                             std::string *error)
    {
      if (out_checksum == nullptr)
      {
        if (error != nullptr)
        {
          *error = "checksum output must not be null";
        }
        return false;
      }

      std::ifstream in(file_path, std::ios::binary);
      if (!in.is_open())
      {
        if (error != nullptr)
        {
          *error = "open file for checksum failed: " + file_path.string();
        }
        return false;
      }

      std::uint32_t checksum = 2166136261U;
      char buffer[64 * 1024];
      while (in)
      {
        in.read(buffer, sizeof(buffer));
        const std::streamsize got = in.gcount();
        if (got > 0)
        {
          checksum = UpdateChecksum(checksum, buffer, static_cast<std::size_t>(got));
        }
      }
      if (!in.eof())
      {
        if (error != nullptr)
        {
          *error = "read file for checksum failed: " + file_path.string();
        }
        return false;
      }
      *out_checksum = checksum;
      return true;
    }

    std::string LastSystemErrorMessage()
    {
#if defined(_WIN32)
      const DWORD error_code = ::GetLastError();
      LPSTR buffer = nullptr;
      const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                          FORMAT_MESSAGE_FROM_SYSTEM |
                          FORMAT_MESSAGE_IGNORE_INSERTS;
      const DWORD length = ::FormatMessageA(flags,
                                            nullptr,
                                            error_code,
                                            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                            reinterpret_cast<LPSTR>(&buffer),
                                            0,
                                            nullptr);
      if (length == 0 || buffer == nullptr)
      {
        std::ostringstream oss;
        oss << "GetLastError=" << static_cast<unsigned long>(error_code);
        return oss.str();
      }

      std::string message(buffer, length);
      ::LocalFree(buffer);
      while (!message.empty() &&
             (message.back() == '\r' || message.back() == '\n' || message.back() == ' '))
      {
        message.pop_back();
      }
      std::ostringstream oss;
      oss << message << " (GetLastError=" << static_cast<unsigned long>(error_code) << ")";
      return oss.str();
#else
      return std::strerror(errno);
#endif
    }

    bool SyncFile(const std::filesystem::path &path, std::string *error)
    {
#if defined(_WIN32)
      const HANDLE handle = ::CreateFileW(path.c_str(),
                                          GENERIC_READ | GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL,
                                          nullptr);
      if (handle == INVALID_HANDLE_VALUE)
      {
        if (error != nullptr)
        {
          *error = "CreateFileW for snapshot file flush failed: " + path.string() + ": " +
                   LastSystemErrorMessage();
        }
        return false;
      }

      if (!::FlushFileBuffers(handle))
      {
        const std::string message = LastSystemErrorMessage();
        ::CloseHandle(handle);
        if (error != nullptr)
        {
          *error = "FlushFileBuffers for snapshot file failed: " + path.string() + ": " + message;
        }
        return false;
      }

      if (!::CloseHandle(handle))
      {
        if (error != nullptr)
        {
          *error = "CloseHandle after snapshot file flush failed: " + path.string() + ": " +
                   LastSystemErrorMessage();
        }
        return false;
      }
      return true;
#else
      int flags = O_RDONLY;
#ifdef O_CLOEXEC
      flags |= O_CLOEXEC;
#endif
      const int fd = ::open(path.c_str(), flags);
      if (fd < 0)
      {
        if (error != nullptr)
        {
          *error = "open snapshot file for fsync failed: " + path.string() + ": " +
                   LastSystemErrorMessage();
        }
        return false;
      }

      if (::fsync(fd) != 0)
      {
        const std::string message = LastSystemErrorMessage();
        ::close(fd);
        if (error != nullptr)
        {
          *error = "fsync snapshot file failed: " + path.string() + ": " + message;
        }
        return false;
      }

      if (::close(fd) != 0)
      {
        if (error != nullptr)
        {
          *error = "close snapshot file after fsync failed: " + path.string() + ": " +
                   LastSystemErrorMessage();
        }
        return false;
      }
      return true;
#endif
    }

    bool SyncDirectory(const std::filesystem::path &path, std::string *error)
    {
#if defined(_WIN32)
      // Windows directory flush requires a writable directory handle. Opening
      // with FILE_LIST_DIRECTORY alone causes FlushFileBuffers to fail with
      // ERROR_ACCESS_DENIED on normal directories.
      const HANDLE handle = ::CreateFileW(path.c_str(),
                                          GENERIC_WRITE,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                          nullptr,
                                          OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS,
                                          nullptr);
      if (handle == INVALID_HANDLE_VALUE)
      {
        if (error != nullptr)
        {
          *error = "CreateFileW for snapshot directory flush failed: " + path.string() + ": " +
                   LastSystemErrorMessage();
        }
        return false;
      }

      if (!::FlushFileBuffers(handle))
      {
        const std::string message = LastSystemErrorMessage();
        ::CloseHandle(handle);
        if (error != nullptr)
        {
          *error = "FlushFileBuffers for snapshot directory failed: " + path.string() + ": " + message;
        }
        return false;
      }

      if (!::CloseHandle(handle))
      {
        if (error != nullptr)
        {
          *error = "CloseHandle after snapshot directory flush failed: " + path.string() + ": " +
                   LastSystemErrorMessage();
        }
        return false;
      }
      return true;
#else
      int flags = O_RDONLY;
#ifdef O_DIRECTORY
      flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
      flags |= O_CLOEXEC;
#endif
      const int fd = ::open(path.c_str(), flags);
      if (fd < 0)
      {
        if (error != nullptr)
        {
          *error = "open snapshot directory for fsync failed: " + path.string() + ": " +
                   LastSystemErrorMessage();
        }
        return false;
      }

      if (::fsync(fd) != 0)
      {
        const std::string message = LastSystemErrorMessage();
        ::close(fd);
        if (error != nullptr)
        {
          *error = "fsync snapshot directory failed: " + path.string() + ": " + message;
        }
        return false;
      }

      if (::close(fd) != 0)
      {
        if (error != nullptr)
        {
          *error = "close snapshot directory after fsync failed: " + path.string() + ": " +
                   LastSystemErrorMessage();
        }
        return false;
      }
      return true;
#endif
    }

    bool CopyFilePortable(const std::filesystem::path &from,
                          const std::filesystem::path &to,
                          std::string *error)
    {
      std::ifstream in(from, std::ios::binary);
      if (!in.is_open())
      {
        if (error != nullptr)
        {
          *error = "open source snapshot failed: " + from.string();
        }
        return false;
      }
      std::ofstream out(to, std::ios::binary | std::ios::trunc);
      if (!out.is_open())
      {
        if (error != nullptr)
        {
          *error = "open target snapshot failed: " + to.string();
        }
        return false;
      }
      out << in.rdbuf();
      out.flush();
      if (!out)
      {
        if (error != nullptr)
        {
          *error = "copy snapshot data failed: " + to.string();
        }
        return false;
      }
      out.close();
      if (!out)
      {
        if (error != nullptr)
        {
          *error = "close snapshot data failed: " + to.string();
        }
        return false;
      }
      if (!SyncFile(to, error))
      {
        return false;
      }
      return true;
    }

  } // namespace

  FileSnapshotStorage::FileSnapshotStorage(std::string snapshot_dir, std::string file_prefix)
      : snapshot_dir_(std::move(snapshot_dir)),
        file_prefix_(std::move(file_prefix)) {}

  bool FileSnapshotStorage::SaveSnapshotFile(const std::string &input_snapshot_file,
                                             std::uint64_t last_included_index,
                                             std::uint64_t last_included_term,
                                             SnapshotMeta *out_meta,
                                             std::string *error)
  {
    if (input_snapshot_file.empty())
    {
      if (error != nullptr)
      {
        *error = "input snapshot file path is empty";
      }
      return false;
    }

    std::error_code ec;
    const std::filesystem::path root(snapshot_dir_);
    std::filesystem::create_directories(root, ec);
    if (ec)
    {
      if (error != nullptr)
      {
        *error = "create snapshot dir failed: " + ec.message();
      }
      return false;
    }

    const std::uint64_t now_ms = NowUnixMillis();
    const std::filesystem::path root_parent = root.parent_path();
    if (!root_parent.empty() && !SyncDirectory(root_parent, error))
    {
      return false;
    }
    const std::string formatted_index = FormatIndex(last_included_index);
    const std::filesystem::path final_dir =
        root / (std::string(kSnapshotPrefix) + formatted_index);

    if (!SyncDirectory(root, error))
    {
      return false;
    }

    SnapshotMeta existing_meta;
    std::string existing_error;
    ec.clear();
    if (std::filesystem::exists(final_dir, ec) &&
        ReadDirectorySnapshot(final_dir, &existing_meta, &existing_error))
    {
      if (existing_meta.last_included_term != last_included_term)
      {
        if (error != nullptr)
        {
          *error = "existing snapshot has same index but different term: " + final_dir.string();
        }
        return false;
      }
      ec.clear();
      std::filesystem::remove(input_snapshot_file, ec);
      if (out_meta != nullptr)
      {
        *out_meta = existing_meta;
      }
      return true;
    }
    if (ec)
    {
      if (error != nullptr)
      {
        *error = "check snapshot dir failed: " + final_dir.string() + ": " + ec.message();
      }
      return false;
    }

    const std::filesystem::path staging_dir =
        root / (std::string(kSnapshotStagingPrefix) + formatted_index + "_" +
                std::to_string(now_ms));
    const std::filesystem::path staging_data_path = staging_dir / kSnapshotDataFileName;
    const std::filesystem::path staging_meta_path = staging_dir / kSnapshotMetaFileName;

    ec.clear();
    std::filesystem::remove_all(staging_dir, ec);
    if (ec)
    {
      if (error != nullptr)
      {
        *error = "remove stale snapshot staging dir failed: " + staging_dir.string() + ": " +
                 ec.message();
      }
      return false;
    }

    ec.clear();
    std::filesystem::create_directories(staging_dir, ec);
    if (ec)
    {
      if (error != nullptr)
      {
        *error = "create snapshot staging dir failed: " + staging_dir.string() + ": " +
                 ec.message();
      }
      return false;
    }
    if (!SyncDirectory(root, error))
    {
      std::filesystem::remove_all(staging_dir, ec);
      return false;
    }

    if (!CopyFilePortable(input_snapshot_file, staging_data_path, error))
    {
      std::filesystem::remove_all(staging_dir, ec);
      return false;
    }

    std::uint32_t checksum = 0;
    if (!ComputeFileChecksum(staging_data_path, &checksum, error))
    {
      std::filesystem::remove_all(staging_dir, ec);
      return false;
    }

    if (!WriteMeta(staging_meta_path, last_included_index, last_included_term, now_ms,
                   kSnapshotDataFileName, checksum, error))
    {
      std::filesystem::remove_all(staging_dir, ec);
      return false;
    }
    if (!SyncDirectory(staging_dir, error))
    {
      std::filesystem::remove_all(staging_dir, ec);
      return false;
    }
    if (ActiveSnapshotStorageFailPoint() ==
        SnapshotStorageFailPoint::kStagedPublishAfterDataMetaWrite)
    {
      return BuildInjectedSnapshotFailure(
          "snapshot staged publish after data/meta write",
          final_dir,
          "replace/rename",
          "if staged snapshot publish fails before the final trusted publish point, restart must keep using the previously trusted snapshot and ignore the incomplete newer snapshot",
          "restart should keep using the last trusted snapshot and reject the incomplete newer snapshot publish attempt",
          "error should identify that the staged snapshot directory was never promoted to the trusted published snapshot boundary",
          "staging snapshot directory left in place for deterministic recovery fallback validation",
          error);
    }

    ec.clear();
    if (std::filesystem::exists(final_dir, ec))
    {
      std::filesystem::remove_all(final_dir, ec);
      if (ec)
      {
        if (error != nullptr)
        {
          *error = "remove invalid snapshot dir failed: " + final_dir.string() + ": " +
                   ec.message();
        }
        std::filesystem::remove_all(staging_dir, ec);
        return false;
      }
      if (!SyncDirectory(root, error))
      {
        std::filesystem::remove_all(staging_dir, ec);
        return false;
      }
    }
    if (ec)
    {
      if (error != nullptr)
      {
        *error = "check invalid snapshot dir failed: " + final_dir.string() + ": " + ec.message();
      }
      std::filesystem::remove_all(staging_dir, ec);
      return false;
    }

    ec.clear();
    std::filesystem::rename(staging_dir, final_dir, ec);
    if (ec)
    {
      if (error != nullptr)
      {
        *error = "publish snapshot staging dir failed: " + staging_dir.string() + " -> " +
                 final_dir.string() + ": " + ec.message();
      }
      std::filesystem::remove_all(staging_dir, ec);
      return false;
    }
    const std::filesystem::path data_path = final_dir / kSnapshotDataFileName;
    const std::filesystem::path meta_path = final_dir / kSnapshotMetaFileName;

    const SnapshotStorageFailPoint failpoint = ActiveSnapshotStorageFailPoint();
    if (failpoint == SnapshotStorageFailPoint::kParentDirectorySyncAfterPublish ||
        failpoint == SnapshotStorageFailPoint::kPublishVisibleBeforeTrustedDirectorySync)
    {
      std::string detail;
      RemovePathIfExists(meta_path, &detail);
      const std::string operation =
          failpoint == SnapshotStorageFailPoint::kParentDirectorySyncAfterPublish
              ? "snapshot parent directory sync after publish"
              : "snapshot publish visible before trusted directory sync";
      const std::filesystem::path failure_path =
          failpoint == SnapshotStorageFailPoint::kParentDirectorySyncAfterPublish
              ? root
              : final_dir;
      const std::string trusted_state_expectation =
          failpoint == SnapshotStorageFailPoint::kParentDirectorySyncAfterPublish
              ? "if the new snapshot directory becomes visible but the parent directory sync does not complete, restart must stay on the last fully durable trusted snapshot boundary"
              : "if restart sees a newer snapshot publish point without the required trusted publish completion, it must reject that snapshot and continue from the previous trusted snapshot plus replayable log tail";
      return BuildInjectedSnapshotFailure(operation,
                                          failure_path,
                                          "directory sync",
                                          trusted_state_expectation,
                                          trusted_state_expectation,
                                          failpoint == SnapshotStorageFailPoint::kParentDirectorySyncAfterPublish
                                              ? "error should identify that the snapshot directory became visible but the parent directory sync boundary did not complete"
                                              : "error should identify that the newer snapshot publish point became visible without a trusted parent directory sync boundary",
                                          detail,
                                          error);
    }
    if (!SyncDirectory(root, error))
    {
      return false;
    }

    ec.clear();
    std::filesystem::remove(input_snapshot_file, ec);

    if (out_meta != nullptr)
    {
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

  bool FileSnapshotStorage::ListSnapshots(std::vector<SnapshotMeta> *out,
                                          std::string *error)
  {
    if (out == nullptr)
    {
      if (error != nullptr)
      {
        *error = "output snapshot list must not be null";
      }
      return false;
    }
    out->clear();

    SnapshotListResult result;
    if (!ListSnapshotsWithDiagnostics(&result, error))
    {
      return false;
    }
    *out = std::move(result.snapshots);
    return true;
  }

  bool FileSnapshotStorage::ListSnapshotsWithDiagnostics(SnapshotListResult *out,
                                                         std::string *error)
  {
    if (out == nullptr)
    {
      if (error != nullptr)
      {
        *error = "snapshot list result must not be null";
      }
      return false;
    }
    out->snapshots.clear();
    out->validation_issues.clear();

    std::error_code ec;
    if (!std::filesystem::exists(snapshot_dir_, ec))
    {
      return true;
    }

    for (const auto &entry : std::filesystem::directory_iterator(snapshot_dir_, ec))
    {
      if (ec)
      {
        if (error != nullptr)
        {
          *error = "iterate snapshot dir failed: " + ec.message();
        }
        return false;
      }

      SnapshotMeta meta;
      std::string local_error;
      if (entry.is_directory())
      {
        const std::string name = entry.path().filename().string();
        if (StartsWith(name, kSnapshotStagingPrefix))
        {
          out->validation_issues.push_back(
              SnapshotValidationIssue{entry.path().string(),
                                      "staging snapshot directory ignored"});
          continue;
        }
        if (!StartsWith(name, kSnapshotPrefix))
        {
          continue;
        }
        if (!ReadDirectorySnapshot(entry.path(), &meta, &local_error))
        {
          out->validation_issues.push_back(
              SnapshotValidationIssue{entry.path().string(), local_error});
          continue;
        }
        out->snapshots.push_back(std::move(meta));
        continue;
      }

      // Backward compatibility with the old flat snapshot_<...>.bin/.meta
      // layout. These snapshots do not have checksums, but can still be loaded.
      if (entry.is_regular_file() && entry.path().extension() == ".meta")
      {
        if (!ReadLegacyFlatSnapshot(entry.path(), &meta, &local_error))
        {
          out->validation_issues.push_back(
              SnapshotValidationIssue{entry.path().string(), local_error});
          continue;
        }
        out->snapshots.push_back(std::move(meta));
      }
    }

    std::sort(out->snapshots.begin(), out->snapshots.end(), [](const SnapshotMeta &a, const SnapshotMeta &b)
              {
      if (a.last_included_index != b.last_included_index) {
        return a.last_included_index > b.last_included_index;
      }
      return a.created_unix_ms > b.created_unix_ms; });
    return true;
  }

  bool FileSnapshotStorage::LoadLatestValidSnapshot(SnapshotMeta *out_meta,
                                                    bool *has_snapshot,
                                                    std::string *error)
  {
    if (has_snapshot == nullptr)
    {
      if (error != nullptr)
      {
        *error = "has_snapshot must not be null";
      }
      return false;
    }
    *has_snapshot = false;

    std::vector<SnapshotMeta> snapshots;
    SnapshotListResult result;
    if (!ListSnapshotsWithDiagnostics(&result, error))
    {
      return false;
    }
    snapshots = std::move(result.snapshots);
    if (snapshots.empty())
    {
      return true;
    }

    if (out_meta != nullptr)
    {
      *out_meta = snapshots.front();
    }
    *has_snapshot = true;
    return true;
  }

  bool FileSnapshotStorage::PruneSnapshots(std::size_t max_keep,
                                           std::string *error)
  {
    if (max_keep == 0)
    {
      max_keep = 1;
    }

    std::vector<SnapshotMeta> snapshots;
    if (!ListSnapshots(&snapshots, error))
    {
      return false;
    }
    if (snapshots.size() <= max_keep)
    {
      return true;
    }

    std::error_code ec;
    bool removed_any = false;
    for (std::size_t i = max_keep; i < snapshots.size(); ++i)
    {
      if (!snapshots[i].snapshot_dir.empty())
      {
        if (ActiveSnapshotStorageFailPoint() ==
            SnapshotStorageFailPoint::kPruneRemoveOldDirectory)
        {
          return BuildInjectedSnapshotFailure(
              "snapshot prune remove old directory",
              snapshots[i].snapshot_dir,
              "prune/remove",
              "if pruning old snapshots fails during remove/publish cleanup, the newest trusted snapshot must remain loadable and restart must not mis-classify prune leftovers as the chosen trusted snapshot",
              "restart should keep the newest trusted snapshot loadable and should not let prune leftovers win trusted snapshot selection",
              "error should identify that pruning the old snapshot directory failed before cleanup completed",
              "old snapshot directory removal was rejected before mutating trusted snapshot selection",
              error);
        }
        ec.clear();
        std::filesystem::remove_all(snapshots[i].snapshot_dir, ec);
        if (ec && error != nullptr)
        {
          *error = "remove old snapshot dir failed: " + ec.message();
          return false;
        }
        removed_any = true;
      }
      else
      {
        ec.clear();
        std::filesystem::remove(snapshots[i].snapshot_path, ec);
        if (ec && error != nullptr)
        {
          *error = "remove old snapshot data failed: " + ec.message();
          return false;
        }
        ec.clear();
        std::filesystem::remove(snapshots[i].meta_path, ec);
        if (ec && error != nullptr)
        {
          *error = "remove old snapshot meta failed: " + ec.message();
          return false;
        }
        removed_any = true;
      }
    }
    if (removed_any && !SyncDirectory(snapshot_dir_, error))
    {
      return false;
    }
    return true;
  }

  const std::string &FileSnapshotStorage::SnapshotDir() const
  {
    return snapshot_dir_;
  }

  bool FileSnapshotStorage::WriteMeta(const std::filesystem::path &meta_path,
                                      std::uint64_t last_included_index,
                                      std::uint64_t last_included_term,
                                      std::uint64_t created_unix_ms,
                                      const std::string &data_file_name,
                                      std::uint32_t checksum,
                                      std::string *error)
  {
    std::ofstream out(meta_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
      if (error != nullptr)
      {
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
        !WriteString(out, data_file_name, error))
    {
      return false;
    }
    out.flush();
    if (!out)
    {
      if (error != nullptr)
      {
        *error = "flush snapshot meta file failed";
      }
      return false;
    }
    out.close();
    if (!out)
    {
      if (error != nullptr)
      {
        *error = "close snapshot meta file failed: " + meta_path.string();
      }
      return false;
    }
    if (!SyncFile(meta_path, error))
    {
      return false;
    }
    return true;
  }

  bool FileSnapshotStorage::ReadDirectorySnapshot(const std::filesystem::path &snapshot_dir,
                                                  SnapshotMeta *out_meta,
                                                  std::string *error)
  {
    if (out_meta == nullptr)
    {
      if (error != nullptr)
      {
        *error = "snapshot meta output must not be null";
      }
      return false;
    }

    const std::filesystem::path meta_path = snapshot_dir / kSnapshotMetaFileName;
    std::ifstream in(meta_path, std::ios::binary);
    if (!in.is_open())
    {
      if (error != nullptr)
      {
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
        !ReadString(in, &data_file_name, error))
    {
      return false;
    }

    if (magic != kMetaMagic || version != kMetaVersion)
    {
      if (error != nullptr)
      {
        std::ostringstream oss;
        oss << "invalid snapshot meta header: path=" << meta_path.string()
            << ", magic=" << magic << ", version=" << version;
        *error = oss.str();
      }
      return false;
    }
    if (data_file_name.empty())
    {
      if (error != nullptr)
      {
        *error = "snapshot data file name is empty: meta_path=" + meta_path.string();
      }
      return false;
    }

    const std::string name = snapshot_dir.filename().string();
    if (StartsWith(name, kSnapshotPrefix))
    {
      const std::string index_part = name.substr(std::string(kSnapshotPrefix).size());
      try
      {
        const std::uint64_t dir_index = static_cast<std::uint64_t>(std::stoull(index_part));
        if (dir_index != last_included_index)
        {
          if (error != nullptr)
          {
            std::ostringstream oss;
            oss << "snapshot directory index does not match meta: snapshot_dir="
                << snapshot_dir.string() << ", dir_index=" << dir_index
                << ", meta_index=" << last_included_index;
            *error = oss.str();
          }
          return false;
        }
      }
      catch (...)
      {
        if (error != nullptr)
        {
          *error = "invalid snapshot directory index: snapshot_dir=" + snapshot_dir.string();
        }
        return false;
      }
    }

    const std::filesystem::path data_path = snapshot_dir / data_file_name;
    std::error_code ec;
    if (!std::filesystem::exists(data_path, ec) || ec)
    {
      if (error != nullptr)
      {
        *error = "snapshot data file missing: " + data_path.string();
      }
      return false;
    }

    std::uint32_t actual_checksum = 0;
    if (!ComputeFileChecksum(data_path, &actual_checksum, error))
    {
      return false;
    }
    if (actual_checksum != checksum)
    {
      if (error != nullptr)
      {
        std::ostringstream oss;
        oss << "snapshot checksum mismatch: data_path=" << data_path.string()
            << ", expected=" << checksum << ", actual=" << actual_checksum;
        *error = oss.str();
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

  bool FileSnapshotStorage::ReadLegacyFlatSnapshot(const std::filesystem::path &meta_path,
                                                   SnapshotMeta *out_meta,
                                                   std::string *error)
  {
    if (out_meta == nullptr)
    {
      if (error != nullptr)
      {
        *error = "snapshot meta output must not be null";
      }
      return false;
    }

    std::ifstream in(meta_path, std::ios::binary);
    if (!in.is_open())
    {
      if (error != nullptr)
      {
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
        !ReadPod(in, &out_meta->last_included_term, error))
    {
      return false;
    }

    if (magic != kLegacyMetaMagic || version != kLegacyMetaVersion)
    {
      if (error != nullptr)
      {
        *error = "invalid legacy snapshot meta header";
      }
      return false;
    }

    const std::filesystem::path data_path = meta_path.parent_path() / (meta_path.stem().string() + ".bin");
    std::error_code ec;
    if (!std::filesystem::exists(data_path, ec) || ec)
    {
      if (error != nullptr)
      {
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

  std::unique_ptr<ISnapshotStorage> CreateFileSnapshotStorage(std::string snapshot_dir,
                                                              std::string file_prefix)
  {
    return std::make_unique<FileSnapshotStorage>(std::move(snapshot_dir), std::move(file_prefix));
  }

} // namespace raftdemo
