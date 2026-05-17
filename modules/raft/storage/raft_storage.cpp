#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "raft/storage/raft_storage.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "raft/node/raft_node.h"

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

    constexpr std::uint32_t kLegacyFileMagic = 0x52465431U; // "RFT1"
    constexpr std::uint32_t kLegacyFileVersion = 2U;
    constexpr const char *kLegacyStateFileName = "raft_state.bin";

    constexpr std::uint32_t kMetaMagic = 0x524D5441U; // "RMTA"
    constexpr std::uint32_t kMetaVersion = 1U;
    constexpr const char *kMetaFileName = "meta.bin";
    constexpr const char *kMetaTempFileName = "meta.bin.tmp";

    constexpr std::uint32_t kSegmentMagic = 0x524C4F47U; // "RLOG"
    constexpr std::uint32_t kSegmentVersion = 1U;
    constexpr std::uint32_t kEntryTypeNormal = 1U;
    constexpr const char *kLogDirName = "log";
    constexpr const char *kLogTempDirName = "log.tmp";
    constexpr const char *kLogBackupDirName = "log.bak";

    // Medium-sized default for this project. Keeping this value small makes tests
    // easy to inspect while still avoiding a single giant log file.
    constexpr std::uint64_t kMaxEntriesPerSegment = 512;

    // Hard safety guard for corrupt input. Real Raft entries in this project are
    // small string commands, so 64 MiB is intentionally generous.
    constexpr std::uint64_t kMaxCommandBytes = 64ULL * 1024ULL * 1024ULL;

    constexpr const char *kStorageFailpointEnv = "RAFT_TEST_STORAGE_FAILPOINT";

    enum class StorageFailpoint
    {
      kNone,
      kMetaFileSync,
      kMetaDirectorySync,
      kLogDirectoryReplace,
      kLogDirectorySync,
      kFinalSegmentPartialWrite,
    };

    StorageFailpoint CurrentStorageFailpoint()
    {
      const char *value = std::getenv(kStorageFailpointEnv);
      if (value == nullptr)
      {
        return StorageFailpoint::kNone;
      }

      const std::string failpoint(value);
      if (failpoint == "meta_file_sync")
      {
        return StorageFailpoint::kMetaFileSync;
      }
      if (failpoint == "meta_directory_sync")
      {
        return StorageFailpoint::kMetaDirectorySync;
      }
      if (failpoint == "log_directory_replace")
      {
        return StorageFailpoint::kLogDirectoryReplace;
      }
      if (failpoint == "log_directory_sync")
      {
        return StorageFailpoint::kLogDirectorySync;
      }
      if (failpoint == "final_segment_partial_write")
      {
        return StorageFailpoint::kFinalSegmentPartialWrite;
      }
      return StorageFailpoint::kNone;
    }

    bool IsStorageFailpointActive(StorageFailpoint failpoint)
    {
      return CurrentStorageFailpoint() == failpoint;
    }

    std::string BuildInjectedFailureMessage(const char *operation,
                                           const std::filesystem::path &path,
                                           const char *failure_class,
                                           const char *trusted_state_expectation,
                                           const char *recovery_expectation,
                                           const char *diagnostic_expectation)
    {
      std::ostringstream oss;
      oss << "injected durability failure"
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
      return oss.str();
    }

    bool InjectFailureIfRequested(StorageFailpoint failpoint,
                                  const char *operation,
                                  const std::filesystem::path &path,
                                  const char *trusted_state_expectation,
                                  const char *failure_class,
                                  const char *diagnostic_expectation,
                                  std::string *error)
    {
      if (!IsStorageFailpointActive(failpoint))
      {
        return false;
      }

      if (error != nullptr)
      {
        *error = BuildInjectedFailureMessage(operation,
                                            path,
                                            failure_class,
                                            trusted_state_expectation,
                                            trusted_state_expectation,
                                            diagnostic_expectation);
      }
      return true;
    }

    void ClampRecoveredHardState(PersistentRaftState *state)
    {
      if (state == nullptr)
      {
        return;
      }

      const std::uint64_t recovered_last_log_index =
          state->log.empty() ? 0 : state->log.back().index;

      if (state->commit_index > recovered_last_log_index)
      {
        state->commit_index = recovered_last_log_index;
      }
      if (state->last_applied > recovered_last_log_index)
      {
        state->last_applied = recovered_last_log_index;
      }
      if (state->last_applied > state->commit_index)
      {
        state->last_applied = state->commit_index;
      }
    }

    struct SegmentEntryHeader
    {
      std::uint32_t magic{0};
      std::uint32_t version{0};
      std::uint64_t index{0};
      std::uint64_t term{0};
      std::uint32_t type{0};
      std::uint32_t data_size{0};
      std::uint32_t checksum{0};
    };

    template <typename T>
    bool WritePod(std::ostream &out, const T &value, std::string *error)
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
    bool ReadPod(std::istream &in, T *value, std::string *error)
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

    bool WriteString(std::ostream &out, const std::string &value, std::string *error)
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
          *error = "write string payload failed";
        }
        return false;
      }
      return true;
    }

    bool ReadString(std::istream &in, std::string *value, std::string *error)
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
      if (size > kMaxCommandBytes)
      {
        if (error != nullptr)
        {
          *error = "string payload too large";
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
          *error = in.eof() ? "unexpected EOF while reading string payload"
                            : "read string payload failed";
        }
        return false;
      }
      return true;
    }

    std::uint32_t Fnv1aAppend(std::uint32_t hash, const void *data, std::size_t size)
    {
      const auto *bytes = static_cast<const unsigned char *>(data);
      for (std::size_t i = 0; i < size; ++i)
      {
        hash ^= static_cast<std::uint32_t>(bytes[i]);
        hash *= 16777619U;
      }
      return hash;
    }

    template <typename T>
    std::uint32_t Fnv1aAppendPod(std::uint32_t hash, const T &value)
    {
      static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
      return Fnv1aAppend(hash, &value, sizeof(T));
    }

    std::uint32_t ComputeEntryChecksum(std::uint64_t index,
                                       std::uint64_t term,
                                       std::uint32_t type,
                                       std::uint32_t data_size,
                                       const std::string &data)
    {
      std::uint32_t hash = 2166136261U;
      hash = Fnv1aAppendPod(hash, index);
      hash = Fnv1aAppendPod(hash, term);
      hash = Fnv1aAppendPod(hash, type);
      hash = Fnv1aAppendPod(hash, data_size);
      if (!data.empty())
      {
        hash = Fnv1aAppend(hash, data.data(), data.size());
      }
      return hash;
    }

    std::string SegmentFileName(std::uint64_t first_index)
    {
      std::ostringstream oss;
      oss << "segment_" << std::setw(20) << std::setfill('0') << first_index << ".log";
      return oss.str();
    }

    std::string DescribeMetaLogBoundary(const std::filesystem::path &meta_path,
                                        std::uint64_t first_log_index,
                                        std::uint64_t last_log_index,
                                        std::uint64_t log_count)
    {
      std::ostringstream oss;
      oss << "meta_path=" << meta_path.string()
          << ", first_log_index=" << first_log_index
          << ", last_log_index=" << last_log_index
          << ", log_count=" << log_count;
      return oss.str();
    }

    std::string DescribeSegmentFiles(
        const std::vector<std::pair<std::uint64_t, std::filesystem::path>> &files)
    {
      std::ostringstream oss;
      oss << '[';
      for (std::size_t i = 0; i < files.size(); ++i)
      {
        if (i > 0)
        {
          oss << ", ";
        }
        oss << files[i].second.filename().string();
      }
      oss << ']';
      return oss.str();
    }

    bool ParseSegmentStartIndex(const std::filesystem::path &path, std::uint64_t *index)
    {
      if (index == nullptr)
      {
        return false;
      }
      const std::string name = path.filename().string();
      constexpr std::size_t prefix_size = 8; // "segment_"
      constexpr std::size_t suffix_size = 4; // ".log"
      if (name.size() <= prefix_size + suffix_size ||
          name.rfind("segment_", 0) != 0 ||
          name.substr(name.size() - suffix_size) != ".log")
      {
        return false;
      }
      const std::string digits = name.substr(prefix_size, name.size() - prefix_size - suffix_size);
      if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char ch)
                                         { return ch >= '0' && ch <= '9'; }))
      {
        return false;
      }
      try
      {
        *index = static_cast<std::uint64_t>(std::stoull(digits));
      }
      catch (...)
      {
        return false;
      }
      return true;
    }

    bool SyncDirectory(const std::filesystem::path &path, std::string *error);

    bool ReplaceFile(const std::filesystem::path &src,
                     const std::filesystem::path &dst,
                     std::string *error)
    {
      std::error_code ec;
      if (std::filesystem::exists(dst, ec))
      {
        ec.clear();
        std::filesystem::remove(dst, ec);
        if (ec)
        {
          if (error != nullptr)
          {
            *error = "remove old file failed: " + dst.string() + ": " + ec.message();
          }
          return false;
        }
      }

      ec.clear();
      std::filesystem::rename(src, dst, ec);
      if (ec)
      {
        if (error != nullptr)
        {
          *error = "rename file failed: " + src.string() + " -> " + dst.string() + ": " + ec.message();
        }
        return false;
      }

      const std::filesystem::path parent_dir = dst.parent_path();
      if (InjectFailureIfRequested(StorageFailpoint::kMetaDirectorySync,
                                   "meta.bin parent directory sync after replace",
                                   parent_dir,
                                   "restart must stay on the previously trusted hard-state boundary if the new meta publish reached rename/replace but the parent directory sync did not complete",
                                   "directory sync",
                                   "error should identify that meta.bin reached rename/replace but the parent directory sync boundary did not complete",
                                   error))
      {
        return false;
      }
      if (!parent_dir.empty() && !SyncDirectory(parent_dir, error))
      {
        return false;
      }
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
          *error = "CreateFileW for file flush failed: " + path.string() + ": " + LastSystemErrorMessage();
        }
        return false;
      }

      if (!::FlushFileBuffers(handle))
      {
        const std::string message = LastSystemErrorMessage();
        ::CloseHandle(handle);
        if (error != nullptr)
        {
          *error = "FlushFileBuffers for file failed: " + path.string() + ": " + message;
        }
        return false;
      }

      if (!::CloseHandle(handle))
      {
        if (error != nullptr)
        {
          *error = "CloseHandle after file flush failed: " + path.string() + ": " + LastSystemErrorMessage();
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
          *error = "open file for fsync failed: " + path.string() + ": " + LastSystemErrorMessage();
        }
        return false;
      }

      if (::fsync(fd) != 0)
      {
        const std::string message = LastSystemErrorMessage();
        ::close(fd);
        if (error != nullptr)
        {
          *error = "fsync file failed: " + path.string() + ": " + message;
        }
        return false;
      }

      if (::close(fd) != 0)
      {
        if (error != nullptr)
        {
          *error = "close file after fsync failed: " + path.string() + ": " + LastSystemErrorMessage();
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
          *error = "CreateFileW for directory flush failed: " + path.string() + ": " + LastSystemErrorMessage();
        }
        return false;
      }

      if (!::FlushFileBuffers(handle))
      {
        const std::string message = LastSystemErrorMessage();
        ::CloseHandle(handle);
        if (error != nullptr)
        {
          *error = "FlushFileBuffers for directory failed: " + path.string() + ": " + message;
        }
        return false;
      }

      if (!::CloseHandle(handle))
      {
        if (error != nullptr)
        {
          *error = "CloseHandle after directory flush failed: " + path.string() + ": " + LastSystemErrorMessage();
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
          *error = "open directory for fsync failed: " + path.string() + ": " + LastSystemErrorMessage();
        }
        return false;
      }

      if (::fsync(fd) != 0)
      {
        const std::string message = LastSystemErrorMessage();
        ::close(fd);
        if (error != nullptr)
        {
          *error = "fsync directory failed: " + path.string() + ": " + message;
        }
        return false;
      }

      if (::close(fd) != 0)
      {
        if (error != nullptr)
        {
          *error = "close directory after fsync failed: " + path.string() + ": " + LastSystemErrorMessage();
        }
        return false;
      }
      return true;
