#include "raft/raft_node.h"

#include <algorithm>
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
#include <utility>
#include <vector>

#include "raft/logging.h"
#include "raft/kv_service_impl.h"
#include "raft/raft_service_impl.h"
#include "raft/replicator.h"
#include "raft/state_machine.h"
#include "raft/snapshot_storage.h"

namespace raftdemo
{
  namespace
  {

    constexpr const char *kInternalNoOpCommand = "__raft_internal_noop__";
    constexpr const char *kSnapshotMarkerCommand = "snapshot";
    constexpr const char *kIdentityFileName = "node_identity.txt";

    std::uint64_t SafeAddOne(std::uint64_t value)
    {
      return value == std::numeric_limits<std::uint64_t>::max() ? value : value + 1;
    }

    std::string NodeTag(int node_id) { return "node-" + std::to_string(node_id); }

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

  } // namespace

  RaftNode::RaftNode(NodeConfig config)
      : RaftNode(std::move(config), snapshotConfig{}, std::make_unique<KvStateMachine>())
  {
  }

  RaftNode::RaftNode(NodeConfig config, snapshotConfig snapshot_config)
      : RaftNode(std::move(config), std::move(snapshot_config), std::make_unique<KvStateMachine>())
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
      // empty KV state after a pure-log restart.
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

      if (commit_index_ > LastLogIndexLocked())
      {
        commit_index_ = LastLogIndexLocked();
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
                                 std::to_string(config_.node_id) + ": " + replay_result.message);
      }
    }
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
        ", cluster_size=", config_.peers.size() + 1,
        ", quorum=", ((config_.peers.size() + 1) / 2 + 1),
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
      return;
    }

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

  void RaftNode::InitServer()
  {
    service_ = std::make_unique<RaftServiceImpl>(*this);
    kv_service_ = std::make_unique<KvServiceImpl>(*this);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(config_.address, grpc::InsecureServerCredentials());
    builder.RegisterService(service_.get());
    builder.RegisterService(kv_service_.get());
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
      auto client = std::make_unique<PeerClient>();
      client->peer_id = peer.node_id;
      client->address = peer.address;
      client->channel = grpc::CreateChannel(peer.address, grpc::InsecureChannelCredentials());
      client->stub = raft::RaftService::NewStub(client->channel);
      clients_[peer.node_id] = std::move(client);
    }
  }


