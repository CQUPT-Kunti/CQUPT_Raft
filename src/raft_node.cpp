#include "raft/raft_node.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "raft/logging.h"
#include "raft/raft_service_impl.h"
#include "raft/state_machine.h"
#include "raft/snapshot_storage.h"

namespace raftdemo
{
  namespace
  {

    constexpr const char *kInternalNoOpCommand = "__raft_internal_noop__";

    std::string NodeTag(int node_id) { return "node-" + std::to_string(node_id); }

    std::string DefaultDataDir(int node_id)
    {
      return "./raft_data/node_" + std::to_string(node_id);
    }

    std::string DefaultSnapshotDir(int node_id)
    {
      return "./raft_snapshots/node_" + std::to_string(node_id);
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
        state_machine_(std::move(state_machine))
  {
    if (config_.data_dir.empty())
    {
      config_.data_dir = DefaultDataDir(config_.node_id);
    }

    if (snapshot_config_.snapshot_dir.empty())
    {
      snapshot_config_.snapshot_dir = DefaultSnapshotDir(config_.node_id);
    }

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
      log_ = std::move(persistent_state.log);
      if (log_.empty())
      {
        log_.push_back(LogRecord{0, 0, "bootstrap"});
      }

      Log(NodeTag(config_.node_id), "loaded persisted state from ", storage_->DataDir(),
          ", term=", current_term_, ", voted_for=", voted_for_,
          ", last_log_index=", LastLogIndexLocked());
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
  }

  RaftNode::~RaftNode() { Stop(); }

  void RaftNode::Start()
  {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true))
    {
      return;
    }

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
      if (election_timer_id_)
      {
        scheduler_.Cancel(*election_timer_id_);
        election_timer_id_.reset();
      }
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

    Log(NodeTag(config_.node_id), "stopped");
  }

  void RaftNode::Wait()
  {
    if (server_)
    {
      server_->Wait();
    }
  }

  void RaftNode::InitServer()
  {
    service_ = std::make_unique<RaftServiceImpl>(*this);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(config_.address, grpc::InsecureServerCredentials());
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    if (!server_)
    {
      running_.store(false);
      throw std::runtime_error("failed to start gRPC server at " + config_.address);
    }
  }

