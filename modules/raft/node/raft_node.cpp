#include "raft/node/raft_node.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "raft/runtime/logging.h"
#include "raft/common/metadata_command.h"
#include "raft/service/metadata_service_impl.h"
#include "raft/service/raft_service_impl.h"
#include "raft/replication/replicator.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "raft/storage/snapshot_storage.h"

namespace raftdemo
{
  namespace
  {
    struct SnapshotAppliedBoundary
    {
      std::uint64_t index{0};
      std::uint64_t term{0};
    };

    constexpr const char *kInternalNoOpCommand = "__raft_internal_noop__";
    constexpr const char *kInternalAtomicBatchPromotionCommandPrefix =
        "__raft_internal_atomic_batch_promote_v1__";
    constexpr const char *kSnapshotMarkerCommand = "snapshot";
    constexpr const char *kIdentityFileName = "node_identity.txt";
    constexpr const char *kStructuredIdentityFileName = "node.identity";
    constexpr std::size_t kMaxInflightMetadataProposals = 4;
    constexpr std::size_t kCompletedMetadataProposalCacheLimit = 64;

    std::chrono::milliseconds ScaleDeadline(std::chrono::milliseconds base,
                                            const int multiplier)
    {
      if (base <= std::chrono::milliseconds::zero())
      {
        return std::chrono::milliseconds::zero();
      }
      const auto count = base.count();
      if (count > std::numeric_limits<std::int64_t>::max() / multiplier)
      {
        return std::chrono::milliseconds::max();
      }
      return std::chrono::milliseconds(count * multiplier);
    }

    std::uint64_t SafeAddOne(std::uint64_t value)
    {
      return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1;
    }

    std::string NodeTag(int node_id) { return "node-" + std::to_string(node_id); }

    std::string HexEncode(std::string_view value)
    {
      static constexpr char kHexDigits[] = "0123456789abcdef";
      std::string encoded;
      encoded.reserve(value.size() * 2U);
      for (const unsigned char ch : value)
      {
        encoded.push_back(kHexDigits[(ch >> 4U) & 0x0FU]);
        encoded.push_back(kHexDigits[ch & 0x0FU]);
      }
      return encoded;
    }

    bool HexDecode(std::string_view encoded, std::string *value)
    {
      if (value == nullptr || encoded.size() % 2U != 0U)
      {
        return false;
      }

      auto decode_nibble = [](const char ch) -> int {
        if (ch >= '0' && ch <= '9')
        {
          return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f')
        {
          return 10 + ch - 'a';
        }
        if (ch >= 'A' && ch <= 'F')
        {
          return 10 + ch - 'A';
        }
        return -1;
      };

      std::string decoded;
      decoded.reserve(encoded.size() / 2U);
      for (std::size_t index = 0; index < encoded.size(); index += 2U)
      {
        const int high = decode_nibble(encoded[index]);
        const int low = decode_nibble(encoded[index + 1U]);
        if (high < 0 || low < 0)
        {
          return false;
        }
        decoded.push_back(static_cast<char>((high << 4U) | low));
      }
      *value = std::move(decoded);
      return true;
    }

    std::string DefaultDataDir(int node_id)
    {
      return "./raft_data/node_" + std::to_string(node_id);
    }

    std::string DefaultSnapshotDir(int node_id)
    {
      return "./raft_snapshots/node_" + std::to_string(node_id);
    }

    std::string Trim(std::string text)
    {
      const auto first = text.find_first_not_of(" \t\r\n");
      if (first == std::string::npos)
      {
        return "";
      }
      const auto last = text.find_last_not_of(" \t\r\n");
      return text.substr(first, last - first + 1);
    }

    std::map<std::string, std::string> ReadIdentityFile(const std::filesystem::path &path)
    {
      std::ifstream in(path);
      if (!in.is_open())
      {
        throw std::runtime_error("failed to open identity file: " + path.string());
      }

      std::map<std::string, std::string> values;
      std::string line;
      while (std::getline(in, line))
      {
        line = Trim(line);
        if (line.empty())
        {
          continue;
        }
        const auto pos = line.find('=');
        if (pos == std::string::npos)
        {
          throw std::runtime_error("invalid identity line: " + line);
        }
        values.emplace(Trim(line.substr(0, pos)), Trim(line.substr(pos + 1)));
      }
      return values;
    }

    std::optional<SnapshotAppliedBoundary> ResolveLoadedSnapshotAppliedBoundary(
        const IStateMachine &state_machine,
        const std::uint64_t expected_index,
        const std::uint64_t expected_term,
        std::string *reason)
    {
      if (const auto *metadata_state_machine =
              dynamic_cast<const MetadataStateMachine *>(&state_machine);
          metadata_state_machine != nullptr)
      {
        const SnapshotAppliedBoundary boundary{
            metadata_state_machine->LastAppliedIndex(),
            metadata_state_machine->LastAppliedTerm()};
        if (boundary.index != expected_index || boundary.term != expected_term)
        {
          if (reason != nullptr)
          {
            std::ostringstream oss;
            oss << "metadata snapshot boundary mismatch, expected_index="
                << expected_index
                << ", expected_term=" << expected_term
                << ", restored_index=" << boundary.index
                << ", restored_term=" << boundary.term;
            *reason = oss.str();
          }
          return std::nullopt;
        }
        return boundary;
      }

      return SnapshotAppliedBoundary{expected_index, expected_term};
    }

    std::size_t ComputeCommittedVoterQuorumSize(const std::size_t voter_count)
    {
      if (voter_count == 0)
      {
        return 0;
      }
      return voter_count / 2 + 1;
    }

    std::vector<PeerConfig> BuildUniqueCommittedVoterPeers(
        const NodeConfig &config)
    {
      std::vector<PeerConfig> peers;
      peers.reserve(config.peers.size());

      std::unordered_set<std::int32_t> seen_ids;
      seen_ids.insert(config.node_id);

      for (const auto &peer : config.peers)
      {
        if (!seen_ids.insert(peer.node_id).second)
        {
          continue;
        }
        peers.push_back(peer);
      }

      return peers;
    }

    std::vector<int> BuildCommittedVoterIds(const NodeConfig &config)
    {
      std::vector<int> voter_ids;
      voter_ids.reserve(config.peers.size() + 1);
      voter_ids.push_back(config.node_id);

      for (const auto &peer : BuildUniqueCommittedVoterPeers(config))
      {
        voter_ids.push_back(peer.node_id);
      }

      std::sort(voter_ids.begin(), voter_ids.end());
      return voter_ids;
    }

    std::size_t CountCommittedVoters(const NodeConfig &config)
    {
      return BuildCommittedVoterIds(config).size();
    }

    std::size_t ComputeCommittedVoterQuorumSize(const NodeConfig &config)
    {
      return ComputeCommittedVoterQuorumSize(CountCommittedVoters(config));
    }

    std::size_t CountReplicatedCommittedVoters(
        const NodeConfig &config,
        const std::unordered_map<int, std::uint64_t> &match_index_by_peer_id,
        const std::uint64_t log_index)
    {
      std::size_t replicated_voter_count = 1;
      for (const auto &peer : BuildUniqueCommittedVoterPeers(config))
      {
        const auto it = match_index_by_peer_id.find(peer.node_id);
        if (it != match_index_by_peer_id.end() && it->second >= log_index)
        {
          ++replicated_voter_count;
        }
      }
      return replicated_voter_count;
    }

    std::optional<std::string> ValidateAddLearnerProposalRequest(
        const AddLearnerProposalRequest &request)
    {
      if (request.cluster_id.empty())
      {
        return "cluster_id is required";
      }
      if (request.node_id.empty())
      {
        return "node_id is required";
      }
      if (request.candidate_raft_id <= 0)
      {
        return "candidate_raft_id must be positive";
      }
      if (request.candidate_client_address.empty())
      {
        return "candidate_client_address is required";
      }
      if (request.candidate_raft_address.empty())
      {
        return "candidate_raft_address is required";
      }
      if (request.candidate_incarnation_id.empty())
      {
        return "candidate_incarnation_id is required";
      }
      if (request.candidate_sequence == 0)
      {
        return "candidate_sequence must be non-zero";
      }
      if (request.persistent_generation == 0)
      {
        return "persistent_generation must be non-zero";
      }
      if (request.data_dir_fingerprint.empty())
      {
        return "data_dir_fingerprint is required";
      }
      return std::nullopt;
    }

    bool HasCommittedVoterRaftId(const NodeConfig &config,
                                 const std::int32_t candidate_raft_id)
    {
      if (candidate_raft_id == config.node_id)
      {
        return true;
      }
      return std::any_of(config.peers.begin(),
                         config.peers.end(),
                         [candidate_raft_id](const PeerConfig &peer) {
                           return peer.node_id == candidate_raft_id;
                         });
    }

    RuntimeMembershipEntry MakeCommittedVoterRuntimeEntry(
        const std::int32_t raft_id,
        std::string address)
    {
      RuntimeMembershipEntry entry;
      entry.raft_id = raft_id;
      entry.address = std::move(address);
      entry.role = RuntimeMembershipRole::kVoter;
      entry.committed = true;
      entry.pending = false;
      return entry;
    }

    RuntimeMembershipRole ParseRuntimeMembershipRoleHint(
        const std::string &membership_state)
    {
      if (membership_state == "voter")
      {
        return RuntimeMembershipRole::kVoter;
      }
      if (membership_state == "learner")
      {
        return RuntimeMembershipRole::kLearner;
      }
      if (membership_state == "joining" ||
          membership_state == "candidate" ||
          membership_state == "non_raft")
      {
        return RuntimeMembershipRole::kNonMember;
      }
      return RuntimeMembershipRole::kUnknown;
    }

    const char *RuntimeMembershipRoleName(const RuntimeMembershipRole role)
    {
      switch (role)
      {
      case RuntimeMembershipRole::kVoter:
        return "voter";
      case RuntimeMembershipRole::kLearner:
        return "learner";
      case RuntimeMembershipRole::kNonMember:
        return "non_member";
      case RuntimeMembershipRole::kUnknown:
      default:
        return "unknown";
      }
    }

  } // namespace

  RaftNode::RaftNode(NodeConfig config)
      : RaftNode(std::move(config), snapshotConfig{}, std::make_unique<MetadataStateMachine>())
  {
  }

  RaftNode::RaftNode(NodeConfig config, snapshotConfig snapshot_config)
      : RaftNode(std::move(config), std::move(snapshot_config), std::make_unique<MetadataStateMachine>())
  {
  }

  RaftNode::RaftNode(NodeConfig config, std::unique_ptr<IStateMachine> state_machine)
      : RaftNode(std::move(config), snapshotConfig{}, std::move(state_machine))
  {
  }

  RaftNode::RaftNode(NodeConfig config, snapshotConfig snapshot_config,
                     std::unique_ptr<IStateMachine> state_machine)
      : config_(std::move(config)),
        snapshot_config_(std::move(snapshot_config)),
        rng_(std::random_device{}()),
        state_machine_(std::move(state_machine)),
        rpc_metrics_(BuildRpcMetricStateTemplate())
  {
    if (config_.data_dir.empty())
    {
      config_.data_dir = DefaultDataDir(config_.node_id);
    }

    if (snapshot_config_.snapshot_dir.empty())
    {
      snapshot_config_.snapshot_dir = DefaultSnapshotDir(config_.node_id);
    }

    ValidateNodeIdentity();

    storage_ = CreateFileRaftStorage(config_.data_dir);
    snapshot_storage_ = CreateFileSnapshotStorage(snapshot_config_.snapshot_dir,
                                                  snapshot_config_.file_prefix);

    PersistentRaftState persistent_state;
    bool has_state = false;
    std::string error;
    if (!storage_->Load(&persistent_state, &has_state, &error))
    {
      throw std::runtime_error("failed to load raft state for node " +
                               std::to_string(config_.node_id) + ": " + error);
    }

    if (has_state)
    {
      current_term_ = persistent_state.current_term;
      voted_for_ = persistent_state.voted_for;
      // Persisted commit/apply boundaries tell us how far the log was known to
      // be committed before restart. The state machine itself is rebuilt from
      // snapshot + committed log replay, so do NOT restore runtime last_applied_
      // to persistent_state.last_applied here. If we did, startup would think
      // those entries were already applied and would skip replay, leaving an
      // empty metadata state after a pure-log restart.
      commit_index_ = std::max<std::uint64_t>(persistent_state.commit_index,
                                             persistent_state.last_applied);
      last_applied_ = 0;
      log_ = std::move(persistent_state.log);
      if (log_.empty())
      {
        log_.push_back(LogRecord{0, 0, "bootstrap"});
      }
      else if (log_.front().index > 0 && log_.front().command == kSnapshotMarkerCommand)
      {
        last_snapshot_index_ = log_.front().index;
        last_snapshot_term_ = log_.front().term;
      }

      const std::uint64_t loaded_last_log_index = LastLogIndexLocked();
      if (commit_index_ > loaded_last_log_index)
      {
        Log(NodeTag(config_.node_id),
            "clamp persisted commit boundary during restart recovery, persisted_commit_index=",
            commit_index_, ", persisted_last_applied=", persistent_state.last_applied,
            ", last_log_index=", loaded_last_log_index);
        commit_index_ = loaded_last_log_index;
      }

      Log(NodeTag(config_.node_id), "loaded persisted state from ", storage_->DataDir(),
          ", term=", current_term_, ", voted_for=", voted_for_,
          ", last_log_index=", LastLogIndexLocked(),
          ", commit_index=", commit_index_,
          ", persisted_last_applied=", persistent_state.last_applied,
          ", replay_from=", last_applied_ + 1);
    }
    else
    {
      log_.push_back(LogRecord{0, 0, "bootstrap"});
    }

    if (snapshot_config_.enabled && snapshot_config_.load_on_startup)
    {
      std::string snapshot_error;
      if (!LoadLatestSnapshotOnStartup(&snapshot_error) && !snapshot_error.empty())
      {
        throw std::runtime_error("failed to load snapshot for node " +
                                 std::to_string(config_.node_id) + ": " + snapshot_error);
      }
    }

    if (commit_index_ > last_applied_)
    {
      ApplyResult replay_result = ApplyCommittedEntries();
      if (!replay_result.Ok)
      {
        throw std::runtime_error("failed to replay committed log entries for node " +
                                 std::to_string(config_.node_id) + ": " + replay_result.message +
                                 ", commit_index=" + std::to_string(commit_index_) +
                                 ", last_applied=" + std::to_string(last_applied_) +
                                 ", last_snapshot_index=" + std::to_string(last_snapshot_index_) +
                                 ", last_snapshot_term=" + std::to_string(last_snapshot_term_) +
                                 ", last_log_index=" + std::to_string(LastLogIndexLocked()));
      }
    }
    Log(NodeTag(config_.node_id),
        "restart recovery complete, has_persisted_state=", has_state,
        ", term=", current_term_, ", voted_for=", voted_for_,
        ", commit_index=", commit_index_, ", last_applied=", last_applied_,
        ", last_snapshot_index=", last_snapshot_index_,
        ", last_snapshot_term=", last_snapshot_term_,
        ", first_log_index=", FirstLogIndexLocked(),
        ", last_log_index=", LastLogIndexLocked());
  }

  RaftNode::~RaftNode() { Stop(); }

