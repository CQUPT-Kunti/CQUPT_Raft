#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "raft.pb.h"
#include "raft.grpc.pb.h"
#include "raft/command.h"
#include "raft/config.h"
#include "raft/min_heap_timer.h"
#include "raft/propose.h"
#include "raft/raft_storage.h"
#include "raft/snapshot_storage.h"
#include "raft/state_machine.h"
#include "raft/thread_pool.h"

namespace raftdemo
{

  class RaftServiceImpl;

  enum class Role
  {
    kFollower,
    kCandidate,
    kLeader,
  };

  struct LogRecord
  {
    std::uint64_t index;
    std::uint64_t term;
    std::string command;
  };

  class RaftNode : public std::enable_shared_from_this<RaftNode>
  {
  public:
    explicit RaftNode(NodeConfig config);
    RaftNode(NodeConfig config, snapshotConfig snapshot_config);
    RaftNode(NodeConfig config, std::unique_ptr<IStateMachine> state_machine);
    RaftNode(NodeConfig config, snapshotConfig snapshot_config,
             std::unique_ptr<IStateMachine> state_machine);
    ~RaftNode();

    void Start();
    void Stop();
    void Wait();

    void OnRequestVote(const raft::VoteRequest &request, raft::VoteResponse *response);
    void OnAppendEntries(const raft::AppendEntriesRequest &request,
                         raft::AppendEntriesResponse *response);
    void OnInstallSnapshot(const raft::InstallSnapshotRequest &request,
                           raft::InstallSnapshotResponse *response);

    std::string Describe() const;
    ProposeResult Propose(const Command &command);
    bool DebugGetValue(const std::string &key, std::string *value) const;

  private:
    struct PeerClient
    {
      int peer_id{0};
      std::string address;
      std::shared_ptr<grpc::Channel> channel;
      std::unique_ptr<raft::RaftService::Stub> stub;
      std::mutex mu;
    };

    void InitServer();
    void InitClients();

    void ResetElectionTimerLocked();
    void ResetHeartbeatTimerLocked();
    void ResetSnapshotTimerLocked();
    std::chrono::milliseconds RandomElectionTimeoutLocked();

    void OnElectionTimeout();
    void StartElection();
    void OnElectionWon(std::uint64_t term);
    void SendHeartbeats();
    void OnSnapshotTimer();

    bool BecomeFollowerLocked(std::uint64_t new_term, int new_leader, const std::string &reason);
    void BecomeLeaderLocked();

    bool IsCandidateLogUpToDateLocked(std::uint64_t last_log_index,
                                      std::uint64_t last_log_term) const;
    std::uint64_t FirstLogIndexLocked() const;
    std::uint64_t LastLogIndexLocked() const;
    std::uint64_t LastLogTermLocked() const;
    bool HasLogAtIndexLocked(std::uint64_t index) const;
    std::size_t LogOffsetLocked(std::uint64_t index) const;
    const LogRecord *LogAtIndexLocked(std::uint64_t index) const;
    std::uint64_t TermAtIndexLocked(std::uint64_t index) const;
    void CompactLogPrefixLocked(std::uint64_t last_included_index,
                                std::uint64_t last_included_term);

    std::optional<raft::VoteResponse> RequestVoteRpc(int peer_id, const raft::VoteRequest &request);
    std::optional<raft::AppendEntriesResponse> AppendEntriesRpc(
        int peer_id, const raft::AppendEntriesRequest &request);
    std::optional<raft::InstallSnapshotResponse> InstallSnapshotRpc(
        int peer_id, const raft::InstallSnapshotRequest &request);
    bool SendInstallSnapshotToPeer(int peer_id, std::uint64_t term);

    static const char *RoleName(Role role);

    bool ValidateCommandUnlocked(const Command &command, std::string *reason) const;
    std::uint64_t AppendLocalLogUnlocked(const std::string &command_data);
    bool ReplicateLogEntryToMajority(std::uint64_t log_index);
    void AdvanceCommitIndexUnlocked();
    ApplyResult ApplyCommittedEntries();
    bool PersistStateLocked(std::string *reason);
    bool ProposeNoOpEntry();

    void StartSnapshotWorker();
    void StopSnapshotWorker();
    void SnapshotWorkerLoop();
    void MaybeScheduleSnapshotLocked(bool force_by_timer);
    bool LoadLatestSnapshotOnStartup(std::string *reason);

    NodeConfig config_;
    snapshotConfig snapshot_config_;

    mutable std::mutex mu_;
    Role role_{Role::kFollower};
    std::uint64_t current_term_{0};
    int voted_for_{-1};
    int leader_id_{-1};

    std::vector<LogRecord> log_;
    std::uint64_t commit_index_{0};
    std::uint64_t last_applied_{0};

    std::uint64_t last_snapshot_index_{0};
    std::uint64_t last_snapshot_term_{0};

    std::unordered_map<int, std::uint64_t> next_index_;
    std::unordered_map<int, std::uint64_t> match_index_;

    std::unordered_map<int, std::unique_ptr<PeerClient>> clients_;

    TimerScheduler scheduler_;
    ThreadPool rpc_pool_{4};
    std::optional<TimerScheduler::TaskId> election_timer_id_;
    std::optional<TimerScheduler::TaskId> heartbeat_timer_id_;
    std::optional<TimerScheduler::TaskId> snapshot_timer_id_;

    std::mt19937 rng_;
    std::atomic<bool> running_{false};

    std::unique_ptr<RaftServiceImpl> service_;
    std::unique_ptr<grpc::Server> server_;

    std::mutex apply_mu_;
    std::unique_ptr<IStateMachine> state_machine_;
    std::unique_ptr<IRaftStorage> storage_;
    std::unique_ptr<ISnapshotStorage> snapshot_storage_;

    std::mutex snapshot_mu_;
    std::condition_variable snapshot_cv_;
    std::thread snapshot_worker_;
    bool snapshot_worker_stop_{false};
    bool snapshot_pending_{false};
    bool snapshot_in_progress_{false};
    std::uint64_t pending_snapshot_index_{0};
    std::uint64_t pending_snapshot_term_{0};
  };

} // namespace raftdemo