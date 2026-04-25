#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace raftdemo
{

  struct LogRecord;

  struct PersistentRaftState
  {
    std::uint64_t current_term{0};
    int voted_for{-1};
    std::uint64_t commit_index{0};
    std::uint64_t last_applied{0};
    std::vector<LogRecord> log;
  };

  class IRaftStorage
  {
  public:
    virtual ~IRaftStorage() = default;

    virtual bool Load(PersistentRaftState *state, bool *has_state, std::string *error) = 0;
    virtual bool Save(const PersistentRaftState &state, std::string *error) = 0;
    virtual const std::string &DataDir() const = 0;
  };

  // Cross-platform file storage.
  // Layout:
  //   data_dir/
  //     meta.bin
  //     log/
  //       segment_00000000000000000001.log
  //       segment_00000000000000000513.log
  //
  // Logs are split into segment files. Obsolete segment files are removed
  // automatically whenever RaftNode compacts log_ after snapshot. The storage
  // layer never deletes log records that still exist in PersistentRaftState::log,
  // because doing so would break Raft recovery and follower catch-up.
  std::unique_ptr<IRaftStorage> CreateFileRaftStorage(std::string data_dir);

} // namespace raftdemo
