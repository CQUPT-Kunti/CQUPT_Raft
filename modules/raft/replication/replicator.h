#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "raft.pb.h"
#include "raft/common/config.h"

namespace raftdemo
{

  class RaftNode;

  enum class ReplicationTargetRole : std::uint8_t
  {
    kCommittedVoter = 0,
    kLearner = 1,
  };

  class Replicator
  {
  public:
    Replicator(RaftNode &node, PeerConfig peer, ReplicationTargetRole target_role);

    int PeerId() const;
    ReplicationTargetRole TargetRole() const;

    // Replicate at most one batch to this follower. target_index == 0 means a
    // heartbeat/probe replication. When target_index > 0, the return value tells
    // whether this follower is known to have replicated at least target_index.
    bool ReplicateOnce(std::uint64_t leader_term, std::uint64_t target_index,
                       bool *should_apply);

    // Runtime state helpers for tests/debugging. They do not affect consensus.
    bool HasInflightRpc() const;
    std::string DebugString() const;

  private:
    bool BuildAppendEntriesRequest(std::uint64_t leader_term,
                                   raft::AppendEntriesRequest *request,
                                   bool *should_install_snapshot);
    bool HandleAppendEntriesResponse(std::uint64_t leader_term,
                                     std::uint64_t target_index,
                                     const raft::AppendEntriesResponse &response,
                                     bool *should_apply);

    bool IsTargetMatched(std::uint64_t target_index) const;
    bool IsBackoffActiveLocked(std::chrono::steady_clock::time_point now) const;
    void ResetBackoffLocked();
    void RecordTransportFailureLocked();
    void FinishAppendRpcLocked();
    void FinishSnapshotRpcLocked();

    RaftNode &node_;
    PeerConfig peer_;
    ReplicationTargetRole target_role_{ReplicationTargetRole::kCommittedVoter};
    mutable std::mutex mu_;

    bool append_inflight_{false};
    bool snapshot_inflight_{false};
    std::chrono::steady_clock::time_point next_retry_time_{};
    std::chrono::milliseconds retry_backoff_{std::chrono::milliseconds(20)};
    std::uint64_t append_rpc_started_{0};
    std::uint64_t append_rpc_finished_{0};
    std::uint64_t snapshot_rpc_started_{0};
    std::uint64_t snapshot_rpc_finished_{0};
    std::uint64_t snapshot_rpc_succeeded_{0};
    std::uint64_t snapshot_rpc_failed_{0};
    std::uint64_t transport_failures_{0};
    std::uint64_t last_snapshot_index_{0};
    std::uint64_t last_snapshot_term_{0};
    std::uint64_t last_snapshot_applied_index_{0};
    std::uint64_t last_snapshot_peer_log_index_{0};
  };

} // namespace raftdemo
