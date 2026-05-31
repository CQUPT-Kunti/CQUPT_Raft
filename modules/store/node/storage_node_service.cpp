#include "store/node/storage_node_service.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "store/chunk/chunk_store.h"

namespace storedemo
{
    namespace
    {
        storage::StorageNodeStatusCode ToProtoStatusCode(const StorageNodeStatusCode code)
        {
            switch (code)
            {
            case StorageNodeStatusCode::kOk:
                return storage::STORAGE_NODE_STATUS_CODE_OK;
            case StorageNodeStatusCode::kAlreadyExists:
                return storage::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS;
            case StorageNodeStatusCode::kNotFound:
                return storage::STORAGE_NODE_STATUS_CODE_NOT_FOUND;
            case StorageNodeStatusCode::kConflict:
                return storage::STORAGE_NODE_STATUS_CODE_CONFLICT;
            case StorageNodeStatusCode::kChecksumMismatch:
                return storage::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH;
            case StorageNodeStatusCode::kCorrupted:
                return storage::STORAGE_NODE_STATUS_CODE_CORRUPTED;
            case StorageNodeStatusCode::kDiskFull:
                return storage::STORAGE_NODE_STATUS_CODE_DISK_FULL;
            case StorageNodeStatusCode::kPermissionDenied:
                return storage::STORAGE_NODE_STATUS_CODE_PERMISSION_DENIED;
            case StorageNodeStatusCode::kIoError:
                return storage::STORAGE_NODE_STATUS_CODE_IO_ERROR;
            case StorageNodeStatusCode::kTimeout:
                return storage::STORAGE_NODE_STATUS_CODE_TIMEOUT;
            case StorageNodeStatusCode::kCancelled:
                return storage::STORAGE_NODE_STATUS_CODE_CANCELLED;
            case StorageNodeStatusCode::kOverloaded:
                return storage::STORAGE_NODE_STATUS_CODE_OVERLOADED;
            case StorageNodeStatusCode::kNodeUnavailable:
                return storage::STORAGE_NODE_STATUS_CODE_NODE_UNAVAILABLE;
            case StorageNodeStatusCode::kUnsupported:
                return storage::STORAGE_NODE_STATUS_CODE_UNSUPPORTED;
            case StorageNodeStatusCode::kInvalidArgument:
                return storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT;
            default:
                return storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED;
            }
        }

        storage::StorageChunkState ToProtoChunkState(const ChunkState state)
        {
            switch (state)
            {
            case ChunkState::kStaging:
                return storage::STORAGE_CHUNK_STATE_STAGING;
            case ChunkState::kLive:
                return storage::STORAGE_CHUNK_STATE_LIVE;
            case ChunkState::kDeleting:
                return storage::STORAGE_CHUNK_STATE_DELETING;
            case ChunkState::kDeleted:
                return storage::STORAGE_CHUNK_STATE_DELETED;
            case ChunkState::kQuarantined:
                return storage::STORAGE_CHUNK_STATE_QUARANTINED;
            case ChunkState::kCorrupted:
                return storage::STORAGE_CHUNK_STATE_CORRUPTED;
            case ChunkState::kMissing:
                return storage::STORAGE_CHUNK_STATE_MISSING;
            default:
                return storage::STORAGE_CHUNK_STATE_UNSPECIFIED;
            }
        }

        storage::StorageChecksumAlgorithm ToProtoChecksumAlgorithm(
            const ChunkChecksumAlgorithm algorithm)
        {
            switch (algorithm)
            {
            case ChunkChecksumAlgorithm::kSha256:
                return storage::STORAGE_CHECKSUM_ALGORITHM_SHA256;
            case ChunkChecksumAlgorithm::kUnknown:
            default:
                return storage::STORAGE_CHECKSUM_ALGORITHM_UNSPECIFIED;
            }
        }

