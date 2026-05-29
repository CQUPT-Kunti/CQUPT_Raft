#include "store/node/storage_node_service.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "store/chunk/chunk_store.h"

namespace storedemo
{
    namespace
    {
        raft::StorageNodeStatusCode ToProtoStatusCode(const StorageNodeStatusCode code)
        {
            switch (code)
            {
            case StorageNodeStatusCode::kOk:
                return raft::STORAGE_NODE_STATUS_CODE_OK;
            case StorageNodeStatusCode::kAlreadyExists:
                return raft::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS;
            case StorageNodeStatusCode::kNotFound:
                return raft::STORAGE_NODE_STATUS_CODE_NOT_FOUND;
            case StorageNodeStatusCode::kConflict:
                return raft::STORAGE_NODE_STATUS_CODE_CONFLICT;
            case StorageNodeStatusCode::kChecksumMismatch:
                return raft::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH;
            case StorageNodeStatusCode::kCorrupted:
                return raft::STORAGE_NODE_STATUS_CODE_CORRUPTED;
            case StorageNodeStatusCode::kDiskFull:
                return raft::STORAGE_NODE_STATUS_CODE_DISK_FULL;
            case StorageNodeStatusCode::kPermissionDenied:
                return raft::STORAGE_NODE_STATUS_CODE_PERMISSION_DENIED;
            case StorageNodeStatusCode::kIoError:
                return raft::STORAGE_NODE_STATUS_CODE_IO_ERROR;
            case StorageNodeStatusCode::kTimeout:
                return raft::STORAGE_NODE_STATUS_CODE_TIMEOUT;
            case StorageNodeStatusCode::kCancelled:
                return raft::STORAGE_NODE_STATUS_CODE_CANCELLED;
            case StorageNodeStatusCode::kOverloaded:
                return raft::STORAGE_NODE_STATUS_CODE_OVERLOADED;
            case StorageNodeStatusCode::kNodeUnavailable:
                return raft::STORAGE_NODE_STATUS_CODE_NODE_UNAVAILABLE;
            case StorageNodeStatusCode::kUnsupported:
                return raft::STORAGE_NODE_STATUS_CODE_UNSUPPORTED;
            case StorageNodeStatusCode::kInvalidArgument:
                return raft::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT;
            default:
                return raft::STORAGE_NODE_STATUS_CODE_UNSPECIFIED;
            }
        }

        raft::StorageChunkState ToProtoChunkState(const ChunkState state)
        {
            switch (state)
            {
            case ChunkState::kStaging:
                return raft::STORAGE_CHUNK_STATE_STAGING;
            case ChunkState::kLive:
                return raft::STORAGE_CHUNK_STATE_LIVE;
            case ChunkState::kDeleting:
                return raft::STORAGE_CHUNK_STATE_DELETING;
            case ChunkState::kDeleted:
                return raft::STORAGE_CHUNK_STATE_DELETED;
            case ChunkState::kQuarantined:
                return raft::STORAGE_CHUNK_STATE_QUARANTINED;
            case ChunkState::kCorrupted:
                return raft::STORAGE_CHUNK_STATE_CORRUPTED;
            case ChunkState::kMissing:
                return raft::STORAGE_CHUNK_STATE_MISSING;
            default:
                return raft::STORAGE_CHUNK_STATE_UNSPECIFIED;
            }
        }

        raft::StorageChecksumAlgorithm ToProtoChecksumAlgorithm(
            const ChunkChecksumAlgorithm algorithm)
        {
            switch (algorithm)
            {
            case ChunkChecksumAlgorithm::kSha256:
                return raft::STORAGE_CHECKSUM_ALGORITHM_SHA256;
            case ChunkChecksumAlgorithm::kUnknown:
            default:
                return raft::STORAGE_CHECKSUM_ALGORITHM_UNSPECIFIED;
            }
        }

        StorageNodeStatusCode FromProtoChecksumAlgorithm(
            const raft::StorageChecksumAlgorithm algorithm,
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
            case raft::STORAGE_CHECKSUM_ALGORITHM_UNSPECIFIED:
                *out_algorithm = ChunkChecksumAlgorithm::kUnknown;
                return StorageNodeStatusCode::kOk;
            case raft::STORAGE_CHECKSUM_ALGORITHM_SHA256:
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

        StorageNodeStatusCode FromProtoChecksum(const raft::StorageChunkChecksum &proto_checksum,
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
                               raft::StorageChunkChecksum *out_checksum)
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

        std::string ResolveResponseChunkId(const raft::WriteChunkRequest &request,
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

        void FillSummary(const raft::WriteChunkRequest &request,
                         const WriteChunkResponse &store_response,
                         const std::string &configured_node_id,
                         raft::StorageNodeResponseSummary *summary)
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

        WriteChunkResponse TranslateWriteRequest(const raft::WriteChunkRequest &request,
                                                 WriteChunkRequest *store_request)
        {
            if (store_request == nullptr)
            {
                return MakeValidationError(StorageNodeStatusCode::kInvalidArgument,
                                           "store write request output must not be null");
            }

            switch (request.durability())
            {
            case raft::WRITE_CHUNK_DURABILITY_UNSPECIFIED:
            case raft::WRITE_CHUNK_DURABILITY_PUBLISH:
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

        void FillWriteResponse(const raft::WriteChunkRequest &request,
                               const WriteChunkResponse &store_response,
                               const std::string &configured_node_id,
                               raft::WriteChunkResponse *response)
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
                response->set_state(raft::STORAGE_CHUNK_STATE_UNSPECIFIED);
            }
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
        const raft::WriteChunkRequest *request,
        raft::WriteChunkResponse *response)
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
}
