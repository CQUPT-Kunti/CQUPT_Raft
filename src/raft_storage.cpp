#include "raft/raft_storage.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include "raft/raft_node.h"

namespace raftdemo {
namespace {

constexpr std::uint32_t kFileMagic = 0x52465431U;   // "RFT1"
constexpr std::uint32_t kFileVersion = 1U;
constexpr const char* kStateFileName = "raft_state.bin";
constexpr const char* kTempFileName = "raft_state.bin.tmp";

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
      *error = "write string payload failed";
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

  value->clear();
  if (size == 0) {
    return true;
  }

  value->resize(static_cast<std::size_t>(size));
  in.read(value->data(), static_cast<std::streamsize>(size));
  if (!in) {
    if (error != nullptr) {
      *error = in.eof() ? "unexpected EOF while reading string payload"
                        : "read string payload failed";
    }
    return false;
  }
  return true;
}

class FileRaftStorage final : public IRaftStorage {
 public:
  explicit FileRaftStorage(std::string data_dir) : data_dir_(std::move(data_dir)) {}

  bool Load(PersistentRaftState* state, bool* has_state, std::string* error) override {
    if (state == nullptr || has_state == nullptr) {
      if (error != nullptr) {
        *error = "Load arguments must not be null";
      }
      return false;
    }

    *has_state = false;
    state->current_term = 0;
    state->voted_for = -1;
    state->log.clear();

    const std::filesystem::path state_path = std::filesystem::path(data_dir_) / kStateFileName;
    if (!std::filesystem::exists(state_path)) {
      return true;
    }

    std::ifstream in(state_path, std::ios::binary);
    if (!in.is_open()) {
      if (error != nullptr) {
        *error = "open state file failed: " + state_path.string();
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
        !ReadPod(in, &voted_for, error) ||
        !ReadPod(in, &log_count, error)) {
      return false;
    }

    if (magic != kFileMagic) {
      if (error != nullptr) {
        *error = "invalid raft state magic";
      }
      return false;
    }

    if (version != kFileVersion) {
      if (error != nullptr) {
        *error = "unsupported raft state version";
      }
      return false;
    }

    state->voted_for = static_cast<int>(voted_for);
    state->log.reserve(static_cast<std::size_t>(log_count));

    for (std::uint64_t i = 0; i < log_count; ++i) {
      LogRecord record{};
      if (!ReadPod(in, &record.index, error) ||
          !ReadPod(in, &record.term, error) ||
          !ReadString(in, &record.command, error)) {
        return false;
      }
      state->log.push_back(std::move(record));
    }

    *has_state = true;
    return true;
  }

  bool Save(const PersistentRaftState& state, std::string* error) override {
    std::error_code ec;
    std::filesystem::create_directories(data_dir_, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "create data dir failed: " + ec.message();
      }
      return false;
    }

    const std::filesystem::path dir_path(data_dir_);
    const std::filesystem::path temp_path = dir_path / kTempFileName;
    const std::filesystem::path state_path = dir_path / kStateFileName;

    {
      std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) {
        if (error != nullptr) {
          *error = "open temp state file failed: " + temp_path.string();
        }
        return false;
      }

      const std::uint32_t magic = kFileMagic;
      const std::uint32_t version = kFileVersion;
      const std::int64_t voted_for = static_cast<std::int64_t>(state.voted_for);
      const std::uint64_t log_count = static_cast<std::uint64_t>(state.log.size());

      if (!WritePod(out, magic, error) ||
          !WritePod(out, version, error) ||
          !WritePod(out, state.current_term, error) ||
          !WritePod(out, voted_for, error) ||
          !WritePod(out, log_count, error)) {
        return false;
      }

      for (const auto& record : state.log) {
        if (!WritePod(out, record.index, error) ||
            !WritePod(out, record.term, error) ||
            !WriteString(out, record.command, error)) {
          return false;
        }
      }

      out.flush();
      if (!out) {
        if (error != nullptr) {
          *error = "flush temp state file failed";
        }
        return false;
      }
    }

    // 为了兼容 Windows，目标文件存在时先删再 rename。
    // 这样做不是严格原子替换，但能保证跨平台可运行。
    ec.clear();
    if (std::filesystem::exists(state_path, ec)) {
      ec.clear();
      std::filesystem::remove(state_path, ec);
      if (ec) {
        if (error != nullptr) {
          *error = "remove old state file failed: " + ec.message();
        }
        return false;
      }
    }

    ec.clear();
    std::filesystem::rename(temp_path, state_path, ec);
    if (ec) {
      if (error != nullptr) {
        *error = "rename state file failed: " + ec.message();
      }
      return false;
    }

    return true;
  }

  const std::string& DataDir() const override { return data_dir_; }

 private:
  std::string data_dir_;
};

}  // namespace

std::unique_ptr<IRaftStorage> CreateFileRaftStorage(std::string data_dir) {
  return std::make_unique<FileRaftStorage>(std::move(data_dir));
}

}  // namespace raftdemo