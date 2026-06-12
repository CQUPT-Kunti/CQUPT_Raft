#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>

#include "raft.pb.h"
#include "raft.grpc.pb.h"
#include "raft/common/command.h"
#include "raft/common/config.h"
#include "raft/runtime/min_heap_timer.h"
#include "raft/common/propose.h"
#include "raft/storage/raft_storage.h"
#include "raft/storage/snapshot_storage.h"
#include "raft/state_machine/state_machine_interface.h"
#include "raft/runtime/thread_pool.h"

namespace raftdemo
{

  class RaftServiceImpl;
  class MetadataServiceImpl;
  class Replicator;
  class MetadataStateMachine;
  class StrongConsistencyMetadataStateMachine;

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

  struct PeerReplicationStatus
  {
    int peer_id{0};
    std::string address;
    std::uint64_t match_index{0};
    std::uint64_t next_index{0};
  };

  struct RpcMetricsSnapshot
  {
    std::string name;
    std::uint64_t success_count{0};
    std::uint64_t failure_count{0};
    std::uint64_t total_latency_us{0};
    std::uint64_t max_latency_us{0};
  };

  struct NodeMetricsSnapshot
  {
    std::uint64_t propose_success_count{0};
    std::uint64_t propose_failure_count{0};
    std::uint64_t election_count{0};
    std::uint64_t leader_change_count{0};
    std::uint64_t snapshot_success_count{0};
    std::uint64_t snapshot_failure_count{0};
    std::uint64_t storage_persist_failure_count{0};
    std::vector<RpcMetricsSnapshot> rpc_metrics;
  };

  struct NodeStatusSnapshot
  {
    int node_id{0};
    std::string address;
    std::string role;
    std::uint64_t term{0};
    int leader_id{-1};
    std::string leader_address;
    std::uint64_t commit_index{0};
    std::uint64_t last_applied{0};
    std::uint64_t last_log_index{0};
    std::uint64_t snapshot_index{0};
    std::vector<PeerReplicationStatus> peers;
  };

  enum class CommittedMembershipRole
  {
    kUnknown,
    kVoter,
    kLearner,
    kNonMember,
  };

  struct CommittedMembershipQuorumSummary
  {
    // 仅用于诊断：必须基于已提交 membership 计算，不能按 live 节点或 ViewNode 观测降 quorum。
    std::uint64_t committed_log_index{0};
    std::uint64_t committed_term{0};
    std::vector<int> voter_ids;
    std::vector<int> learner_ids;
    std::size_t voter_count{0};
    std::size_t learner_count{0};
    std::size_t quorum_size{0};
    CommittedMembershipRole local_role{CommittedMembershipRole::kUnknown};
  };

  enum class AddLearnerProposalStatus : std::uint8_t
  {
    kAcceptedPendingCommit = 0,
    kDuplicate = 1,
    kPendingMembershipChange = 2,
    kRejected = 3,
    kNotLeader = 4,
    kNodeStopping = 5,
    kInvalidArgument = 6,
  };

  struct AddLearnerProposalRequest
  {
    std::string cluster_id;
    std::string node_id;
    std::int32_t candidate_raft_id{0};
    std::string candidate_client_address;
    std::string candidate_raft_address;
    std::string candidate_incarnation_id;
    std::uint64_t candidate_sequence{0};
    std::uint64_t persistent_generation{0};
    std::string data_dir_fingerprint;
  };

  struct AddLearnerProposalResult
  {
    AddLearnerProposalStatus status{
        AddLearnerProposalStatus::kRejected};
    int leader_id{-1};
    std::uint64_t term{0};
    std::uint64_t membership_epoch{0};
    bool committed_membership_changed{false};
    std::string canonical_node_id;
    std::int32_t assigned_raft_id{0};
    std::string message;