Replicator *RaftNode::GetOrCreateReplicatorLocked(const PeerConfig &peer)
{
  auto it = replicators_.find(peer.node_id);
  if (it != replicators_.end())
  {
    return it->second.get();
  }

  auto replicator = std::make_unique<Replicator>(*this, peer);
  Replicator *raw = replicator.get();
  replicators_.emplace(peer.node_id, std::move(replicator));
  return raw;
}

  bool RaftNode::IsRunning() const
  {
    return running_.load();
  }

  bool RaftNode::ValidateKey(const std::string &key, std::string *reason) const
  {
    std::lock_guard<std::mutex> lk(mu_);
    return ValidateKeyUnlocked(key, reason);
  }

  bool RaftNode::ValidateValue(const std::string &value, std::string *reason) const
  {
    std::lock_guard<std::mutex> lk(mu_);
    return ValidateValueUnlocked(value, reason);
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

    const auto *kv_sm = dynamic_cast<const KvStateMachine *>(state_machine_.get());
    if (kv_sm != nullptr)
    {
      oss << ", kv=" << kv_sm->DebugString();
    }

    return oss.str();
  }

  bool RaftNode::DebugGetValue(const std::string &key, std::string *value) const
  {
    const auto *kv_sm = dynamic_cast<const KvStateMachine *>(state_machine_.get());
    if (kv_sm == nullptr)
    {
      return false;
    }
    return kv_sm->Get(key, value);
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
    RecordElectionStarted();

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
      peers = config_.peers;
      quorum = static_cast<int>((peers.size() + 1) / 2) + 1;

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
    peers = config_.peers;
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
    next_index_.clear();
    match_index_.clear();
    match_index_[config_.node_id] = last_log_index;
    next_index_[config_.node_id] = SafeAddOne(last_log_index);
    for (const auto &peer : config_.peers)
    {
      next_index_[peer.node_id] = SafeAddOne(last_log_index);
      match_index_[peer.node_id] = 0;
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

  bool RaftNode::SendInstallSnapshotToPeer(int peer_id, std::uint64_t term)
  {
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

      std::lock_guard<std::mutex> lk(mu_);
      if (request.term() < current_term_)
      {
        response->set_term(current_term_);
        response->set_message("term changed while installing snapshot");
        return;
      }

      CompactLogPrefixLocked(request.last_included_index(), request.last_included_term());
      commit_index_ = std::max<std::uint64_t>(commit_index_, request.last_included_index());
      last_applied_ = std::max<std::uint64_t>(last_applied_, request.last_included_index());

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
    case RpcKind::kKvPut:
      return "kv_put";
    case RpcKind::kKvDelete:
      return "kv_delete";
    case RpcKind::kKvGet:
      return "kv_get";
    case RpcKind::kKvStatus:
      return "kv_status";
    case RpcKind::kKvHealth:
      return "kv_health";
    case RpcKind::kKvMetrics:
      return "kv_metrics";
    }
    return "unknown";
  }

  std::vector<RaftNode::RpcMetricState> RaftNode::BuildRpcMetricStateTemplate()
  {
    std::vector<RpcMetricState> metrics;
    metrics.reserve(9);
    for (const RpcKind kind : {
             RpcKind::kRequestVote,
             RpcKind::kAppendEntries,
             RpcKind::kInstallSnapshot,
             RpcKind::kKvPut,
             RpcKind::kKvDelete,
             RpcKind::kKvGet,
             RpcKind::kKvStatus,
             RpcKind::kKvHealth,
             RpcKind::kKvMetrics,
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
    return "";
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

    if (!ValidateKeyUnlocked(command.key, reason))
    {
      return false;
    }

    if (command.type == CommandType::kSet && !ValidateValueUnlocked(command.value, reason))
    {
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
    if (serialized.size() > config_.kv_limits.max_command_bytes)
    {
      if (reason != nullptr)
      {
        *reason = "command size exceeds limit";
      }
      return false;
    }
    return true;
  }

  bool RaftNode::ValidateKeyUnlocked(const std::string &key, std::string *reason) const
  {
    if (key.empty())
    {
      if (reason != nullptr)
      {
        *reason = "key must not be empty";
      }
      return false;
    }
    if (key.find('|') != std::string::npos)
    {
      if (reason != nullptr)
      {
        *reason = "key must not contain '|'";
      }
      return false;
    }
    if (key.size() > config_.kv_limits.max_key_bytes)
    {
      if (reason != nullptr)
      {
        *reason = "key size exceeds limit";
      }
      return false;
    }
    return true;
  }

  bool RaftNode::ValidateValueUnlocked(const std::string &value, std::string *reason) const
  {
    if (value.find('|') != std::string::npos)
    {
      if (reason != nullptr)
      {
        *reason = "value must not contain '|'";
      }
      return false;
    }
    if (value.size() > config_.kv_limits.max_value_bytes)
    {
      if (reason != nullptr)
      {
        *reason = "value size exceeds limit";
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

RaftNode::ReplicationOutcome RaftNode::ReplicateLogEntryToMajority(std::uint64_t log_index)
{
  const auto deadline = std::chrono::steady_clock::now() + config_.rpc_deadline * 20;

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
      peers = config_.peers;
      term = current_term_;

      for (const auto &peer : peers)
      {
        GetOrCreateReplicatorLocked(peer);
      }
    }

    const std::size_t total_nodes = peers.size() + 1;
    const std::size_t majority = total_nodes / 2 + 1;
    if (majority <= 1)
    {
      return ReplicationOutcome::kReplicated;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      std::size_t replicated_count = 1;
      for (const auto &peer : peers)
      {
        const auto it = match_index_.find(peer.node_id);
        if (it != match_index_.end() && it->second >= log_index)
        {
          ++replicated_count;
        }
      }
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
        std::size_t replicated_count = 1;
        for (const auto &candidate : peers)
        {
          const auto it = match_index_.find(candidate.node_id);
          if (it != match_index_.end() && it->second >= log_index)
          {
            ++replicated_count;
          }
        }
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

    const std::size_t total_nodes = config_.peers.size() + 1;
    const std::size_t majority = total_nodes / 2 + 1;
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
      for (const auto &peer : config_.peers)
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
          return {false, "apply index out of range"};
        }

        const LogRecord *record = LogAtIndexLocked(apply_index);
        if (record == nullptr)
        {
          return {false, "apply log record is missing"};
        }

        command_data = record->command;
        state_machine = state_machine_.get();
      }

      if (state_machine == nullptr)
      {
        return {false, "state machine is null"};
      }

      ApplyResult result = state_machine->Apply(apply_index, command_data);
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

    std::vector<SnapshotMeta> snapshots;
    std::string list_error;
    if (!snapshot_storage_->ListSnapshots(&snapshots, &list_error))
    {
      if (reason != nullptr)
      {
        *reason = list_error;
      }
      return false;
    }

    for (const auto &meta : snapshots)
    {
      SnapshotResult load_result = state_machine_->LoadSnapshot(meta.snapshot_path);
      if (!load_result.Ok())
      {
        Log(NodeTag(config_.node_id), "skip invalid snapshot ", meta.snapshot_path,
            ", reason=", load_result.message);
        continue;
      }

      {
        std::lock_guard<std::mutex> lk(mu_);
        CompactLogPrefixLocked(meta.last_included_index, meta.last_included_term);
        commit_index_ = std::max<std::uint64_t>(commit_index_, meta.last_included_index);
        last_applied_ = meta.last_included_index;

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
          ", index=", meta.last_included_index, ", term=", meta.last_included_term);
      return true;
    }

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

} // namespace raftdemo
