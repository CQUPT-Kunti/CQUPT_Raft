#include "raft/raft_service_impl.h"

#include "raft/raft_node.h"

namespace raftdemo
{

  RaftServiceImpl::RaftServiceImpl(RaftNode &node) : node_(node) {}

  grpc::ServerUnaryReactor *RaftServiceImpl::RequestVote(grpc::CallbackServerContext *context,
                                                         const raft::VoteRequest *request,
                                                         raft::VoteResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    node_.OnRequestVote(*request, response);
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *RaftServiceImpl::AppendEntries(
      grpc::CallbackServerContext *context, const raft::AppendEntriesRequest *request,
      raft::AppendEntriesResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    node_.OnAppendEntries(*request, response);
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *RaftServiceImpl::InstallSnapshot(
      grpc::CallbackServerContext *context, const raft::InstallSnapshotRequest *request,
      raft::InstallSnapshotResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    node_.OnInstallSnapshot(*request, response);
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

} // namespace raftdemo
