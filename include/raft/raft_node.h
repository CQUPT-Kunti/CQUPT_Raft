#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "raft.pb.h"
#include "raft.grpc.pb.h"
#include "raft/config.h"
#include "raft/min_heap_timer.h"
#include "raft/thread_pool.h"
#include "raft/command.h"
#include "raft/propose.h"
#include "raft/state_machine.h"

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
    ~RaftNode();

    void Start();
    void Stop();
    void Wait();
    // 收到别的节点发来的投票请求时的处理逻辑：
    void OnRequestVote(const raft::VoteRequest &request, raft::VoteResponse *response);
    // 收到 Leader 发来的心跳或日志复制请求时的处理逻辑：
    void OnAppendEntries(const raft::AppendEntriesRequest &request,
                         raft::AppendEntriesResponse *response);

    std::string Describe() const;
    // 客户端提案入口。上层业务应通过这个接口把一条业务命令提交给当前节点。
    ProposeResult Propose(const Command &command);

  private:
    struct PeerClient
    {
      int peer_id{0};
      std::string address;
      std::shared_ptr<grpc::Channel> channel;
      std::unique_ptr<raft::RaftService::Stub> stub;
      std::mutex mu;
    };
    // 初始化并启动当前节点的 gRPC 服务端。
    void InitServer();
    // 为所有 peer 节点创建 RPC 客户端，后续用于发投票和日志复制请求。
    void InitClients();
    // 重置选举超时计时器。
    void ResetElectionTimerLocked();
    // 重置心跳定时器。
    void ResetHeartbeatTimerLocked();
    // 生成一个随机选举超时时间，避免多个节点同时发起选举。
    std::chrono::milliseconds RandomElectionTimeoutLocked();
    // 选举超时后的处理入口。
    void OnElectionTimeout();
    // 开始选举
    void StartElection();
    // 当候选人拿到多数票后调用
    void OnElectionWon(std::uint64_t term);
    // 发送心跳
    void SendHeartbeats();

    void BecomeFollowerLocked(std::uint64_t new_term, int new_leader, const std::string &reason);
    void BecomeLeaderLocked();

    bool IsCandidateLogUpToDateLocked(std::uint64_t last_log_index,
                                      std::uint64_t last_log_term) const;
    std::uint64_t LastLogIndexLocked() const;
    std::uint64_t LastLogTermLocked() const;
    // 向某个 peer 发送投票请求 RPC。
    std::optional<raft::VoteResponse> RequestVoteRpc(int peer_id, const raft::VoteRequest &request);
    // 向某个 peer 发送追加日志 / 心跳请求 RPC。
    std::optional<raft::AppendEntriesResponse> AppendEntriesRpc(
        int peer_id, const raft::AppendEntriesRequest &request);

    static const char *RoleName(Role role);
    // 在持锁条件下校验命令是否合法。这里只做命令本身的合法性检查
    bool ValidateCommandUnlocked(const Command &command, std::string *reason) const;
    // 在持锁条件下将命令追加到本地日志
    std::uint64_t AppendLocalLogUnlocked(const std::string &command_data);
    // 将指定日志项复制到多数派节点。
    bool ReplicateLogEntryToMajority(std::uint64_t log_index);
    // 在持锁条件下推进提交下标。
    void AdvanceCommitIndexUnlocked();
    // 将已提交但尚未执行的日志应用到状态机。
    void ApplyCommittedEntries();

    NodeConfig config_;

    mutable std::mutex mu_;
    Role role_{Role::kFollower};
    std::uint64_t current_term_{0};
    int voted_for_{-1};
    int leader_id_{-1};

    std::vector<LogRecord> log_;
    std::uint64_t commit_index_{0};
    std::uint64_t last_applied_{0};

    std::unordered_map<int, std::uint64_t> next_index_;
    std::unordered_map<int, std::uint64_t> match_index_;

    std::unordered_map<int, std::unique_ptr<PeerClient>> clients_;

    TimerScheduler scheduler_;
    ThreadPool rpc_pool_{4};
    std::optional<TimerScheduler::TaskId> election_timer_id_;
    std::optional<TimerScheduler::TaskId> heartbeat_timer_id_;

    std::mt19937 rng_;
    std::atomic<bool> running_{false};

    std::unique_ptr<RaftServiceImpl> service_;
    std::unique_ptr<grpc::Server> server_;
    // 当前节点绑定的状态机实例
    std::unique_ptr<IStateMachine> state_machine_;
  };

} // namespace raftdemo
