#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    enum class ReadReplicaFailureAction : std::uint8_t
    {
        kStop = 0,
        kTryNext = 1,
    };

    struct ReadReplicaAttempt
    {
        StorageNodeId node_id;
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        ReadReplicaFailureAction action{ReadReplicaFailureAction::kStop};
    };

    struct ReadReplicaFallbackResult
    {
        ReadChunkResponse response;
        StorageNodeId selected_node_id;
        std::vector<ReadReplicaAttempt> attempts;

        [[nodiscard]] bool ok() const
        {
            return response.status == StorageNodeStatusCode::kOk;
        }
    };

    using ReadChunkReplicaInvoker =
        std::function<ReadChunkResponse(const StorageNodeId &node_id,
                                        const ReadChunkRequest &request,
                                        StorageNodeClientReadChunkOptions options)>;

    ReadChunkRequest MakeReadChunkRequestForCommittedManifestReplica(
        std::string_view request_id,
        std::string_view chunk_id,
        std::uint64_t expected_size,
        std::string_view expected_checksum);

    ReadReplicaFailureAction ClassifyReadReplicaFailure(
        const ReadChunkResponse &response);

    ReadReplicaFallbackResult ReadChunkWithReplicaFallback(
        std::span<const StorageNodeId> replica_nodes,
        const ReadChunkRequest &request,
        StorageNodeClientReadChunkOptions options,
        const ReadChunkReplicaInvoker &invoker);

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
