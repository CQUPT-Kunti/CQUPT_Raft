#pragma once

#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "storage_node.grpc.pb.h"

namespace storedemo
{
    class ChunkStore;

    class StorageNodeService final : public storage::StorageNodeService::CallbackService
    {
    public:
        explicit StorageNodeService(std::shared_ptr<ChunkStore> chunk_store,
                                    std::string node_id = {});

        grpc::ServerUnaryReactor *WriteChunk(
            grpc::CallbackServerContext *context,
            const storage::WriteChunkRequest *request,
            storage::WriteChunkResponse *response) override;

        grpc::ServerUnaryReactor *ReadChunk(
            grpc::CallbackServerContext *context,
            const storage::ReadChunkRequest *request,
            storage::ReadChunkResponse *response) override;

    private:
        std::shared_ptr<ChunkStore> chunk_store_;
        std::string node_id_;
    };
}