  void RaftNode::InitClients()
  {
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

  std::string RaftNode::Describe() const
  {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream oss;
    oss << "node=" << config_.node_id
        << ", role=" << RoleName(role_)
        << ", term=" << current_term_
        << ", voted_for=" << voted_for_
        << ", leader=" << leader_id_
        << ", last_log_index=" << LastLogIndexLocked()
        << ", commit_index=" << commit_index_
        << ", last_applied=" << last_applied_
        << ", last_snapshot_index=" << last_snapshot_index_;

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

  void RaftNode::ResetElectionTimerLocked()
  {
    if (election_timer_id_)
    {
      scheduler_.Cancel(*election_timer_id_);
      election_timer_id_.reset();
    }

    const auto timeout = RandomElectionTimeoutLocked();
    auto weak = weak_from_this();
    election_timer_id_ = scheduler_.ScheduleAfter(timeout, [weak]
                                                  {
    if (auto self = weak.lock()) {
      self->OnElectionTimeout();
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

  void RaftNode::OnElectionTimeout()
  {
    if (!running_.load())
    {
      return;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      if (role_ == Role::kLeader)
      {
        return;
      }
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
    std::uint64_t leader_commit = 0;

    {
      std::lock_guard<std::mutex> lk(mu_);
      if (!running_.load() || role_ != Role::kLeader)
      {
        return;
      }
      peers = config_.peers;
      term = current_term_;
      leader_commit = commit_index_;
    }

    auto weak = weak_from_this();
    for (const auto &peer : peers)
    {
      rpc_pool_.Submit([weak, peer, term, leader_commit]
                       {
      auto self = weak.lock();
      if (!self || !self->running_.load()) {
        return;
      }

      raft::AppendEntriesRequest request;
      request.set_term(term);
      request.set_leader_id(self->config_.node_id);
      request.set_leader_commit(leader_commit);

      {
        std::lock_guard<std::mutex> lk(self->mu_);
        if (self->role_ != Role::kLeader || self->current_term_ != term) {
          return;
        }

        const auto next_index_it = self->next_index_.find(peer.node_id);
        const std::uint64_t next_index =
            next_index_it == self->next_index_.end() ? self->LastLogIndexLocked() + 1
                                                     : next_index_it->second;
        const std::uint64_t prev_log_index = next_index == 0 ? 0 : next_index - 1;
        const std::uint64_t prev_log_term =
            prev_log_index < self->log_.size() ? self->log_[prev_log_index].term : 0;

        request.set_prev_log_index(prev_log_index);
        request.set_prev_log_term(prev_log_term);

        for (std::uint64_t i = next_index; i < self->log_.size(); ++i) {
          auto* entry = request.add_entries();
          entry->set_index(self->log_[i].index);
          entry->set_term(self->log_[i].term);
          entry->set_command(self->log_[i].command);
        }
      }

      auto response = self->AppendEntriesRpc(peer.node_id, request);
      if (!response.has_value()) {
        return;
      }

      bool should_apply = false;

      {
        std::lock_guard<std::mutex> lk(self->mu_);
        if (!self->running_.load()) {
          return;
        }
        if (response->term() > self->current_term_) {
          self->BecomeFollowerLocked(response->term(), -1,
                                     "peer replied higher term in AppendEntries");
          return;
        }
        if (self->role_ != Role::kLeader || self->current_term_ != term) {
          return;
        }

        if (response->success()) {
          const std::uint64_t match_index =
              std::min<std::uint64_t>(response->match_index(), self->LastLogIndexLocked());
          auto &peer_match_index = self->match_index_[peer.node_id];
          if (match_index > peer_match_index) {
            peer_match_index = match_index;
          }
          self->next_index_[peer.node_id] = peer_match_index + 1;

          const std::uint64_t old_commit_index = self->commit_index_;
          self->AdvanceCommitIndexUnlocked();
          should_apply = self->commit_index_ > old_commit_index;
        } else {
          auto& next_index = self->next_index_[peer.node_id];
          if (next_index > 1) {
            --next_index;
          }
        }
      }

      if (should_apply) {
        ApplyResult result = self->ApplyCommittedEntries();
        if (!result.Ok) {
          Log(NodeTag(self->config_.node_id),
              "apply committed entries failed after heartbeat, reason=",
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
    bool hard_state_changed = false;

    if (new_term > current_term_)
    {
      current_term_ = new_term;
      voted_for_ = -1;
      hard_state_changed = true;
    }

    role_ = Role::kFollower;
    leader_id_ = new_leader;

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
    role_ = Role::kLeader;
    leader_id_ = config_.node_id;

    if (election_timer_id_)
    {
      scheduler_.Cancel(*election_timer_id_);
      election_timer_id_.reset();
    }

    const auto last_log_index = LastLogIndexLocked();
    next_index_.clear();
    match_index_.clear();
    match_index_[config_.node_id] = last_log_index;
    next_index_[config_.node_id] = last_log_index + 1;
    for (const auto &peer : config_.peers)
    {
      next_index_[peer.node_id] = last_log_index + 1;
      match_index_[peer.node_id] = 0;
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

  std::uint64_t RaftNode::LastLogIndexLocked() const { return log_.empty() ? 0 : log_.back().index; }

  std::uint64_t RaftNode::LastLogTermLocked() const { return log_.empty() ? 0 : log_.back().term; }

  std::optional<raft::VoteResponse> RaftNode::RequestVoteRpc(int peer_id,
                                                             const raft::VoteRequest &request)
  {
    auto it = clients_.find(peer_id);
    if (it == clients_.end())
    {
      return std::nullopt;
    }

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
      return std::nullopt;
    }
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
      return std::nullopt;
    }
    return response;
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
    response->set_match_index(0);

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
        return;
      }
    }
    else
    {
      leader_id_ = request.leader_id();
      ResetElectionTimerLocked();
    }

    if (request.prev_log_index() >= log_.size())
    {
      response->set_term(current_term_);
      return;
    }

    if (log_[request.prev_log_index()].term != request.prev_log_term())
    {
      response->set_term(current_term_);
      return;
    }

    bool log_changed = false;
    std::uint64_t append_at = request.prev_log_index() + 1;
    int req_idx = 0;
    while (req_idx < request.entries_size() && append_at < log_.size())
    {
      const auto &req_entry = request.entries(req_idx);
      if (req_entry.index() != append_at)
      {
        response->set_term(current_term_);
        response->set_success(false);
        return;
      }
      if (log_[append_at].term != req_entry.term())
      {
        log_.resize(append_at);
        log_changed = true;
        break;
      }
      ++append_at;
      ++req_idx;
    }

    for (; req_idx < request.entries_size(); ++req_idx)
    {
      const auto &req_entry = request.entries(req_idx);
      if (req_entry.index() != append_at)
      {
        response->set_term(current_term_);
        response->set_success(false);
        return;
      }
      log_.push_back(LogRecord{req_entry.index(), req_entry.term(), req_entry.command()});
      ++append_at;
      log_changed = true;
    }

    if (log_changed)
    {
      std::string persist_error;
      if (!PersistStateLocked(&persist_error))
      {
        Log(NodeTag(config_.node_id), "append entries persist failed: ", persist_error);
        response->set_term(current_term_);
        response->set_success(false);
        response->set_match_index(LastLogIndexLocked());
        return;
      }
    }

    if (request.leader_commit() > commit_index_)
    {
      const std::uint64_t new_commit =
          std::min<std::uint64_t>(request.leader_commit(), LastLogIndexLocked());

      if (new_commit > commit_index_)
      {
        commit_index_ = new_commit;
        should_apply = true;
      }
    }

    response->set_term(current_term_);
    response->set_success(true);
    response->set_match_index(request.prev_log_index() +
                              static_cast<std::uint64_t>(request.entries_size()));

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
        return result;
      }

      if (role_ != Role::kLeader)
      {
        result.status = ProposeStatus::kNotLeader;
        result.leader_id = leader_id_;
        result.term = current_term_;
        result.message = "node is not the leader";
        return result;
      }

      if (!ValidateCommandUnlocked(command, &reason))
      {
        result.status = ProposeStatus::kInvalidCommand;
        result.leader_id = config_.node_id;
        result.term = current_term_;
        result.message = reason;
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
        return result;
      }

      result.leader_id = config_.node_id;
      result.term = term;
      result.log_index = log_index;
      result.message = "log appended locally";
    }

    // 锁外复制，避免长时间阻塞 Raft 核心状态
    const bool replicated = ReplicateLogEntryToMajority(log_index);
    if (!replicated)
    {
      result.status = ProposeStatus::kTimeout;
      result.message = "log entry was not committed before timeout";
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
      return result;
    }

    {
      std::lock_guard<std::mutex> lk(mu_);
      if (last_applied_ < log_index)
      {
        result.status = ProposeStatus::kApplyFailed;
        result.message = "log committed but not applied";
        return result;
      }
    }

    result.status = ProposeStatus::kOk;
    result.message = "command committed and applied";
    return result;
  }

  bool RaftNode::ValidateCommandUnlocked(const Command &command, std::string *reason) const
  {
    if (!command.IsValid())
    {
      *reason = "Invalid command";
      return false;
    }
    // 2. 限制命令大小，避免异常大 payload 进入日志
    constexpr std::size_t kMaxCommandBytes = 1024 * 1024;
    const std::string serialized = command.Serialize();
    if (serialized.empty())
    {
      if (reason != nullptr)
      {
        *reason = "command serialization result is empty";
      }
      return false;
    }
    if (serialized.size() > kMaxCommandBytes)
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
    const std::uint64_t new_index = static_cast<std::uint64_t>(log_.size());
    log_.push_back(LogRecord{
        new_index,
        current_term_,
        command_data,
    });

    match_index_[config_.node_id] = new_index;
    next_index_[config_.node_id] = new_index + 1;

    std::string persist_error;
    if (!PersistStateLocked(&persist_error))
    {
      log_.pop_back();
      match_index_[config_.node_id] = LastLogIndexLocked();
      next_index_[config_.node_id] = LastLogIndexLocked() + 1;
      Log(NodeTag(config_.node_id), "append local log persist failed: ", persist_error);
      return 0;
    }

    return new_index;
  }

  bool RaftNode::ReplicateLogEntryToMajority(std::uint64_t log_index)
  {
    const auto timeout = std::max(config_.rpc_deadline * 8, config_.heartbeat_interval * 4);
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (running_.load())
    {
      std::vector<PeerConfig> peers;
      std::uint64_t term = 0;
      std::uint64_t leader_commit = 0;
      std::size_t majority = 0;

      {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_.load() || role_ != Role::kLeader)
        {
          return false;
        }
        if (log_index >= log_.size())
        {
          return false;
        }

        const std::size_t total_nodes = config_.peers.size() + 1;
        majority = total_nodes / 2 + 1;
        std::size_t replicated_count = 1;
        for (const auto &peer : config_.peers)
        {
          const auto it = match_index_.find(peer.node_id);
          if (it != match_index_.end() && it->second >= log_index)
          {
            ++replicated_count;
          }
        }
        if (replicated_count >= majority)
        {
          AdvanceCommitIndexUnlocked();
          if (commit_index_ >= log_index)
          {
            return true;
          }
        }

        peers = config_.peers;
        term = current_term_;
        leader_commit = commit_index_;
      }

      bool should_apply = false;
      for (const auto &peer : peers)
      {
        raft::AppendEntriesRequest request;
        {
          std::lock_guard<std::mutex> lk(mu_);
          if (!running_.load() || role_ != Role::kLeader || current_term_ != term)
          {
            return false;
          }
          if (commit_index_ >= log_index)
          {
            return true;
          }

          auto &next_index_ref = next_index_[peer.node_id];
          if (next_index_ref == 0 || next_index_ref > LastLogIndexLocked() + 1)
          {
            next_index_ref = LastLogIndexLocked() + 1;
          }

          const std::uint64_t next_index = next_index_ref;
          const std::uint64_t prev_log_index = next_index == 0 ? 0 : next_index - 1;
          const std::uint64_t prev_log_term = TermAtIndexLocked(prev_log_index);

          request.set_term(term);
          request.set_leader_id(config_.node_id);
          request.set_prev_log_index(prev_log_index);
          request.set_prev_log_term(prev_log_term);
          request.set_leader_commit(leader_commit);

          for (std::uint64_t i = next_index; i < log_.size(); ++i)
          {
            auto *entry = request.add_entries();
            entry->set_index(log_[i].index);
            entry->set_term(log_[i].term);
            entry->set_command(log_[i].command);
          }
        }

        auto response = AppendEntriesRpc(peer.node_id, request);
        if (!response.has_value())
        {
          continue;
        }

        {
          std::lock_guard<std::mutex> lk(mu_);

          if (!running_.load())
          {
            return false;
          }

          if (response->term() > current_term_)
          {
            BecomeFollowerLocked(response->term(), -1,
                                 "peer replied higher term while replicating log entry");
            return false;
          }

          if (role_ != Role::kLeader || current_term_ != term)
          {
            return false;
          }

          if (response->success())
          {
            const std::uint64_t match_index =
                std::min<std::uint64_t>(response->match_index(), LastLogIndexLocked());
            auto &peer_match_index = match_index_[peer.node_id];
            if (match_index > peer_match_index)
            {
              peer_match_index = match_index;
            }
            next_index_[peer.node_id] = peer_match_index + 1;

            const std::uint64_t old_commit_index = commit_index_;
            AdvanceCommitIndexUnlocked();
            should_apply = should_apply || commit_index_ > old_commit_index;

            if (commit_index_ >= log_index)
            {
              break;
            }
          }
          else
          {
            auto &next_index_ref = next_index_[peer.node_id];
            if (next_index_ref > 1)
            {
              --next_index_ref;
            }
          }
        }
      }

      if (should_apply)
      {
        ApplyResult result = ApplyCommittedEntries();
        if (!result.Ok)
        {
          Log(NodeTag(config_.node_id),
              "apply committed entries failed while replicating, reason=",
              result.message);
          return false;
        }
      }

      {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_.load() || role_ != Role::kLeader || current_term_ != term)
        {
          return false;
        }
        if (commit_index_ >= log_index)
        {
          return true;
        }
      }

      if (std::chrono::steady_clock::now() >= deadline)
      {
        return false;
      }

      const auto sleep_time = std::min(config_.heartbeat_interval,
                                       std::chrono::milliseconds(20));
      std::this_thread::sleep_for(sleep_time);
    }

    return false;
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

    // 从大到小找，找到的第一个满足条件的下标，
    // 就是当前能够推进到的最大 commit index。
    for (std::uint64_t index = last_index; index > commit_index_; --index)
    {
      if (index >= log_.size())
      {
        continue;
      }

      // Raft 规则：Leader 只能依据当前任期的日志推进 commit_index。
      if (log_[index].term != current_term_)
      {
        continue;
      }

      std::size_t replicated_count = 1; // Leader 自己算一个

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

        // 没有新的已提交日志需要 apply
        if (last_applied_ >= commit_index_)
        {
          return {true, "nothing to apply"};
        }

        apply_index = last_applied_ + 1;

        if (apply_index >= log_.size())
        {
          return {false, "apply index out of range"};
        }

        command_data = log_[apply_index].command;
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
        }
        MaybeScheduleSnapshotLocked(false);
      }
    }
  }

  std::uint64_t RaftNode::TermAtIndexLocked(std::uint64_t index) const
  {
    if (index < log_.size())
    {
      return log_[index].term;
    }
    if (index == last_snapshot_index_)
    {
      return last_snapshot_term_;
    }
    return 0;
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

      if (meta.last_included_index > LastLogIndexLocked())
      {
        Log(NodeTag(config_.node_id), "skip snapshot beyond raft log, snapshot_index=",
            meta.last_included_index, ", last_log_index=", LastLogIndexLocked());
        continue;
      }

      last_snapshot_index_ = meta.last_included_index;
      last_snapshot_term_ = meta.last_included_term;
      last_applied_ = meta.last_included_index;
      commit_index_ = meta.last_included_index;

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
              last_snapshot_index_ = snapshot_index;
              last_snapshot_term_ = snapshot_term;
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
        }
        else
        {
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
    state.log = log_;
    return storage_->Save(state, reason);
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

    if (!ReplicateLogEntryToMajority(log_index))
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