  void RaftNode::Start()
  {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
    {
      return;
    }

    rpc_pool_.Start();
    InitClients();
    scheduler_.Start();
    StartSnapshotWorker();
    InitServer();

    {
      std::lock_guard<std::mutex> lk(mu_);
      ResetElectionTimerLocked();
      ResetSnapshotTimerLocked();
    }

    Log(NodeTag(config_.node_id), "started at ", config_.address, ", peers=", config_.peers.size(),
        ", committed_voter_count=", CountCommittedVoters(config_),
        ", quorum=", ComputeCommittedVoterQuorumSize(config_),
        ", data_dir=", config_.data_dir, ", snapshot_dir=", snapshot_config_.snapshot_dir);
  }

  void RaftNode::Stop()
  {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false))
    {
      return;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      CancelElectionTimerLocked();
      if (heartbeat_timer_id_)
      {
        scheduler_.Cancel(*heartbeat_timer_id_);
        heartbeat_timer_id_.reset();
      }
      if (snapshot_timer_id_)
      {
        scheduler_.Cancel(*snapshot_timer_id_);
        snapshot_timer_id_.reset();
      }
    }

    StopSnapshotWorker();

    if (server_)
    {
      server_->Shutdown();
    }
    scheduler_.Stop();
    rpc_pool_.Stop();

    {
      std::lock_guard<std::mutex> lk(mu_);
      std::string persist_error;
      if (!PersistStateLocked(&persist_error))
      {
        Log(NodeTag(config_.node_id), "persist state on stop failed: ", persist_error);
      }
    }

    Log(NodeTag(config_.node_id), "stopped");
  }

  void RaftNode::Wait()
  {
    if (server_)
    {
      server_->Wait();
    }
  }

  void RaftNode::ValidateNodeIdentity()
  {
    std::error_code ec;
    std::filesystem::create_directories(config_.data_dir, ec);
    if (ec)
    {
      throw std::runtime_error("failed to create data directory " + config_.data_dir +
                               ": " + ec.message());
    }

    const std::filesystem::path identity_path =
        std::filesystem::path(config_.data_dir) / kIdentityFileName;
    const std::filesystem::path structured_identity_path =
        std::filesystem::path(config_.data_dir) / kStructuredIdentityFileName;

    if (std::filesystem::exists(identity_path, ec))
    {
      const auto values = ReadIdentityFile(identity_path);
      const auto node_id_it = values.find("node_id");
      if (node_id_it == values.end())
      {
        throw std::runtime_error("identity file missing node_id: " + identity_path.string());
      }

      const int stored_node_id = std::stoi(node_id_it->second);
      if (stored_node_id != config_.node_id)
      {
        throw std::runtime_error("data directory identity mismatch: data_dir=" + config_.data_dir +
                                 ", expected node_id=" + std::to_string(config_.node_id) +
                                 ", found node_id=" + std::to_string(stored_node_id));
      }
    }
    else
    {
      std::ofstream out(identity_path, std::ios::trunc);
      if (!out.is_open())
      {
        throw std::runtime_error("failed to create identity file: " + identity_path.string());
      }

      out << "node_id=" << config_.node_id << '\n';
      out << "address=" << config_.address << '\n';
      out.flush();
      if (!out)
      {
        throw std::runtime_error("failed to write identity file: " + identity_path.string());
      }
    }

    local_runtime_membership_role_hint_ = RuntimeMembershipRole::kVoter;
    if (std::filesystem::exists(structured_identity_path, ec))
    {
      const auto values = ReadIdentityFile(structured_identity_path);
      if (const auto membership_state_it = values.find("membership_state");
          membership_state_it != values.end())
      {
        const auto parsed_role =
            ParseRuntimeMembershipRoleHint(membership_state_it->second);
        if (parsed_role != RuntimeMembershipRole::kUnknown)
        {
          local_runtime_membership_role_hint_ = parsed_role;
        }
      }
    }
  }

  void RaftNode::InitServer()
  {
    service_ = std::make_unique<RaftServiceImpl>(*this);
    metadata_service_ = std::make_unique<MetadataServiceImpl>(*this);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(config_.address, grpc::InsecureServerCredentials());
    builder.RegisterService(service_.get());
    builder.RegisterService(metadata_service_.get());
    server_ = builder.BuildAndStart();
    if (!server_)
    {
      running_.store(false);
      throw std::runtime_error("failed to start gRPC server at " + config_.address);
    }
  }

  void RaftNode::InitClients()
  {
    clients_.clear();
    for (const auto &peer : config_.peers)
    {
      EnsurePeerClientLocked(peer);
    }
  }


