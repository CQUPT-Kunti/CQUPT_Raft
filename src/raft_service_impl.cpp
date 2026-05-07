#include "raft/raft_service_impl.h"

#include <chrono>

#include "raft/raft_node.h"

namespace raftdemo {

RaftServiceImpl::RaftServiceImpl(RaftNode& node) : node_(node) {}

grpc::ServerUnaryReactor* RaftServiceImpl::RequestVote(grpc::CallbackServerContext* context,
                                                       const raft::VoteRequest* request,
                                                       raft::VoteResponse* response) {
  auto* reactor = context->DefaultReactor();
  const auto start = std::chrono::steady_clock::now();
  node_.OnRequestVote(*request, response);
  node_.RecordRpcLatency(RaftNode::RpcKind::kRequestVote, true,
                         std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start));
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor* RaftServiceImpl::AppendEntries(
    grpc::CallbackServerContext* context, const raft::AppendEntriesRequest* request,
    raft::AppendEntriesResponse* response) {
  auto* reactor = context->DefaultReactor();
  const auto start = std::chrono::steady_clock::now();
  node_.OnAppendEntries(*request, response);
  node_.RecordRpcLatency(RaftNode::RpcKind::kAppendEntries, true,
                         std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start));
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

grpc::ServerUnaryReactor* RaftServiceImpl::InstallSnapshot(
    grpc::CallbackServerContext* context, const raft::InstallSnapshotRequest* request,
    raft::InstallSnapshotResponse* response) {
  auto* reactor = context->DefaultReactor();
  const auto start = std::chrono::steady_clock::now();
  node_.OnInstallSnapshot(*request, response);
  node_.RecordRpcLatency(RaftNode::RpcKind::kInstallSnapshot, true,
                         std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - start));
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

}  // namespace raftdemo
