#include "raft/kv_service_impl.h"

#include <chrono>
#include <utility>

#include "raft/command.h"
#include "raft/raft_node.h"

namespace raftdemo
{
  namespace
  {

    raft::KvStatusCode ToKvStatusCode(ProposeStatus status)
    {
      switch (status)
      {
      case ProposeStatus::kOk:
        return raft::KV_STATUS_CODE_OK;
      case ProposeStatus::kNotLeader:
        return raft::KV_STATUS_CODE_NOT_LEADER;
      case ProposeStatus::kInvalidCommand:
        return raft::KV_STATUS_CODE_INVALID_ARGUMENT;
      case ProposeStatus::kNodeStopping:
        return raft::KV_STATUS_CODE_NODE_STOPPING;
      case ProposeStatus::kTimeout:
        return raft::KV_STATUS_CODE_TIMEOUT;
      case ProposeStatus::kReplicationFailed:
      case ProposeStatus::kCommitFailed:
      case ProposeStatus::kApplyFailed:
      default:
        return raft::KV_STATUS_CODE_INTERNAL_ERROR;
      }
    }

    void FillCommonWriteResponse(const NodeStatusSnapshot &status,
                                 const ProposeResult &result,
                                 raft::PutResponse *response)
    {
      response->set_code(ToKvStatusCode(result.status));
      response->set_message(result.message);
      response->set_leader_id(result.leader_id);
      response->set_leader_address(status.leader_address);
      response->set_term(result.term);
      response->set_log_index(result.log_index);
    }

    void FillCommonWriteResponse(const NodeStatusSnapshot &status,
                                 const ProposeResult &result,
                                 raft::DeleteResponse *response)
    {
      response->set_code(ToKvStatusCode(result.status));
      response->set_message(result.message);
      response->set_leader_id(result.leader_id);
      response->set_leader_address(status.leader_address);
      response->set_term(result.term);
      response->set_log_index(result.log_index);
    }

    void FillMetricsSnapshot(const NodeMetricsSnapshot &snapshot,
                             raft::MetricsSnapshot *out)
    {
      if (out == nullptr)
      {
        return;
      }

      out->set_propose_success_count(snapshot.propose_success_count);
      out->set_propose_failure_count(snapshot.propose_failure_count);
      out->set_election_count(snapshot.election_count);
      out->set_leader_change_count(snapshot.leader_change_count);
      out->set_snapshot_success_count(snapshot.snapshot_success_count);
      out->set_snapshot_failure_count(snapshot.snapshot_failure_count);
      out->set_storage_persist_failure_count(snapshot.storage_persist_failure_count);

      for (const auto &rpc_metric : snapshot.rpc_metrics)
      {
        auto *metric = out->add_rpc_metrics();
        metric->set_name(rpc_metric.name);
        metric->set_success_count(rpc_metric.success_count);
        metric->set_failure_count(rpc_metric.failure_count);
        metric->set_total_latency_us(rpc_metric.total_latency_us);
        metric->set_max_latency_us(rpc_metric.max_latency_us);
      }
    }

  } // namespace

  KvServiceImpl::KvServiceImpl(RaftNode &node) : node_(node) {}

