#include "store/node/storage_node_client.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace storedemo
{
    namespace
    {
        raft::WriteChunkDurability ToProtoDurability(
            const StorageNodeWriteDurability durability)
        {
            switch (durability)
            {
            case StorageNodeWriteDurability::kUnspecified:
                return raft::WRITE_CHUNK_DURABILITY_UNSPECIFIED;
            case StorageNodeWriteDurability::kPublish:
            default:
                return raft::WRITE_CHUNK_DURABILITY_PUBLISH;
            }
        }

        StorageNodeStatusCode FromProtoStatusCode(const raft::StorageNodeStatusCode code)
        {
            switch (code)
            {
            case raft::STORAGE_NODE_STATUS_CODE_OK:
                return StorageNodeStatusCode::kOk;
            case raft::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS:
                return StorageNodeStatusCode::kAlreadyExists;
            case raft::STORAGE_NODE_STATUS_CODE_NOT_FOUND:
                return StorageNodeStatusCode::kNotFound;
            case raft::STORAGE_NODE_STATUS_CODE_CONFLICT:
                return StorageNodeStatusCode::kConflict;
            case raft::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH:
                return StorageNodeStatusCode::kChecksumMismatch;
            case raft::STORAGE_NODE_STATUS_CODE_CORRUPTED:
                return StorageNodeStatusCode::kCorrupted;
            case raft::STORAGE_NODE_STATUS_CODE_DISK_FULL:
                return StorageNodeStatusCode::kDiskFull;
            case raft::STORAGE_NODE_STATUS_CODE_PERMISSION_DENIED:
                return StorageNodeStatusCode::kPermissionDenied;
            case raft::STORAGE_NODE_STATUS_CODE_IO_ERROR:
                return StorageNodeStatusCode::kIoError;
            case raft::STORAGE_NODE_STATUS_CODE_TIMEOUT:
                return StorageNodeStatusCode::kTimeout;
            case raft::STORAGE_NODE_STATUS_CODE_CANCELLED:
                return StorageNodeStatusCode::kCancelled;
            case raft::STORAGE_NODE_STATUS_CODE_OVERLOADED:
                return StorageNodeStatusCode::kOverloaded;
            case raft::STORAGE_NODE_STATUS_CODE_NODE_UNAVAILABLE:
                return StorageNodeStatusCode::kNodeUnavailable;
            case raft::STORAGE_NODE_STATUS_CODE_UNSUPPORTED:
                return StorageNodeStatusCode::kUnsupported;
            case raft::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT:
                return StorageNodeStatusCode::kInvalidArgument;
            case raft::STORAGE_NODE_STATUS_CODE_UNSPECIFIED:
            default:
                return StorageNodeStatusCode::kIoError;
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
                    *error_detail = "WriteChunk response checksum algorithm is not supported";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
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

        StorageNodeStatusCode FillChecksumFromProto(
            const raft::StorageChunkChecksum &proto_checksum,
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
            const auto algorithm_status = FromProtoChecksumAlgorithm(
                proto_checksum.algorithm(),
                &checksum.algorithm,
                error_detail);
            if (algorithm_status != StorageNodeStatusCode::kOk)
            {
                return algorithm_status;
            }

            checksum.value = proto_checksum.value();
            checksum.size_bytes = proto_checksum.size_bytes();
            checksum.computed_at = proto_checksum.computed_at_unix_ms();
            *out_checksum = std::move(checksum);
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode FromProtoChunkState(const raft::StorageChunkState state)
        {
            switch (state)
            {
            case raft::STORAGE_CHUNK_STATE_UNSPECIFIED:
                return StorageNodeStatusCode::kOk;
            case raft::STORAGE_CHUNK_STATE_STAGING:
            case raft::STORAGE_CHUNK_STATE_LIVE:
            case raft::STORAGE_CHUNK_STATE_DELETING:
            case raft::STORAGE_CHUNK_STATE_DELETED:
            case raft::STORAGE_CHUNK_STATE_QUARANTINED:
            case raft::STORAGE_CHUNK_STATE_CORRUPTED:
            case raft::STORAGE_CHUNK_STATE_MISSING:
                return StorageNodeStatusCode::kOk;
            default:
                return StorageNodeStatusCode::kInvalidArgument;
            }
        }

        ChunkState ToStoreChunkState(const raft::StorageChunkState state)
        {
            switch (state)
            {
            case raft::STORAGE_CHUNK_STATE_STAGING:
                return ChunkState::kStaging;
            case raft::STORAGE_CHUNK_STATE_LIVE:
                return ChunkState::kLive;
            case raft::STORAGE_CHUNK_STATE_DELETING:
                return ChunkState::kDeleting;
            case raft::STORAGE_CHUNK_STATE_DELETED:
                return ChunkState::kDeleted;
            case raft::STORAGE_CHUNK_STATE_QUARANTINED:
                return ChunkState::kQuarantined;
            case raft::STORAGE_CHUNK_STATE_CORRUPTED:
                return ChunkState::kCorrupted;
            case raft::STORAGE_CHUNK_STATE_MISSING:
            case raft::STORAGE_CHUNK_STATE_UNSPECIFIED:
            default:
                return ChunkState::kMissing;
            }
        }

        StorageNodeStatusCode MapGrpcStatusCode(const grpc::StatusCode code)
        {
            switch (code)
            {
            case grpc::StatusCode::OK:
                return StorageNodeStatusCode::kOk;
            case grpc::StatusCode::ALREADY_EXISTS:
                return StorageNodeStatusCode::kAlreadyExists;
            case grpc::StatusCode::NOT_FOUND:
                return StorageNodeStatusCode::kNotFound;
            case grpc::StatusCode::FAILED_PRECONDITION:
                return StorageNodeStatusCode::kConflict;
            case grpc::StatusCode::INVALID_ARGUMENT:
                return StorageNodeStatusCode::kInvalidArgument;
            case grpc::StatusCode::DEADLINE_EXCEEDED:
                return StorageNodeStatusCode::kTimeout;
            case grpc::StatusCode::CANCELLED:
                return StorageNodeStatusCode::kCancelled;
            case grpc::StatusCode::UNAVAILABLE:
                return StorageNodeStatusCode::kNodeUnavailable;
            case grpc::StatusCode::RESOURCE_EXHAUSTED:
                return StorageNodeStatusCode::kOverloaded;
            case grpc::StatusCode::UNIMPLEMENTED:
                return StorageNodeStatusCode::kUnsupported;
            case grpc::StatusCode::PERMISSION_DENIED:
            case grpc::StatusCode::UNAUTHENTICATED:
                return StorageNodeStatusCode::kPermissionDenied;
            case grpc::StatusCode::INTERNAL:
            case grpc::StatusCode::UNKNOWN:
            case grpc::StatusCode::DATA_LOSS:
            default:
                return StorageNodeStatusCode::kIoError;
            }
        }

        void FillProtoWriteRequest(const WriteChunkRequest &request,
                                   const StorageNodeClientWriteChunkOptions &options,
                                   raft::WriteChunkRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            proto_request->set_chunk_id(request.identity.chunk_id);
            proto_request->set_object_id(request.identity.object_id);
            proto_request->set_version(request.identity.version);
            proto_request->set_chunk_index(request.identity.chunk_index);
            proto_request->set_offset(request.identity.offset);
            proto_request->set_expected_size(request.expected_size.value_or(
                static_cast<std::uint64_t>(request.payload.size())));
            FillProtoChecksum(request.expected_checksum,
                              proto_request->mutable_expected_checksum());
            proto_request->set_payload(request.payload);
            proto_request->set_timeout_ms(options.context.timeout_ms);
            proto_request->set_best_effort_cancel(options.context.best_effort_cancel);
            proto_request->set_durability(ToProtoDurability(options.durability));
        }

        StorageNodeStatusCode ResolveResponseIdentity(
            const WriteChunkRequest &request,
            const raft::WriteChunkResponse &proto_response,
            ChunkIdentity *out_identity,
            std::string *error_detail)
        {
            if (out_identity == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "identity output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            ChunkIdentity identity = request.identity;

            const std::string &response_chunk_id = proto_response.summary().chunk_id();
            if (!response_chunk_id.empty())
            {
                ChunkIdentity parsed_identity;
                const auto parse_status =
                    ParseChunkId(response_chunk_id, &parsed_identity, error_detail);
                if (parse_status != StorageNodeStatusCode::kOk)
                {
                    return parse_status;
                }

                parsed_identity.offset = request.identity.offset;
                identity = std::move(parsed_identity);
            }
            else if (identity.chunk_id.empty() &&
                     !identity.object_id.empty() &&
                     identity.version != 0)
            {
                ChunkId chunk_id;
                const auto make_status = MakeChunkId(identity.object_id,
                                                     identity.version,
                                                     identity.chunk_index,
                                                     &chunk_id,
                                                     error_detail);
                if (make_status != StorageNodeStatusCode::kOk)
                {
                    return make_status;
                }
                identity.chunk_id = std::move(chunk_id);
            }

            *out_identity = std::move(identity);
            return StorageNodeStatusCode::kOk;
        }

        WriteChunkResponse TranslateProtoWriteResponse(
            const WriteChunkRequest &request,
            const raft::WriteChunkResponse &proto_response)
        {
            WriteChunkResponse response;
            response.status = FromProtoStatusCode(proto_response.summary().code());
            response.error_detail = proto_response.summary().message();
            response.retry_after_ms = proto_response.summary().retry_after_ms();
            response.durable = proto_response.durable();
            response.already_exists = proto_response.already_exists();

            response.metadata.node_id = proto_response.summary().node_id();
            response.metadata.size = proto_response.size();
            response.metadata.state = ToStoreChunkState(proto_response.state());
            response.metadata.last_error = response.status;

            if (response.status == StorageNodeStatusCode::kIoError &&
                proto_response.summary().code() ==
                    raft::STORAGE_NODE_STATUS_CODE_UNSPECIFIED &&
                response.error_detail.empty())
            {
                response.error_detail = "WriteChunk response status is unspecified";
            }

            std::string error_detail;
            const auto identity_status = ResolveResponseIdentity(request,
                                                                 proto_response,
                                                                 &response.metadata.identity,
                                                                 &error_detail);
            if (identity_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid WriteChunk response chunk identity: " +
                                        error_detail;
                response.durable = false;
                return response;
            }

            const auto checksum_status =
                FillChecksumFromProto(proto_response.checksum(),
                                      &response.metadata.checksum,
                                      &error_detail);
            if (checksum_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid WriteChunk response checksum: " +
                                        error_detail;
                response.durable = false;
                return response;
            }

            const auto state_status = FromProtoChunkState(proto_response.state());
            if (state_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid WriteChunk response chunk state";
                response.durable = false;
                return response;
            }

            return response;
        }

        WriteChunkResponse MakeGrpcFailureResponse(const grpc::Status &status)
        {
            WriteChunkResponse response;
            response.status = MapGrpcStatusCode(status.error_code());
            response.error_detail = status.error_message();
            return response;
        }

        bool ShouldRetryWriteChunk(const WriteChunkResponse &response,
                                   const std::uint32_t attempt_index,
                                   const StorageNodeClientConfig &config)
        {
            return IsRetriableStatus(response.status) &&
                   attempt_index < config.max_write_retries;
        }
    }

    StorageNodeClient::StorageNodeClient(
        std::unique_ptr<raft::StorageNodeService::StubInterface> stub,
        StorageNodeClientConfig config)
        : stub_(std::move(stub))
        , config_(config)
    {
        if (stub_ == nullptr)
        {
            throw std::invalid_argument("StorageNodeClient requires a non-null stub");
        }
    }

    StorageNodeClient::StorageNodeClient(std::shared_ptr<grpc::Channel> channel,
                                         StorageNodeClientConfig config)
        : StorageNodeClient(raft::StorageNodeService::NewStub(std::move(channel)),
                            config)
    {
    }

    WriteChunkResponse StorageNodeClient::WriteChunk(
        const WriteChunkRequest &request,
        StorageNodeClientWriteChunkOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            options.context.timeout_ms == 0
                ? std::chrono::system_clock::time_point::max()
                : start_time + std::chrono::milliseconds(options.context.timeout_ms);

        for (std::uint32_t attempt_index = 0;; ++attempt_index)
        {
            if (options.context.timeout_ms != 0 &&
                std::chrono::system_clock::now() >= absolute_deadline)
            {
                WriteChunkResponse response;
                response.status = StorageNodeStatusCode::kTimeout;
                response.error_detail = "WriteChunk client-side deadline expired";
                return response;
            }

            grpc::ClientContext context;
            if (options.context.timeout_ms != 0)
            {
                context.set_deadline(absolute_deadline);
            }

            raft::WriteChunkRequest proto_request;
            FillProtoWriteRequest(request, options, &proto_request);

            raft::WriteChunkResponse proto_response;
            const grpc::Status grpc_status =
                stub_->WriteChunk(&context, proto_request, &proto_response);

            WriteChunkResponse response;
            if (grpc_status.ok())
            {
                response = TranslateProtoWriteResponse(request, proto_response);
            }
            else
            {
                response = MakeGrpcFailureResponse(grpc_status);
            }

            if (!ShouldRetryWriteChunk(response, attempt_index, config_))
            {
                return response;
            }
        }
    }

    const StorageNodeClientConfig &StorageNodeClient::config() const
    {
        return config_;
    }
}
