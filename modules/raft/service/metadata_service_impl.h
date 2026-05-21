#pragma once

#include <grpcpp/grpcpp.h>

#include "metadata.grpc.pb.h"

namespace raftdemo
{

  class RaftNode;

  class MetadataServiceImpl final : public raft::MetadataService::CallbackService
  {
  public:
    explicit MetadataServiceImpl(RaftNode &node);

    grpc::ServerUnaryReactor *CreateMetadataRecord(
        grpc::CallbackServerContext *context,
        const raft::CreateMetadataRecordRequest *request,
        raft::CreateMetadataRecordResponse *response) override;

    grpc::ServerUnaryReactor *CommitMetadataRecord(
        grpc::CallbackServerContext *context,
        const raft::CommitMetadataRecordRequest *request,
        raft::CommitMetadataRecordResponse *response) override;

    grpc::ServerUnaryReactor *DeleteMetadataRecord(
        grpc::CallbackServerContext *context,
        const raft::DeleteMetadataRecordRequest *request,
        raft::DeleteMetadataRecordResponse *response) override;

    grpc::ServerUnaryReactor *HeadMetadataRecord(
        grpc::CallbackServerContext *context,
        const raft::HeadMetadataRecordRequest *request,
        raft::HeadMetadataRecordResponse *response) override;

    grpc::ServerUnaryReactor *ListMetadataRecords(
        grpc::CallbackServerContext *context,
        const raft::ListMetadataRecordsRequest *request,
        raft::ListMetadataRecordsResponse *response) override;

  private:
    RaftNode &node_;
  };

} // namespace raftdemo