        StorageNodeStatusCode FromProtoChecksumAlgorithm(
            const storage::StorageChecksumAlgorithm algorithm,
            ChunkChecksumAlgorithm *out_algorithm,
            std::string *error_detail)
        {
            if (out_algorithm == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "checksum algorithm output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            switch (algorithm)
            {
            case storage::STORAGE_CHECKSUM_ALGORITHM_UNSPECIFIED:
                *out_algorithm = ChunkChecksumAlgorithm::kUnknown;
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_CHECKSUM_ALGORITHM_SHA256:
                *out_algorithm = ChunkChecksumAlgorithm::kSha256;
                return StorageNodeStatusCode::kOk;
            default:
                if (error_detail != nullptr)
                {
                    *error_detail = "expected_checksum algorithm is not supported";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
        }

        StorageNodeStatusCode FromProtoChecksum(const storage::StorageChunkChecksum &proto_checksum,
                                                ChunkChecksum *out_checksum,
                                                std::string *error_detail)
        {
            if (out_checksum == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "checksum output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            ChunkChecksum checksum;
            const auto status = FromProtoChecksumAlgorithm(proto_checksum.algorithm(),
                                                           &checksum.algorithm,
                                                           error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            checksum.value = proto_checksum.value();
            checksum.size_bytes = proto_checksum.size_bytes();
            checksum.computed_at = proto_checksum.computed_at_unix_ms();
            *out_checksum = std::move(checksum);
            return StorageNodeStatusCode::kOk;
        }

        void FillProtoChecksum(const ChunkChecksum &checksum,
                               storage::StorageChunkChecksum *out_checksum)
        {
            if (out_checksum == nullptr)
            {
                return;
            }

            out_checksum->set_algorithm(ToProtoChecksumAlgorithm(checksum.algorithm));
            out_checksum->set_value(checksum.value);
            out_checksum->set_size_bytes(checksum.size_bytes);
            out_checksum->set_computed_at_unix_ms(checksum.computed_at);
        }

        std::string ResolveResponseChunkId(const storage::WriteChunkRequest &request,
                                           const WriteChunkResponse &store_response)
        {
            if (!store_response.metadata.identity.chunk_id.empty())
            {
                return store_response.metadata.identity.chunk_id;
            }

            if (!request.chunk_id().empty())
            {
                return request.chunk_id();
            }

            if (request.object_id().empty() || request.version() == 0)
            {
                return {};
            }

            ChunkId chunk_id;
            std::string error_detail;
            const auto status = MakeChunkId(request.object_id(),
                                            request.version(),
                                            request.chunk_index(),
                                            &chunk_id,
                                            &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return {};
            }

            return chunk_id;
        }

        std::string ResolveResponseChunkId(const storage::ReadChunkRequest &request,
                                           const ReadChunkResponse &store_response)
        {
            if (!store_response.metadata.identity.chunk_id.empty())
            {
                return store_response.metadata.identity.chunk_id;
            }

            if (!request.chunk_id().empty())
            {
                return request.chunk_id();
            }

            if (request.object_id().empty() || request.version() == 0)
            {
                return {};
            }

            ChunkId chunk_id;
            std::string error_detail;
            const auto status = MakeChunkId(request.object_id(),
                                            request.version(),
                                            request.chunk_index(),
                                            &chunk_id,
                                            &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return {};
            }

            return chunk_id;
        }

        bool HasObjectIdentity(const std::string_view object_id,
                               const std::uint64_t version,
                               const std::uint32_t chunk_index)
        {
            return !object_id.empty() || version != 0 || chunk_index != 0;
        }

        StorageNodeStatusCode ResolveDeleteRequestChunkId(const std::string_view chunk_id,
                                                          const std::string_view object_id,
                                                          const std::uint64_t version,
                                                          const std::uint32_t chunk_index,
                                                          ChunkId *out_chunk_id,
                                                          std::string *error_detail)
        {
            if (out_chunk_id == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "delete chunk_id output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            const bool has_identity = HasObjectIdentity(object_id, version, chunk_index);
            if (!chunk_id.empty())
            {
                *out_chunk_id = std::string(chunk_id);
                if (!has_identity)
                {
                    return StorageNodeStatusCode::kOk;
                }

                ChunkId derived_chunk_id;
                const auto derive_status = MakeChunkId(object_id,
                                                       version,
                                                       chunk_index,
                                                       &derived_chunk_id,
                                                       error_detail);
                if (derive_status != StorageNodeStatusCode::kOk)
                {
                    return derive_status;
                }

                if (derived_chunk_id != chunk_id)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail =
                            "DeleteChunk chunk_id does not match object identity";
                    }
                    return StorageNodeStatusCode::kInvalidArgument;
                }

                return StorageNodeStatusCode::kOk;
            }

            if (!has_identity)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "DeleteChunk requires chunk_id or object identity";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return MakeChunkId(object_id,
                               version,
                               chunk_index,
                               out_chunk_id,
                               error_detail);
        }

        std::string ResolveResponseChunkId(const storage::DeleteChunkRequest &request,
                                           const DeleteChunkResponse &store_response)
        {
            if (!store_response.metadata.identity.chunk_id.empty())
            {
                return store_response.metadata.identity.chunk_id;
            }

            if (!request.chunk_id().empty())
            {
                return request.chunk_id();
            }

            if (!HasObjectIdentity(request.object_id(),
                                   request.version(),
                                   request.chunk_index()))
            {
                return {};
            }

            ChunkId chunk_id;
            std::string error_detail;
            const auto status = MakeChunkId(request.object_id(),
                                            request.version(),
                                            request.chunk_index(),
                                            &chunk_id,
                                            &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return {};
            }

            return chunk_id;
        }

        void FillSummary(const storage::WriteChunkRequest &request,
                         const WriteChunkResponse &store_response,
                         const std::string &configured_node_id,
                         storage::StorageNodeResponseSummary *summary)
        {
            if (summary == nullptr)
            {
                return;
            }

            summary->set_code(ToProtoStatusCode(store_response.status));
            summary->set_message(store_response.error_detail);
            summary->set_request_id(request.request_id());

            if (!store_response.metadata.node_id.empty())
            {
                summary->set_node_id(store_response.metadata.node_id);
            }
            else
            {
                summary->set_node_id(configured_node_id);
            }

            summary->set_chunk_id(ResolveResponseChunkId(request, store_response));
            summary->set_retry_after_ms(store_response.retry_after_ms);
        }

        void FillSummary(const storage::DeleteChunkRequest &request,
                         const DeleteChunkResponse &store_response,
                         const std::string &configured_node_id,
                         storage::StorageNodeResponseSummary *summary)
        {
            if (summary == nullptr)
            {
                return;
            }

            summary->set_code(ToProtoStatusCode(store_response.status));
            summary->set_message(store_response.error_detail);
            summary->set_request_id(request.request_id());

            if (!store_response.metadata.node_id.empty())
            {
                summary->set_node_id(store_response.metadata.node_id);
            }
            else
            {
                summary->set_node_id(configured_node_id);
            }

            summary->set_chunk_id(ResolveResponseChunkId(request, store_response));
            summary->set_retry_after_ms(store_response.retry_after_ms);
        }

        void FillSummary(const storage::ReadChunkRequest &request,
                         const ReadChunkResponse &store_response,
                         const std::string &configured_node_id,
                         storage::StorageNodeResponseSummary *summary)
        {
            if (summary == nullptr)
            {
                return;
            }

            summary->set_code(ToProtoStatusCode(store_response.status));
            summary->set_message(store_response.error_detail);
            summary->set_request_id(request.request_id());

            if (!store_response.metadata.node_id.empty())
            {
                summary->set_node_id(store_response.metadata.node_id);
            }
            else
            {
                summary->set_node_id(configured_node_id);
            }

            summary->set_chunk_id(ResolveResponseChunkId(request, store_response));
            summary->set_retry_after_ms(store_response.retry_after_ms);
        }

        WriteChunkResponse MakeValidationError(const StorageNodeStatusCode status,
                                               std::string error_detail)
        {
            WriteChunkResponse response;
            response.status = status;
            response.error_detail = std::move(error_detail);
            return response;
        }

        ReadChunkResponse MakeReadValidationError(const StorageNodeStatusCode status,
                                                  std::string error_detail)
        {
            ReadChunkResponse response;
            response.status = status;
            response.error_detail = std::move(error_detail);
            return response;
        }

        DeleteChunkResponse MakeDeleteValidationError(const StorageNodeStatusCode status,
                                                      std::string error_detail)
        {
            DeleteChunkResponse response;
            response.status = status;
            response.error_detail = std::move(error_detail);
            return response;
        }

        WriteChunkResponse TranslateWriteRequest(const storage::WriteChunkRequest &request,
                                                 WriteChunkRequest *store_request)
        {
            if (store_request == nullptr)
            {
                return MakeValidationError(StorageNodeStatusCode::kInvalidArgument,
                                           "store write request output must not be null");
            }

            switch (request.durability())
            {
            case storage::WRITE_CHUNK_DURABILITY_UNSPECIFIED:
            case storage::WRITE_CHUNK_DURABILITY_PUBLISH:
                break;
            default:
                return MakeValidationError(StorageNodeStatusCode::kInvalidArgument,
                                           "WriteChunk durability is not supported");
            }

            store_request->request_id = request.request_id();
            store_request->identity.chunk_id = request.chunk_id();
            store_request->identity.object_id = request.object_id();
            store_request->identity.version = request.version();
            store_request->identity.chunk_index = request.chunk_index();
            store_request->identity.offset = request.offset();
            store_request->expected_size = request.expected_size();
            store_request->payload = request.payload();

            std::string error_detail;
            const auto checksum_status = FromProtoChecksum(request.expected_checksum(),
                                                           &store_request->expected_checksum,
                                                           &error_detail);
            if (checksum_status != StorageNodeStatusCode::kOk)
            {
                return MakeValidationError(checksum_status, std::move(error_detail));
            }

            return {};
        }

        ReadChunkResponse TranslateReadRequest(const storage::ReadChunkRequest &request,
                                               ReadChunkRequest *store_request)
        {
            if (store_request == nullptr)
            {
                return MakeReadValidationError(StorageNodeStatusCode::kInvalidArgument,
                                               "store read request output must not be null");
            }

            store_request->request_id = request.request_id();

            if (!request.chunk_id().empty())
            {
                store_request->chunk_id = request.chunk_id();
            }
            else if (!request.object_id().empty() && request.version() != 0)
            {
                ChunkId chunk_id;
                std::string error_detail;
                const auto chunk_status = MakeChunkId(request.object_id(),
                                                      request.version(),
                                                      request.chunk_index(),
                                                      &chunk_id,
                                                      &error_detail);
                if (chunk_status != StorageNodeStatusCode::kOk)
                {
                    return MakeReadValidationError(chunk_status, std::move(error_detail));
                }

                store_request->chunk_id = std::move(chunk_id);
            }
            else
            {
                return MakeReadValidationError(StorageNodeStatusCode::kInvalidArgument,
                                               "ReadChunk requires chunk_id or object identity");
            }

            if (request.length() != 0)
            {
                store_request->range = ChunkReadRange{
                    .offset = request.offset(),
                    .length = request.length()};
            }

            std::string error_detail;
            const auto checksum_status = FromProtoChecksum(request.expected_checksum(),
                                                           &store_request->expected_checksum,
                                                           &error_detail);
            if (checksum_status != StorageNodeStatusCode::kOk)
            {
                return MakeReadValidationError(checksum_status, std::move(error_detail));
            }

            store_request->verify_checksum = request.verify_checksum();
            return {};
        }

        DeleteChunkResponse TranslateDeleteRequest(const storage::DeleteChunkRequest &request,
                                                   DeleteChunkRequest *store_request)
        {
            if (store_request == nullptr)
            {
                return MakeDeleteValidationError(StorageNodeStatusCode::kInvalidArgument,
                                                 "store delete request output must not be null");
            }

            store_request->request_id = request.request_id();
            store_request->reason = request.reason();
            store_request->metadata_boundary = request.metadata_boundary();

            std::string error_detail;
            const auto chunk_status = ResolveDeleteRequestChunkId(request.chunk_id(),
                                                                  request.object_id(),
                                                                  request.version(),
                                                                  request.chunk_index(),
                                                                  &store_request->chunk_id,
                                                                  &error_detail);
            if (chunk_status != StorageNodeStatusCode::kOk)
            {
                return MakeDeleteValidationError(chunk_status, std::move(error_detail));
            }

            const auto checksum_status = FromProtoChecksum(request.expected_checksum(),
                                                           &store_request->expected_checksum,
                                                           &error_detail);
            if (checksum_status != StorageNodeStatusCode::kOk)
            {
                return MakeDeleteValidationError(checksum_status,
                                                 std::move(error_detail));
            }

            return {};
        }

        DeleteChunkResponse TranslateBatchDeleteItemRequest(
            const storage::BatchDeleteChunksRequest &request,
            const storage::BatchDeleteChunkRequest &item,
            const std::size_t index,
            DeleteChunkRequest *store_request)
        {
            if (store_request == nullptr)
            {
                return MakeDeleteValidationError(StorageNodeStatusCode::kInvalidArgument,
                                                 "store batch delete request output must not be null");
            }

            store_request->request_id =
                request.request_id() + "/item/" + std::to_string(index);
            store_request->reason = item.reason();
            store_request->metadata_boundary = item.metadata_boundary();

            std::string error_detail;
            const auto chunk_status = ResolveDeleteRequestChunkId(item.chunk_id(),
                                                                  item.object_id(),
                                                                  item.version(),
                                                                  item.chunk_index(),
                                                                  &store_request->chunk_id,
                                                                  &error_detail);
            if (chunk_status != StorageNodeStatusCode::kOk)
            {
                return MakeDeleteValidationError(chunk_status, std::move(error_detail));
            }

            const auto checksum_status = FromProtoChecksum(item.expected_checksum(),
                                                           &store_request->expected_checksum,
                                                           &error_detail);
            if (checksum_status != StorageNodeStatusCode::kOk)
            {
                return MakeDeleteValidationError(checksum_status,
                                                 std::move(error_detail));
            }

            return {};
        }

        void FillWriteResponse(const storage::WriteChunkRequest &request,
                               const WriteChunkResponse &store_response,
                               const std::string &configured_node_id,
                               storage::WriteChunkResponse *response)
        {
            if (response == nullptr)
            {
                return;
            }

            FillSummary(request,
                        store_response,
                        configured_node_id,
                        response->mutable_summary());

            response->set_size(store_response.metadata.size);
            FillProtoChecksum(store_response.metadata.checksum, response->mutable_checksum());
            response->set_durable(store_response.durable);
            response->set_already_exists(store_response.already_exists);

            if (!store_response.metadata.identity.chunk_id.empty() ||
                store_response.metadata.state != ChunkState::kMissing)
            {
                response->set_state(ToProtoChunkState(store_response.metadata.state));
            }
            else
            {
                response->set_state(storage::STORAGE_CHUNK_STATE_UNSPECIFIED);
            }
        }

        void FillReadResponse(const storage::ReadChunkRequest &request,
                              const ReadChunkResponse &store_response,
                              const std::string &configured_node_id,
                              storage::ReadChunkResponse *response)
        {
            if (response == nullptr)
            {
                return;
            }

            FillSummary(request,
                        store_response,
                        configured_node_id,
                        response->mutable_summary());

            const auto resolved_chunk_id = ResolveResponseChunkId(request, store_response);
            response->set_chunk_id(resolved_chunk_id);
            response->set_payload(store_response.payload);

            if (store_response.metadata.size != 0)
            {
                response->set_size(store_response.metadata.size);
            }
            else
            {
                response->set_size(
                    static_cast<std::uint64_t>(store_response.payload.size()));
            }

            if (store_response.actual_checksum.IsSet())
            {
                FillProtoChecksum(store_response.actual_checksum,
                                  response->mutable_checksum());
            }
            else
            {
                FillProtoChecksum(store_response.metadata.checksum,
                                  response->mutable_checksum());
            }

            if (!resolved_chunk_id.empty() ||
                store_response.metadata.state != ChunkState::kMissing)
            {
                response->set_state(ToProtoChunkState(store_response.metadata.state));
            }
            else
            {
                response->set_state(storage::STORAGE_CHUNK_STATE_UNSPECIFIED);
            }

            response->set_offset(store_response.metadata.identity.offset);

            const bool has_range = request.length() != 0;
            response->set_complete(store_response.status == StorageNodeStatusCode::kOk);
            response->set_full_read(store_response.status == StorageNodeStatusCode::kOk &&
                                    !has_range);
        }

        bool IsDeleteIdempotentSuccess(const DeleteChunkResponse &store_response)
        {
            return store_response.status == StorageNodeStatusCode::kOk &&
                   store_response.already_missing;
        }

        bool IsDeleteRetryableFailure(const DeleteChunkResponse &store_response)
        {
            return store_response.status != StorageNodeStatusCode::kOk &&
                   IsRetriableStatus(store_response.status);
        }

        void FillDeleteResponse(const storage::DeleteChunkRequest &request,
                                const DeleteChunkResponse &store_response,
                                const std::string &configured_node_id,
                                storage::DeleteChunkResponse *response)
        {
            if (response == nullptr)
            {
                return;
            }

            FillSummary(request,
                        store_response,
                        configured_node_id,
                        response->mutable_summary());

            const auto resolved_chunk_id = ResolveResponseChunkId(request, store_response);
            response->set_chunk_id(resolved_chunk_id);
            response->set_size(store_response.metadata.size);
            FillProtoChecksum(store_response.metadata.checksum, response->mutable_checksum());

            if (!resolved_chunk_id.empty() ||
                store_response.metadata.state != ChunkState::kMissing)
            {
                response->set_state(ToProtoChunkState(store_response.metadata.state));
            }
            else
            {
                response->set_state(storage::STORAGE_CHUNK_STATE_UNSPECIFIED);
            }

            response->set_deleted(store_response.deleted);
            response->set_already_missing(store_response.already_missing);
            response->set_already_deleted(store_response.already_missing &&
                                          store_response.metadata.state ==
                                              ChunkState::kDeleted);
            response->set_retryable(IsDeleteRetryableFailure(store_response));
        }

        void FillBatchDeleteResult(const storage::BatchDeleteChunksRequest &request,
                                   const storage::BatchDeleteChunkRequest &item,
                                   const std::size_t index,
                                   const DeleteChunkResponse &store_response,
                                   const std::string &configured_node_id,
                                   storage::BatchDeleteChunkResult *result)
        {
            if (result == nullptr)
            {
                return;
            }

            storage::DeleteChunkRequest synthetic_request;
            synthetic_request.set_request_id(request.request_id() + "/item/" +
                                             std::to_string(index));
            synthetic_request.set_chunk_id(item.chunk_id());
            synthetic_request.set_object_id(item.object_id());
            synthetic_request.set_version(item.version());
            synthetic_request.set_chunk_index(item.chunk_index());
            *synthetic_request.mutable_expected_checksum() = item.expected_checksum();
            synthetic_request.set_reason(item.reason());
            synthetic_request.set_metadata_boundary(item.metadata_boundary());

            storage::DeleteChunkResponse single_response;
            FillDeleteResponse(synthetic_request,
                               store_response,
                               configured_node_id,
                               &single_response);

            *result->mutable_summary() = single_response.summary();
            result->set_chunk_id(single_response.chunk_id());
            result->set_size(single_response.size());
            *result->mutable_checksum() = single_response.checksum();
            result->set_state(single_response.state());
            result->set_deleted(single_response.deleted());
            result->set_already_missing(single_response.already_missing());
            result->set_already_deleted(single_response.already_deleted());
            result->set_retryable(single_response.retryable());
        }

        void FillBatchSummary(const storage::BatchDeleteChunksRequest &request,
                              const std::string &configured_node_id,
                              const std::uint32_t success_count,
                              const std::uint32_t idempotent_count,
                              const std::uint32_t retryable_failure_count,
                              const std::uint32_t non_retryable_failure_count,
                              const std::uint64_t retry_after_ms,
                              storage::StorageNodeResponseSummary *summary)
        {
            if (summary == nullptr)
            {
                return;
            }

            summary->set_code(storage::STORAGE_NODE_STATUS_CODE_OK);
            if (retryable_failure_count != 0 ||
                non_retryable_failure_count != 0)
            {
                if (success_count != 0 || idempotent_count != 0)
                {
                    summary->set_message(
                        "BatchDeleteChunks completed with partial failures");
                }
                else
                {
                    summary->set_message(
                        "BatchDeleteChunks completed with item failures");
                }
            }
            else
            {
                summary->set_message("");
            }
            summary->set_request_id(request.request_id());
            summary->set_node_id(configured_node_id);
            summary->set_chunk_id("");
            summary->set_retry_after_ms(retry_after_ms);
        }

        void FillBatchValidationError(const storage::BatchDeleteChunksRequest &request,
                                      const StorageNodeStatusCode status,
                                      std::string error_detail,
                                      const std::string &configured_node_id,
                                      storage::BatchDeleteChunksResponse *response)
        {
            if (response == nullptr)
            {
                return;
            }

            auto *summary = response->mutable_summary();
            summary->set_code(ToProtoStatusCode(status));
            summary->set_message(std::move(error_detail));
            summary->set_request_id(request.request_id());
            summary->set_node_id(configured_node_id);
            summary->set_chunk_id("");
            summary->set_retry_after_ms(0);
            response->set_partial_failure(false);
        }
    }

    StorageNodeService::StorageNodeService(std::shared_ptr<ChunkStore> chunk_store,
                                           std::string node_id)
        : chunk_store_(std::move(chunk_store))
        , node_id_(std::move(node_id))
    {
        if (chunk_store_ == nullptr)
        {
            throw std::invalid_argument("StorageNodeService requires a non-null ChunkStore");
        }
    }

    grpc::ServerUnaryReactor *StorageNodeService::WriteChunk(
        grpc::CallbackServerContext *context,
        const storage::WriteChunkRequest *request,
        storage::WriteChunkResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "WriteChunk request/response must not be null"));
            return reactor;
        }

        (void)request->timeout_ms();
        (void)request->best_effort_cancel();

        WriteChunkRequest store_request;
        auto store_response = TranslateWriteRequest(*request, &store_request);
        if (store_response.status == StorageNodeStatusCode::kOk)
        {
            store_response = chunk_store_->WriteChunk(store_request);
        }

        FillWriteResponse(*request, store_response, node_id_, response);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }

    grpc::ServerUnaryReactor *StorageNodeService::ReadChunk(
        grpc::CallbackServerContext *context,
        const storage::ReadChunkRequest *request,
        storage::ReadChunkResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "ReadChunk request/response must not be null"));
            return reactor;
        }

        (void)request->timeout_ms();
        (void)request->best_effort_cancel();

        ReadChunkRequest store_request;
        auto store_response = TranslateReadRequest(*request, &store_request);
        if (store_response.status == StorageNodeStatusCode::kOk)
        {
            store_response = chunk_store_->ReadChunk(store_request);
        }

        FillReadResponse(*request, store_response, node_id_, response);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }

    grpc::ServerUnaryReactor *StorageNodeService::DeleteChunk(
        grpc::CallbackServerContext *context,
        const storage::DeleteChunkRequest *request,
        storage::DeleteChunkResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "DeleteChunk request/response must not be null"));
            return reactor;
        }

        (void)request->timeout_ms();
        (void)request->best_effort_cancel();

        DeleteChunkRequest store_request;
        auto store_response = TranslateDeleteRequest(*request, &store_request);
        if (store_response.status == StorageNodeStatusCode::kOk)
        {
            store_response = chunk_store_->DeleteChunk(store_request);
        }

        FillDeleteResponse(*request, store_response, node_id_, response);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }

    grpc::ServerUnaryReactor *StorageNodeService::BatchDeleteChunks(
        grpc::CallbackServerContext *context,
        const storage::BatchDeleteChunksRequest *request,
        storage::BatchDeleteChunksResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "BatchDeleteChunks request/response must not be null"));
            return reactor;
        }

        (void)request->timeout_ms();
        (void)request->best_effort_cancel();

        if (request->request_id().empty())
        {
            FillBatchValidationError(*request,
                                     StorageNodeStatusCode::kInvalidArgument,
                                     "BatchDeleteChunks request_id must not be empty",
                                     node_id_,
                                     response);
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }

        if (request->chunks().empty())
        {
            FillBatchValidationError(*request,
                                     StorageNodeStatusCode::kInvalidArgument,
                                     "BatchDeleteChunks requires at least one chunk request",
                                     node_id_,
                                     response);
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }

        std::uint32_t success_count = 0;
        std::uint32_t idempotent_count = 0;
        std::uint32_t retryable_failure_count = 0;
        std::uint32_t non_retryable_failure_count = 0;
        std::uint64_t retry_after_ms = 0;

        for (int index = 0; index < request->chunks_size(); ++index)
        {
            const auto &item = request->chunks(index);
            DeleteChunkRequest store_request;
            auto store_response =
                TranslateBatchDeleteItemRequest(*request,
                                                item,
                                                static_cast<std::size_t>(index),
                                                &store_request);
            if (store_response.status == StorageNodeStatusCode::kOk)
            {
                store_response = chunk_store_->DeleteChunk(store_request);
            }

            auto *result = response->add_results();
            FillBatchDeleteResult(*request,
                                  item,
                                  static_cast<std::size_t>(index),
                                  store_response,
                                  node_id_,
                                  result);

            if (store_response.status == StorageNodeStatusCode::kOk)
            {
                if (IsDeleteIdempotentSuccess(store_response))
                {
                    ++idempotent_count;
                }
                else
                {
                    ++success_count;
                }
                continue;
            }

            if (IsDeleteRetryableFailure(store_response))
            {
                ++retryable_failure_count;
                if (store_response.retry_after_ms != 0)
                {
                    retry_after_ms = retry_after_ms == 0
                                         ? store_response.retry_after_ms
                                         : std::min(retry_after_ms,
                                                    store_response.retry_after_ms);
                }
            }
            else
            {
                ++non_retryable_failure_count;
            }
        }

        response->set_success_count(success_count);
        response->set_idempotent_count(idempotent_count);
        response->set_retryable_failure_count(retryable_failure_count);
        response->set_non_retryable_failure_count(non_retryable_failure_count);
        response->set_partial_failure(
            (retryable_failure_count != 0 || non_retryable_failure_count != 0) &&
            (success_count != 0 || idempotent_count != 0));

        FillBatchSummary(*request,
                         node_id_,
                         success_count,
                         idempotent_count,
                         retryable_failure_count,
                         non_retryable_failure_count,
                         retry_after_ms,
                         response->mutable_summary());
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }
}