Replicator *RaftNode::GetOrCreateReplicatorLocked(const PeerConfig &peer)
{
  auto it = replicators_.find(peer.node_id);
  if (it != replicators_.end())
  {
    return it->second.get();
  }

  const bool is_pending_learner =
      std::any_of(pending_add_learner_proposals_.begin(),
                  pending_add_learner_proposals_.end(),
                  [&peer](const PendingAddLearnerProposal &proposal) {
                    return proposal.candidate_raft_id == peer.node_id &&
                           proposal.candidate_raft_address == peer.address;
                  });
  const auto target_role = is_pending_learner
                               ? ReplicationTargetRole::kLearner
                               : ReplicationTargetRole::kCommittedVoter;
  auto replicator = std::make_unique<Replicator>(*this, peer, target_role);
  Replicator *raw = replicator.get();
  replicators_.emplace(peer.node_id, std::move(replicator));
  return raw;
}

  bool RaftNode::IsRunning() const
  {
    return running_.load();
  }

  NodeStatusSnapshot RaftNode::GetStatusSnapshot() const
  {
    std::lock_guard<std::mutex> lk(mu_);

    NodeStatusSnapshot snapshot;
    snapshot.node_id = config_.node_id;
    snapshot.address = config_.address;
    snapshot.role = RoleName(role_);
    snapshot.term = current_term_;
    snapshot.leader_id = leader_id_;
    snapshot.leader_address = AddressForNodeLocked(leader_id_);
    snapshot.commit_index = commit_index_;
    snapshot.last_applied = last_applied_;
    snapshot.last_log_index = LastLogIndexLocked();
    snapshot.snapshot_index = last_snapshot_index_;

    snapshot.peers.reserve(config_.peers.size());
    for (const auto &peer : config_.peers)
    {
      PeerReplicationStatus peer_status;
      peer_status.peer_id = peer.node_id;
      peer_status.address = peer.address;
      if (const auto match_it = match_index_.find(peer.node_id); match_it != match_index_.end())
      {
        peer_status.match_index = match_it->second;
      }
      if (const auto next_it = next_index_.find(peer.node_id); next_it != next_index_.end())
      {
        peer_status.next_index = next_it->second;
      }
      snapshot.peers.push_back(std::move(peer_status));
    }

    return snapshot;
  }

  NodeMetricsSnapshot RaftNode::GetMetricsSnapshot() const
  {
    std::lock_guard<std::mutex> lk(metrics_mu_);

    NodeMetricsSnapshot snapshot;
    snapshot.propose_success_count = propose_success_count_;
    snapshot.propose_failure_count = propose_failure_count_;
    snapshot.election_count = election_count_;
    snapshot.leader_change_count = leader_change_count_;
    snapshot.snapshot_success_count = snapshot_success_count_;
    snapshot.snapshot_failure_count = snapshot_failure_count_;
    snapshot.storage_persist_failure_count = storage_persist_failure_count_;
    snapshot.rpc_metrics.reserve(rpc_metrics_.size());
    for (const auto &metric : rpc_metrics_)
    {
      snapshot.rpc_metrics.push_back(RpcMetricsSnapshot{
          metric.name,
          metric.success_count,
          metric.failure_count,
          metric.total_latency_us,
          metric.max_latency_us,
      });
    }
    return snapshot;
  }

  RuntimeMembershipSummary RaftNode::GetRuntimeMembershipSummary() const
  {
    std::lock_guard<std::mutex> lk(mu_);
    return BuildRuntimeMembershipSummaryLocked();
  }

  CommittedMembershipQuorumSummary RaftNode::GetCommittedMembershipQuorumSummary() const
  {
    std::lock_guard<std::mutex> lk(mu_);

    CommittedMembershipQuorumSummary summary;
    summary.committed_log_index = commit_index_;
    summary.committed_term = TermAtIndexLocked(commit_index_);

    // 当前阶段 RaftNode 内部没有运行时 membership authority；诊断摘要必须只读取
    // 已提交配置边界下当前节点已知的成员集，不能根据 live 节点或 ViewNode 观测降 quorum。
    summary.voter_ids = BuildCommittedVoterIds(config_);

    // 第一阶段暂未把 learner membership 下沉到 RaftNode 运行时，因此这里保持只读空集，
    // 避免把 registered-only 或观测节点误计入 committed voter quorum。
    summary.voter_count = summary.voter_ids.size();
    summary.learner_count = summary.learner_ids.size();
    summary.quorum_size = ComputeCommittedVoterQuorumSize(config_);
    summary.local_role = std::binary_search(summary.voter_ids.begin(),
                                            summary.voter_ids.end(),
                                            config_.node_id)
                             ? CommittedMembershipRole::kVoter
                             : CommittedMembershipRole::kNonMember;
    return summary;
  }

  RuntimeMembershipSummary RaftNode::BuildRuntimeMembershipSummaryLocked() const
  {
    RuntimeMembershipSummary summary;
    summary.committed_log_index = commit_index_;
    summary.committed_term = TermAtIndexLocked(commit_index_);

    std::map<std::int32_t, RuntimeMembershipEntry> voter_entries_by_id;
    if (local_runtime_membership_role_hint_ != RuntimeMembershipRole::kLearner &&
        local_runtime_membership_role_hint_ != RuntimeMembershipRole::kNonMember)
    {
      voter_entries_by_id.emplace(
          config_.node_id,
          MakeCommittedVoterRuntimeEntry(config_.node_id, config_.address));
    }
    for (const auto &peer : BuildUniqueCommittedVoterPeers(config_))
    {
      voter_entries_by_id.try_emplace(
          peer.node_id,
          MakeCommittedVoterRuntimeEntry(peer.node_id, peer.address));
    }

    for (const auto &[raft_id, entry] : voter_entries_by_id)
    {
      auto voter_entry = entry;
      if (const auto match_it = match_index_.find(raft_id); match_it != match_index_.end())
      {
        voter_entry.match_index = match_it->second;
      }
      if (const auto next_it = next_index_.find(raft_id); next_it != next_index_.end())
      {
        voter_entry.next_index = next_it->second;
      }
      if (raft_id == config_.node_id)
      {
        voter_entry.last_snapshot_index = last_snapshot_index_;
        voter_entry.last_snapshot_term = last_snapshot_term_;
        voter_entry.last_applied_index = last_applied_;
        voter_entry.observed_last_log_index = LastLogIndexLocked();
      }
      else if (const auto snapshot_it = peer_snapshot_progress_.find(raft_id);
               snapshot_it != peer_snapshot_progress_.end())
      {
        voter_entry.last_snapshot_index = snapshot_it->second.last_snapshot_index;
        voter_entry.last_snapshot_term = snapshot_it->second.last_snapshot_term;
        voter_entry.last_applied_index = snapshot_it->second.last_applied_index;
        voter_entry.observed_last_log_index = snapshot_it->second.last_log_index;
      }
      summary.voter_ids.push_back(raft_id);
      summary.voter_entries.push_back(std::move(voter_entry));
    }

    if (local_runtime_membership_role_hint_ == RuntimeMembershipRole::kLearner)
    {
      RuntimeMembershipEntry local_learner_entry;
      local_learner_entry.raft_id = config_.node_id;
      local_learner_entry.address = config_.address;
      local_learner_entry.role = RuntimeMembershipRole::kLearner;
      local_learner_entry.committed = false;
      local_learner_entry.pending = false;
      if (const auto match_it = match_index_.find(local_learner_entry.raft_id);
          match_it != match_index_.end())
      {
        local_learner_entry.match_index = match_it->second;
      }
      if (const auto next_it = next_index_.find(local_learner_entry.raft_id);
          next_it != next_index_.end())
      {
        local_learner_entry.next_index = next_it->second;
      }
      local_learner_entry.last_snapshot_index = last_snapshot_index_;
      local_learner_entry.last_snapshot_term = last_snapshot_term_;
      local_learner_entry.last_applied_index = last_applied_;
      local_learner_entry.observed_last_log_index = LastLogIndexLocked();
      summary.learner_ids.push_back(local_learner_entry.raft_id);
      summary.learner_entries.push_back(local_learner_entry);
    }

    for (const auto &pending_proposal : pending_add_learner_proposals_)
    {
      RuntimeMembershipEntry learner_entry;
      learner_entry.raft_id = pending_proposal.candidate_raft_id;
      learner_entry.address = pending_proposal.candidate_raft_address;
      learner_entry.role = RuntimeMembershipRole::kLearner;
      learner_entry.committed = false;
      learner_entry.pending = true;
      if (const auto match_it = match_index_.find(learner_entry.raft_id);
          match_it != match_index_.end())
      {
        learner_entry.match_index = match_it->second;
      }
      if (const auto next_it = next_index_.find(learner_entry.raft_id);
          next_it != next_index_.end())
      {
        learner_entry.next_index = next_it->second;
      }
      if (const auto snapshot_it = peer_snapshot_progress_.find(learner_entry.raft_id);
          snapshot_it != peer_snapshot_progress_.end())
      {
        learner_entry.last_snapshot_index = snapshot_it->second.last_snapshot_index;
        learner_entry.last_snapshot_term = snapshot_it->second.last_snapshot_term;
        learner_entry.last_applied_index = snapshot_it->second.last_applied_index;
        learner_entry.observed_last_log_index = snapshot_it->second.last_log_index;
      }
      learner_entry.canonical_node_id = pending_proposal.node_id;
      learner_entry.candidate_incarnation_id =
          pending_proposal.candidate_incarnation_id;
      learner_entry.candidate_sequence =
          pending_proposal.candidate_sequence;
      learner_entry.persistent_generation =
          pending_proposal.persistent_generation;
      learner_entry.data_dir_fingerprint =
          pending_proposal.data_dir_fingerprint;
      if (voter_entries_by_id.find(learner_entry.raft_id) ==
              voter_entries_by_id.end() &&
          std::find(summary.learner_ids.begin(),
                    summary.learner_ids.end(),
                    learner_entry.raft_id) == summary.learner_ids.end())
      {
        summary.learner_ids.push_back(learner_entry.raft_id);
        summary.learner_entries.push_back(learner_entry);
      }
    }

    summary.voter_count = summary.voter_entries.size();
    summary.learner_count = summary.learner_entries.size();
    summary.committed_voter_quorum_size =
        ComputeCommittedVoterQuorumSize(summary.voter_count);

    if (local_runtime_membership_role_hint_ == RuntimeMembershipRole::kLearner)
    {
      summary.local_role = RuntimeMembershipRole::kLearner;
    }
    else if (local_runtime_membership_role_hint_ == RuntimeMembershipRole::kNonMember)
    {
      summary.local_role = RuntimeMembershipRole::kNonMember;
    }
    else if (std::binary_search(summary.voter_ids.begin(),
                                summary.voter_ids.end(),
                                config_.node_id))
    {
      summary.local_role = RuntimeMembershipRole::kVoter;
    }
    else if (std::binary_search(summary.learner_ids.begin(),
                                summary.learner_ids.end(),
                                config_.node_id))
    {
      summary.local_role = RuntimeMembershipRole::kLearner;
    }
    else
    {
      summary.local_role = RuntimeMembershipRole::kNonMember;
    }

    return summary;
  }

  AddLearnerProposalResult RaftNode::ProposeAddLearner(
      const AddLearnerProposalRequest &request)
  {
    AddLearnerProposalResult result;
    result.canonical_node_id = request.node_id;
    result.assigned_raft_id = request.candidate_raft_id;

    if (const auto validation_error = ValidateAddLearnerProposalRequest(request);
        validation_error.has_value())
    {
      result.status = AddLearnerProposalStatus::kInvalidArgument;
      result.message = *validation_error;
      return result;
    }

    std::lock_guard<std::mutex> lk(mu_);
    result.leader_id = leader_id_;
    result.term = current_term_;
    result.membership_epoch = commit_index_;

    if (!running_.load())
    {
      result.status = AddLearnerProposalStatus::kNodeStopping;
      result.message = "node is stopping";
      return result;
    }

    if (role_ != Role::kLeader)
    {
      result.status = AddLearnerProposalStatus::kNotLeader;
      result.message = "AddLearner authority belongs to the current leader";
      return result;
    }

    result.leader_id = config_.node_id;

    if (HasCommittedVoterRaftId(config_, request.candidate_raft_id))
    {
      result.status = AddLearnerProposalStatus::kRejected;
      result.message = "candidate_raft_id already exists in committed voter set";
      return result;
    }

    const auto is_same_pending = [&](const PendingAddLearnerProposal &pending) {
      return pending.cluster_id == request.cluster_id &&
             pending.node_id == request.node_id &&
             pending.candidate_raft_id == request.candidate_raft_id &&
             pending.candidate_client_address == request.candidate_client_address &&
             pending.candidate_raft_address == request.candidate_raft_address &&
             pending.candidate_incarnation_id == request.candidate_incarnation_id &&
             pending.candidate_sequence == request.candidate_sequence &&
             pending.persistent_generation == request.persistent_generation &&
             pending.data_dir_fingerprint == request.data_dir_fingerprint;
    };

    const auto conflicts_with_pending = [&](const PendingAddLearnerProposal &pending) {
      if (pending.cluster_id != request.cluster_id)
      {
        return false;
      }
      return pending.node_id == request.node_id ||
             pending.candidate_raft_id == request.candidate_raft_id ||
             pending.candidate_client_address == request.candidate_client_address ||
             pending.candidate_raft_address == request.candidate_raft_address ||
             pending.data_dir_fingerprint == request.data_dir_fingerprint;
    };

    for (const auto &pending : pending_add_learner_proposals_)
    {
      result.membership_epoch = pending.accepted_membership_epoch;
      if (is_same_pending(pending))
      {
        result.status = AddLearnerProposalStatus::kDuplicate;
        result.message =
            "duplicate AddLearner proposal for pending learner candidate";
        return result;
      }

      if (conflicts_with_pending(pending))
      {
        result.status = AddLearnerProposalStatus::kRejected;
        result.message =
            "conflicting AddLearner proposal for learner candidate already pending";
        return result;
      }
    }

    if (pending_add_learner_proposals_.size() >= 2U)
    {
      result.status = AddLearnerProposalStatus::kPendingMembershipChange;
      result.message = "pending AddLearner proposal set already reached atomic batch boundary";
      return result;
    }

    const PendingAddLearnerProposal pending_proposal{
        .cluster_id = request.cluster_id,
        .node_id = request.node_id,
        .candidate_raft_id = request.candidate_raft_id,
        .candidate_client_address = request.candidate_client_address,
        .candidate_raft_address = request.candidate_raft_address,
        .candidate_incarnation_id = request.candidate_incarnation_id,
        .candidate_sequence = request.candidate_sequence,
        .persistent_generation = request.persistent_generation,
        .data_dir_fingerprint = request.data_dir_fingerprint,
        .accepted_membership_epoch = commit_index_,
    };
    pending_add_learner_proposals_.push_back(pending_proposal);
    std::sort(pending_add_learner_proposals_.begin(),
              pending_add_learner_proposals_.end(),
              [](const PendingAddLearnerProposal &lhs,
                 const PendingAddLearnerProposal &rhs) {
                return lhs.candidate_raft_id < rhs.candidate_raft_id;
              });
    InitializePendingLearnerReplicationStateLocked(pending_proposal);
    result.status = AddLearnerProposalStatus::kAcceptedPendingCommit;
    result.membership_epoch = commit_index_;
    result.message = pending_add_learner_proposals_.size() >= 2U
                         ? "AddLearner proposal admitted into atomic batch learner set"
                         : "AddLearner proposal admitted on leader; learner catch-up remains pending until atomic batch promote is safe";
    return result;
  }

  AddLearnerProposalResult RaftNode::PromoteReadyLearnerBatch(
      const AddLearnerProposalRequest &request)
  {
    AddLearnerProposalResult result;
    result.canonical_node_id = request.node_id;
    result.assigned_raft_id = request.candidate_raft_id;

    if (const auto validation_error = ValidateAddLearnerProposalRequest(request);
        validation_error.has_value())
    {
      result.status = AddLearnerProposalStatus::kInvalidArgument;
      result.message = *validation_error;
      return result;
    }

    std::optional<std::uint64_t> atomic_batch_log_index;
    {
      std::lock_guard<std::mutex> lk(mu_);
      result.leader_id = leader_id_;
      result.term = current_term_;
      result.membership_epoch = commit_index_;

      if (!running_.load())
      {
        result.status = AddLearnerProposalStatus::kNodeStopping;
        result.message = "node is stopping";
        return result;
      }

      if (role_ != Role::kLeader)
      {
        result.status = AddLearnerProposalStatus::kNotLeader;
        result.message = "batch learner promotion authority belongs to the current leader";
        return result;
      }

      result.leader_id = config_.node_id;

      auto pending_it = std::find_if(
          pending_add_learner_proposals_.begin(),
          pending_add_learner_proposals_.end(),
          [&request](const PendingAddLearnerProposal &pending) {
            return pending.cluster_id == request.cluster_id &&
                   pending.node_id == request.node_id &&
                   pending.candidate_raft_id == request.candidate_raft_id &&
                   pending.candidate_client_address == request.candidate_client_address &&
                   pending.candidate_raft_address == request.candidate_raft_address &&
                   pending.candidate_incarnation_id ==
                       request.candidate_incarnation_id &&
                   pending.candidate_sequence == request.candidate_sequence &&
                   pending.persistent_generation ==
                       request.persistent_generation &&
                   pending.data_dir_fingerprint ==
                       request.data_dir_fingerprint;
          });
      if (pending_it == pending_add_learner_proposals_.end())
      {
        if (HasCommittedVoterRaftId(config_, request.candidate_raft_id))
        {
          result.status = AddLearnerProposalStatus::kRejected;
          result.message =
              "candidate_raft_id already exists in committed voter set";
          return result;
        }
        result.status = AddLearnerProposalStatus::kRejected;
        result.message =
            "batch learner promotion requires an existing pending learner";
        return result;
      }

      if (!IsPendingLearnerReadyForPromotionLocked(*pending_it))
      {
        result.status = AddLearnerProposalStatus::kPendingMembershipChange;
        result.message =
            "batch learner promotion blocked because learner is still catching up";
        return result;
      }

      const auto targets = CollectAtomicBatchPromotionTargetsLocked();
      if (targets.empty())
      {
        const std::size_t single_target_voter_count =
            CommittedVoterCountLocked() + 1U;
        if (const auto validation_error =
                ValidateTargetCommittedVoterCountLocked(single_target_voter_count);
            validation_error.has_value())
        {
          result.status = AddLearnerProposalStatus::kRejected;
          result.message =
              *validation_error +
              "; waiting for another ready learner before membership commit";
          return result;
        }
        result.status = AddLearnerProposalStatus::kPendingMembershipChange;
        result.message =
            "batch learner promotion waiting for another ready learner";
        return result;
      }

      if (const auto validation_error =
              ValidateAtomicBatchPromotionTargetsLocked(targets);
          validation_error.has_value())
      {
        result.status = AddLearnerProposalStatus::kRejected;
        result.message = *validation_error;
        return result;
      }

      atomic_batch_log_index = PrepareAtomicBatchPromotionLogIndexLocked(targets);
      if (!atomic_batch_log_index.has_value())
      {
        result.status = AddLearnerProposalStatus::kPendingMembershipChange;
        result.message =
            "batch learner promotion boundary is not ready to append committed membership change";
        return result;
      }
    }

    const ReplicationOutcome replication_outcome =
        ReplicateLogEntryToMajority(*atomic_batch_log_index);
    if (replication_outcome != ReplicationOutcome::kReplicated)
    {
      std::lock_guard<std::mutex> lk(mu_);
      result.leader_id = leader_id_;
      result.term = current_term_;
      result.membership_epoch = commit_index_;
      if (!running_.load())
      {
        result.status = AddLearnerProposalStatus::kNodeStopping;
        result.message = "node is stopping";
        return result;
      }
      if (role_ != Role::kLeader)
      {
        result.status = AddLearnerProposalStatus::kNotLeader;
        result.message =
            "batch learner promotion lost leader before membership commit";
        return result;
      }
      result.leader_id = config_.node_id;
      result.status = AddLearnerProposalStatus::kPendingMembershipChange;
      result.message =
          "batch learner promotion did not reach committed membership";
      return result;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      AdvanceCommitIndexUnlocked();
    }

    const ApplyResult apply_result = ApplyCommittedEntries();
    if (!apply_result.Ok)
    {
      std::lock_guard<std::mutex> lk(mu_);
      result.leader_id = leader_id_;
      result.term = current_term_;
      result.membership_epoch = commit_index_;
      if (!running_.load())
      {
        result.status = AddLearnerProposalStatus::kNodeStopping;
        result.message = "node is stopping";
        return result;
      }
      if (role_ != Role::kLeader)
      {
        result.status = AddLearnerProposalStatus::kNotLeader;
        result.message =
            "batch learner promotion lost leader before membership apply";
        return result;
      }
      result.leader_id = config_.node_id;
      result.status = AddLearnerProposalStatus::kRejected;
      result.message = apply_result.message;
      return result;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      result.leader_id = leader_id_;
      result.term = current_term_;
      result.membership_epoch = commit_index_;
      result.committed_membership_changed =
          HasCommittedVoterRaftId(config_, request.candidate_raft_id);
    }
    result.status = AddLearnerProposalStatus::kAcceptedPendingCommit;
    result.message = apply_result.message;
    return result;
  }

  std::string RaftNode::Describe() const
  {
    const NodeStatusSnapshot status = GetStatusSnapshot();
    std::ostringstream oss;
    oss << "node=" << status.node_id
        << ", role=" << status.role
        << ", term=" << status.term;
    {
      std::lock_guard<std::mutex> lk(mu_);
      oss << ", voted_for=" << voted_for_;
    }
    oss << ", leader=" << status.leader_id
        << ", leader_address=" << status.leader_address
        << ", last_log_index=" << status.last_log_index
        << ", commit_index=" << status.commit_index
        << ", last_applied=" << status.last_applied
        << ", last_snapshot_index=" << status.snapshot_index;

    if (!status.peers.empty())
    {
      oss << ", peers=[";
      for (std::size_t i = 0; i < status.peers.size(); ++i)
      {
        if (i > 0)
        {
          oss << "; ";
        }
        oss << status.peers[i].peer_id
            << "(match=" << status.peers[i].match_index
            << ",next=" << status.peers[i].next_index << ")";
      }
      oss << "]";
    }

    return oss.str();
  }

  MetadataStateMachine *RaftNode::GetMetadataStateMachineV2()
  {
    return dynamic_cast<MetadataStateMachine *>(state_machine_.get());
  }

  const MetadataStateMachine *RaftNode::GetMetadataStateMachineV2() const
  {
    return dynamic_cast<const MetadataStateMachine *>(state_machine_.get());
  }

  void RaftNode::CancelElectionTimerLocked()
  {
    // Cancel() may race with a callback which has already been dequeued by the
    // scheduler. The generation check makes such old callbacks harmless.
    ++election_timer_generation_;

    if (election_timer_id_)
    {
      scheduler_.Cancel(*election_timer_id_);
      election_timer_id_.reset();
    }
  }

  void RaftNode::ResetElectionTimerLocked()
  {
    CancelElectionTimerLocked();

    // Leaders send heartbeats; they do not wait for heartbeats from others.
    // Therefore a leader must not keep an election timer running.
    if (!running_.load() || role_ == Role::kLeader)
    {
      return;
    }

    const auto timeout = RandomElectionTimeoutLocked();
    const auto timer_generation = election_timer_generation_;
    auto weak = weak_from_this();
    election_timer_id_ = scheduler_.ScheduleAfter(timeout, [weak, timer_generation]
                                                  {
    if (auto self = weak.lock()) {
      self->OnElectionTimeout(timer_generation);
    } });
  }

  void RaftNode::ResetHeartbeatTimerLocked()
  {
    if (heartbeat_timer_id_)
    {
      scheduler_.Cancel(*heartbeat_timer_id_);
      heartbeat_timer_id_.reset();
    }

    if (role_ != Role::kLeader)
    {
      return;
    }

    auto weak = weak_from_this();
    const auto interval = config_.heartbeat_interval;
    heartbeat_timer_id_ = scheduler_.ScheduleAfter(interval, [weak]
                                                   {
    if (auto self = weak.lock()) {
      self->SendHeartbeats();
      std::lock_guard<std::mutex> lk(self->mu_);
      if (self->running_.load() && self->role_ == Role::kLeader) {
        self->ResetHeartbeatTimerLocked();
      }
    } });
  }

  void RaftNode::ResetSnapshotTimerLocked()
  {
    if (snapshot_timer_id_)
    {
      scheduler_.Cancel(*snapshot_timer_id_);
      snapshot_timer_id_.reset();
    }

    if (!snapshot_config_.enabled || snapshot_config_.snapshot_interval.count() <= 0)
    {
      return;
    }

    auto weak = weak_from_this();
    snapshot_timer_id_ = scheduler_.ScheduleAfter(snapshot_config_.snapshot_interval, [weak]
                                                  {
      if (auto self = weak.lock())
      {
        self->OnSnapshotTimer();
      } });
  }

  std::chrono::milliseconds RaftNode::RandomElectionTimeoutLocked()
  {
    const auto min_ms = static_cast<int>(config_.election_timeout_min.count());
    const auto max_ms = static_cast<int>(config_.election_timeout_max.count());
    std::uniform_int_distribution<int> dist(min_ms, max_ms);
    return std::chrono::milliseconds(dist(rng_));
  }

  void RaftNode::OnElectionTimeout(std::uint64_t timer_generation)
  {
    {
      std::lock_guard<std::mutex> lk(mu_);

      if (!running_.load())
      {
        return;
      }

      // Ignore callbacks from stale election timers. This prevents an old timer
      // from starting a new election after this node has already become leader.
      if (timer_generation != election_timer_generation_)
      {
        return;
      }

      if (role_ == Role::kLeader)
      {
        return;
      }

      election_timer_id_.reset();
    }

    StartElection();
  }

  void RaftNode::OnSnapshotTimer()
  {
    if (!running_.load())
    {
      return;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!running_.load())
      {
        return;
      }
      MaybeScheduleSnapshotLocked(true);
      if (running_.load())
      {
        ResetSnapshotTimerLocked();
      }
    }
  }

  void RaftNode::StartElection()
  {
    std::uint64_t term = 0;
    std::uint64_t last_log_index = 0;
    std::uint64_t last_log_term = 0;
    std::vector<PeerConfig> peers;
    int quorum = 0;

    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!running_.load() || role_ == Role::kLeader)
      {
        return;
      }

      const RuntimeMembershipSummary runtime_membership =
          BuildRuntimeMembershipSummaryLocked();
      if (runtime_membership.local_role != RuntimeMembershipRole::kVoter)
      {
        ResetElectionTimerLocked();
        Log(NodeTag(config_.node_id),
            "skip election because local runtime membership is non-voter, role=",
            RuntimeMembershipRoleName(runtime_membership.local_role));
        return;
      }

      RecordElectionStarted();

      const auto old_role = role_;
      const auto old_term = current_term_;
      const auto old_voted_for = voted_for_;
      const auto old_leader_id = leader_id_;

      role_ = Role::kCandidate;
      ++current_term_;
      voted_for_ = config_.node_id;
      leader_id_ = -1;

      std::string persist_error;
      if (!PersistStateLocked(&persist_error))
      {
        role_ = old_role;
        current_term_ = old_term;
        voted_for_ = old_voted_for;
        leader_id_ = old_leader_id;
        ResetElectionTimerLocked();
        Log(NodeTag(config_.node_id), "start election aborted, persist failed: ", persist_error);
        return;
      }

      term = current_term_;
      last_log_index = LastLogIndexLocked();
      last_log_term = LastLogTermLocked();
      peers.clear();
      peers.reserve(runtime_membership.voter_entries.size());
      for (const auto &entry : runtime_membership.voter_entries)
      {
        if (entry.raft_id == config_.node_id)
        {
          continue;
        }
        peers.push_back(PeerConfig{entry.raft_id, entry.address});
      }
      quorum = static_cast<int>(runtime_membership.committed_voter_quorum_size);

      ResetElectionTimerLocked();
      Log(NodeTag(config_.node_id), "start election, term=", term,
          ", last_log_index=", last_log_index, ", last_log_term=", last_log_term);
    }

    auto votes = std::make_shared<std::atomic<int>>(1);
    auto won = std::make_shared<std::atomic<bool>>(false);
    auto weak = weak_from_this();

    if (quorum <= 1)
    {
      OnElectionWon(term);
      return;
    }

    for (const auto &peer : peers)
    {
      rpc_pool_.Submit([weak, peer, term, last_log_index, last_log_term, votes, won, quorum]
                       {
      auto self = weak.lock();
      if (!self || !self->running_.load()) {
        return;
      }

      raft::VoteRequest request;
      request.set_term(term);
      request.set_candidate_id(self->config_.node_id);
      request.set_last_log_index(last_log_index);
      request.set_last_log_term(last_log_term);

      auto response = self->RequestVoteRpc(peer.node_id, request);
      if (!response.has_value()) {
        return;
      }

      {
        std::lock_guard<std::mutex> lk(self->mu_);
        if (!self->running_.load()) {
          return;
        }
        if (response->term() > self->current_term_) {
          self->BecomeFollowerLocked(response->term(), -1,
                                     "peer replied higher term in RequestVote");
          return;
        }
        if (self->role_ != Role::kCandidate || self->current_term_ != term) {
          return;
        }
      }

      if (!response->vote_granted()) {
        return;
      }

      const int total = votes->fetch_add(1) + 1;
      if (total >= quorum && !won->exchange(true)) {
        self->OnElectionWon(term);
      } });
    }
  }

  void RaftNode::OnElectionWon(std::uint64_t term)
  {
    bool should_send_heartbeat = false;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!running_.load())
      {
        return;
      }
      if (role_ != Role::kCandidate || current_term_ != term)
      {
        return;
      }

      const RuntimeMembershipSummary runtime_membership =
          BuildRuntimeMembershipSummaryLocked();
      if (runtime_membership.local_role != RuntimeMembershipRole::kVoter)
      {
        Log(NodeTag(config_.node_id),
            "reject leadership transition because local runtime membership is non-voter, role=",
            RuntimeMembershipRoleName(runtime_membership.local_role));
        return;
      }

      BecomeLeaderLocked();
      should_send_heartbeat = true;
      Log(NodeTag(config_.node_id), "won election, become leader, term=", current_term_);
    }

    if (should_send_heartbeat)
    {
      if (!ProposeNoOpEntry())
      {
        Log(NodeTag(config_.node_id), "leader no-op append/replication did not complete");
      }
      SendHeartbeats();
    }
  }

