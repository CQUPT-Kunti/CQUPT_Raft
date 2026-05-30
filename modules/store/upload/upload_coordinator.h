#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "store/chunk/chunk_store.h"
#include "store/placement/placement_manager.h"
#include "store/runtime/storage_executor.h"

namespace storedemo
{
    struct UploadChunkInput
    {
        std::uint32_t chunk_index{0};
        std::uint64_t offset{0};
        std::string payload;
        std::optional<std::uint64_t> expected_size;
        ChunkChecksum expected_checksum;
    };

    struct UploadCommittedChunk
    {
        ChunkIdentity identity;
        std::uint64_t offset{0};
        std::uint64_t size{0};
        ChunkChecksum checksum;
        std::vector<StorageNodeId> replica_nodes;
    };

    struct UploadReplicaWriteResult
    {
        StorageNodeId node_id;
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::uint64_t retry_after_ms{0};
        bool durable{false};
        bool already_exists{false};
        ChunkMetadata metadata;
    };

    struct UploadChunkExecution
    {
        ChunkIdentity identity;
        PlacementDecision placement_decision;
        std::vector<UploadReplicaWriteResult> replica_results;
        std::size_t durable_success_count{0};
        bool commit_eligible{false};
    };

    struct UploadCoordinatorRequest
    {
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        std::string etag;
        std::vector<UploadChunkInput> chunks;
        ReplicaPolicy replica_policy;
        std::vector<StorageNodePlacementCandidate> candidates;
        std::vector<StorageNodeId> excluded_nodes;
        StorageTaskContext context;
        std::uint64_t client_time_unix_ms{0};
    };

    struct UploadCoordinatorResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        bool create_succeeded{false};
        bool committed{false};
        bool pending_object_possible{false};
        bool orphan_chunk_possible{false};
        std::vector<UploadCommittedChunk> committed_chunks;
        std::vector<UploadChunkExecution> chunk_executions;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    struct UploadMetadataCreateRequest
    {
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        std::uint64_t size{0};
        std::string etag;
        std::uint64_t client_time_unix_ms{0};
    };

    struct UploadMetadataCommitRequest
    {
        std::string request_id;
        std::string bucket;
        std::string object_key;
        std::string object_id;
        std::uint64_t version{0};
        std::uint64_t size{0};
        std::string etag;
        std::vector<UploadCommittedChunk> chunks;
        std::uint64_t client_time_unix_ms{0};
    };

    struct UploadMetadataResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    class UploadMetadataClient
    {
    public:
        virtual ~UploadMetadataClient();

        virtual UploadMetadataResult CreateObject(
            const UploadMetadataCreateRequest &request) = 0;
        virtual UploadMetadataResult CommitObject(
            const UploadMetadataCommitRequest &request) = 0;
    };

    class UploadChunkWriter
    {
    public:
        virtual ~UploadChunkWriter();

        virtual WriteChunkResponse WriteChunkToNode(
            const StorageNodePlacementCandidate &target,
            const WriteChunkRequest &request,
            const StorageTaskContext &context) = 0;
    };

    class UploadCoordinator
    {
    public:
        UploadCoordinator(std::shared_ptr<UploadMetadataClient> metadata_client,
                          std::shared_ptr<UploadChunkWriter> chunk_writer);

        [[nodiscard]] UploadCoordinatorResult UploadObject(
            const UploadCoordinatorRequest &request) const;

    private:
        std::shared_ptr<UploadMetadataClient> metadata_client_;
        std::shared_ptr<UploadChunkWriter> chunk_writer_;
        PlacementManager placement_manager_;
    };
}
