#pragma once

#include <cstdint>
#include <memory>

#include <grpcpp/grpcpp.h>

#include "storage_node.grpc.pb.h"
#include "store/chunk/chunk_store.h"
#include "store/runtime/storage_executor.h"

namespace storedemo
{
    enum class StorageNodeWriteDurability : std::uint8_t
    {
        kUnspecified = 0,
        kPublish = 1,
    };

    struct StorageNodeClientConfig
    {
        std::uint32_t max_write_retries{0};
    };

    struct StorageNodeClientWriteChunkOptions
    {
        StorageTaskContext context;
        StorageNodeWriteDurability durability{StorageNodeWriteDurability::kPublish};
    };

    struct StorageNodeClientReadChunkOptions
    {
        StorageTaskContext context;
    };

    class StorageNodeClient
    {
    public:
        explicit StorageNodeClient(
            std::unique_ptr<storage::StorageNodeService::StubInterface> stub,
            StorageNodeClientConfig config = {});
        explicit StorageNodeClient(std::shared_ptr<grpc::Channel> channel,
                                   StorageNodeClientConfig config = {});

        WriteChunkResponse WriteChunk(
            const WriteChunkRequest &request,
            StorageNodeClientWriteChunkOptions options = {});

        ReadChunkResponse ReadChunk(
            const ReadChunkRequest &request,
            StorageNodeClientReadChunkOptions options = {});

        [[nodiscard]] const StorageNodeClientConfig &config() const;

    private:
        std::unique_ptr<storage::StorageNodeService::StubInterface> stub_;
        StorageNodeClientConfig config_;
    };
}