    [[nodiscard]] bool ok() const
    {
      return status == AddLearnerProposalStatus::kAcceptedPendingCommit ||
             status == AddLearnerProposalStatus::kDuplicate;
    }
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
    ProposeResult ProposeMetadata(const std::string &metadata_command_data);
    MetadataStateMachine *GetMetadataStateMachineV2();
    const MetadataStateMachine *GetMetadataStateMachineV2() const;
    StrongConsistencyMetadataStateMachine *GetMetadataStateMachine();
    const StrongConsistencyMetadataStateMachine *GetMetadataStateMachine() const;
    NodeStatusSnapshot GetStatusSnapshot() const;
    NodeMetricsSnapshot GetMetricsSnapshot() const;
    // 只读诊断接口：返回当前已提交 membership / quorum 摘要，不提供任何可变入口。
    CommittedMembershipQuorumSummary GetCommittedMembershipQuorumSummary() const;
    // AddLearner proposal path 目前只提供 leader admission / duplicate /
    // pending-conflict 边界。它不会直接修改 voter set，也不会伪造 committed
    // membership 成功；后续真实 membership log proposal/catch-up/promote
    // 需要在此边界之上继续实现。
    AddLearnerProposalResult ProposeAddLearner(
        const AddLearnerProposalRequest &request);
    bool IsRunning() const;

  private:
    enum class ReplicationOutcome
    {
      kReplicated,
      kTimeout,
      kLostLeadership,
      kLogUnavailable,
    };

    struct PeerClient
    {
      int peer_id{0};
      std::string address;
      std::shared_ptr<grpc::Channel> channel;
      std::unique_ptr<raft::RaftService::Stub> stub;
      std::mutex mu;
    };

    enum class RpcKind : std::uint8_t
    {
      kRequestVote,
      kAppendEntries,
      kInstallSnapshot,
    };

    struct RpcMetricState
    {
      std::string name;
      std::uint64_t success_count{0};
      std::uint64_t failure_count{0};
      std::uint64_t total_latency_us{0};
      std::uint64_t max_latency_us{0};
    };

    struct MetadataProposalTracker
    {
      std::mutex mu;
      std::condition_variable cv;
      std::string fingerprint;
      bool completed{false};
      ProposeResult result;
    };

    struct CompletedMetadataProposal
    {
      std::string fingerprint;
      ProposeResult result;
    };

    struct PendingAddLearnerProposal
    {
      std::string cluster_id;
      std::string node_id;
      std::int32_t candidate_raft_id{0};
      std::string candidate_client_address;
      std::string candidate_raft_address;
      std::string candidate_incarnation_id;
      std::uint64_t candidate_sequence{0};
      std::uint64_t persistent_generation{0};
      std::string data_dir_fingerprint;
      std::uint64_t accepted_membership_epoch{0};
    };

    friend class Replicator;
    friend class RaftServiceImpl;
    void InitServer();
    void InitClients();
    Replicator *GetOrCreateReplicatorLocked(const PeerConfig &peer);

    void CancelElectionTimerLocked();
    void ResetElectionTimerLocked();
    void ResetHeartbeatTimerLocked();
    void ResetSnapshotTimerLocked();
    std::chrono::milliseconds RandomElectionTimeoutLocked();

    void OnElectionTimeout(std::uint64_t timer_generation);
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
    std::uint64_t FirstIndexOfTermLocked(std::uint64_t term) const;
    void CompactLogPrefixLocked(std::uint64_t last_included_index,
                                std::uint64_t last_included_term);
    void RestoreLogAfterSnapshotLocked(std::uint64_t last_included_index,
                                       std::uint64_t last_included_term,
                                       bool keep_suffix_when_boundary_matches);
    void SetAppendEntriesConflictHintLocked(std::uint64_t probe_index,
                                            raft::AppendEntriesResponse *response) const;

