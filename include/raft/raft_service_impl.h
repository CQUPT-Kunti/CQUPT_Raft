#pragma once

#include <grpcpp/grpcpp.h>

#include "raft.grpc.pb.h"

namespace raftdemo
{

  class RaftNode;

  class RaftServiceImpl final : public raft::RaftService::CallbackService
  {
  public:
    explicit RaftServiceImpl(RaftNode &node);

    grpc::ServerUnaryReactor *RequestVote(grpc::CallbackServerContext *context,
                                          const raft::VoteRequest *request,
                                          raft::VoteResponse *response) override;

    grpc::ServerUnaryReactor *AppendEntries(grpc::CallbackServerContext *context,
                                            const raft::AppendEntriesRequest *request,
                                            raft::AppendEntriesResponse *response) override;

    grpc::ServerUnaryReactor *InstallSnapshot(grpc::CallbackServerContext *context,
                                              const raft::InstallSnapshotRequest *request,
                                              raft::InstallSnapshotResponse *response) override;

  private:
    RaftNode &node_;
  };

} // namespace raftdemo