#include "raft/raft_storage.h"

#include <algorithm>
#include <array>
#include <cstdint>
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

#include "raft/raft_node.h"

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

      ec.clear();
      std::filesystem::remove_all(backup, ec);
      if (ec && error != nullptr)
      {
        // Non-fatal for correctness. Keep the save successful but surface the message.
        *error = "saved state but failed to remove old log backup dir: " + ec.message();
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
        if (!ReadMeta(meta_path, state, &first_log_index, &last_log_index, &log_count, error))
        {
          return false;
        }

        std::vector<LogRecord> loaded;
        if (!LoadSegments(log_dir, first_log_index, last_log_index, log_count, &loaded, error))
        {
          return false;
        }

        state->log = std::move(loaded);
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
        if (!ReadPod(in, &magic, error) ||
            !ReadPod(in, &version, error) ||
            !ReadPod(in, &state->current_term, error) ||
            !ReadPod(in, &voted_for, error) ||
            !ReadPod(in, &state->commit_index, error) ||
            !ReadPod(in, &state->last_applied, error) ||
            !ReadPod(in, first_log_index, error) ||
            !ReadPod(in, last_log_index, error) ||
            !ReadPod(in, log_count, error))
        {
          return false;
        }
        if (magic != kMetaMagic)
        {
          if (error != nullptr)
          {
            *error = "invalid raft meta magic";
          }
          return false;
        }
        if (version != kMetaVersion)
        {
          if (error != nullptr)
          {
            *error = "unsupported raft meta version";
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
        return true;
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
        for (const auto &[start_index, path] : files)
        {
          (void)start_index;
          if (tail_truncated)
          {
            // A previous segment had a bad tail. Later segments cannot be trusted.
            std::filesystem::remove(path, ec);
            continue;
          }
          if (!ReadSegmentFile(path, first_log_index, last_log_index, records, &tail_truncated, error))
          {
            return false;
          }
        }

        if (records->size() != expected_count)
        {
          if (error != nullptr)
          {
            std::ostringstream oss;
            oss << "log count mismatch, expected=" << expected_count << ", actual=" << records->size();
            *error = oss.str();
          }
          return false;
        }
        if (!records->empty())
        {
          if (records->front().index != first_log_index || records->back().index != last_log_index)
          {
            if (error != nullptr)
            {
              *error = "log boundary mismatch while loading segments";
            }
            return false;
          }
          for (std::size_t i = 1; i < records->size(); ++i)
          {
            if ((*records)[i].index != (*records)[i - 1].index + 1)
            {
              if (error != nullptr)
              {
                *error = "non-contiguous log indexes while loading segments";
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
            TruncateFile(path, last_good_pos);
            *tail_truncated = true;
            break;
          }
          if (header.magic != kSegmentMagic || header.version != kSegmentVersion ||
              header.type != kEntryTypeNormal || header.data_size > kMaxCommandBytes)
          {
            TruncateFile(path, last_good_pos);
            *tail_truncated = true;
            break;
          }

          std::string data;
          data.resize(header.data_size);
          if (header.data_size > 0)
          {
            in.read(data.data(), static_cast<std::streamsize>(header.data_size));
            if (!in)
            {
              TruncateFile(path, last_good_pos);
              *tail_truncated = true;
              break;
            }
          }

          const std::uint32_t checksum = ComputeEntryChecksum(header.index, header.term,
                                                             header.type, header.data_size, data);
          if (checksum != header.checksum)
          {
            TruncateFile(path, last_good_pos);
            *tail_truncated = true;
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

      void TruncateFile(const std::filesystem::path &path, std::uintmax_t size)
      {
        std::error_code ec;
        std::filesystem::resize_file(path, size, ec);
      }

      bool WriteSegments(const std::filesystem::path &log_dir,
                         const std::vector<LogRecord> &records,
                         std::string *error)
      {
        if (records.empty())
        {
          return true;
        }

        std::ofstream out;
        std::uint64_t current_segment_start = 0;
        std::uint64_t current_segment_count = 0;

        for (const auto &record : records)
        {
          if (!out.is_open() || current_segment_count >= kMaxEntriesPerSegment)
          {
            if (out.is_open())
            {
              out.flush();
              if (!out)
              {
                if (error != nullptr)
                {
                  *error = "flush segment file failed";
                }
                return false;
              }
              out.close();
            }
            current_segment_start = record.index;
            current_segment_count = 0;
            const std::filesystem::path segment_path = log_dir / SegmentFileName(current_segment_start);
            out.open(segment_path, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
              if (error != nullptr)
              {
                *error = "open segment file for write failed: " + segment_path.string();
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
          out.flush();
          if (!out)
          {
            if (error != nullptr)
            {
              *error = "flush final segment file failed";
            }
            return false;
          }
        }
        return true;
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
