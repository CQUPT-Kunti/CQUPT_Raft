#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "raft/common/command.h"
#include "raft/state_machine/state_machine_interface.h"

namespace raftdemo
{
    class KvStateMachine final : public IStateMachine
    {
    public:
        ApplyResult Apply(std::uint64_t index,
                          const std::string &command_data) override;

        SnapshotResult SaveSnapshot(const std::string &file_path) const override;
        SnapshotResult LoadSnapshot(const std::string &file_path) override;

        bool Get(const std::string &key, std::string *value) const;
        std::string DebugString() const;

    private:
        mutable std::mutex mu_;
        std::unordered_map<std::string, std::string> kv_;
    };
} // namespace raftdemo