void RaftNode::SendHeartbeats()
{
  std::vector<PeerConfig> peers;
  std::uint64_t term = 0;

  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_.load() || role_ != Role::kLeader)
    {
      return;
    }
    peers = BuildUniqueCommittedVoterPeers(config_);
    const auto learner_peers = LearnerReplicationPeersLocked();
    peers.insert(peers.end(), learner_peers.begin(), learner_peers.end());
    term = current_term_;

    for (const auto &peer : peers)
    {
      GetOrCreateReplicatorLocked(peer);
    }
  }

  auto weak = weak_from_this();
  for (const auto &peer : peers)
  {
    rpc_pool_.Submit([weak, peer, term]
                     {
    auto self = weak.lock();
    if (!self || !self->running_.load()) {
      return;
    }

    Replicator* replicator = nullptr;
    {
      std::lock_guard<std::mutex> lk(self->mu_);
      if (!self->running_.load() || self->role_ != Role::kLeader || self->current_term_ != term) {
        return;
      }
      replicator = self->GetOrCreateReplicatorLocked(peer);
    }

    bool should_apply = false;
    if (replicator != nullptr) {
      replicator->ReplicateOnce(term, 0, &should_apply);
    }

    if (should_apply) {
      ApplyResult result = self->ApplyCommittedEntries();
      if (!result.Ok) {
        Log(NodeTag(self->config_.node_id),
            "apply committed entries failed after heartbeat replication, reason=",
            result.message);
      }
    } });
  }
}

  bool RaftNode::BecomeFollowerLocked(std::uint64_t new_term, int new_leader,
                                      const std::string &reason)
  {
    const auto old_role = role_;
    const auto old_term = current_term_;
    const auto old_leader_id = leader_id_;
    bool hard_state_changed = false;

    if (new_term > current_term_)
    {
      current_term_ = new_term;
      voted_for_ = -1;
      hard_state_changed = true;
    }

    role_ = Role::kFollower;
    leader_id_ = new_leader;
    MaybeRecordLeaderChangeLocked(old_leader_id, leader_id_);

    if (heartbeat_timer_id_)
    {
      scheduler_.Cancel(*heartbeat_timer_id_);
      heartbeat_timer_id_.reset();
    }

    if (role_ != Role::kLeader)
    {
      ResetAllPendingLearnerReplicationStateLocked();
      pending_add_learner_proposals_.clear();
      inflight_atomic_batch_promotion_log_index_.reset();
    }

    ResetElectionTimerLocked();

    bool persist_ok = true;
    if (hard_state_changed)
    {
      std::string persist_error;
      persist_ok = PersistStateLocked(&persist_error);
      if (!persist_ok)
      {
        Log(NodeTag(config_.node_id), "persist follower hard state failed: ", persist_error);
      }
    }

    if (old_role != role_ || old_term != current_term_)
    {
      Log(NodeTag(config_.node_id), "become follower, term=", current_term_, ", leader=",
          leader_id_, ", reason=", reason);
    }

    return persist_ok;
  }

  void RaftNode::BecomeLeaderLocked()
  {
    const auto old_leader_id = leader_id_;
    role_ = Role::kLeader;
    leader_id_ = config_.node_id;
    MaybeRecordLeaderChangeLocked(old_leader_id, leader_id_);

    CancelElectionTimerLocked();

    const auto last_log_index = LastLogIndexLocked();
    ResetAllPendingLearnerReplicationStateLocked();
    pending_add_learner_proposals_.clear();
    inflight_atomic_batch_promotion_log_index_.reset();
    next_index_.clear();
    match_index_.clear();
    match_index_[config_.node_id] = last_log_index;
    next_index_[config_.node_id] = SafeAddOne(last_log_index);
    for (const auto &peer : config_.peers)
    {
      next_index_[peer.node_id] = SafeAddOne(last_log_index);
      match_index_[peer.node_id] = 0;
      EnsurePeerClientLocked(peer);
      GetOrCreateReplicatorLocked(peer);
    }

    ResetHeartbeatTimerLocked();
  }

  bool RaftNode::IsCandidateLogUpToDateLocked(std::uint64_t last_log_index,
                                              std::uint64_t last_log_term) const
  {
    const auto my_last_term = LastLogTermLocked();
    if (last_log_term != my_last_term)
    {
      return last_log_term > my_last_term;
    }
    return last_log_index >= LastLogIndexLocked();
  }

  std::uint64_t RaftNode::FirstLogIndexLocked() const
  {
    return log_.empty() ? last_snapshot_index_ : log_.front().index;
  }

  std::uint64_t RaftNode::LastLogIndexLocked() const
  {
    return log_.empty() ? last_snapshot_index_ : log_.back().index;
  }

  std::uint64_t RaftNode::LastLogTermLocked() const
  {
    return log_.empty() ? last_snapshot_term_ : log_.back().term;
  }

  bool RaftNode::HasLogAtIndexLocked(std::uint64_t index) const
  {
    if (log_.empty())
    {
      return false;
    }
    return index >= log_.front().index && index <= log_.back().index;
  }

  std::size_t RaftNode::LogOffsetLocked(std::uint64_t index) const
  {
    return static_cast<std::size_t>(index - log_.front().index);
  }

  const LogRecord *RaftNode::LogAtIndexLocked(std::uint64_t index) const
  {
    if (!HasLogAtIndexLocked(index))
    {
      return nullptr;
    }
    return &log_[LogOffsetLocked(index)];
  }

  std::uint64_t RaftNode::TermAtIndexLocked(std::uint64_t index) const
  {
    if (index == last_snapshot_index_)
    {
      return last_snapshot_term_;
    }
    const LogRecord *record = LogAtIndexLocked(index);
    return record == nullptr ? 0 : record->term;
  }

  std::uint64_t RaftNode::FirstIndexOfTermLocked(std::uint64_t term) const
  {
    for (const auto &record : log_)
    {
      if (record.term == term)
      {
        return record.index;
      }
    }
    return 0;
  }

  void RaftNode::CompactLogPrefixLocked(std::uint64_t last_included_index,
                                        std::uint64_t last_included_term)
  {
    RestoreLogAfterSnapshotLocked(last_included_index, last_included_term, true);
  }

  void RaftNode::RestoreLogAfterSnapshotLocked(std::uint64_t last_included_index,
                                               std::uint64_t last_included_term,
                                               bool keep_suffix_when_boundary_matches)
  {
    if (last_included_index <= last_snapshot_index_ && !log_.empty() &&
        log_.front().index == last_snapshot_index_)
    {
      return;
    }

    bool keep_suffix = false;
    if (keep_suffix_when_boundary_matches)
    {
      const LogRecord *boundary = LogAtIndexLocked(last_included_index);
      keep_suffix = boundary != nullptr && boundary->term == last_included_term;
      if (!keep_suffix && last_included_index == last_snapshot_index_)
      {
        keep_suffix = last_snapshot_term_ == last_included_term;
      }
    }

    std::vector<LogRecord> compacted;
    compacted.push_back(LogRecord{last_included_index, last_included_term,
                                  kSnapshotMarkerCommand});

    if (keep_suffix)
    {
      for (const auto &record : log_)
      {
        if (record.index > last_included_index)
        {
          compacted.push_back(record);
        }
      }
    }

    log_ = std::move(compacted);
    last_snapshot_index_ = last_included_index;
    last_snapshot_term_ = last_included_term;

    if (commit_index_ < last_snapshot_index_)
    {
      commit_index_ = last_snapshot_index_;
    }
    if (last_applied_ < last_snapshot_index_)
    {
      last_applied_ = last_snapshot_index_;
    }
  }

  void RaftNode::SetAppendEntriesConflictHintLocked(
      std::uint64_t probe_index, raft::AppendEntriesResponse *response) const
  {
    if (response == nullptr)
    {
      return;
    }

    response->set_last_log_index(LastLogIndexLocked());
    response->set_conflict_index(0);
    response->set_conflict_term(0);

    if (probe_index < last_snapshot_index_)
    {
      response->set_conflict_index(SafeAddOne(last_snapshot_index_));
      return;
    }

    if (!HasLogAtIndexLocked(probe_index))
    {
      response->set_conflict_index(SafeAddOne(LastLogIndexLocked()));
      return;
    }

    const std::uint64_t conflict_term = TermAtIndexLocked(probe_index);
    response->set_conflict_term(conflict_term);
    response->set_conflict_index(FirstIndexOfTermLocked(conflict_term));
  }

  std::optional<raft::VoteResponse> RaftNode::RequestVoteRpc(int peer_id,
                                                             const raft::VoteRequest &request)
  {
    auto it = clients_.find(peer_id);
    if (it == clients_.end())
    {
      return std::nullopt;
    }

    const auto start = std::chrono::steady_clock::now();
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + config_.rpc_deadline);

    raft::VoteResponse response;
    grpc::Status status;
    {
      std::lock_guard<std::mutex> lk(it->second->mu);
      status = it->second->stub->RequestVote(&context, request, &response);
    }

    if (!status.ok())
    {
      RecordRpcLatency(RpcKind::kRequestVote, false,
                       std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start));
      return std::nullopt;
    }
    RecordRpcLatency(RpcKind::kRequestVote, true,
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start));
    return response;
  }

  std::optional<raft::AppendEntriesResponse> RaftNode::AppendEntriesRpc(
      int peer_id, const raft::AppendEntriesRequest &request)
  {
    auto it = clients_.find(peer_id);
    if (it == clients_.end())
    {
      return std::nullopt;
    }

    const auto start = std::chrono::steady_clock::now();
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + config_.rpc_deadline);

    raft::AppendEntriesResponse response;
    grpc::Status status;
    {
      std::lock_guard<std::mutex> lk(it->second->mu);
      status = it->second->stub->AppendEntries(&context, request, &response);
    }

    if (!status.ok())
    {
      RecordRpcLatency(RpcKind::kAppendEntries, false,
                       std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start));
      return std::nullopt;
    }
    RecordRpcLatency(RpcKind::kAppendEntries, true,
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start));
    return response;
  }

  std::optional<raft::InstallSnapshotResponse> RaftNode::InstallSnapshotRpc(
      int peer_id, const raft::InstallSnapshotRequest &request)
  {
    auto it = clients_.find(peer_id);
    if (it == clients_.end())
    {
      return std::nullopt;
    }

    const auto start = std::chrono::steady_clock::now();
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + config_.rpc_deadline * 20);

    raft::InstallSnapshotResponse response;
    grpc::Status status;
    {
      std::lock_guard<std::mutex> lk(it->second->mu);
      status = it->second->stub->InstallSnapshot(&context, request, &response);
    }

    if (!status.ok())
    {
      RecordRpcLatency(RpcKind::kInstallSnapshot, false,
                       std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now() - start));
      return std::nullopt;
    }
    RecordRpcLatency(RpcKind::kInstallSnapshot, true,
                     std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - start));
    return response;
  }

  bool RaftNode::SendInstallSnapshotToPeer(int peer_id,
                                           std::uint64_t term,
                                           SnapshotProgress *progress)
  {
    if (progress != nullptr)
    {
      *progress = SnapshotProgress{};
    }

    SnapshotMeta meta;
    std::string error;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!running_.load() || role_ != Role::kLeader || current_term_ != term)
      {
        return false;
      }
      meta.last_included_index = last_snapshot_index_;
      meta.last_included_term = last_snapshot_term_;
    }

    if (snapshot_storage_ == nullptr || meta.last_included_index == 0)
    {
      return false;
    }

    std::vector<SnapshotMeta> snapshots;
    if (!snapshot_storage_->ListSnapshots(&snapshots, &error))
    {
      Log(NodeTag(config_.node_id), "list snapshots before install failed: ", error);
      return false;
    }

    bool found = false;
    for (const auto &candidate : snapshots)
    {
      if (candidate.last_included_index == meta.last_included_index &&
          candidate.last_included_term == meta.last_included_term)
      {
        meta = candidate;
        found = true;
        break;
      }
    }
    if (!found && !snapshots.empty())
    {
      meta = snapshots.front();
      found = true;
    }
    if (!found)
    {
      return false;
    }

    if (progress != nullptr)
    {
      progress->last_snapshot_index = meta.last_included_index;
      progress->last_snapshot_term = meta.last_included_term;
    }

    std::ifstream in(meta.snapshot_path, std::ios::binary);
    if (!in.is_open())
    {
      Log(NodeTag(config_.node_id), "open snapshot for install failed: ", meta.snapshot_path);
      return false;
    }
    std::string snapshot_data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof())
    {
      Log(NodeTag(config_.node_id), "read snapshot for install failed: ", meta.snapshot_path);
      return false;
    }

    raft::InstallSnapshotRequest request;
    request.set_term(term);
    request.set_leader_id(config_.node_id);
    request.set_last_included_index(meta.last_included_index);
    request.set_last_included_term(meta.last_included_term);
    request.set_snapshot_data(snapshot_data);

    auto response = InstallSnapshotRpc(peer_id, request);
    if (!response.has_value())
    {
      return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    if (response->term() > current_term_)
    {
      BecomeFollowerLocked(response->term(), -1,
                           "peer replied higher term in InstallSnapshot");
      return false;
    }
    if (!running_.load() || role_ != Role::kLeader || current_term_ != term)
    {
      return false;
    }
    if (!response->success())
    {
      auto &next_index = next_index_[peer_id];
      const std::uint64_t hinted_next = SafeAddOne(response->last_log_index());
      if (progress != nullptr)
      {
        progress->last_log_index = response->last_log_index();
      }
      if (hinted_next > 0)
      {
        next_index = hinted_next;
      }
      return false;
    }

    auto &match_index = match_index_[peer_id];
    auto &next_index = next_index_[peer_id];
    match_index = std::max<std::uint64_t>(match_index, meta.last_included_index);
    next_index = std::max<std::uint64_t>(next_index, SafeAddOne(meta.last_included_index));
    auto &snapshot_progress = peer_snapshot_progress_[peer_id];
    const std::uint64_t previous_snapshot_index = snapshot_progress.last_snapshot_index;
    if (meta.last_included_index > previous_snapshot_index)
    {
      snapshot_progress.last_snapshot_index = meta.last_included_index;
      snapshot_progress.last_snapshot_term = meta.last_included_term;
    }
    else if (meta.last_included_index == previous_snapshot_index)
    {
      snapshot_progress.last_snapshot_term =
          std::max<std::uint64_t>(snapshot_progress.last_snapshot_term,
                                  meta.last_included_term);
    }
    snapshot_progress.last_applied_index =
        std::max<std::uint64_t>(snapshot_progress.last_applied_index,
                                meta.last_included_index);
    snapshot_progress.last_log_index =
        std::max<std::uint64_t>(snapshot_progress.last_log_index,
                                response->last_log_index());
    if (progress != nullptr)
    {
      *progress = snapshot_progress;
    }
    return true;
  }

  void RaftNode::OnRequestVote(const raft::VoteRequest &request, raft::VoteResponse *response)
  {
    std::lock_guard<std::mutex> lk(mu_);
    response->set_term(current_term_);
    response->set_vote_granted(false);

    if (request.term() < current_term_)
    {
      return;
    }

    if (request.term() > current_term_)
    {
      if (!BecomeFollowerLocked(request.term(), -1, "received higher term vote request"))
      {
        response->set_term(current_term_);
        return;
      }
    }

    const RuntimeMembershipSummary runtime_membership =
        BuildRuntimeMembershipSummaryLocked();
    if (runtime_membership.local_role != RuntimeMembershipRole::kVoter)
    {
      response->set_term(current_term_);
      Log(NodeTag(config_.node_id),
          "reject vote request because local runtime membership is non-voter, role=",
          RuntimeMembershipRoleName(runtime_membership.local_role),
          ", candidate=", request.candidate_id(),
          ", term=", request.term());
      return;
    }

    const bool up_to_date =
        IsCandidateLogUpToDateLocked(request.last_log_index(), request.last_log_term());
    const bool can_vote = (voted_for_ == -1 || voted_for_ == request.candidate_id());

    if (can_vote && up_to_date)
    {
      const int old_voted_for = voted_for_;
      voted_for_ = request.candidate_id();

      std::string persist_error;
      if (PersistStateLocked(&persist_error))
      {
        response->set_vote_granted(true);
        ResetElectionTimerLocked();
        Log(NodeTag(config_.node_id), "grant vote to candidate=", request.candidate_id(),
            ", term=", current_term_);
      }
      else
      {
        voted_for_ = old_voted_for;
        Log(NodeTag(config_.node_id), "reject vote because persist failed, candidate=",
            request.candidate_id(), ", reason=", persist_error);
      }
    }

    response->set_term(current_term_);
  }

  void RaftNode::OnAppendEntries(const raft::AppendEntriesRequest &request,
                                 raft::AppendEntriesResponse *response)
  {
    std::unique_lock<std::mutex> lk(mu_);
    response->set_term(current_term_);
    response->set_success(false);
    response->set_match_index(last_snapshot_index_);
    response->set_last_log_index(LastLogIndexLocked());
    response->set_conflict_index(0);
    response->set_conflict_term(0);

    bool should_apply = false;

    if (request.term() < current_term_)
    {
      return;
    }

    if (request.term() > current_term_ || role_ != Role::kFollower ||
        leader_id_ != request.leader_id())
    {
      if (!BecomeFollowerLocked(request.term(), request.leader_id(), "received append entries"))
      {
        response->set_term(current_term_);
        response->set_last_log_index(LastLogIndexLocked());
        return;
      }
    }
    else
    {
      leader_id_ = request.leader_id();
      ResetElectionTimerLocked();
    }

    if (request.prev_log_index() < last_snapshot_index_)
    {
      response->set_term(current_term_);
      SetAppendEntriesConflictHintLocked(request.prev_log_index(), response);
      return;
    }

    if (!HasLogAtIndexLocked(request.prev_log_index()))
    {
      response->set_term(current_term_);
      SetAppendEntriesConflictHintLocked(request.prev_log_index(), response);
      return;
    }

    if (TermAtIndexLocked(request.prev_log_index()) != request.prev_log_term())
    {
      response->set_term(current_term_);
      SetAppendEntriesConflictHintLocked(request.prev_log_index(), response);
      return;
    }

    bool log_changed = false;
    std::optional<std::vector<LogRecord>> old_log;
    std::uint64_t match_index = request.prev_log_index();

    for (int req_idx = 0; req_idx < request.entries_size(); ++req_idx)
    {
      const auto &req_entry = request.entries(req_idx);
      if (req_entry.index() <= last_snapshot_index_)
      {
        match_index = std::max<std::uint64_t>(match_index, req_entry.index());
        continue;
      }

      if (req_entry.index() > SafeAddOne(LastLogIndexLocked()))
      {
        response->set_term(current_term_);
        SetAppendEntriesConflictHintLocked(req_entry.index() - 1, response);
        return;
      }

      if (HasLogAtIndexLocked(req_entry.index()))
      {
        const std::size_t offset = LogOffsetLocked(req_entry.index());
        if (log_[offset].term != req_entry.term())
        {
          if (!old_log.has_value())
          {
            old_log = log_;
          }
          log_.resize(offset);
          log_changed = true;
        }
        else
        {
          match_index = req_entry.index();
          continue;
        }
      }

      if (!old_log.has_value())
      {
        old_log = log_;
      }
      log_.push_back(LogRecord{req_entry.index(), req_entry.term(), req_entry.command()});
      log_changed = true;
      match_index = req_entry.index();
    }

    if (log_changed)
    {
      std::string persist_error;
      if (!PersistStateLocked(&persist_error))
      {
        if (old_log.has_value())
        {
          log_ = std::move(*old_log);
        }
        Log(NodeTag(config_.node_id), "append entries persist failed: ", persist_error);
        response->set_term(current_term_);
        response->set_success(false);
        response->set_match_index(match_index);
        response->set_last_log_index(LastLogIndexLocked());
        return;
      }
    }

    if (request.leader_commit() > commit_index_)
    {
      const std::uint64_t new_commit =
          std::min<std::uint64_t>(request.leader_commit(), LastLogIndexLocked());

      if (new_commit > commit_index_)
      {
        const std::uint64_t old_commit_index = commit_index_;
        commit_index_ = new_commit;
        std::string persist_error;
        if (!PersistStateLocked(&persist_error))
        {
          commit_index_ = old_commit_index;
          Log(NodeTag(config_.node_id), "persist commit index after append entries failed: ", persist_error);
          response->set_term(current_term_);
          response->set_success(false);
          response->set_match_index(match_index);
          response->set_last_log_index(LastLogIndexLocked());
          return;
        }
        should_apply = true;
      }
    }

    response->set_term(current_term_);
    response->set_success(true);
    response->set_match_index(match_index);
    response->set_last_log_index(LastLogIndexLocked());

    lk.unlock();

    if (should_apply)
    {
      ApplyResult result = ApplyCommittedEntries();
      if (!result.Ok)
      {
        Log(NodeTag(config_.node_id),
            "apply committed entries failed after append entries, reason=",
            result.message);
      }
    }
  }

  void RaftNode::OnInstallSnapshot(const raft::InstallSnapshotRequest &request,
                                   raft::InstallSnapshotResponse *response)
  {
    response->set_success(false);
    {
      std::lock_guard<std::mutex> lk(mu_);
      response->set_term(current_term_);
      response->set_last_log_index(LastLogIndexLocked());
    }

    if (!snapshot_config_.enabled || snapshot_storage_ == nullptr || state_machine_ == nullptr)
    {
      response->set_message("snapshot is disabled");
      return;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      response->set_term(current_term_);
      response->set_last_log_index(LastLogIndexLocked());

      if (request.term() < current_term_)
      {
        response->set_message("stale term");
        return;
      }

      if (request.term() > current_term_ || role_ != Role::kFollower ||
          leader_id_ != request.leader_id())
      {
        if (!BecomeFollowerLocked(request.term(), request.leader_id(), "received install snapshot"))
        {
          response->set_term(current_term_);
          response->set_message("persist higher term failed");
          return;
        }
      }
      else
      {
        leader_id_ = request.leader_id();
        ResetElectionTimerLocked();
      }

      if (request.last_included_index() <= last_snapshot_index_)
      {
        response->set_term(current_term_);
        response->set_success(true);
        response->set_last_log_index(LastLogIndexLocked());
        response->set_message("snapshot already installed");
        return;
      }
    }

    std::error_code ec;
    std::filesystem::create_directories(snapshot_config_.snapshot_dir, ec);
    if (ec)
    {
      response->set_message("create snapshot directory failed: " + ec.message());
      return;
    }

    const std::filesystem::path temp_path = std::filesystem::path(snapshot_config_.snapshot_dir) /
                                            ("install_snapshot_node_" + std::to_string(config_.node_id) + ".bin.tmp");
    {
      std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
      if (!out.is_open())
      {
        response->set_message("open install snapshot temp file failed");
        return;
      }
      const std::string &data = request.snapshot_data();
      if (!data.empty())
      {
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
      }
      out.flush();
      if (!out)
      {
        response->set_message("write install snapshot temp file failed");
        return;
      }
    }

    SnapshotMeta saved_meta;
    std::string save_error;
    if (!snapshot_storage_->SaveSnapshotFile(temp_path.string(), request.last_included_index(),
                                             request.last_included_term(), &saved_meta, &save_error))
    {
      response->set_message("persist installed snapshot failed: " + save_error);
      std::filesystem::remove(temp_path, ec);
      return;
    }

    {
      std::lock_guard<std::mutex> apply_lk(apply_mu_);
      SnapshotResult load_result = state_machine_->LoadSnapshot(saved_meta.snapshot_path);
      if (!load_result.Ok())
      {
        response->set_message("load installed snapshot failed: " + load_result.message);
        return;
      }

      std::string boundary_error;
      const auto boundary = ResolveLoadedSnapshotAppliedBoundary(
          *state_machine_,
          request.last_included_index(),
          request.last_included_term(),
          &boundary_error);
      if (!boundary.has_value())
      {
        response->set_message("load installed snapshot boundary check failed: " + boundary_error);
        return;
      }

      std::lock_guard<std::mutex> lk(mu_);
      if (request.term() < current_term_)
      {
        response->set_term(current_term_);
        response->set_message("term changed while installing snapshot");
        return;
      }

      CompactLogPrefixLocked(boundary->index, boundary->term);
      commit_index_ = std::max<std::uint64_t>(commit_index_, boundary->index);
      last_applied_ = boundary->index;

      std::string persist_error;
      if (!PersistStateLocked(&persist_error))
      {
        response->set_term(current_term_);
        response->set_last_log_index(LastLogIndexLocked());
        response->set_message("persist installed snapshot raft state failed: " + persist_error);
        return;
      }

      response->set_term(current_term_);
      response->set_success(true);
      response->set_last_log_index(LastLogIndexLocked());
      response->set_message("snapshot installed");
    }

    std::string prune_error;
    if (!snapshot_storage_->PruneSnapshots(snapshot_config_.max_snapshot_count, &prune_error) &&
        !prune_error.empty())
    {
      Log(NodeTag(config_.node_id), "prune snapshots after install failed: ", prune_error);
    }
  }

  const char *RaftNode::RoleName(Role role)
  {
    switch (role)
    {
    case Role::kFollower:
      return "Follower";
    case Role::kCandidate:
      return "Candidate";
    case Role::kLeader:
      return "Leader";
    }
    return "Unknown";
  }

  const char *RaftNode::RpcKindName(RpcKind kind)
  {
    switch (kind)
    {
    case RpcKind::kRequestVote:
      return "request_vote";
    case RpcKind::kAppendEntries:
      return "append_entries";
    case RpcKind::kInstallSnapshot:
      return "install_snapshot";
    }
    return "unknown";
  }

  std::vector<RaftNode::RpcMetricState> RaftNode::BuildRpcMetricStateTemplate()
  {
    std::vector<RpcMetricState> metrics;
    metrics.reserve(3);
    for (const RpcKind kind : {
             RpcKind::kRequestVote,
             RpcKind::kAppendEntries,
             RpcKind::kInstallSnapshot,
         })
    {
      metrics.push_back(RpcMetricState{RpcKindName(kind), 0, 0, 0, 0});
    }
    return metrics;
  }

  RaftNode::RpcMetricState &RaftNode::RpcMetricLocked(RpcKind kind)
  {
    return rpc_metrics_.at(static_cast<std::size_t>(kind));
  }

  std::string RaftNode::AddressForNodeLocked(int node_id) const
  {
    if (node_id == config_.node_id)
    {
      return config_.address;
    }
    for (const auto &peer : config_.peers)
    {
      if (peer.node_id == node_id)
      {
        return peer.address;
      }
    }
    for (const auto &pending_proposal : pending_add_learner_proposals_)
    {
      if (pending_proposal.candidate_raft_id == node_id)
      {
        return pending_proposal.candidate_raft_address;
      }
    }
    return "";
  }

  void RaftNode::EnsurePeerClientLocked(const PeerConfig &peer)
  {
    if (clients_.find(peer.node_id) != clients_.end())
    {
      return;
    }

    auto client = std::make_unique<PeerClient>();
    client->peer_id = peer.node_id;
    client->address = peer.address;
    client->channel = grpc::CreateChannel(peer.address, grpc::InsecureChannelCredentials());
    client->stub = raft::RaftService::NewStub(client->channel);
    clients_[peer.node_id] = std::move(client);
  }

  std::vector<PeerConfig> RaftNode::LearnerReplicationPeersLocked() const
  {
    std::vector<PeerConfig> peers;
    peers.reserve(pending_add_learner_proposals_.size());
    for (const auto &pending : pending_add_learner_proposals_)
    {
      if (pending.candidate_raft_id <= 0 || pending.candidate_raft_address.empty())
      {
        continue;
      }
      if (pending.candidate_raft_id == config_.node_id ||
          std::any_of(config_.peers.begin(),
                      config_.peers.end(),
                      [&pending](const PeerConfig &peer) {
                        return peer.node_id == pending.candidate_raft_id;
                      }))
      {
        continue;
      }
      peers.push_back(
          PeerConfig{pending.candidate_raft_id, pending.candidate_raft_address});
    }
    return peers;
  }

  void RaftNode::InitializePendingLearnerReplicationStateLocked(
      const PendingAddLearnerProposal &proposal)
  {
    if (proposal.candidate_raft_id <= 0 || proposal.candidate_raft_address.empty())
    {
      return;
    }

    const PeerConfig pending_learner{proposal.candidate_raft_id,
                                     proposal.candidate_raft_address};
    match_index_[pending_learner.node_id] = 0;
    next_index_[pending_learner.node_id] = SafeAddOne(LastLogIndexLocked());
    EnsurePeerClientLocked(pending_learner);
    GetOrCreateReplicatorLocked(pending_learner);
  }

  void RaftNode::ResetPendingLearnerReplicationStateLocked(
      const std::int32_t learner_raft_id)
  {
    if (learner_raft_id <= 0)
    {
      return;
    }

    match_index_.erase(learner_raft_id);
    next_index_.erase(learner_raft_id);
    peer_snapshot_progress_.erase(learner_raft_id);
    replicators_.erase(learner_raft_id);
    clients_.erase(learner_raft_id);
  }

  void RaftNode::ResetAllPendingLearnerReplicationStateLocked()
  {
    for (const auto &pending : pending_add_learner_proposals_)
    {
      ResetPendingLearnerReplicationStateLocked(pending.candidate_raft_id);
    }
  }

  void RaftNode::MaybeRecordLeaderChangeLocked(int old_leader_id, int new_leader_id)
  {
    if (old_leader_id == new_leader_id || new_leader_id < 0)
    {
      return;
    }
    std::lock_guard<std::mutex> lk(metrics_mu_);
    ++leader_change_count_;
  }

  void RaftNode::RecordProposeResult(bool success)
  {
    std::lock_guard<std::mutex> lk(metrics_mu_);
    if (success)
    {
      ++propose_success_count_;
    }
    else
    {
      ++propose_failure_count_;
    }
  }

  void RaftNode::RecordElectionStarted()
  {
    std::lock_guard<std::mutex> lk(metrics_mu_);
    ++election_count_;
  }

  void RaftNode::RecordRpcLatency(RpcKind kind, bool success, std::chrono::microseconds latency)
  {
    std::lock_guard<std::mutex> lk(metrics_mu_);
    auto &metric = RpcMetricLocked(kind);
    if (success)
    {
      ++metric.success_count;
    }
    else
    {
      ++metric.failure_count;
    }
    const auto latency_us = static_cast<std::uint64_t>(std::max<std::int64_t>(0, latency.count()));
    metric.total_latency_us += latency_us;
    metric.max_latency_us = std::max(metric.max_latency_us, latency_us);
  }

  void RaftNode::RecordSnapshotOutcome(bool success)
  {
    std::lock_guard<std::mutex> lk(metrics_mu_);
    if (success)
    {
      ++snapshot_success_count_;
    }
    else
    {
      ++snapshot_failure_count_;
    }
  }

  void RaftNode::RecordStoragePersistFailure()
  {
    std::lock_guard<std::mutex> lk(metrics_mu_);
    ++storage_persist_failure_count_;
  }

  ProposeResult RaftNode::Propose(const Command &command)
  {
    ProposeResult result;
    std::string reason;
    std::string command_data;
    std::uint64_t log_index = 0;
    std::uint64_t term = 0;

    {
      std::unique_lock<std::mutex> lk(mu_);

      if (!running_.load())
      {
        result.status = ProposeStatus::kNodeStopping;
        result.leader_id = leader_id_;
        result.term = current_term_;
        result.message = "node is stopping";
        RecordProposeResult(false);
        return result;
      }

      if (role_ != Role::kLeader)
      {
        result.status = ProposeStatus::kNotLeader;
        result.leader_id = leader_id_;
        result.term = current_term_;
        result.message = "node is not the leader";
        RecordProposeResult(false);
        return result;
      }

      if (!ValidateCommandUnlocked(command, &reason))
      {
        result.status = ProposeStatus::kInvalidCommand;
        result.leader_id = config_.node_id;
        result.term = current_term_;
        result.message = reason;
        RecordProposeResult(false);
        return result;
      }

      // 命令序列化后准备写入日志
      command_data = command.Serialize();
      if (command_data.empty())
      {
        result.status = ProposeStatus::kInvalidCommand;
        result.leader_id = config_.node_id;
        result.term = current_term_;
        result.message = "failed to serialize command";
        RecordProposeResult(false);
        return result;
      }

      // 先追加到本地日志，后续再复制到其他节点
      term = current_term_;
      log_index = AppendLocalLogUnlocked(command_data);
      if (log_index == 0)
      {
        result.status = ProposeStatus::kReplicationFailed;
        result.leader_id = config_.node_id;
        result.term = current_term_;
        result.message = "failed to append and persist local log entry";
        RecordProposeResult(false);
        return result;
      }

      result.leader_id = config_.node_id;
      result.term = term;
      result.log_index = log_index;
      result.message = "log appended locally";
    }

    // 锁外复制，避免长时间阻塞 Raft 核心状态
    const ReplicationOutcome replicated = ReplicateLogEntryToMajority(log_index);
    if (replicated != ReplicationOutcome::kReplicated)
    {
      std::lock_guard<std::mutex> lk(mu_);
      result.leader_id = leader_id_;
      result.term = current_term_;
      if (replicated == ReplicationOutcome::kTimeout)
      {
        result.status = ProposeStatus::kTimeout;
        result.message = "timed out waiting for majority replication";
      }
      else if (replicated == ReplicationOutcome::kLostLeadership)
      {
        result.status = ProposeStatus::kNotLeader;
        result.message = "lost leadership before the log entry reached a majority";
      }
      else
      {
        result.status = ProposeStatus::kReplicationFailed;
        result.message = "failed to replicate log entry to majority";
      }
      RecordProposeResult(false);
      return result;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      // 达到多数派节点后推进提交位置
      AdvanceCommitIndexUnlocked();
    }

    ApplyResult apply_result = ApplyCommittedEntries();
    if (!apply_result.Ok)
    {
      result.status = ProposeStatus::kApplyFailed;
      result.message = apply_result.message;
      RecordProposeResult(false);
      return result;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      if (last_applied_ < log_index)
      {
        result.status = ProposeStatus::kApplyFailed;
        result.message = "log committed but not applied";
        RecordProposeResult(false);
        return result;
      }
    }

    result.status = ProposeStatus::kOk;
    result.message = "command committed and applied";
    RecordProposeResult(true);
    return result;
  }

  ProposeResult RaftNode::ProposeMetadata(const std::string &metadata_command_data)
  {
    ProposeResult result;
    std::string reason;
    MetadataCommand metadata_command;
    if (!ParseMetadataCommand(metadata_command_data, &metadata_command))
    {
      result.status = ProposeStatus::kInvalidCommand;
      result.message = "failed to parse metadata command";
      RecordProposeResult(false);
      return result;
    }

    if (!ValidateMetadataCommand(metadata_command, &reason))
    {
      result.status = ProposeStatus::kInvalidCommand;
      result.message = "invalid metadata command: " + reason;
      RecordProposeResult(false);
      return result;
    }

    const auto wait_timeout = config_.rpc_deadline;
    if (wait_timeout <= std::chrono::milliseconds::zero())
    {
      result.status = ProposeStatus::kTimeout;
      result.message = "metadata proposal deadline already expired before admission";
      RecordProposeResult(false);
      return result;
    }

    const auto wait_deadline = std::chrono::steady_clock::now() + wait_timeout;
    const std::string fingerprint =
        ComputeMetadataCommandFingerprint(metadata_command);
    const std::string &request_id = metadata_command.request_id;
    std::shared_ptr<MetadataProposalTracker> tracker;
    bool joined_existing = false;

    {
      std::unique_lock<std::mutex> lk(mu_);

      if (!running_.load())
      {
        result.status = ProposeStatus::kNodeStopping;
        result.leader_id = leader_id_;
        result.term = current_term_;
        result.message = "node is stopping";
        RecordProposeResult(false);
        return result;
      }

      if (role_ != Role::kLeader)
      {
        result.status = ProposeStatus::kNotLeader;
        result.leader_id = leader_id_;
        result.term = current_term_;
        result.message = "node is not the leader";
        RecordProposeResult(false);
        return result;
      }

      if (std::chrono::steady_clock::now() >= wait_deadline)
      {
        result.status = ProposeStatus::kTimeout;
        result.leader_id = config_.node_id;
        result.term = current_term_;
        result.message = "metadata proposal deadline already expired before admission";
        RecordProposeResult(false);
        return result;
      }

      PruneCompletedMetadataProposalsLocked();

      const auto completed_it = metadata_completed_proposals_.find(request_id);
      if (completed_it != metadata_completed_proposals_.end())
      {
        if (completed_it->second.fingerprint != fingerprint)
        {
          result.status = ProposeStatus::kApplyFailed;
          result.leader_id = config_.node_id;
          result.term = current_term_;
          result.message = "idempotency conflict: request_id maps to different command";
          RecordProposeResult(false);
          return result;
        }

        result = completed_it->second.result;
        RecordProposeResult(result.Ok());
        return result;
      }

      const auto inflight_it = metadata_inflight_proposals_.find(request_id);
      if (inflight_it != metadata_inflight_proposals_.end())
      {
        if (inflight_it->second->fingerprint != fingerprint)
        {
          result.status = ProposeStatus::kApplyFailed;
          result.leader_id = config_.node_id;
          result.term = current_term_;
          result.message = "idempotency conflict: request_id maps to different command";
          RecordProposeResult(false);
          return result;
        }

        tracker = inflight_it->second;
        joined_existing = true;
      }
      else
      {
        if (metadata_inflight_proposals_.size() >= kMaxInflightMetadataProposals)
        {
          result.status = ProposeStatus::kOverloaded;
          result.leader_id = config_.node_id;
          result.term = current_term_;
          result.message =
              "metadata proposal admission rejected: in-flight limit reached";
          RecordProposeResult(false);
          return result;
        }

        tracker = std::make_shared<MetadataProposalTracker>();
        tracker->fingerprint = fingerprint;
        metadata_inflight_proposals_.emplace(request_id, tracker);
      }
    }

    if (!joined_existing)
    {
      rpc_pool_.Submit(
          [this, request_id, metadata_command_data, tracker]()
          {
            ExecuteMetadataProposal(request_id, metadata_command_data, tracker);
          });
    }

    if (WaitForMetadataProposalTracker(tracker, wait_deadline, &result))
    {
      RecordProposeResult(result.Ok());
      return result;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      result.status = ProposeStatus::kTimeout;
      result.leader_id = leader_id_;
      result.term = current_term_;
      result.message = "timed out waiting for metadata proposal completion";
    }
    RecordProposeResult(false);
    return result;
  }

  void RaftNode::ExecuteMetadataProposal(
      const std::string &request_id,
      const std::string &metadata_command_data,
      std::shared_ptr<MetadataProposalTracker> tracker)
  {
    Command command;
    command.type = CommandType::kMetadata;
    command.metadata_payload = metadata_command_data;

    ProposeResult result;
    std::string reason;
    std::string command_data;
    std::uint64_t log_index = 0;
    std::uint64_t term = 0;
    bool completed_in_lock_scope = false;

    {
      std::unique_lock<std::mutex> lk(mu_);

      if (!running_.load())
      {
        result.status = ProposeStatus::kNodeStopping;
        result.leader_id = leader_id_;
        result.term = current_term_;
        result.message = "node is stopping";
        completed_in_lock_scope = true;
      }
      else if (role_ != Role::kLeader)
      {
        result.status = ProposeStatus::kNotLeader;
        result.leader_id = leader_id_;
        result.term = current_term_;
        result.message = "node is not the leader";
        completed_in_lock_scope = true;
      }
      else if (!ValidateCommandUnlocked(command, &reason))
      {
        result.status = ProposeStatus::kInvalidCommand;
        result.leader_id = config_.node_id;
        result.term = current_term_;
        result.message = reason;
        completed_in_lock_scope = true;
      }
      else
      {
        command_data = command.Serialize();
        if (command_data.empty())
        {
          result.status = ProposeStatus::kInvalidCommand;
          result.leader_id = config_.node_id;
          result.term = current_term_;
          result.message = "failed to serialize command";
          completed_in_lock_scope = true;
        }

        if (!completed_in_lock_scope)
        {
          term = current_term_;
          log_index = AppendLocalLogUnlocked(command_data);
          if (log_index == 0)
          {
            result.status = ProposeStatus::kReplicationFailed;
            result.leader_id = config_.node_id;
            result.term = current_term_;
            result.message = "failed to append and persist local log entry";
            completed_in_lock_scope = true;
          }
        }

        if (!completed_in_lock_scope)
        {
          result.leader_id = config_.node_id;
          result.term = term;
          result.log_index = log_index;
          result.message = "log appended locally";
        }
      }
    }

    if (completed_in_lock_scope)
    {
      CompleteMetadataProposal(request_id, tracker, tracker->fingerprint, result);
      return;
    }

    const ReplicationOutcome replicated = ReplicateLogEntryToMajority(
        log_index,
        std::chrono::steady_clock::now() + ScaleDeadline(config_.rpc_deadline, 20));
    if (replicated != ReplicationOutcome::kReplicated)
    {
      {
        std::lock_guard<std::mutex> lk(mu_);
        result.leader_id = leader_id_;
        result.term = current_term_;
      }
      if (replicated == ReplicationOutcome::kTimeout)
      {
        result.status = ProposeStatus::kTimeout;
        result.message = "timed out waiting for majority replication";
      }
      else if (replicated == ReplicationOutcome::kLostLeadership)
      {
        result.status = ProposeStatus::kNotLeader;
        result.message = "lost leadership before the log entry reached a majority";
      }
      else
      {
        result.status = ProposeStatus::kReplicationFailed;
        result.message = "failed to replicate log entry to majority";
      }
      CompleteMetadataProposal(request_id, tracker, tracker->fingerprint, result);
      return;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      AdvanceCommitIndexUnlocked();
    }

    ApplyResult apply_result = ApplyCommittedEntries();
    if (!apply_result.Ok)
    {
      result.status = ProposeStatus::kApplyFailed;
      result.message = apply_result.message;
      CompleteMetadataProposal(request_id, tracker, tracker->fingerprint, result);
      return;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      if (last_applied_ < log_index)
      {
        result.status = ProposeStatus::kApplyFailed;
        result.message = "log committed but not applied";
        completed_in_lock_scope = true;
      }
    }

    if (completed_in_lock_scope)
    {
      CompleteMetadataProposal(request_id, tracker, tracker->fingerprint, result);
      return;
    }

    result.status = ProposeStatus::kOk;
    result.message = apply_result.message;
    CompleteMetadataProposal(request_id, tracker, tracker->fingerprint, result);
  }

  bool RaftNode::WaitForMetadataProposalTracker(
      const std::shared_ptr<MetadataProposalTracker> &tracker,
      std::chrono::steady_clock::time_point deadline,
      ProposeResult *result) const
  {
    if (tracker == nullptr)
    {
      return false;
    }

    std::unique_lock<std::mutex> lk(tracker->mu);
    while (!tracker->completed)
    {
      if (tracker->cv.wait_until(lk, deadline) == std::cv_status::timeout)
      {
        return false;
      }
    }

    if (result != nullptr)
    {
      *result = tracker->result;
    }
    return true;
  }

  void RaftNode::CompleteMetadataProposal(
      const std::string &request_id,
      const std::shared_ptr<MetadataProposalTracker> &tracker,
      const std::string &fingerprint,
      const ProposeResult &result)
  {
    if (tracker != nullptr)
    {
      {
        std::lock_guard<std::mutex> tracker_lk(tracker->mu);
        tracker->completed = true;
        tracker->result = result;
      }
      tracker->cv.notify_all();
    }

    std::lock_guard<std::mutex> lk(mu_);
    const auto inflight_it = metadata_inflight_proposals_.find(request_id);
    if (inflight_it != metadata_inflight_proposals_.end() &&
        inflight_it->second == tracker)
    {
      metadata_inflight_proposals_.erase(inflight_it);
    }
    if (result.status == ProposeStatus::kOk ||
        result.status == ProposeStatus::kApplyFailed ||
        result.status == ProposeStatus::kInvalidCommand)
    {
      metadata_completed_proposals_[request_id] =
          CompletedMetadataProposal{fingerprint, result};
      metadata_completed_proposal_order_.push_back(request_id);
      PruneCompletedMetadataProposalsLocked();
    }
  }

  void RaftNode::PruneCompletedMetadataProposalsLocked()
  {
    while (metadata_completed_proposal_order_.size() >
           kCompletedMetadataProposalCacheLimit)
    {
      const std::string request_id =
          metadata_completed_proposal_order_.front();
      metadata_completed_proposal_order_.erase(
          metadata_completed_proposal_order_.begin());
      metadata_completed_proposals_.erase(request_id);
    }
  }

  StrongConsistencyMetadataStateMachine *RaftNode::GetMetadataStateMachine()
  {
    return dynamic_cast<StrongConsistencyMetadataStateMachine *>(state_machine_.get());
  }

  const StrongConsistencyMetadataStateMachine *RaftNode::GetMetadataStateMachine() const
  {
    return dynamic_cast<const StrongConsistencyMetadataStateMachine *>(state_machine_.get());
  }

  bool RaftNode::ValidateCommandUnlocked(const Command &command, std::string *reason) const
  {
    if (!command.IsValid())
    {
      if (reason != nullptr)
      {
        *reason = "invalid command";
      }
      return false;
    }

    const std::string serialized = command.Serialize();
    if (serialized.empty())
    {
      if (reason != nullptr)
      {
        *reason = "command serialization result is empty";
      }
      return false;
    }
    if (serialized.size() > config_.proposal_limits.max_command_bytes)
    {
      if (reason != nullptr)
      {
        *reason = "command size exceeds limit";
      }
      return false;
    }
    return true;
  }

  std::uint64_t RaftNode::AppendLocalLogUnlocked(const std::string &command_data)
  {
    const std::uint64_t new_index = SafeAddOne(LastLogIndexLocked());
    log_.push_back(LogRecord{
        new_index,
        current_term_,
        command_data,
    });

    match_index_[config_.node_id] = new_index;
    next_index_[config_.node_id] = SafeAddOne(new_index);

    std::string persist_error;
    if (!PersistStateLocked(&persist_error))
    {
      log_.pop_back();
      match_index_[config_.node_id] = LastLogIndexLocked();
      next_index_[config_.node_id] = SafeAddOne(LastLogIndexLocked());
      Log(NodeTag(config_.node_id), "append local log persist failed: ", persist_error);
      return 0;
    }

    return new_index;
  }

