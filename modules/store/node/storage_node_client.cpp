#include "store/node/storage_node_client.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace storedemo
{
    namespace
    {
        storage::WriteChunkDurability ToProtoDurability(
            const StorageNodeWriteDurability durability)
        {
            switch (durability)
            {
            case StorageNodeWriteDurability::kUnspecified:
                return storage::WRITE_CHUNK_DURABILITY_UNSPECIFIED;
            case StorageNodeWriteDurability::kPublish:
            default:
                return storage::WRITE_CHUNK_DURABILITY_PUBLISH;
            }
        }

        StorageNodeStatusCode FromProtoStatusCode(const storage::StorageNodeStatusCode code)
        {
            switch (code)
            {
            case storage::STORAGE_NODE_STATUS_CODE_OK:
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS:
                return StorageNodeStatusCode::kAlreadyExists;
            case storage::STORAGE_NODE_STATUS_CODE_NOT_FOUND:
                return StorageNodeStatusCode::kNotFound;
            case storage::STORAGE_NODE_STATUS_CODE_CONFLICT:
                return StorageNodeStatusCode::kConflict;
            case storage::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH:
                return StorageNodeStatusCode::kChecksumMismatch;
            case storage::STORAGE_NODE_STATUS_CODE_CORRUPTED:
                return StorageNodeStatusCode::kCorrupted;
            case storage::STORAGE_NODE_STATUS_CODE_DISK_FULL:
                return StorageNodeStatusCode::kDiskFull;
            case storage::STORAGE_NODE_STATUS_CODE_PERMISSION_DENIED:
                return StorageNodeStatusCode::kPermissionDenied;
            case storage::STORAGE_NODE_STATUS_CODE_IO_ERROR:
                return StorageNodeStatusCode::kIoError;
            case storage::STORAGE_NODE_STATUS_CODE_TIMEOUT:
                return StorageNodeStatusCode::kTimeout;
            case storage::STORAGE_NODE_STATUS_CODE_CANCELLED:
                return StorageNodeStatusCode::kCancelled;
            case storage::STORAGE_NODE_STATUS_CODE_OVERLOADED:
                return StorageNodeStatusCode::kOverloaded;
            case storage::STORAGE_NODE_STATUS_CODE_NODE_UNAVAILABLE:
                return StorageNodeStatusCode::kNodeUnavailable;
            case storage::STORAGE_NODE_STATUS_CODE_UNSUPPORTED:
                return StorageNodeStatusCode::kUnsupported;
            case storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT:
                return StorageNodeStatusCode::kInvalidArgument;
            case storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED:
            default:
                return StorageNodeStatusCode::kIoError;
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
                    *error_detail = "WriteChunk response checksum algorithm is not supported";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
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

        StorageNodeStatusCode FillChecksumFromProto(
            const storage::StorageChunkChecksum &proto_checksum,
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

        StorageNodeStatusCode FromProtoChunkState(const storage::StorageChunkState state)
        {
            switch (state)
            {
            case storage::STORAGE_CHUNK_STATE_UNSPECIFIED:
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_CHUNK_STATE_STAGING:
            case storage::STORAGE_CHUNK_STATE_LIVE:
            case storage::STORAGE_CHUNK_STATE_DELETING:
            case storage::STORAGE_CHUNK_STATE_DELETED:
            case storage::STORAGE_CHUNK_STATE_QUARANTINED:
            case storage::STORAGE_CHUNK_STATE_CORRUPTED:
            case storage::STORAGE_CHUNK_STATE_MISSING:
                return StorageNodeStatusCode::kOk;
            default:
                return StorageNodeStatusCode::kInvalidArgument;
            }
        }

        ChunkState ToStoreChunkState(const storage::StorageChunkState state)
        {
            switch (state)
            {
            case storage::STORAGE_CHUNK_STATE_STAGING:
                return ChunkState::kStaging;
            case storage::STORAGE_CHUNK_STATE_LIVE:
                return ChunkState::kLive;
            case storage::STORAGE_CHUNK_STATE_DELETING:
                return ChunkState::kDeleting;
            case storage::STORAGE_CHUNK_STATE_DELETED:
                return ChunkState::kDeleted;
            case storage::STORAGE_CHUNK_STATE_QUARANTINED:
                return ChunkState::kQuarantined;
            case storage::STORAGE_CHUNK_STATE_CORRUPTED:
                return ChunkState::kCorrupted;
            case storage::STORAGE_CHUNK_STATE_MISSING:
            case storage::STORAGE_CHUNK_STATE_UNSPECIFIED:
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

        std::chrono::system_clock::time_point ResolveAbsoluteDeadline(
            const StorageTaskContext &context,
            const std::chrono::system_clock::time_point start_time)
        {
            return context.timeout_ms == 0
                       ? std::chrono::system_clock::time_point::max()
                       : start_time + std::chrono::milliseconds(context.timeout_ms);
        }

        bool HasDeadlineExpired(const StorageTaskContext &context,
                                const std::chrono::system_clock::time_point absolute_deadline)
        {
            return context.timeout_ms != 0 &&
                   std::chrono::system_clock::now() >= absolute_deadline;
        }

        void ApplyDeadlineToContext(const StorageTaskContext &context,
                                    const std::chrono::system_clock::time_point absolute_deadline,
                                    grpc::ClientContext *grpc_context)
        {
            if (grpc_context == nullptr || context.timeout_ms == 0)
            {
                return;
            }

            grpc_context->set_deadline(absolute_deadline);
        }

        void FillProtoWriteRequest(const WriteChunkRequest &request,
                                   const StorageNodeClientWriteChunkOptions &options,
                                   storage::WriteChunkRequest *proto_request)
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

        void FillProtoReadRequest(const ReadChunkRequest &request,
                                  const StorageNodeClientReadChunkOptions &options,
                                  storage::ReadChunkRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            proto_request->set_chunk_id(request.chunk_id);
            if (request.range.has_value())
            {
                proto_request->set_offset(request.range->offset);
                proto_request->set_length(request.range->length);
            }
            FillProtoChecksum(request.expected_checksum,
                              proto_request->mutable_expected_checksum());
            proto_request->set_timeout_ms(options.context.timeout_ms);
            proto_request->set_best_effort_cancel(options.context.best_effort_cancel);
            proto_request->set_verify_checksum(request.verify_checksum);
        }

        StorageNodeStatusCode ResolveResponseIdentity(
            const WriteChunkRequest &request,
            const storage::WriteChunkResponse &proto_response,
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
            const storage::WriteChunkResponse &proto_response)
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
                    storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED &&
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

        StorageNodeStatusCode ResolveReadResponseIdentity(
            const ReadChunkRequest &request,
            const storage::ReadChunkResponse &proto_response,
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

            ChunkIdentity identity;
            identity.chunk_id = request.chunk_id;
            identity.offset = proto_response.offset();

            const std::string candidate_chunk_id =
                !proto_response.chunk_id().empty()
                    ? proto_response.chunk_id()
                    : proto_response.summary().chunk_id();
            if (candidate_chunk_id.empty())
            {
                if (request.chunk_id.empty())
                {
                    *out_identity = std::move(identity);
                    return StorageNodeStatusCode::kOk;
                }

                ChunkIdentity parsed_identity;
                const auto parse_status =
                    ParseChunkId(request.chunk_id, &parsed_identity, error_detail);
                if (parse_status == StorageNodeStatusCode::kOk)
                {
                    parsed_identity.offset = proto_response.offset();
                    identity = std::move(parsed_identity);
                }

                *out_identity = std::move(identity);
                return StorageNodeStatusCode::kOk;
            }

            ChunkIdentity parsed_identity;
            const auto parse_status =
                ParseChunkId(candidate_chunk_id, &parsed_identity, error_detail);
            if (parse_status != StorageNodeStatusCode::kOk)
            {
                return parse_status;
            }

            parsed_identity.offset = proto_response.offset();
            *out_identity = std::move(parsed_identity);
            return StorageNodeStatusCode::kOk;
        }

        ReadChunkResponse TranslateProtoReadResponse(
            const ReadChunkRequest &request,
            const storage::ReadChunkResponse &proto_response)
        {
            ReadChunkResponse response;
            response.status = FromProtoStatusCode(proto_response.summary().code());
            response.error_detail = proto_response.summary().message();
            response.retry_after_ms = proto_response.summary().retry_after_ms();
            response.payload = proto_response.payload();

            response.metadata.node_id = proto_response.summary().node_id();
            response.metadata.size = proto_response.size();
            response.metadata.state = ToStoreChunkState(proto_response.state());
            response.metadata.last_error = response.status;

            if (response.status == StorageNodeStatusCode::kIoError &&
                proto_response.summary().code() ==
                    storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED &&
                response.error_detail.empty())
            {
                response.error_detail = "ReadChunk response status is unspecified";
            }

            std::string error_detail;
            const auto identity_status = ResolveReadResponseIdentity(request,
                                                                     proto_response,
                                                                     &response.metadata.identity,
                                                                     &error_detail);
            if (identity_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid ReadChunk response chunk identity: " +
                                        error_detail;
                response.payload.clear();
                return response;
            }

            ChunkChecksum checksum;
            const auto checksum_status = FillChecksumFromProto(proto_response.checksum(),
                                                               &checksum,
                                                               &error_detail);
            if (checksum_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid ReadChunk response checksum: " +
                                        error_detail;
                response.payload.clear();
                return response;
            }

            const auto state_status = FromProtoChunkState(proto_response.state());
            if (state_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid ReadChunk response chunk state";
                response.payload.clear();
                return response;
            }

            response.metadata.checksum = checksum;
            response.actual_checksum = checksum;
            response.verified =
                checksum.IsSet() &&
                (response.status == StorageNodeStatusCode::kOk ||
                 response.status == StorageNodeStatusCode::kChecksumMismatch);

            if (response.status == StorageNodeStatusCode::kOk)
            {
                if (!proto_response.complete())
                {
                    response.status = StorageNodeStatusCode::kIoError;
                    response.error_detail =
                        "invalid ReadChunk response: successful response is incomplete";
                    response.payload.clear();
                    response.verified = false;
                    return response;
                }

                if (!request.range.has_value() && !proto_response.full_read())
                {
                    response.status = StorageNodeStatusCode::kIoError;
                    response.error_detail =
                        "invalid ReadChunk response: full read request returned partial response";
                    response.payload.clear();
                    response.verified = false;
                    return response;
                }

                if (request.range.has_value() && proto_response.full_read())
                {
                    response.status = StorageNodeStatusCode::kIoError;
                    response.error_detail =
                        "invalid ReadChunk response: range read must not report full_read";
                    response.payload.clear();
                    response.verified = false;
                    return response;
                }
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

        ReadChunkResponse MakeGrpcReadFailureResponse(const grpc::Status &status)
        {
            ReadChunkResponse response;
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
        std::unique_ptr<storage::StorageNodeService::StubInterface> stub,
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
        : StorageNodeClient(storage::StorageNodeService::NewStub(std::move(channel)),
                            config)
    {
    }

    WriteChunkResponse StorageNodeClient::WriteChunk(
        const WriteChunkRequest &request,
        StorageNodeClientWriteChunkOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        for (std::uint32_t attempt_index = 0;; ++attempt_index)
        {
            if (HasDeadlineExpired(options.context, absolute_deadline))
            {
                WriteChunkResponse response;
                response.status = StorageNodeStatusCode::kTimeout;
                response.error_detail = "WriteChunk client-side deadline expired";
                return response;
            }

            grpc::ClientContext context;
            ApplyDeadlineToContext(options.context, absolute_deadline, &context);

            storage::WriteChunkRequest proto_request;
            FillProtoWriteRequest(request, options, &proto_request);

            storage::WriteChunkResponse proto_response;
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

    ReadChunkResponse StorageNodeClient::ReadChunk(
        const ReadChunkRequest &request,
        StorageNodeClientReadChunkOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        if (HasDeadlineExpired(options.context, absolute_deadline))
        {
            ReadChunkResponse response;
            response.status = StorageNodeStatusCode::kTimeout;
            response.error_detail = "ReadChunk client-side deadline expired";
            return response;
        }

        grpc::ClientContext context;
        ApplyDeadlineToContext(options.context, absolute_deadline, &context);

        storage::ReadChunkRequest proto_request;
        FillProtoReadRequest(request, options, &proto_request);

        storage::ReadChunkResponse proto_response;
        const grpc::Status grpc_status =
            stub_->ReadChunk(&context, proto_request, &proto_response);

        if (!grpc_status.ok())
        {
            return MakeGrpcReadFailureResponse(grpc_status);
        }

        return TranslateProtoReadResponse(request, proto_response);
    }

    const StorageNodeClientConfig &StorageNodeClient::config() const
    {
        return config_;
    }
}