  grpc::ServerUnaryReactor *KvServiceImpl::Put(grpc::CallbackServerContext *context,
                                               const raft::PutRequest *request,
                                               raft::PutResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto start = std::chrono::steady_clock::now();

    std::string reason;
    if (!node_.ValidateKey(request->key(), &reason) || !node_.ValidateValue(request->value(), &reason))
    {
      const auto status = node_.GetStatusSnapshot();
      response->set_code(raft::KV_STATUS_CODE_INVALID_ARGUMENT);
      response->set_message(reason);
      response->set_leader_id(status.leader_id);
      response->set_leader_address(status.leader_address);
      response->set_term(status.term);
      node_.RecordRpcLatency(RaftNode::RpcKind::kKvPut, false,
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start));
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    Command command;
    command.type = CommandType::kSet;
    command.key = request->key();
    command.value = request->value();

    const ProposeResult result = node_.Propose(command);
    const auto status = node_.GetStatusSnapshot();
    FillCommonWriteResponse(status, result, response);
    node_.RecordRpcLatency(RaftNode::RpcKind::kKvPut, result.Ok(),
                           std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - start));
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *KvServiceImpl::Delete(grpc::CallbackServerContext *context,
                                                  const raft::DeleteRequest *request,
                                                  raft::DeleteResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto start = std::chrono::steady_clock::now();

    std::string reason;
    if (!node_.ValidateKey(request->key(), &reason))
    {
      const auto status = node_.GetStatusSnapshot();
      response->set_code(raft::KV_STATUS_CODE_INVALID_ARGUMENT);
      response->set_message(reason);
      response->set_leader_id(status.leader_id);
      response->set_leader_address(status.leader_address);
      response->set_term(status.term);
      node_.RecordRpcLatency(RaftNode::RpcKind::kKvDelete, false,
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start));
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    Command command;
    command.type = CommandType::kDelete;
    command.key = request->key();

    const ProposeResult result = node_.Propose(command);
    const auto status = node_.GetStatusSnapshot();
    FillCommonWriteResponse(status, result, response);
    node_.RecordRpcLatency(RaftNode::RpcKind::kKvDelete, result.Ok(),
                           std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - start));
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *KvServiceImpl::Get(grpc::CallbackServerContext *context,
                                               const raft::GetRequest *request,
                                               raft::GetResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto start = std::chrono::steady_clock::now();

    std::string reason;
    if (!node_.ValidateKey(request->key(), &reason))
    {
      const auto status = node_.GetStatusSnapshot();
      response->set_code(raft::KV_STATUS_CODE_INVALID_ARGUMENT);
      response->set_message(reason);
      response->set_leader_id(status.leader_id);
      response->set_leader_address(status.leader_address);
      response->set_term(status.term);
      node_.RecordRpcLatency(RaftNode::RpcKind::kKvGet, false,
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start));
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    const auto status = node_.GetStatusSnapshot();
    response->set_leader_id(status.leader_id);
    response->set_leader_address(status.leader_address);
    response->set_term(status.term);

    if (status.role != "Leader")
    {
      response->set_code(raft::KV_STATUS_CODE_NOT_LEADER);
      response->set_message("node is not the leader");
      node_.RecordRpcLatency(RaftNode::RpcKind::kKvGet, false,
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start));
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    std::string value;
    if (!node_.DebugGetValue(request->key(), &value))
    {
      response->set_code(raft::KV_STATUS_CODE_KEY_NOT_FOUND);
      response->set_message("key not found");
      response->set_found(false);
      node_.RecordRpcLatency(RaftNode::RpcKind::kKvGet, false,
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start));
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    response->set_code(raft::KV_STATUS_CODE_OK);
    response->set_message("ok");
    response->set_found(true);
    response->set_value(value);
    node_.RecordRpcLatency(RaftNode::RpcKind::kKvGet, true,
                           std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - start));
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *KvServiceImpl::Status(grpc::CallbackServerContext *context,
                                                  const raft::StatusRequest *,
                                                  raft::StatusResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto start = std::chrono::steady_clock::now();

    const auto status = node_.GetStatusSnapshot();
    const auto metrics = node_.GetMetricsSnapshot();

    response->set_node_id(status.node_id);
    response->set_address(status.address);
    response->set_role(status.role);
    response->set_term(status.term);
    response->set_leader_id(status.leader_id);
    response->set_leader_address(status.leader_address);
    response->set_commit_index(status.commit_index);
    response->set_last_applied(status.last_applied);
    response->set_last_log_index(status.last_log_index);
    response->set_snapshot_index(status.snapshot_index);

    for (const auto &peer : status.peers)
    {
      auto *progress = response->add_peers();
      progress->set_peer_id(peer.peer_id);
      progress->set_address(peer.address);
      progress->set_match_index(peer.match_index);
      progress->set_next_index(peer.next_index);
    }

    FillMetricsSnapshot(metrics, response->mutable_metrics());
    node_.RecordRpcLatency(RaftNode::RpcKind::kKvStatus, true,
                           std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - start));
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *KvServiceImpl::Health(grpc::CallbackServerContext *context,
                                                  const raft::HealthRequest *,
                                                  raft::HealthResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto start = std::chrono::steady_clock::now();
    const auto status = node_.GetStatusSnapshot();

    response->set_ok(node_.IsRunning());
    response->set_message(node_.IsRunning() ? "ok" : "stopping");
    response->set_node_id(status.node_id);
    response->set_role(status.role);
    response->set_term(status.term);
    response->set_leader_id(status.leader_id);

    node_.RecordRpcLatency(RaftNode::RpcKind::kKvHealth, response->ok(),
                           std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - start));
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *KvServiceImpl::Metrics(grpc::CallbackServerContext *context,
                                                   const raft::MetricsRequest *,
                                                   raft::MetricsResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto start = std::chrono::steady_clock::now();
    const auto status = node_.GetStatusSnapshot();
    const auto metrics = node_.GetMetricsSnapshot();

    response->set_node_id(status.node_id);
    response->set_role(status.role);
    response->set_term(status.term);
    response->set_leader_id(status.leader_id);
    response->set_leader_address(status.leader_address);
    FillMetricsSnapshot(metrics, response->mutable_metrics());

    node_.RecordRpcLatency(RaftNode::RpcKind::kKvMetrics, true,
                           std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - start));
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

} // namespace raftdemo