RaftNode::ReplicationOutcome RaftNode::ReplicateLogEntryToMajority(
    std::uint64_t log_index)
{
  return ReplicateLogEntryToMajority(
      log_index,
      std::chrono::steady_clock::now() + ScaleDeadline(config_.rpc_deadline, 20));
}

RaftNode::ReplicationOutcome RaftNode::ReplicateLogEntryToMajority(
    std::uint64_t log_index,
    std::chrono::steady_clock::time_point deadline)
{

  while (std::chrono::steady_clock::now() < deadline)
  {
    std::vector<PeerConfig> peers;
    std::uint64_t term = 0;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!running_.load() || role_ != Role::kLeader)
      {
        return ReplicationOutcome::kLostLeadership;
      }
      if (!HasLogAtIndexLocked(log_index))
      {
        return log_index <= last_snapshot_index_ ? ReplicationOutcome::kReplicated
                                                 : ReplicationOutcome::kLogUnavailable;
      }
      peers = BuildUniqueCommittedVoterPeers(config_);
      term = current_term_;

      for (const auto &peer : peers)
      {
        GetOrCreateReplicatorLocked(peer);
      }
    }

    const std::size_t majority = ComputeCommittedVoterQuorumSize(config_);
    if (majority <= 1)
    {
      return ReplicationOutcome::kReplicated;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      const std::size_t replicated_count =
          CountReplicatedCommittedVoters(config_, match_index_, log_index);
      if (replicated_count >= majority)
      {
        return ReplicationOutcome::kReplicated;
      }
    }

    for (const auto &peer : peers)
    {
      Replicator *replicator = nullptr;
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_.load() || role_ != Role::kLeader || current_term_ != term)
        {
          return ReplicationOutcome::kLostLeadership;
        }
        const auto it = match_index_.find(peer.node_id);
        if (it != match_index_.end() && it->second >= log_index)
        {
          continue;
        }
        replicator = GetOrCreateReplicatorLocked(peer);
      }

      bool should_apply = false;
      if (replicator != nullptr)
      {
        replicator->ReplicateOnce(term, log_index, &should_apply);
      }

      if (should_apply)
      {
        ApplyResult result = ApplyCommittedEntries();
        if (!result.Ok)
        {
          Log(NodeTag(config_.node_id),
              "apply committed entries failed after replication, reason=",
              result.message);
        }
      }

      {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_.load())
        {
          return ReplicationOutcome::kLostLeadership;
        }
        if (role_ != Role::kLeader || current_term_ != term)
        {
          return ReplicationOutcome::kLostLeadership;
        }
        const std::size_t replicated_count =
            CountReplicatedCommittedVoters(config_, match_index_, log_index);
        if (replicated_count >= majority)
        {
          return ReplicationOutcome::kReplicated;
        }
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  return ReplicationOutcome::kTimeout;
}

  void RaftNode::AdvanceCommitIndexUnlocked()
  {
    if (role_ != Role::kLeader)
    {
      return;
    }

    if (log_.empty())
    {
      return;
    }

    const auto committed_voter_peers = BuildUniqueCommittedVoterPeers(config_);
    const std::size_t majority = ComputeCommittedVoterQuorumSize(config_);
    const std::uint64_t last_index = LastLogIndexLocked();

    for (std::uint64_t index = last_index; index > commit_index_; --index)
    {
      if (!HasLogAtIndexLocked(index))
      {
        continue;
      }

      if (TermAtIndexLocked(index) != current_term_)
      {
        continue;
      }

      std::size_t replicated_count = 1;
      for (const auto &peer : committed_voter_peers)
      {
        const auto it = match_index_.find(peer.node_id);
        if (it != match_index_.end() && it->second >= index)
        {
          ++replicated_count;
        }
      }

      if (replicated_count >= majority)
      {
        commit_index_ = index;
        std::string persist_error;
        if (!PersistStateLocked(&persist_error))
        {
          Log(NodeTag(config_.node_id), "persist advanced commit index failed: ", persist_error);
        }
        return;
      }
    }
  }

  ApplyResult RaftNode::ApplyCommittedEntries()
  {
    std::lock_guard<std::mutex> apply_lk(apply_mu_);

    while (true)
    {
      std::uint64_t apply_index = 0;
      std::uint64_t apply_term = 0;
      std::string command_data;
      IStateMachine *state_machine = nullptr;

      {
        std::lock_guard<std::mutex> lk(mu_);

        if (last_applied_ < last_snapshot_index_)
        {
          last_applied_ = last_snapshot_index_;
        }

        if (last_applied_ >= commit_index_)
        {
          return {true, "nothing to apply"};
        }

        apply_index = last_applied_ + 1;

        if (!HasLogAtIndexLocked(apply_index))
        {
          std::ostringstream oss;
          oss << "apply index out of range, apply_index=" << apply_index
              << ", commit_index=" << commit_index_
              << ", last_applied=" << last_applied_
              << ", last_snapshot_index=" << last_snapshot_index_
              << ", first_log_index=" << FirstLogIndexLocked()
              << ", last_log_index=" << LastLogIndexLocked();
          return {false, oss.str()};
        }

        const LogRecord *record = LogAtIndexLocked(apply_index);
        if (record == nullptr)
        {
          std::ostringstream oss;
          oss << "apply log record is missing, apply_index=" << apply_index
              << ", commit_index=" << commit_index_
              << ", last_applied=" << last_applied_
              << ", last_snapshot_index=" << last_snapshot_index_
              << ", first_log_index=" << FirstLogIndexLocked()
              << ", last_log_index=" << LastLogIndexLocked();
          return {false, oss.str()};
        }

        command_data = record->command;
        apply_term = record->term;
        state_machine = state_machine_.get();
      }

      ApplyResult result;
      if (IsAtomicBatchPromotionCommand(command_data))
      {
        result = ApplyAtomicBatchPromotionCommand(apply_index, command_data);
      }
      else
      {
        if (state_machine == nullptr)
        {
          return {false, "state machine is null"};
        }
        result = state_machine->Apply(apply_index, apply_term, command_data);
      }
      if (!result.Ok)
      {
        Log(NodeTag(config_.node_id),
            "state machine apply failed, index=", apply_index,
            ", reason=", result.message);
        return result;
      }

      {
        std::lock_guard<std::mutex> lk(mu_);
        if (last_applied_ < apply_index)
        {
          last_applied_ = apply_index;
          std::string persist_error;
          if (!PersistStateLocked(&persist_error))
          {
            Log(NodeTag(config_.node_id), "persist last applied after apply failed: ", persist_error);
            return {false, persist_error};
          }
        }
        MaybeScheduleSnapshotLocked(false);
      }
    }
  }

  void RaftNode::StartSnapshotWorker()
  {
    std::lock_guard<std::mutex> lk(snapshot_mu_);
    snapshot_worker_stop_ = false;
    if (snapshot_worker_.joinable())
    {
      return;
    }
    snapshot_worker_ = std::thread(&RaftNode::SnapshotWorkerLoop, this);
  }

  void RaftNode::StopSnapshotWorker()
  {
    {
      std::lock_guard<std::mutex> lk(snapshot_mu_);
      snapshot_worker_stop_ = true;
      snapshot_pending_ = false;
    }
    snapshot_cv_.notify_all();
    if (snapshot_worker_.joinable())
    {
      snapshot_worker_.join();
    }
  }

  void RaftNode::MaybeScheduleSnapshotLocked(bool force_by_timer)
  {
    if (!snapshot_config_.enabled || snapshot_storage_ == nullptr || state_machine_ == nullptr)
    {
      return;
    }
    if (last_applied_ <= last_snapshot_index_)
    {
      return;
    }
    if (!force_by_timer)
    {
      const std::uint64_t delta = last_applied_ - last_snapshot_index_;
      if (delta < snapshot_config_.log_threshold)
      {
        return;
      }
    }

    std::lock_guard<std::mutex> lk(snapshot_mu_);
    if (snapshot_pending_ || snapshot_in_progress_)
    {
      return;
    }

    pending_snapshot_index_ = last_applied_;
    pending_snapshot_term_ = TermAtIndexLocked(last_applied_);
    snapshot_pending_ = true;
    snapshot_cv_.notify_one();
  }

  bool RaftNode::LoadLatestSnapshotOnStartup(std::string *reason)
  {
    if (!snapshot_storage_ || !snapshot_config_.enabled || !snapshot_config_.load_on_startup)
    {
      return true;
    }

    SnapshotListResult snapshot_result;
    std::string list_error;
    if (!snapshot_storage_->ListSnapshotsWithDiagnostics(&snapshot_result, &list_error))
    {
      if (reason != nullptr)
      {
        *reason = list_error;
      }
      return false;
    }

    const auto &snapshots = snapshot_result.snapshots;
    Log(NodeTag(config_.node_id),
        "startup snapshot recovery scan, snapshot_dir=", snapshot_storage_->SnapshotDir(),
        ", valid_candidates=", snapshots.size(),
        ", skipped_catalog_entries=", snapshot_result.validation_issues.size());
    for (const auto &issue : snapshot_result.validation_issues)
    {
      Log(NodeTag(config_.node_id),
          "skip snapshot catalog entry during startup recovery, path=", issue.path,
          ", reason=", issue.reason);
    }

    for (const auto &meta : snapshots)
    {
      SnapshotResult load_result = state_machine_->LoadSnapshot(meta.snapshot_path);
      if (!load_result.Ok())
      {
        Log(NodeTag(config_.node_id), "skip invalid snapshot ", meta.snapshot_path,
            ", index=", meta.last_included_index,
            ", term=", meta.last_included_term,
            ", reason=", load_result.message);
        continue;
      }

      std::string boundary_error;
      const auto boundary = ResolveLoadedSnapshotAppliedBoundary(
          *state_machine_,
          meta.last_included_index,
          meta.last_included_term,
          &boundary_error);
      if (!boundary.has_value())
      {
        Log(NodeTag(config_.node_id), "skip invalid snapshot ", meta.snapshot_path,
            ", index=", meta.last_included_index,
            ", term=", meta.last_included_term,
            ", reason=", boundary_error);
        continue;
      }

      {
        std::lock_guard<std::mutex> lk(mu_);
        CompactLogPrefixLocked(boundary->index, boundary->term);
        commit_index_ = std::max<std::uint64_t>(commit_index_, boundary->index);
        last_applied_ = boundary->index;

        std::string persist_error;
        if (!PersistStateLocked(&persist_error))
        {
          if (reason != nullptr)
          {
            *reason = persist_error;
          }
          return false;
        }
      }

      Log(NodeTag(config_.node_id), "loaded snapshot from ", meta.snapshot_path,
          ", index=", boundary->index, ", term=", boundary->term,
          ", commit_index=", commit_index_, ", last_applied=", last_applied_,
          ", last_snapshot_index=", last_snapshot_index_,
          ", last_snapshot_term=", last_snapshot_term_,
          ", last_log_index=", LastLogIndexLocked());
      return true;
    }

    Log(NodeTag(config_.node_id),
        "no startup snapshot loaded, valid_candidates=", snapshots.size(),
        ", skipped_catalog_entries=", snapshot_result.validation_issues.size(),
        ", commit_index=", commit_index_,
        ", last_applied=", last_applied_,
        ", last_snapshot_index=", last_snapshot_index_,
        ", last_snapshot_term=", last_snapshot_term_,
        ", last_log_index=", LastLogIndexLocked());
    return true;
  }

  void RaftNode::SnapshotWorkerLoop()
  {
    while (true)
    {
      std::uint64_t snapshot_index = 0;
      std::uint64_t snapshot_term = 0;

      {
        std::unique_lock<std::mutex> lk(snapshot_mu_);
        snapshot_cv_.wait(lk, [this]
                          { return snapshot_worker_stop_ || snapshot_pending_; });
        if (snapshot_worker_stop_)
        {
          return;
        }

        snapshot_index = pending_snapshot_index_;
        snapshot_term = pending_snapshot_term_;
        snapshot_pending_ = false;
        snapshot_in_progress_ = true;
      }

      std::string snapshot_dir = snapshot_config_.snapshot_dir;
      const std::filesystem::path temp_path = std::filesystem::path(snapshot_dir) /
                                              ("snapshot_work_node_" + std::to_string(config_.node_id) + ".bin");

      SnapshotResult save_result;
      {
        std::lock_guard<std::mutex> apply_lk(apply_mu_);
        {
          std::lock_guard<std::mutex> lk(mu_);
          snapshot_index = last_applied_;
          snapshot_term = TermAtIndexLocked(snapshot_index);
        }
        save_result = state_machine_->SaveSnapshot(temp_path.string());
      }

      if (!save_result.Ok())
      {
        RecordSnapshotOutcome(false);
        Log(NodeTag(config_.node_id), "save state machine snapshot failed: ", save_result.message);
      }
      else
      {
        SnapshotMeta meta;
        std::string error;
        if (snapshot_storage_->SaveSnapshotFile(temp_path.string(), snapshot_index, snapshot_term,
                                                &meta, &error))
        {
          {
            std::lock_guard<std::mutex> lk(mu_);
            if (snapshot_index > last_snapshot_index_)
            {
              CompactLogPrefixLocked(snapshot_index, snapshot_term);
              std::string persist_error;
              if (!PersistStateLocked(&persist_error))
              {
                Log(NodeTag(config_.node_id), "persist compacted raft state failed: ", persist_error);
              }
            }
          }

          std::string prune_error;
          if (!snapshot_storage_->PruneSnapshots(snapshot_config_.max_snapshot_count, &prune_error) &&
              !prune_error.empty())
          {
            Log(NodeTag(config_.node_id), "prune snapshots failed: ", prune_error);
          }

          Log(NodeTag(config_.node_id), "snapshot saved: ", meta.snapshot_path,
              ", index=", snapshot_index, ", term=", snapshot_term);
          RecordSnapshotOutcome(true);
        }
        else
        {
          RecordSnapshotOutcome(false);
          Log(NodeTag(config_.node_id), "persist snapshot file failed: ", error);
        }
      }

      {
        std::lock_guard<std::mutex> lk(snapshot_mu_);
        snapshot_in_progress_ = false;
      }

      {
        std::lock_guard<std::mutex> lk(mu_);
        MaybeScheduleSnapshotLocked(false);
      }
    }
  }

  bool RaftNode::PersistStateLocked(std::string *reason)
  {
    if (!storage_)
    {
      if (reason != nullptr)
      {
        *reason = "storage is null";
      }
      return false;
    }

    PersistentRaftState state;
    state.current_term = current_term_;
    state.voted_for = voted_for_;
    state.commit_index = commit_index_;
    state.last_applied = last_applied_;
    state.log = log_;
    const bool ok = storage_->Save(state, reason);
    if (!ok)
    {
      if (reason != nullptr && reason->empty())
      {
        *reason = "persist raft state failed";
      }
      RecordStoragePersistFailure();
    }
    return ok;
  }

  bool RaftNode::ProposeNoOpEntry()
  {
    std::uint64_t log_index = 0;

    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!running_.load() || role_ != Role::kLeader)
      {
        return false;
      }
      log_index = AppendLocalLogUnlocked(kInternalNoOpCommand);
      if (log_index == 0)
      {
        return false;
      }
    }

    if (ReplicateLogEntryToMajority(log_index) != ReplicationOutcome::kReplicated)
    {
      return false;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      AdvanceCommitIndexUnlocked();
    }

    ApplyResult apply_result = ApplyCommittedEntries();
    if (!apply_result.Ok)
    {
      Log(NodeTag(config_.node_id), "leader no-op apply failed, reason=", apply_result.message);
      return false;
    }

    return true;
  }

  bool RaftNode::IsAtomicBatchPromotionCommand(
      const std::string &command_data) const
  {
    return command_data.rfind(kInternalAtomicBatchPromotionCommandPrefix, 0) == 0;
  }

  ApplyResult RaftNode::ApplyAtomicBatchPromotionCommand(
      const std::uint64_t apply_index,
      const std::string &command_data)
  {
    std::vector<AtomicBatchPromotionTarget> targets;
    std::string reason;
    if (!ParseAtomicBatchPromotionCommand(command_data, &targets, &reason))
    {
      return {false, "invalid atomic batch promotion command: " + reason};
    }

    std::lock_guard<std::mutex> lk(mu_);
    if (const auto validation_error =
            ValidateAtomicBatchPromotionTargetsLocked(targets);
        validation_error.has_value())
    {
      return {false, *validation_error};
    }
    std::map<std::int32_t, std::string> target_voters;
    for (const auto &target : targets)
    {
      target_voters[target.raft_id] = target.address;
    }

    std::unordered_set<std::int32_t> previous_peer_ids;
    for (const auto &peer : config_.peers)
    {
      previous_peer_ids.insert(peer.node_id);
    }

    std::vector<PeerConfig> new_peers;
    new_peers.reserve(target_voters.size() - 1U);
    for (const auto &[raft_id, address] : target_voters)
    {
      if (raft_id == config_.node_id)
      {
        continue;
      }
      new_peers.push_back(PeerConfig{raft_id, address});
    }

    config_.peers = std::move(new_peers);
    inflight_atomic_batch_promotion_log_index_.reset();
    pending_add_learner_proposals_.erase(
        std::remove_if(pending_add_learner_proposals_.begin(),
                       pending_add_learner_proposals_.end(),
                       [&target_voters](const PendingAddLearnerProposal &proposal) {
                         return target_voters.find(proposal.candidate_raft_id) !=
                                target_voters.end();
                       }),
        pending_add_learner_proposals_.end());
    local_runtime_membership_role_hint_ = RuntimeMembershipRole::kVoter;

    for (const auto &peer : config_.peers)
    {
      const bool was_new_peer =
          previous_peer_ids.find(peer.node_id) == previous_peer_ids.end();
      if (was_new_peer)
      {
        replicators_.erase(peer.node_id);
      }
      EnsurePeerClientLocked(peer);
      if (role_ == Role::kLeader)
      {
        GetOrCreateReplicatorLocked(peer);
      }
    }

    match_index_[config_.node_id] =
        std::max<std::uint64_t>(match_index_[config_.node_id], LastLogIndexLocked());
    next_index_[config_.node_id] = SafeAddOne(LastLogIndexLocked());

    return {true,
            "atomic batch learner promotion committed at index " +
                std::to_string(apply_index)};
  }

  std::string RaftNode::BuildAtomicBatchPromotionCommandLocked(
      const std::vector<AtomicBatchPromotionTarget> &targets) const
  {
    std::ostringstream oss;
    oss << kInternalAtomicBatchPromotionCommandPrefix;
    bool first = true;
    for (const auto &target : targets)
    {
      if (!first)
      {
        oss << ';';
      }
      first = false;
      oss << target.raft_id << ',' << HexEncode(target.address);
    }
    return oss.str();
  }

  bool RaftNode::ParseAtomicBatchPromotionCommand(
      const std::string &command_data,
      std::vector<AtomicBatchPromotionTarget> *targets,
      std::string *reason) const
  {
    if (targets == nullptr)
    {
      if (reason != nullptr)
      {
        *reason = "targets output is null";
      }
      return false;
    }
    targets->clear();

    if (!IsAtomicBatchPromotionCommand(command_data))
    {
      if (reason != nullptr)
      {
        *reason = "missing internal command prefix";
      }
      return false;
    }

    const std::string payload =
        command_data.substr(std::char_traits<char>::length(
            kInternalAtomicBatchPromotionCommandPrefix));
    std::size_t begin = 0U;
    while (begin < payload.size())
    {
      const std::size_t end = payload.find(';', begin);
      const std::string token =
          payload.substr(begin,
                         end == std::string::npos ? std::string::npos : end - begin);
      if (token.empty())
      {
        if (reason != nullptr)
        {
          *reason = "empty promotion target token";
        }
        return false;
      }

      const std::size_t comma = token.find(',');
      if (comma == std::string::npos)
      {
        if (reason != nullptr)
        {
          *reason = "promotion target is missing delimiter";
        }
        return false;
      }

      AtomicBatchPromotionTarget target;
      try
      {
        target.raft_id = std::stoi(token.substr(0, comma));
      }
      catch (const std::exception &)
      {
        if (reason != nullptr)
        {
          *reason = "promotion target raft_id is invalid";
        }
        return false;
      }
      if (!HexDecode(std::string_view(token).substr(comma + 1U), &target.address) ||
          target.address.empty())
      {
        if (reason != nullptr)
        {
          *reason = "promotion target address is invalid";
        }
        return false;
      }
      targets->push_back(std::move(target));

      if (end == std::string::npos)
      {
        break;
      }
      begin = end + 1U;
    }

    if (targets->size() < 3U)
    {
      if (reason != nullptr)
      {
        *reason = "promotion target voter count is too small";
      }
      return false;
    }
    return true;
  }

  std::size_t RaftNode::CommittedVoterCountLocked() const
  {
    return BuildUniqueCommittedVoterPeers(config_).size() + 1U;
  }

  std::optional<std::string> RaftNode::ValidateTargetCommittedVoterCountLocked(
      const std::size_t target_voter_count) const
  {
    if (target_voter_count < 3U)
    {
      return "target committed voter count must be at least 3 before membership commit";
    }
    if (target_voter_count % 2U == 0U)
    {
      return "target committed voter count " +
             std::to_string(target_voter_count) +
             " must stay odd before membership commit";
    }
    return std::nullopt;
  }

  std::optional<std::string> RaftNode::ValidateAtomicBatchPromotionTargetsLocked(
      const std::vector<AtomicBatchPromotionTarget> &targets) const
  {
    std::map<std::int32_t, std::string> target_voters;
    for (const auto &target : targets)
    {
      if (target.raft_id <= 0 || target.address.empty())
      {
        return "atomic batch promotion target is incomplete";
      }
      target_voters[target.raft_id] = target.address;
    }

    if (const auto validation_error =
            ValidateTargetCommittedVoterCountLocked(target_voters.size());
        validation_error.has_value())
    {
      return validation_error;
    }
    if (target_voters.find(config_.node_id) == target_voters.end())
    {
      return "atomic batch promotion would remove local node";
    }
    return std::nullopt;
  }

  bool RaftNode::IsPendingLearnerReadyForPromotionLocked(
      const PendingAddLearnerProposal &proposal) const
  {
    const auto match_it = match_index_.find(proposal.candidate_raft_id);
    const auto snapshot_it =
        peer_snapshot_progress_.find(proposal.candidate_raft_id);
    std::uint64_t highest_observed = 0;
    if (match_it != match_index_.end())
    {
      highest_observed = std::max(highest_observed, match_it->second);
    }
    if (snapshot_it != peer_snapshot_progress_.end())
    {
      highest_observed =
          std::max(highest_observed, snapshot_it->second.last_snapshot_index);
      highest_observed =
          std::max(highest_observed, snapshot_it->second.last_applied_index);
      highest_observed =
          std::max(highest_observed, snapshot_it->second.last_log_index);
    }
    return highest_observed > 0 && highest_observed >= commit_index_;
  }

  std::vector<RaftNode::AtomicBatchPromotionTarget>
  RaftNode::CollectAtomicBatchPromotionTargetsLocked() const
  {
    std::vector<AtomicBatchPromotionTarget> ready_promotions;
    for (const auto &pending : pending_add_learner_proposals_)
    {
      if (!IsPendingLearnerReadyForPromotionLocked(pending))
      {
        continue;
      }
      ready_promotions.push_back(
          AtomicBatchPromotionTarget{pending.candidate_raft_id,
                                     pending.candidate_raft_address});
    }

    if (ready_promotions.size() < 2U)
    {
      return {};
    }

    std::sort(ready_promotions.begin(),
              ready_promotions.end(),
              [](const AtomicBatchPromotionTarget &lhs,
                 const AtomicBatchPromotionTarget &rhs) {
                return lhs.raft_id < rhs.raft_id;
              });
    ready_promotions.resize(2U);

    std::map<std::int32_t, std::string> target_voters;
    target_voters[config_.node_id] = config_.address;
    for (const auto &peer : BuildUniqueCommittedVoterPeers(config_))
    {
      target_voters[peer.node_id] = peer.address;
    }
    for (const auto &promoted : ready_promotions)
    {
      target_voters[promoted.raft_id] = promoted.address;
    }

    if (target_voters.size() % 2U == 0U)
    {
      return {};
    }

    std::vector<AtomicBatchPromotionTarget> targets;
    targets.reserve(target_voters.size());
    for (const auto &[raft_id, address] : target_voters)
    {
      targets.push_back(AtomicBatchPromotionTarget{raft_id, address});
    }
    return targets;
  }

  std::optional<std::uint64_t> RaftNode::PrepareAtomicBatchPromotionLogIndexLocked(
      const std::vector<AtomicBatchPromotionTarget> &targets)
  {
    if (!running_.load() || role_ != Role::kLeader)
    {
      return std::nullopt;
    }

    if (inflight_atomic_batch_promotion_log_index_.has_value())
    {
      if (*inflight_atomic_batch_promotion_log_index_ <= commit_index_)
      {
        inflight_atomic_batch_promotion_log_index_.reset();
        return std::nullopt;
      }
      if (HasLogAtIndexLocked(*inflight_atomic_batch_promotion_log_index_))
      {
        return inflight_atomic_batch_promotion_log_index_;
      }
      inflight_atomic_batch_promotion_log_index_.reset();
    }

    if (targets.empty())
    {
      return std::nullopt;
    }
    if (ValidateAtomicBatchPromotionTargetsLocked(targets).has_value())
    {
      return std::nullopt;
    }

    const std::uint64_t log_index =
        AppendLocalLogUnlocked(BuildAtomicBatchPromotionCommandLocked(targets));
    if (log_index == 0)
    {
      return std::nullopt;
    }
    inflight_atomic_batch_promotion_log_index_ = log_index;
    return inflight_atomic_batch_promotion_log_index_;
  }

} // namespace raftdemo