#endif
    }

    bool FlushAndSyncSegmentFile(std::ofstream *out,
                                 const std::filesystem::path &segment_path,
                                 bool is_final_segment,
                                 std::string *error)
    {
      if (out == nullptr || !out->is_open())
      {
        return true;
      }

      out->flush();
      if (!(*out))
      {
        if (error != nullptr)
        {
          *error = is_final_segment ? "flush final segment file failed"
                                    : "flush segment file failed";
        }
        return false;
      }

      if (is_final_segment &&
          IsStorageFailpointActive(StorageFailpoint::kFinalSegmentPartialWrite))
      {
        const char partial_header = '\x42';
        out->write(&partial_header, 1);
        out->flush();
        out->close();
        if (error != nullptr)
        {
          *error = BuildInjectedFailureMessage(
              "final segment partial write during save",
              segment_path,
              "partial write",
              "save must fail before publishing the new log directory, so restart stays on the previously durable log boundary and ignores the partially written temp segment",
              "save must fail before publishing the new log directory, so restart stays on the previously durable log boundary and ignores the partially written temp segment",
              "error should identify that the final temp segment write stopped at a partial-record boundary and that recovery must reject the untrusted tail");
        }
        return false;
      }

      if (!SyncFile(segment_path, error))
      {
        return false;
      }

      out->close();
      return true;
    }

    bool ReplaceDirectory(const std::filesystem::path &src,
                          const std::filesystem::path &dst,
                          const std::filesystem::path &backup,
                          std::string *error)
    {
      std::error_code ec;
      std::filesystem::remove_all(backup, ec);
      if (ec)
      {
        if (error != nullptr)
        {
          *error = "remove old log backup dir failed: " + ec.message();
        }
        return false;
      }

      ec.clear();
      if (std::filesystem::exists(dst, ec))
      {
        ec.clear();
        std::filesystem::rename(dst, backup, ec);
        if (ec)
        {
          if (error != nullptr)
          {
            *error = "rename old log dir to backup failed: " + ec.message();
          }
          return false;
        }
      }

      ec.clear();
      if (InjectFailureIfRequested(StorageFailpoint::kLogDirectoryReplace,
                                   "log directory replace",
                                   dst,
                                   "restart must keep the previously durable segment set and must not trust a partially published newer log boundary when replacing log/ fails",
                                   "replace/rename",
                                   "error should identify that replacing log/ failed before the newer segment tree became the trusted published boundary",
                                   error))
      {
        return false;
      }
      std::filesystem::rename(src, dst, ec);
      if (ec)
      {
        std::error_code restore_ec;
        if (std::filesystem::exists(backup, restore_ec) && !std::filesystem::exists(dst, restore_ec))
        {
          std::filesystem::rename(backup, dst, restore_ec);
        }
        if (error != nullptr)
        {
          *error = "rename new log dir failed: " + ec.message();
        }
        return false;
      }

      const std::filesystem::path parent_dir = dst.parent_path();
      if (InjectFailureIfRequested(StorageFailpoint::kLogDirectorySync,
                                   "log parent directory sync after publish",
                                   parent_dir,
                                   "restart must stay bounded by the last fully durable segment publish if the new log tree becomes visible but the parent directory sync does not complete",
                                   "directory sync",
                                   "error should identify that the parent directory sync boundary failed after the newer log tree became visible",
                                   error))
      {
        return false;
      }
      if (!parent_dir.empty() && !SyncDirectory(parent_dir, error))
      {
        return false;
      }

      ec.clear();
      std::filesystem::remove_all(backup, ec);
      if (ec && error != nullptr)
      {
        // Non-fatal for correctness. Keep the save successful but surface the message.
        *error = "saved state but failed to remove old log backup dir: " + ec.message();
      }
      else if (!parent_dir.empty())
      {
        std::string sync_error;
        if (!SyncDirectory(parent_dir, &sync_error) && error != nullptr)
        {
          *error = sync_error;
        }
      }
      return true;
    }

    class FileRaftStorage final : public IRaftStorage
    {
    public:
      explicit FileRaftStorage(std::string data_dir) : data_dir_(std::move(data_dir)) {}

      bool Load(PersistentRaftState *state, bool *has_state, std::string *error) override
      {
        if (state == nullptr || has_state == nullptr)
        {
          if (error != nullptr)
          {
            *error = "Load arguments must not be null";
          }
          return false;
        }

        *has_state = false;
        state->current_term = 0;
        state->voted_for = -1;
        state->commit_index = 0;
        state->last_applied = 0;
        state->log.clear();

        const std::filesystem::path dir_path(data_dir_);
        const std::filesystem::path meta_path = dir_path / kMetaFileName;
        const std::filesystem::path log_dir = dir_path / kLogDirName;

        if (std::filesystem::exists(meta_path))
        {
          if (!LoadSegmented(meta_path, log_dir, state, has_state, error))
          {
            return false;
          }
          return true;
        }

        const std::filesystem::path legacy_path = dir_path / kLegacyStateFileName;
        if (std::filesystem::exists(legacy_path))
        {
          return LoadLegacy(legacy_path, state, has_state, error);
        }

        return true;
      }

      bool Save(const PersistentRaftState &state, std::string *error) override
      {
        std::error_code ec;
        const std::filesystem::path dir_path(data_dir_);
        std::filesystem::create_directories(dir_path, ec);
        if (ec)
        {
          if (error != nullptr)
          {
            *error = "create data dir failed: " + ec.message();
          }
          return false;
        }

        const std::filesystem::path log_dir = dir_path / kLogDirName;
        const std::filesystem::path log_tmp_dir = dir_path / kLogTempDirName;
        const std::filesystem::path log_backup_dir = dir_path / kLogBackupDirName;
        const std::filesystem::path meta_tmp_path = dir_path / kMetaTempFileName;
        const std::filesystem::path meta_path = dir_path / kMetaFileName;

        ec.clear();
        std::filesystem::remove_all(log_tmp_dir, ec);
        if (ec)
        {
          if (error != nullptr)
          {
            *error = "remove stale temp log dir failed: " + ec.message();
          }
          return false;
        }
        ec.clear();
        std::filesystem::create_directories(log_tmp_dir, ec);
        if (ec)
        {
          if (error != nullptr)
          {
            *error = "create temp log dir failed: " + ec.message();
          }
          return false;
        }

        if (!WriteSegments(log_tmp_dir, state.log, error))
        {
          return false;
        }
        if (!WriteMeta(meta_tmp_path, state, error))
        {
          return false;
        }

        // Replace log directory first, then meta. If the process dies between the
        // two operations, the next load either sees the old complete layout or a
        // new log dir with old meta. The meta still bounds what Load accepts.
        std::string replace_error;
        if (!ReplaceDirectory(log_tmp_dir, log_dir, log_backup_dir, &replace_error))
        {
          if (error != nullptr)
          {
            *error = replace_error;
          }
          return false;
        }
        if (!ReplaceFile(meta_tmp_path, meta_path, error))
        {
          return false;
        }

        // Remove legacy monolithic state after the segmented layout is durable
        // enough for this simplified implementation. This is automatic log-file
        // cleanup and is cross-platform through std::filesystem.
        ec.clear();
        std::filesystem::remove(dir_path / kLegacyStateFileName, ec);
        ec.clear();
        std::filesystem::remove(dir_path / "raft_state.bin.tmp", ec);
        return true;
      }

      const std::string &DataDir() const override { return data_dir_; }

    private:
      bool LoadSegmented(const std::filesystem::path &meta_path,
                         const std::filesystem::path &log_dir,
                         PersistentRaftState *state,
                         bool *has_state,
                         std::string *error)
      {
        std::uint64_t first_log_index = 0;
        std::uint64_t last_log_index = 0;
        std::uint64_t log_count = 0;
        std::string meta_error;
        if (!ReadMeta(meta_path, state, &first_log_index, &last_log_index, &log_count, &meta_error))
        {
          if (error != nullptr)
          {
            *error = "load segmented raft state failed: meta=" + meta_path.string() +
                     ", log_dir=" + log_dir.string() + ", reason=" + meta_error;
          }
          return false;
        }

        std::vector<LogRecord> loaded;
        std::string log_error;
        if (!LoadSegments(log_dir, first_log_index, last_log_index, log_count, &loaded, &log_error))
        {
          if (error != nullptr)
          {
            *error = "load segmented raft state failed: meta=" + meta_path.string() +
                     ", log_dir=" + log_dir.string() + ", " +
                     DescribeMetaLogBoundary(meta_path, first_log_index, last_log_index, log_count) +
                     ", reason=" + log_error;
          }
          return false;
        }

        state->log = std::move(loaded);
        ClampRecoveredHardState(state);
        *has_state = true;
        return true;
      }

      bool ReadMeta(const std::filesystem::path &meta_path,
                    PersistentRaftState *state,
                    std::uint64_t *first_log_index,
                    std::uint64_t *last_log_index,
                    std::uint64_t *log_count,
                    std::string *error)
      {
        std::ifstream in(meta_path, std::ios::binary);
        if (!in.is_open())
        {
          if (error != nullptr)
          {
            *error = "open meta file failed: " + meta_path.string();
          }
          return false;
        }

        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::int64_t voted_for = -1;
        auto read_field = [&](const char *field_name, auto *target)
        {
          std::string field_error;
          if (!ReadPod(in, target, &field_error))
          {
            if (error != nullptr)
            {
              *error = "read meta field failed: path=" + meta_path.string() +
                       ", field=" + field_name + ", reason=" + field_error;
            }
            return false;
          }
          return true;
        };
        if (!read_field("magic", &magic) ||
            !read_field("version", &version) ||
            !read_field("current_term", &state->current_term) ||
            !read_field("voted_for", &voted_for) ||
            !read_field("commit_index", &state->commit_index) ||
            !read_field("last_applied", &state->last_applied) ||
            !read_field("first_log_index", first_log_index) ||
            !read_field("last_log_index", last_log_index) ||
            !read_field("log_count", log_count))
        {
          return false;
        }
        if (magic != kMetaMagic)
        {
          if (error != nullptr)
          {
            std::ostringstream oss;
            oss << "invalid raft meta magic: path=" << meta_path.string()
                << ", magic=" << magic;
            *error = oss.str();
          }
          return false;
        }
        if (version != kMetaVersion)
        {
          if (error != nullptr)
          {
            std::ostringstream oss;
            oss << "unsupported raft meta version: path=" << meta_path.string()
                << ", version=" << version;
            *error = oss.str();
          }
          return false;
        }
        if (*log_count == 0)
        {
          if (*first_log_index != 0 || *last_log_index != 0)
          {
            if (error != nullptr)
            {
              *error = "meta log boundary invariant failed: " +
                       DescribeMetaLogBoundary(meta_path, *first_log_index, *last_log_index, *log_count);
            }
            return false;
          }
        }
        else if (*last_log_index < *first_log_index ||
                 (*last_log_index - *first_log_index + 1) != *log_count)
        {
          if (error != nullptr)
          {
            *error = "meta log boundary invariant failed: " +
                     DescribeMetaLogBoundary(meta_path, *first_log_index, *last_log_index, *log_count);
          }
          return false;
        }
        state->voted_for = static_cast<int>(voted_for);
        return true;
      }

      bool WriteMeta(const std::filesystem::path &meta_path,
                     const PersistentRaftState &state,
                     std::string *error)
      {
        std::ofstream out(meta_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
          if (error != nullptr)
          {
            *error = "open temp meta file failed: " + meta_path.string();
          }
          return false;
        }

        const std::uint32_t magic = kMetaMagic;
        const std::uint32_t version = kMetaVersion;
        const std::int64_t voted_for = static_cast<std::int64_t>(state.voted_for);
        const std::uint64_t first_log_index = state.log.empty() ? 0 : state.log.front().index;
        const std::uint64_t last_log_index = state.log.empty() ? 0 : state.log.back().index;
        const std::uint64_t log_count = static_cast<std::uint64_t>(state.log.size());

        if (!WritePod(out, magic, error) ||
            !WritePod(out, version, error) ||
            !WritePod(out, state.current_term, error) ||
            !WritePod(out, voted_for, error) ||
            !WritePod(out, state.commit_index, error) ||
            !WritePod(out, state.last_applied, error) ||
            !WritePod(out, first_log_index, error) ||
            !WritePod(out, last_log_index, error) ||
            !WritePod(out, log_count, error))
        {
          return false;
        }
        out.flush();
        if (!out)
        {
          if (error != nullptr)
          {
            *error = "flush temp meta file failed";
          }
          return false;
        }
        out.close();
        if (!out)
        {
          if (error != nullptr)
          {
            *error = "close temp meta file failed";
        }
        return false;
      }
      if (InjectFailureIfRequested(StorageFailpoint::kMetaFileSync,
                                   "meta.bin file sync",
                                   meta_path,
                                   "restart must keep the previously durable term/vote/commit_index/last_applied and must not expose newer hard state when meta file data was not durably synced",
                                   "file sync",
                                   "error should identify that the meta temp file sync boundary failed before the new hard-state publish became durable",
                                   error))
      {
        return false;
      }
      return SyncFile(meta_path, error);
    }

      bool LoadSegments(const std::filesystem::path &log_dir,
                        std::uint64_t first_log_index,
                        std::uint64_t last_log_index,
                        std::uint64_t expected_count,
                        std::vector<LogRecord> *records,
                        std::string *error)
      {
        if (records == nullptr)
        {
          if (error != nullptr)
          {
            *error = "records must not be null";
          }
          return false;
        }
        records->clear();
        if (expected_count == 0)
        {
          return true;
        }
        if (!std::filesystem::exists(log_dir))
        {
          if (error != nullptr)
          {
            *error = "log dir does not exist: " + log_dir.string();
          }
          return false;
        }

        std::vector<std::pair<std::uint64_t, std::filesystem::path>> files;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(log_dir, ec))
        {
          if (ec)
          {
            if (error != nullptr)
            {
              *error = "iterate log dir failed: " + ec.message();
            }
            return false;
          }
          if (!entry.is_regular_file())
          {
            continue;
          }
          std::uint64_t start_index = 0;
          if (ParseSegmentStartIndex(entry.path(), &start_index))
          {
            files.emplace_back(start_index, entry.path());
          }
        }
        std::sort(files.begin(), files.end(), [](const auto &lhs, const auto &rhs)
                  { return lhs.first < rhs.first; });

        records->reserve(static_cast<std::size_t>(expected_count));
        bool tail_truncated = false;
        std::vector<std::string> recovery_diagnostics;
        for (const auto &[start_index, path] : files)
        {
          (void)start_index;
          if (tail_truncated)
          {
            // A previous segment had a bad tail. Later segments cannot be trusted.
            std::string cleanup = "removed_later_segment=" + path.string();
            std::filesystem::remove(path, ec);
            if (ec)
            {
              if (error != nullptr)
              {
                *error = cleanup + ", reason=" + ec.message();
              }
              return false;
            }
            recovery_diagnostics.push_back(std::move(cleanup));
            continue;
          }
          if (!ReadSegmentFile(path, first_log_index, last_log_index, records,
                               &tail_truncated, &recovery_diagnostics, error))
          {
            return false;
          }
        }

        if (records->size() != expected_count)
        {
          bool removed_later_segment = false;
          for (const auto &diagnostic : recovery_diagnostics)
          {
            if (diagnostic.rfind("removed_later_segment=", 0) == 0)
            {
              removed_later_segment = true;
              break;
            }
          }
          const bool accepted_truncated_prefix =
              tail_truncated &&
              !removed_later_segment &&
              ((records->empty() && first_log_index == 0) ||
               (!records->empty() && records->front().index == first_log_index));
          if (!accepted_truncated_prefix && error != nullptr)
          {
            std::ostringstream oss;
            oss << "log count mismatch, log_dir=" << log_dir.string()
                << ", expected_count=" << expected_count
                << ", actual_count=" << records->size()
                << ", expected_first=" << first_log_index
                << ", expected_last=" << last_log_index;
            if (!records->empty())
            {
              oss << ", actual_first=" << records->front().index
                  << ", actual_last=" << records->back().index;
            }
            oss << ", segment_files=" << DescribeSegmentFiles(files);
            for (const auto &diagnostic : recovery_diagnostics)
            {
              oss << ", " << diagnostic;
            }
            *error = oss.str();
          }
          if (!accepted_truncated_prefix)
          {
            return false;
          }
        }
        if (!records->empty())
        {
          if (records->front().index != first_log_index ||
              (!tail_truncated && records->back().index != last_log_index))
          {
            if (error != nullptr)
            {
              std::ostringstream oss;
                oss << "log boundary mismatch while loading segments, log_dir=" << log_dir.string()
                  << ", expected_first=" << first_log_index
                  << ", expected_last=" << last_log_index
                  << ", actual_first=" << records->front().index
                  << ", actual_last=" << records->back().index
                  << ", segment_files=" << DescribeSegmentFiles(files);
              *error = oss.str();
            }
            return false;
          }
          for (std::size_t i = 1; i < records->size(); ++i)
          {
            if ((*records)[i].index != (*records)[i - 1].index + 1)
            {
              if (error != nullptr)
              {
                std::ostringstream oss;
                oss << "non-contiguous log indexes while loading segments, log_dir="
                    << log_dir.string() << ", previous_index=" << (*records)[i - 1].index
                    << ", actual_index=" << (*records)[i].index
                    << ", segment_files=" << DescribeSegmentFiles(files);
                *error = oss.str();
              }
              return false;
            }
          }
        }
        return true;
      }

      bool ReadSegmentFile(const std::filesystem::path &path,
                           std::uint64_t first_log_index,
                           std::uint64_t last_log_index,
                           std::vector<LogRecord> *records,
                           bool *tail_truncated,
                           std::vector<std::string> *recovery_diagnostics,
                           std::string *error)
      {
        std::ifstream in(path, std::ios::binary);
        if (!in.is_open())
        {
          if (error != nullptr)
          {
            *error = "open segment file failed: " + path.string();
          }
          return false;
        }

        std::streamoff last_good_pos = 0;
        while (true)
        {
          const std::streamoff header_pos = static_cast<std::streamoff>(in.tellg());
          SegmentEntryHeader header{};
          in.read(reinterpret_cast<char *>(&header), sizeof(header));
          if (in.eof() && in.gcount() == 0)
          {
            break;
          }
          if (!in || in.gcount() != static_cast<std::streamsize>(sizeof(header)))
          {
            const std::string reason = "partial segment entry header";
            if (!TruncateFile(path, last_good_pos, error))
            {
              return false;
            }
            *tail_truncated = true;
            if (recovery_diagnostics != nullptr)
            {
              std::ostringstream oss;
              oss << "segment tail truncated, path=" << path.string()
                  << ", reason=" << reason
                  << ", truncate_offset=" << last_good_pos
                  << ", accepted_records=" << records->size();
              recovery_diagnostics->push_back(oss.str());
            }
            break;
          }
          if (header.magic != kSegmentMagic || header.version != kSegmentVersion ||
              header.type != kEntryTypeNormal || header.data_size > kMaxCommandBytes)
          {
            std::ostringstream reason;
            reason << "invalid segment entry header at offset=" << header_pos
                   << ", magic=" << header.magic
                   << ", version=" << header.version
                   << ", type=" << header.type
                   << ", data_size=" << header.data_size;
            if (!TruncateFile(path, last_good_pos, error))
            {
              return false;
            }
            *tail_truncated = true;
            if (recovery_diagnostics != nullptr)
            {
              std::ostringstream oss;
              oss << "segment tail truncated, path=" << path.string()
                  << ", reason=" << reason.str()
                  << ", truncate_offset=" << last_good_pos
                  << ", accepted_records=" << records->size();
              recovery_diagnostics->push_back(oss.str());
            }
            break;
          }

          std::string data;
          data.resize(header.data_size);
          if (header.data_size > 0)
          {
            in.read(data.data(), static_cast<std::streamsize>(header.data_size));
            if (!in)
            {
              const std::string reason = "partial segment entry payload";
              if (!TruncateFile(path, last_good_pos, error))
              {
                return false;
              }
              *tail_truncated = true;
              if (recovery_diagnostics != nullptr)
              {
                std::ostringstream oss;
                oss << "segment tail truncated, path=" << path.string()
                    << ", reason=" << reason
                    << ", truncate_offset=" << last_good_pos
                    << ", accepted_records=" << records->size();
                recovery_diagnostics->push_back(oss.str());
              }
              break;
            }
          }

          const std::uint32_t checksum = ComputeEntryChecksum(header.index, header.term,
                                                             header.type, header.data_size, data);
          if (checksum != header.checksum)
          {
            std::ostringstream reason;
            reason << "segment checksum mismatch at index=" << header.index
                   << ", expected=" << header.checksum
                   << ", actual=" << checksum;
            if (!TruncateFile(path, last_good_pos, error))
            {
              return false;
            }
            *tail_truncated = true;
            if (recovery_diagnostics != nullptr)
            {
              std::ostringstream oss;
              oss << "segment tail truncated, path=" << path.string()
                  << ", reason=" << reason.str()
                  << ", truncate_offset=" << last_good_pos
                  << ", accepted_records=" << records->size();
              recovery_diagnostics->push_back(oss.str());
            }
            break;
          }

          if (header.index >= first_log_index && header.index <= last_log_index)
          {
            records->push_back(LogRecord{header.index, header.term, std::move(data)});
          }
          last_good_pos = static_cast<std::streamoff>(in.tellg());
          if (header_pos == last_good_pos)
          {
            if (error != nullptr)
            {
              *error = "segment reader made no progress";
            }
            return false;
          }
        }
        return true;
      }

      bool TruncateFile(const std::filesystem::path &path, std::uintmax_t size, std::string *error)
      {
        std::error_code ec;
        std::filesystem::resize_file(path, size, ec);
        if (ec)
        {
          if (error != nullptr)
          {
            *error = "truncate segment file failed: " + path.string() + ": " + ec.message();
          }
          return false;
        }
        return SyncFile(path, error);
      }

      bool WriteSegments(const std::filesystem::path &log_dir,
                         const std::vector<LogRecord> &records,
                         std::string *error)
      {
        if (records.empty())
        {
          return SyncDirectory(log_dir, error);
        }

        std::ofstream out;
        std::filesystem::path current_segment_path;
        std::uint64_t current_segment_start = 0;
        std::uint64_t current_segment_count = 0;

        for (const auto &record : records)
        {
          if (!out.is_open() || current_segment_count >= kMaxEntriesPerSegment)
          {
            if (!current_segment_path.empty())
            {
              if (!FlushAndSyncSegmentFile(&out, current_segment_path, false, error))
              {
                return false;
              }
            }
            current_segment_start = record.index;
            current_segment_count = 0;
            current_segment_path = log_dir / SegmentFileName(current_segment_start);
            out.open(current_segment_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
              if (error != nullptr)
              {
                *error = "open segment file for write failed: " + current_segment_path.string();
              }
              return false;
            }
          }

          SegmentEntryHeader header{};
          header.magic = kSegmentMagic;
          header.version = kSegmentVersion;
          header.index = record.index;
          header.term = record.term;
          header.type = kEntryTypeNormal;
          header.data_size = static_cast<std::uint32_t>(record.command.size());
          if (record.command.size() > std::numeric_limits<std::uint32_t>::max())
          {
            if (error != nullptr)
            {
              *error = "log command too large";
            }
            return false;
          }
          header.checksum = ComputeEntryChecksum(header.index, header.term, header.type,
                                                 header.data_size, record.command);
          if (!WritePod(out, header, error))
          {
            return false;
          }
          if (!record.command.empty())
          {
            out.write(record.command.data(), static_cast<std::streamsize>(record.command.size()));
            if (!out)
            {
              if (error != nullptr)
              {
                *error = "write segment command failed";
              }
              return false;
            }
          }
          ++current_segment_count;
        }

        if (out.is_open())
        {
          if (!FlushAndSyncSegmentFile(&out, current_segment_path, true, error))
          {
            return false;
          }
        }
        return SyncDirectory(log_dir, error);
      }

      bool LoadLegacy(const std::filesystem::path &state_path,
                      PersistentRaftState *state,
                      bool *has_state,
                      std::string *error)
      {
        std::ifstream in(state_path, std::ios::binary);
        if (!in.is_open())
        {
          if (error != nullptr)
          {
            *error = "open legacy state file failed: " + state_path.string();
          }
          return false;
        }

        std::uint32_t magic = 0;
        std::uint32_t version = 0;
        std::int64_t voted_for = -1;
        std::uint64_t log_count = 0;

        if (!ReadPod(in, &magic, error) ||
            !ReadPod(in, &version, error) ||
            !ReadPod(in, &state->current_term, error) ||
            !ReadPod(in, &voted_for, error))
        {
          return false;
        }

        if (magic != kLegacyFileMagic)
        {
          if (error != nullptr)
          {
            *error = "invalid legacy raft state magic";
          }
          return false;
        }
        if (version != 1U && version != kLegacyFileVersion)
        {
          if (error != nullptr)
          {
            *error = "unsupported legacy raft state version";
          }
          return false;
        }

        state->voted_for = static_cast<int>(voted_for);
        if (version == 1U)
        {
          if (!ReadPod(in, &log_count, error))
          {
            return false;
          }
        }
        else
        {
          if (!ReadPod(in, &state->commit_index, error) ||
              !ReadPod(in, &state->last_applied, error) ||
              !ReadPod(in, &log_count, error))
          {
            return false;
          }
        }

        state->log.reserve(static_cast<std::size_t>(log_count));
        for (std::uint64_t i = 0; i < log_count; ++i)
        {
          LogRecord record{};
          if (!ReadPod(in, &record.index, error) ||
              !ReadPod(in, &record.term, error) ||
              !ReadString(in, &record.command, error))
          {
            return false;
          }
          state->log.push_back(std::move(record));
        }

        ClampRecoveredHardState(state);
        *has_state = true;
        return true;
      }

      std::string data_dir_;
    };

  } // namespace

  std::unique_ptr<IRaftStorage> CreateFileRaftStorage(std::string data_dir)
  {
    return std::make_unique<FileRaftStorage>(std::move(data_dir));
  }

} // namespace raftdemo
