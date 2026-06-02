#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "storage_node.grpc.pb.h"

namespace storedemo
{
    class ChunkStore;
    class StorageNodeRegistry;

    class StorageNodeService final : public storage::StorageNodeService::CallbackService
    {
    public:
        explicit StorageNodeService(std::shared_ptr<ChunkStore> chunk_store,
                                    std::string node_id = {},
                                    std::shared_ptr<StorageNodeRegistry> storage_node_registry = nullptr);

        grpc::ServerUnaryReactor *WriteChunk(
            grpc::CallbackServerContext *context,
            const storage::WriteChunkRequest *request,
            storage::WriteChunkResponse *response) override;

        grpc::ServerUnaryReactor *ReadChunk(
            grpc::CallbackServerContext *context,
            const storage::ReadChunkRequest *request,
            storage::ReadChunkResponse *response) override;

        grpc::ServerUnaryReactor *DeleteChunk(
            grpc::CallbackServerContext *context,
            const storage::DeleteChunkRequest *request,
            storage::DeleteChunkResponse *response) override;

        grpc::ServerUnaryReactor *BatchDeleteChunks(
            grpc::CallbackServerContext *context,
            const storage::BatchDeleteChunksRequest *request,
            storage::BatchDeleteChunksResponse *response) override;

        grpc::ServerUnaryReactor *RegisterStorageNode(
            grpc::CallbackServerContext *context,
            const storage::RegisterStorageNodeRequest *request,
            storage::RegisterStorageNodeResponse *response) override;

        grpc::ServerUnaryReactor *UpdateStorageNodeHeartbeat(
            grpc::CallbackServerContext *context,
            const storage::UpdateStorageNodeHeartbeatRequest *request,
            storage::StorageNodeFactUpdateResponse *response) override;

        grpc::ServerUnaryReactor *ReportHealth(
            grpc::CallbackServerContext *context,
            const storage::ReportHealthRequest *request,
            storage::StorageNodeFactUpdateResponse *response) override;

        grpc::ServerUnaryReactor *ReportCapacity(
            grpc::CallbackServerContext *context,
            const storage::ReportCapacityRequest *request,
            storage::StorageNodeFactUpdateResponse *response) override;

        grpc::ServerUnaryReactor *ReportLoad(
            grpc::CallbackServerContext *context,
            const storage::ReportLoadRequest *request,
            storage::StorageNodeFactUpdateResponse *response) override;

    private:
        std::shared_ptr<ChunkStore> chunk_store_;
        std::string node_id_;
        std::shared_ptr<StorageNodeRegistry> storage_node_registry_;
    };
}
