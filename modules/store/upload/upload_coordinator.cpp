#include "store/upload/upload_coordinator.h"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace storedemo
{
    namespace
    {
        std::string JoinChunkRequestId(std::string_view base_request_id,
                                       std::string_view suffix)
        {
            return std::string(base_request_id) + "/" + std::string(suffix);
        }

        StorageNodeStatusCode ValidateUploadRequest(const UploadCoordinatorRequest &request,
                                                    std::string *error_detail)
        {
            if (request.request_id.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "upload request_id must not be empty";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.bucket.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "upload bucket must not be empty";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.object_key.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "upload object_key must not be empty";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.object_id.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "upload object_id must not be empty";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.version == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "upload version must be greater than zero";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (request.chunks.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "upload must contain at least one chunk";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            for (const auto &chunk : request.chunks)
            {
                if (chunk.payload.empty())
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = "upload chunk payload must not be empty";
                    }
                    return StorageNodeStatusCode::kInvalidArgument;
                }

                if (chunk.expected_size.has_value() &&
                    *chunk.expected_size !=
                        static_cast<std::uint64_t>(chunk.payload.size()))
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail =
                            "upload chunk expected_size must match payload size";
                    }
                    return StorageNodeStatusCode::kInvalidArgument;
                }
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ResolveChunkIdentity(const UploadCoordinatorRequest &request,
                                                   const UploadChunkInput &chunk,
                                                   ChunkIdentity *identity,
                                                   std::string *error_detail)
        {
            if (identity == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk identity output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            ChunkId chunk_id;
            const auto status = MakeChunkId(request.object_id,
                                            request.version,
                                            chunk.chunk_index,
                                            &chunk_id,
                                            error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            identity->chunk_id = std::move(chunk_id);
            identity->object_id = request.object_id;
            identity->version = request.version;
            identity->chunk_index = chunk.chunk_index;
            identity->offset = chunk.offset;
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ResolveExpectedChecksum(const UploadChunkInput &chunk,
                                                     ChunkChecksum *checksum,
                                                     std::string *error_detail)
        {
            if (checksum == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk checksum output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (chunk.expected_checksum.IsSet())
            {
                *checksum = chunk.expected_checksum;
                return StorageNodeStatusCode::kOk;
            }

            return ComputeChunkChecksum(chunk.payload, checksum, error_detail);
        }

        std::uint64_t ResolveExpectedSize(const UploadChunkInput &chunk)
        {
            return chunk.expected_size.value_or(
                static_cast<std::uint64_t>(chunk.payload.size()));
        }

        StorageNodeStatusCode ComputeObjectEtag(
            const UploadCoordinatorRequest &request,
            std::string *etag,
            std::string *error_detail)
        {
            if (etag == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "etag output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (!request.etag.empty())
            {
                *etag = request.etag;
                return StorageNodeStatusCode::kOk;
            }

            std::string object_payload;
            object_payload.reserve(std::accumulate(
                request.chunks.begin(),
                request.chunks.end(),
                std::uint64_t{0},
                [](const std::uint64_t total, const UploadChunkInput &chunk)
                {
                    return total + static_cast<std::uint64_t>(chunk.payload.size());
                }));

            for (const auto &chunk : request.chunks)
            {
                object_payload.append(chunk.payload);
            }

            ChunkChecksum checksum;
            const auto status =
                ComputeChunkChecksum(object_payload, &checksum, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            *etag = checksum.value;
            return StorageNodeStatusCode::kOk;
        }

        std::uint64_t ComputeObjectSize(const UploadCoordinatorRequest &request)
        {
            return std::accumulate(
                request.chunks.begin(),
                request.chunks.end(),
                std::uint64_t{0},
                [](const std::uint64_t total, const UploadChunkInput &chunk)
                {
                    return total + static_cast<std::uint64_t>(chunk.payload.size());
                });
        }

        bool IsDurableWriteSuccess(const WriteChunkResponse &response)
        {
            return (response.status == StorageNodeStatusCode::kOk ||
                    response.status == StorageNodeStatusCode::kAlreadyExists) &&
                   response.durable;
        }

        StorageNodeStatusCode ResolveUploadFailureStatus(
            const std::vector<UploadReplicaWriteResult> &replica_results)
        {
            for (const auto &result : replica_results)
            {
                if (result.status == StorageNodeStatusCode::kChecksumMismatch)
                {
                    return result.status;
                }
            }

            for (const auto &result : replica_results)
            {
                if (result.status == StorageNodeStatusCode::kConflict)
                {
                    return result.status;
                }
            }

            for (const auto &result : replica_results)
            {
                if (result.status == StorageNodeStatusCode::kOverloaded ||
                    result.status == StorageNodeStatusCode::kTimeout ||
                    result.status == StorageNodeStatusCode::kCancelled ||
                    result.status == StorageNodeStatusCode::kNodeUnavailable ||
                    result.status == StorageNodeStatusCode::kIoError)
                {
                    return result.status;
                }
            }

            return StorageNodeStatusCode::kNodeUnavailable;
        }

        std::string ResolveUploadFailureDetail(
            const ChunkIdentity &identity,
            const std::vector<UploadReplicaWriteResult> &replica_results,
            const std::size_t minimum_successful_writes)
        {
            for (const auto &result : replica_results)
            {
                if (result.status != StorageNodeStatusCode::kOk &&
                    result.status != StorageNodeStatusCode::kAlreadyExists)
                {
                    if (!result.error_detail.empty())
                    {
                        return "chunk " + identity.chunk_id +
                               " did not reach minimum_successful_writes=" +
                               std::to_string(minimum_successful_writes) + ": " +
                               result.error_detail;
                    }
                }
            }

            return "chunk " + identity.chunk_id +
                   " did not reach minimum_successful_writes=" +
                   std::to_string(minimum_successful_writes);
        }

        UploadReplicaWriteResult MakeReplicaWriteResult(
            std::string node_id,
            const WriteChunkResponse &response)
        {
            UploadReplicaWriteResult result;
            result.node_id = std::move(node_id);
            result.status = response.status;
            result.error_detail = response.error_detail;
            result.retry_after_ms = response.retry_after_ms;
            result.durable = response.durable;
            result.already_exists = response.already_exists;
            result.metadata = response.metadata;
            return result;
        }
    }

    UploadMetadataClient::~UploadMetadataClient() = default;
    UploadChunkWriter::~UploadChunkWriter() = default;

    UploadCoordinator::UploadCoordinator(
        std::shared_ptr<UploadMetadataClient> metadata_client,
        std::shared_ptr<UploadChunkWriter> chunk_writer)
        : metadata_client_(std::move(metadata_client))
        , chunk_writer_(std::move(chunk_writer))
    {
        if (metadata_client_ == nullptr)
        {
            throw std::invalid_argument(
                "UploadCoordinator requires a non-null UploadMetadataClient");
        }
        if (chunk_writer_ == nullptr)
        {
            throw std::invalid_argument(
                "UploadCoordinator requires a non-null UploadChunkWriter");
        }
    }

    UploadCoordinatorResult UploadCoordinator::UploadObject(
        const UploadCoordinatorRequest &request) const
    {
        UploadCoordinatorResult result;

        result.status = ValidateUploadRequest(request, &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        const std::uint64_t object_size = ComputeObjectSize(request);
        std::string object_etag;
        result.status =
            ComputeObjectEtag(request, &object_etag, &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        const auto create_result =
            metadata_client_->CreateObject(UploadMetadataCreateRequest{
                .request_id = JoinChunkRequestId(request.request_id, "create"),
                .bucket = request.bucket,
                .object_key = request.object_key,
                .object_id = request.object_id,
                .version = request.version,
                .size = object_size,
                .etag = object_etag,
                .client_time_unix_ms = request.client_time_unix_ms});
        result.status = create_result.status;

        if (result.status != StorageNodeStatusCode::kOk)
        {
            result.error_detail = "CreateObject failed: " +
                                  create_result.error_detail;
            return result;
        }

        result.create_succeeded = true;
        result.pending_object_possible = true;

        for (const auto &chunk : request.chunks)
        {
            result.chunk_executions.push_back({});
            auto &chunk_execution = result.chunk_executions.back();

            std::string error_detail;
            result.status = ResolveChunkIdentity(request,
                                                 chunk,
                                                 &chunk_execution.identity,
                                                 &error_detail);
            if (result.status != StorageNodeStatusCode::kOk)
            {
                result.error_detail = "failed to resolve chunk identity: " +
                                      error_detail;
                return result;
            }

            chunk_execution.placement_decision = {};
            auto placement_result = placement_manager_.SelectPlacement(
                PlacementRequest{
                    .identity = chunk_execution.identity,
                    .chunk_size_bytes = ResolveExpectedSize(chunk),
                    .policy = request.replica_policy,
                    .excluded_nodes = request.excluded_nodes,
                    .decision_epoch = request.client_time_unix_ms},
                request.candidates);
            chunk_execution.placement_decision = placement_result.decision;
            if (!placement_result.ok())
            {
                result.status = placement_result.status;
                result.error_detail = "PlacementManager failed for chunk " +
                                      chunk_execution.identity.chunk_id + ": " +
                                      placement_result.error_detail;
                return result;
            }

            ChunkChecksum expected_checksum;
            result.status =
                ResolveExpectedChecksum(chunk, &expected_checksum, &error_detail);
            if (result.status != StorageNodeStatusCode::kOk)
            {
                result.error_detail = "failed to compute expected checksum for chunk " +
                                      chunk_execution.identity.chunk_id + ": " +
                                      error_detail;
                return result;
            }

            std::vector<StorageNodeId> durable_replicas;
            WriteChunkResponse first_durable_response;

            for (const auto &target :
                 chunk_execution.placement_decision.replica_nodes)
            {
                const auto write_response = chunk_writer_->WriteChunkToNode(
                    target,
                    WriteChunkRequest{
                        .request_id = JoinChunkRequestId(
                            request.request_id,
                            "write-" + chunk_execution.identity.chunk_id + "-" +
                                target.node_id),
                        .identity = chunk_execution.identity,
                        .expected_size = ResolveExpectedSize(chunk),
                        .expected_checksum = expected_checksum,
                        .payload = chunk.payload},
                    request.context);

                chunk_execution.replica_results.push_back(
                    MakeReplicaWriteResult(target.node_id, write_response));

                if (IsDurableWriteSuccess(write_response))
                {
                    if (durable_replicas.empty())
                    {
                        first_durable_response = write_response;
                    }

                    if (std::find(durable_replicas.begin(),
                                  durable_replicas.end(),
                                  target.node_id) == durable_replicas.end())
                    {
                        durable_replicas.push_back(target.node_id);
                    }
                }
            }

            chunk_execution.durable_success_count = durable_replicas.size();
            chunk_execution.commit_eligible =
                chunk_execution.durable_success_count >=
                chunk_execution.placement_decision.minimum_successful_writes;

            if (!chunk_execution.commit_eligible)
            {
                result.status =
                    ResolveUploadFailureStatus(chunk_execution.replica_results);
                result.error_detail = ResolveUploadFailureDetail(
                    chunk_execution.identity,
                    chunk_execution.replica_results,
                    chunk_execution.placement_decision.minimum_successful_writes);
                result.orphan_chunk_possible =
                    result.orphan_chunk_possible ||
                    chunk_execution.durable_success_count > 0 ||
                    !result.committed_chunks.empty();
                return result;
            }

            UploadCommittedChunk committed_chunk;
            committed_chunk.identity = chunk_execution.identity;
            committed_chunk.offset = chunk.offset;
            committed_chunk.size = first_durable_response.metadata.size == 0
                                       ? ResolveExpectedSize(chunk)
                                       : first_durable_response.metadata.size;
            committed_chunk.checksum =
                first_durable_response.metadata.checksum.IsSet()
                    ? first_durable_response.metadata.checksum
                    : expected_checksum;
            committed_chunk.replica_nodes = std::move(durable_replicas);
            result.committed_chunks.push_back(std::move(committed_chunk));
        }

        std::sort(result.committed_chunks.begin(),
                  result.committed_chunks.end(),
                  [](const UploadCommittedChunk &lhs,
                     const UploadCommittedChunk &rhs)
                  {
                      if (lhs.offset != rhs.offset)
                      {
                          return lhs.offset < rhs.offset;
                      }
                      return lhs.identity.chunk_index < rhs.identity.chunk_index;
                  });

        const auto commit_result = metadata_client_->CommitObject(
            UploadMetadataCommitRequest{
                .request_id = JoinChunkRequestId(request.request_id, "commit"),
                .bucket = request.bucket,
                .object_key = request.object_key,
                .object_id = request.object_id,
                .version = request.version,
                .size = object_size,
                .etag = object_etag,
                .chunks = result.committed_chunks,
                .client_time_unix_ms = request.client_time_unix_ms});

        if (!commit_result.ok())
        {
            result.status = commit_result.status;
            result.error_detail = "CommitObject failed: " +
                                  commit_result.error_detail;
            result.orphan_chunk_possible = !result.committed_chunks.empty();
            return result;
        }

        result.status = StorageNodeStatusCode::kOk;
        result.error_detail.clear();
        result.committed = true;
        result.pending_object_possible = false;
        result.orphan_chunk_possible = false;
        return result;
    }
}