    std::optional<raft::VoteResponse> RequestVoteRpc(int peer_id, const raft::VoteRequest &request);
    std::optional<raft::AppendEntriesResponse> AppendEntriesRpc(
        int peer_id, const raft::AppendEntriesRequest &request);
    std::optional<raft::InstallSnapshotResponse> InstallSnapshotRpc(
        int peer_id, const raft::InstallSnapshotRequest &request);
    bool SendInstallSnapshotToPeer(int peer_id, std::uint64_t term);

    static const char *RoleName(Role role);

    bool ValidateCommandUnlocked(const Command &command, std::string *reason) const;
    std::uint64_t AppendLocalLogUnlocked(const std::string &command_data);
    ReplicationOutcome ReplicateLogEntryToMajority(std::uint64_t log_index);
    ReplicationOutcome ReplicateLogEntryToMajority(
        std::uint64_t log_index,
        std::chrono::steady_clock::time_point deadline);
    void AdvanceCommitIndexUnlocked();
    ApplyResult ApplyCommittedEntries();
    void ExecuteMetadataProposal(
        const std::string &request_id,
        const std::string &metadata_command_data,
        std::shared_ptr<MetadataProposalTracker> tracker);
    bool WaitForMetadataProposalTracker(
        const std::shared_ptr<MetadataProposalTracker> &tracker,
        std::chrono::steady_clock::time_point deadline,
        ProposeResult *result) const;
    void CompleteMetadataProposal(
        const std::string &request_id,
        const std::shared_ptr<MetadataProposalTracker> &tracker,
        const std::string &fingerprint,
        const ProposeResult &result);
    void PruneCompletedMetadataProposalsLocked();
    bool PersistStateLocked(std::string *reason);
    bool ProposeNoOpEntry();
    std::string AddressForNodeLocked(int node_id) const;
    void MaybeRecordLeaderChangeLocked(int old_leader_id, int new_leader_id);
    void RecordProposeResult(bool success);
    void RecordElectionStarted();
    void RecordRpcLatency(RpcKind kind, bool success, std::chrono::microseconds latency);
    void RecordSnapshotOutcome(bool success);
    void RecordStoragePersistFailure();
    static const char *RpcKindName(RpcKind kind);
    static std::vector<RpcMetricState> BuildRpcMetricStateTemplate();
    RpcMetricState &RpcMetricLocked(RpcKind kind);
    void ValidateNodeIdentity();

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
    std::unordered_map<int, std::unique_ptr<Replicator>> replicators_;

    std::unordered_map<int, std::unique_ptr<PeerClient>> clients_;

    TimerScheduler scheduler_;
    ThreadPool rpc_pool_{4};
    std::optional<TimerScheduler::TaskId> election_timer_id_;
    std::uint64_t election_timer_generation_{0};
    std::optional<TimerScheduler::TaskId> heartbeat_timer_id_;
    std::optional<TimerScheduler::TaskId> snapshot_timer_id_;

    std::mt19937 rng_;
    std::atomic<bool> running_{false};

    std::unique_ptr<RaftServiceImpl> service_;
    std::unique_ptr<MetadataServiceImpl> metadata_service_;
    std::unique_ptr<grpc::Server> server_;

    std::mutex apply_mu_;
    std::optional<PendingAddLearnerProposal> pending_add_learner_proposal_;
    std::unordered_map<std::string, std::shared_ptr<MetadataProposalTracker>>
        metadata_inflight_proposals_;
    std::unordered_map<std::string, CompletedMetadataProposal>
        metadata_completed_proposals_;
    std::vector<std::string> metadata_completed_proposal_order_;
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

    mutable std::mutex metrics_mu_;
    std::uint64_t propose_success_count_{0};
    std::uint64_t propose_failure_count_{0};
    std::uint64_t election_count_{0};
    std::uint64_t leader_change_count_{0};
    std::uint64_t snapshot_success_count_{0};
    std::uint64_t snapshot_failure_count_{0};
    std::uint64_t storage_persist_failure_count_{0};
    std::vector<RpcMetricState> rpc_metrics_;
  };

} // namespace raftdemo
