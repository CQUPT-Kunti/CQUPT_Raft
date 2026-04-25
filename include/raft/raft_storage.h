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

  std::unique_ptr<IRaftStorage> CreateFileRaftStorage(std::string data_dir);

} // namespace raftdemo