#pragma once

#include <grpcpp/grpcpp.h>

#include "raft.grpc.pb.h"

namespace raftdemo
{

  class RaftNode;

  class KvServiceImpl final : public raft::KvService::CallbackService
  {
  public:
    explicit KvServiceImpl(RaftNode &node);

    grpc::ServerUnaryReactor *Put(grpc::CallbackServerContext *context,
                                  const raft::PutRequest *request,
                                  raft::PutResponse *response) override;

    grpc::ServerUnaryReactor *Delete(grpc::CallbackServerContext *context,
                                     const raft::DeleteRequest *request,
                                     raft::DeleteResponse *response) override;

    grpc::ServerUnaryReactor *Get(grpc::CallbackServerContext *context,
                                  const raft::GetRequest *request,
                                  raft::GetResponse *response) override;

    grpc::ServerUnaryReactor *Status(grpc::CallbackServerContext *context,
                                     const raft::StatusRequest *request,
                                     raft::StatusResponse *response) override;

    grpc::ServerUnaryReactor *Health(grpc::CallbackServerContext *context,
                                     const raft::HealthRequest *request,
                                     raft::HealthResponse *response) override;

    grpc::ServerUnaryReactor *Metrics(grpc::CallbackServerContext *context,
                                      const raft::MetricsRequest *request,
                                      raft::MetricsResponse *response) override;

  private:
    RaftNode &node_;
  };

} // namespace raftdemo
