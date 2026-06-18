#pragma once

#include <grpcpp/grpcpp.h>

#include <memory>

#include "metadata.pb.h"
#include "metadata.grpc.pb.h"

namespace raftdemo
{

  class RaftNode;

  class MetadataServiceImpl final : public raft::MetadataService::CallbackService
  {
  public:
    explicit MetadataServiceImpl(RaftNode &node);
    ~MetadataServiceImpl() override;

    grpc::ServerUnaryReactor *CreateBucket(
        grpc::CallbackServerContext *context,
        const raft::CreateBucketRequest *request,
        raft::CreateBucketResponse *response) override;

    grpc::ServerUnaryReactor *DeleteBucket(
        grpc::CallbackServerContext *context,
        const raft::DeleteBucketRequest *request,
        raft::DeleteBucketResponse *response) override;

    grpc::ServerUnaryReactor *CreateObject(
        grpc::CallbackServerContext *context,
        const raft::CreateObjectRequest *request,
        raft::CreateObjectResponse *response) override;

    grpc::ServerUnaryReactor *CommitObject(
        grpc::CallbackServerContext *context,
        const raft::CommitObjectRequest *request,
        raft::CommitObjectResponse *response) override;

    grpc::ServerUnaryReactor *AbortObject(
        grpc::CallbackServerContext *context,
        const raft::AbortObjectRequest *request,
        raft::AbortObjectResponse *response) override;

    grpc::ServerUnaryReactor *DeleteObject(
        grpc::CallbackServerContext *context,
        const raft::DeleteObjectRequest *request,
        raft::DeleteObjectResponse *response) override;

    grpc::ServerUnaryReactor *HeadObject(
        grpc::CallbackServerContext *context,
        const raft::HeadObjectRequest *request,
        raft::HeadObjectResponse *response) override;

    grpc::ServerUnaryReactor *ListObjects(
        grpc::CallbackServerContext *context,
        const raft::ListObjectsRequest *request,
        raft::ListObjectsResponse *response) override;

    grpc::ServerUnaryReactor *JoinMetadataCluster(
        grpc::CallbackServerContext *context,
        const raft::JoinMetadataClusterRequest *request,
        raft::JoinMetadataClusterResponse *response) override;

  private:
    RaftNode &node_;
  };

} // namespace raftdemo
