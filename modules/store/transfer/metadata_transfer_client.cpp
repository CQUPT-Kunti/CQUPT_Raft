#include "store/transfer/metadata_transfer_client.h"

#include <chrono>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace storedemo
{
    namespace
    {
        // NOT_LEADER 只做一次基于 leader hint 的有限重试，避免无界阻塞。
        constexpr int kMaxMetadataNotLeaderAttempts = 2;

        bool IsRetryableGrpcFailure(const grpc::StatusCode code)
        {
            return code == grpc::StatusCode::DEADLINE_EXCEEDED ||
                   code == grpc::StatusCode::UNAVAILABLE ||
                   code == grpc::StatusCode::RESOURCE_EXHAUSTED;
        }

        bool IsRetryableMetadataStatus(const MetadataTransferStatusCode status)
        {
            return status == MetadataTransferStatusCode::kNotLeader ||
                   status == MetadataTransferStatusCode::kQuorumUnavailable ||
                   status == MetadataTransferStatusCode::kTimeout ||
                   status == MetadataTransferStatusCode::kOverloaded ||
                   status == MetadataTransferStatusCode::kServiceUnavailable;
        }

        bool LooksLikeSha256Hex(const std::string_view value)
        {
            if (value.size() != kSha256DigestHexChars)
            {
                return false;
            }

            for (const unsigned char ch : value)
            {
                if (std::isxdigit(ch) == 0)
                {
                    return false;
                }
            }
            return true;
        }

        bool MessageImpliesQuorumUnavailable(const std::string_view message)
        {
            return message.find("majority") != std::string_view::npos ||
                   message.find("quorum") != std::string_view::npos;
        }

        std::string AppendMessageDetail(const std::string_view message,
                                        const std::string_view detail)
        {
            if (detail.empty())
            {
                return std::string(message);
            }
            if (message.empty())
            {
                return std::string(detail);
            }
            return std::string(message) + " [" + std::string(detail) + "]";
        }

        MetadataTransferStatusCode FromProtoStatusCode(
            const raft::MetadataStatusCode code,
            const std::string_view message)
        {
            switch (code)
            {
            case raft::METADATA_STATUS_CODE_OK:
                return MetadataTransferStatusCode::kOk;
            case raft::METADATA_STATUS_CODE_NOT_LEADER:
                return MetadataTransferStatusCode::kNotLeader;
            case raft::METADATA_STATUS_CODE_INVALID_ARGUMENT:
                return MetadataTransferStatusCode::kInvalidArgument;
            case raft::METADATA_STATUS_CODE_NOT_FOUND:
                return MetadataTransferStatusCode::kNotFound;
            case raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY:
                return MetadataTransferStatusCode::kIdempotentReplay;
            case raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT:
                return MetadataTransferStatusCode::kIdempotencyConflict;
            case raft::METADATA_STATUS_CODE_STATE_CONFLICT:
                return MetadataTransferStatusCode::kStateConflict;
            case raft::METADATA_STATUS_CODE_TIMEOUT:
                return MetadataTransferStatusCode::kTimeout;
            case raft::METADATA_STATUS_CODE_OVERLOADED:
                return MetadataTransferStatusCode::kOverloaded;
            case raft::METADATA_STATUS_CODE_SERVICE_UNAVAILABLE:
                return MetadataTransferStatusCode::kServiceUnavailable;
            case raft::METADATA_STATUS_CODE_INTERNAL_ERROR:
                if (MessageImpliesQuorumUnavailable(message))
                {
                    return MetadataTransferStatusCode::kQuorumUnavailable;
                }
                return MetadataTransferStatusCode::kInternalError;
            case raft::METADATA_STATUS_CODE_UNSPECIFIED:
            default:
                return MetadataTransferStatusCode::kInternalError;
            }
        }

        MetadataTransferStatusCode MapGrpcStatusCode(
            const grpc::StatusCode code)
        {
            switch (code)
            {
            case grpc::StatusCode::DEADLINE_EXCEEDED:
                return MetadataTransferStatusCode::kTimeout;
            case grpc::StatusCode::RESOURCE_EXHAUSTED:
                return MetadataTransferStatusCode::kOverloaded;
            case grpc::StatusCode::UNAVAILABLE:
                return MetadataTransferStatusCode::kServiceUnavailable;
            case grpc::StatusCode::UNIMPLEMENTED:
                return MetadataTransferStatusCode::kUnsupported;
            default:
                return MetadataTransferStatusCode::kInternalError;
            }
        }

        raftdemo::ObjectState FromProtoObjectState(
            const raft::MetadataObjectState state)
        {
            switch (state)
            {
            case raft::METADATA_OBJECT_STATE_COMMITTED:
                return raftdemo::ObjectState::COMMITTED;
            case raft::METADATA_OBJECT_STATE_DELETED:
                return raftdemo::ObjectState::DELETED;
            case raft::METADATA_OBJECT_STATE_PENDING:
            case raft::METADATA_OBJECT_STATE_UNSPECIFIED:
            default:
                return raftdemo::ObjectState::PENDING;
            }
        }

        std::chrono::milliseconds ResolveTimeout(
            const std::chrono::milliseconds default_timeout,
            const MetadataTransferClientCallOptions &options)
        {
            if (options.timeout.has_value())
            {
                return *options.timeout;
            }
            return default_timeout;
        }

        bool ResolveWaitForReady(
            const bool default_wait_for_ready,
            const MetadataTransferClientCallOptions &options)
        {
            if (options.wait_for_ready.has_value())
            {
                return *options.wait_for_ready;
            }
            return default_wait_for_ready;
        }

        void ApplyRpcOptions(const std::chrono::milliseconds timeout,
                             const bool wait_for_ready,
                             grpc::ClientContext *context)
        {
            if (context == nullptr)
            {
                return;
            }

            if (timeout.count() > 0)
            {
                context->set_deadline(std::chrono::system_clock::now() + timeout);
            }
            context->set_wait_for_ready(wait_for_ready);
        }

        MetadataTransferClientCallDiagnostics MakeRpcDiagnostics(
            const std::string &request_id,
            const std::string &bucket,
            const std::string &object_key,
            const std::string &object_id,
            const std::string &target_endpoint,
            const std::chrono::milliseconds effective_timeout,
            const bool wait_for_ready)
        {
            MetadataTransferClientCallDiagnostics diagnostics;
            diagnostics.request_id = request_id;
            diagnostics.bucket = bucket;
            diagnostics.object_key = object_key;
            diagnostics.object_id = object_id;
            diagnostics.target_endpoint = target_endpoint;
            diagnostics.effective_timeout = effective_timeout;
            diagnostics.wait_for_ready = wait_for_ready;
            return diagnostics;
        }

        void FillRpcFailureDiagnostics(const grpc::Status &grpc_status,
                                       MetadataTransferClientCallDiagnostics *diagnostics)
        {
            if (diagnostics == nullptr)
            {
                return;
            }

            diagnostics->grpc_status_code = grpc_status.error_code();
            diagnostics->grpc_error_message = grpc_status.error_message();
            diagnostics->grpc_error_details = grpc_status.error_details();
            diagnostics->retryable =
                IsRetryableGrpcFailure(grpc_status.error_code());
        }

        std::optional<MetadataTransferLeaderHint> FromProtoLeaderHint(
            const raft::MetadataResponseSummary &summary)
        {
            if (!summary.has_leader_hint())
            {
                return std::nullopt;
            }

            MetadataTransferLeaderHint leader_hint;
            leader_hint.leader_id = summary.leader_hint().leader_id();
            leader_hint.leader_address = summary.leader_hint().leader_address();
            return leader_hint;
        }

        void FillSummaryFromProto(const raft::MetadataResponseSummary &proto_summary,
                                  MetadataTransferSummary *summary)
        {
            if (summary == nullptr)
            {
                return;
            }

            summary->status =
                FromProtoStatusCode(proto_summary.code(), proto_summary.message());
            summary->message = proto_summary.message();
            summary->request_id = proto_summary.request_id();
            summary->bucket = proto_summary.bucket();
            summary->object_key = proto_summary.object_key();
            summary->object_id = proto_summary.object_id();
            summary->object_state = FromProtoObjectState(proto_summary.state());
            summary->term = proto_summary.term();
            summary->log_index = proto_summary.log_index();
            summary->leader_hint = FromProtoLeaderHint(proto_summary);
        }

        void FillTransportFailureSummary(
            const MetadataTransferClientCallDiagnostics &rpc,
            const std::string &message,
            MetadataTransferSummary *summary)
        {
            if (summary == nullptr)
            {
                return;
            }

            summary->status = MapGrpcStatusCode(rpc.grpc_status_code);
            summary->message = message;
            summary->request_id = rpc.request_id;
            summary->bucket = rpc.bucket;
            summary->object_key = rpc.object_key;
            summary->object_id = rpc.object_id;
        }

        void FillLocalValidationFailureSummary(const std::string &request_id,
                                               const std::string &bucket,
                                               const std::string &object_key,
                                               const std::string &object_id,
                                               const std::string &message,
                                               MetadataTransferSummary *summary)
        {
            if (summary == nullptr)
            {
                return;
            }

            summary->status = MetadataTransferStatusCode::kInvalidArgument;
            summary->message = message;
            summary->request_id = request_id;
            summary->bucket = bucket;
            summary->object_key = object_key;
            summary->object_id = object_id;
        }

        MetadataTransferDiagnostic MakeDiagnostic(
            const MetadataTransferSummary &summary,
            const MetadataTransferClientCallDiagnostics &rpc)
        {
            MetadataTransferDiagnostic diagnostic;
            diagnostic.status = summary.status;
            diagnostic.message = summary.message;
            diagnostic.request_id = summary.request_id.empty() ? rpc.request_id
                                                               : summary.request_id;
            diagnostic.bucket = summary.bucket.empty() ? rpc.bucket : summary.bucket;
            diagnostic.object_key =
                summary.object_key.empty() ? rpc.object_key : summary.object_key;
            diagnostic.object_id =
                summary.object_id.empty() ? rpc.object_id : summary.object_id;
            diagnostic.endpoint = rpc.target_endpoint;
            diagnostic.retryable =
                rpc.retryable || IsRetryableMetadataStatus(summary.status);
            diagnostic.leader_hint = summary.leader_hint;
            return diagnostic;
        }

        MetadataTransferDiagnostic MakeCommittedManifestBoundaryDiagnostic(
            const MetadataTransferSummary &summary,
            const MetadataTransferClientCallDiagnostics &rpc)
        {
            MetadataTransferDiagnostic diagnostic =
                MakeDiagnostic(summary, rpc);
            diagnostic.status = MetadataTransferStatusCode::kOk;
            diagnostic.retryable = false;
            diagnostic.message =
                "committed manifest is served from metadata committed state; "
                "dynamic StorageNode discovery only affects future placement "
                "and must not rewrite or rebalance existing manifest replica facts";
            return diagnostic;
        }

        TransferObjectChecksumFacts InferObjectChecksumFacts(
            const std::uint64_t size,
            const std::string &etag)
        {
            TransferObjectChecksumFacts object_checksum;
            object_checksum.size = size;
            object_checksum.etag = etag;
            if (LooksLikeSha256Hex(etag))
            {
                object_checksum.checksum.algorithm =
                    ChunkChecksumAlgorithm::kSha256;
                object_checksum.checksum.value = etag;
                object_checksum.checksum.size_bytes = size;
                object_checksum.checksum.computed_at = 0;
            }
            return object_checksum;
        }

        ChunkChecksum InferChunkChecksum(const std::uint64_t size,
                                         const std::string &checksum)
        {
            ChunkChecksum chunk_checksum;
            if (!LooksLikeSha256Hex(checksum))
            {
                return chunk_checksum;
            }

            chunk_checksum.algorithm = ChunkChecksumAlgorithm::kSha256;
            chunk_checksum.value = checksum;
            chunk_checksum.size_bytes = size;
            chunk_checksum.computed_at = 0;
            return chunk_checksum;
        }

        TransferObjectHead BuildTransferObjectHead(const raft::ObjectRecord &record)
        {
            TransferObjectHead head;
            head.bucket = record.bucket();
            head.object_key = record.object_key();
            head.object_id = record.object_id();
            head.version = record.version();
            head.object_checksum =
                InferObjectChecksumFacts(record.size(), record.etag());
            head.state = FromProtoObjectState(record.state());
            head.created_at_unix_ms = record.create_time();
            if (record.commit_time() != 0)
            {
                head.committed_at_unix_ms = record.commit_time();
            }
            return head;
        }

        TransferCommittedChunk BuildTransferCommittedChunk(
            const raft::ObjectRecord &object,
            const raft::ChunkRef &chunk,
            const std::uint32_t fallback_chunk_index)
        {
            TransferCommittedChunk committed_chunk;
            committed_chunk.identity.chunk_id = chunk.chunk_id();
            committed_chunk.identity.object_id = object.object_id();
            committed_chunk.identity.version = object.version();
            committed_chunk.identity.chunk_index = fallback_chunk_index;
            committed_chunk.identity.offset = chunk.offset();
            ParseChunkId(chunk.chunk_id(),
                         &committed_chunk.identity,
                         nullptr);
            committed_chunk.identity.offset = chunk.offset();
            committed_chunk.size = chunk.size();
            committed_chunk.checksum =
                InferChunkChecksum(chunk.size(), chunk.checksum());
            committed_chunk.replica_nodes.assign(chunk.replica_nodes().begin(),
                                                 chunk.replica_nodes().end());
            return committed_chunk;
        }

        TransferCommittedManifest BuildCommittedManifest(
            const raft::ObjectRecord &record)
        {
            TransferCommittedManifest manifest;
            manifest.bucket = record.bucket();
            manifest.object_key = record.object_key();
            manifest.object_id = record.object_id();
            manifest.version = record.version();
            manifest.object_checksum =
                InferObjectChecksumFacts(record.size(), record.etag());
            manifest.committed_at_unix_ms = record.commit_time();

            manifest.chunks.reserve(
                static_cast<std::size_t>(record.chunks_size()));
            for (int index = 0; index < record.chunks_size(); ++index)
            {
                manifest.chunks.push_back(BuildTransferCommittedChunk(
                    record,
                    record.chunks(index),
                    static_cast<std::uint32_t>(index)));
            }
            return manifest;
        }

        bool ValidateCreateWritePlanRequest(
            const MetadataTransferCreateWritePlanRequest &request,
            std::string *error_detail)
        {
            if (request.request_id.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "request_id must not be empty";
                }
                return false;
            }
            if (request.bucket.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "bucket must not be empty";
                }
                return false;
            }
            if (request.object_key.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "object_key must not be empty";
                }
                return false;
            }
            if (request.object_id.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "object_id must not be empty";
                }
                return false;
            }
            if (request.chunk_size == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_size must be greater than zero";
                }
                return false;
            }
            if (request.desired_replica_count == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "desired_replica_count must be greater than zero";
                }
                return false;
            }
            if (request.minimum_successful_writes == 0 ||
                request.minimum_successful_writes >
                    request.desired_replica_count)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "minimum_successful_writes must be in [1, desired_replica_count]";
                }
                return false;
            }
            return true;
        }

        bool ValidateCommitObjectRequest(
            const MetadataTransferCommitObjectRequest &request,
            std::string *error_detail)
        {
            if (request.request_id.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "request_id must not be empty";
                }
                return false;
            }
            if (request.bucket.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "bucket must not be empty";
                }
                return false;
            }
            if (request.object_key.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "object_key must not be empty";
                }
                return false;
            }
            if (request.object_id.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "object_id must not be empty";
                }
                return false;
            }
            if (request.object_checksum.size != 0 &&
                request.committed_chunks.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "committed_chunks must not be empty for non-empty object";
                }
                return false;
            }
            return true;
        }

        bool ValidateHeadLikeRequest(const std::string &bucket,
                                     const std::string &object_key,
                                     std::string *error_detail)
        {
            if (bucket.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "bucket must not be empty";
                }
                return false;
            }
            if (object_key.empty())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "object_key must not be empty";
                }
                return false;
            }
            return true;
        }

        std::unique_ptr<raft::MetadataService::StubInterface> MakeMetadataStub(
            std::shared_ptr<grpc::Channel> channel)
        {
            if (channel == nullptr)
            {
                throw std::invalid_argument(
                    "MetadataTransferClient requires a non-null gRPC channel");
            }
            return raft::MetadataService::NewStub(std::move(channel));
        }

        std::shared_ptr<grpc::ChannelCredentials> ResolveCredentials(
            const MetadataTransferClientConfig &config)
        {
            if (config.channel_credentials != nullptr)
            {
                return config.channel_credentials;
            }
            return grpc::InsecureChannelCredentials();
        }

        std::unique_ptr<raft::MetadataService::StubInterface>
        MakeMetadataStubForEndpoint(const std::string &target_endpoint,
                                    const MetadataTransferClientConfig &config)
        {
            auto channel = grpc::CreateChannel(target_endpoint,
                                               ResolveCredentials(config));
            return MakeMetadataStub(std::move(channel));
        }

        std::optional<std::string> ResolveRedirectedMetadataEndpoint(
            const MetadataTransferSummary &summary,
            const std::string_view current_endpoint,
            std::string *reason)
        {
            // leader hint 只是候选 endpoint；缺失、为空或指回当前地址都视为不可安全重试。
            if (!summary.leader_hint.has_value())
            {
                if (reason != nullptr)
                {
                    *reason =
                        "MetadataService returned NOT_LEADER without a usable leader hint; metadata_transfer_client will not guess a new leader";
                }
                return std::nullopt;
            }

            const std::string_view hinted_endpoint =
                summary.leader_hint->leader_address;
            if (hinted_endpoint.empty())
            {
                if (reason != nullptr)
                {
                    *reason =
                        "MetadataService returned NOT_LEADER but leader hint endpoint is empty; metadata_transfer_client will stop without discovery fallback";
                }
                return std::nullopt;
            }
            if (hinted_endpoint == current_endpoint)
            {
                if (reason != nullptr)
                {
                    *reason =
                        "MetadataService returned NOT_LEADER but leader hint still points to the current endpoint; treating leader hint as stale";
                }
                return std::nullopt;
            }

            if (reason != nullptr)
            {
                reason->clear();
            }
            return std::string(hinted_endpoint);
        }
    }

    MetadataTransferClient::MetadataTransferClient(
        std::unique_ptr<raft::MetadataService::StubInterface> stub,
        std::string target_endpoint,
        MetadataTransferClientConfig config)
        : stub_(std::move(stub))
        , target_endpoint_(std::move(target_endpoint))
        , config_(config)
    {
        if (stub_ == nullptr)
        {
            throw std::invalid_argument(
                "MetadataTransferClient requires a non-null MetadataService stub");
        }
        if (target_endpoint_.empty())
        {
            throw std::invalid_argument(
                "MetadataTransferClient target_endpoint must not be empty");
        }
    }

    MetadataTransferClient::MetadataTransferClient(
        std::shared_ptr<grpc::Channel> channel,
        std::string target_endpoint,
        MetadataTransferClientConfig config)
        : MetadataTransferClient(MakeMetadataStub(std::move(channel)),
                                 std::move(target_endpoint),
                                 config)
    {
    }

    MetadataTransferClientCreateWritePlanCallResult
    MetadataTransferClient::CreateWritePlan(
        const MetadataTransferCreateWritePlanRequest &request,
        MetadataTransferClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.create_write_plan_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        MetadataTransferClientCreateWritePlanCallResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.bucket,
                                             request.object_key,
                                             request.object_id,
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        std::string validation_error;
        if (!ValidateCreateWritePlanRequest(request, &validation_error))
        {
            FillLocalValidationFailureSummary(request.request_id,
                                             request.bucket,
                                             request.object_key,
                                             request.object_id,
                                             validation_error,
                                             &call_result.result.summary);
            call_result.result.diagnostics.push_back(
                MakeDiagnostic(call_result.result.summary, call_result.rpc));
            return call_result;
        }

        raft::CreateObjectRequest proto_request;
        proto_request.set_request_id(request.request_id);
        proto_request.set_bucket(request.bucket);
        proto_request.set_object_key(request.object_key);
        proto_request.set_object_id(request.object_id);
        proto_request.set_version(0);
        proto_request.set_size(request.expected_object_checksum.size);
        proto_request.set_etag(request.expected_object_checksum.etag);
        proto_request.set_client_time_unix_ms(request.client_time_unix_ms);

        std::string current_endpoint = target_endpoint_;
        std::unique_ptr<raft::MetadataService::StubInterface> redirected_stub;

        for (int attempt = 0; attempt < kMaxMetadataNotLeaderAttempts; ++attempt)
        {
            call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                                 request.bucket,
                                                 request.object_key,
                                                 request.object_id,
                                                 current_endpoint,
                                                 effective_timeout,
                                                 wait_for_ready);

            grpc::ClientContext context;
            ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

            raft::CreateObjectResponse proto_response;
            auto *stub = redirected_stub != nullptr ? redirected_stub.get()
                                                    : stub_.get();
            const grpc::Status grpc_status =
                stub->CreateObject(&context, proto_request, &proto_response);
            if (!grpc_status.ok())
            {
                FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
                FillTransportFailureSummary(call_result.rpc,
                                            "CreateObject RPC failed: " +
                                                grpc_status.error_message(),
                                            &call_result.result.summary);
                call_result.result.diagnostics.push_back(
                    MakeDiagnostic(call_result.result.summary, call_result.rpc));
                return call_result;
            }

            FillSummaryFromProto(proto_response.summary(),
                                 &call_result.result.summary);
            call_result.result.idempotent =
                call_result.result.summary.status ==
                MetadataTransferStatusCode::kIdempotentReplay;

            if (call_result.result.summary.status ==
                MetadataTransferStatusCode::kNotLeader)
            {
                std::string retry_reason;
                const auto redirected_endpoint =
                    ResolveRedirectedMetadataEndpoint(call_result.result.summary,
                                                      current_endpoint,
                                                      &retry_reason);
                MetadataTransferDiagnostic diagnostic =
                    MakeDiagnostic(call_result.result.summary, call_result.rpc);
                if (redirected_endpoint.has_value() &&
                    attempt + 1 < kMaxMetadataNotLeaderAttempts)
                {
                    diagnostic.message = AppendMessageDetail(
                        diagnostic.message,
                        "metadata_transfer_client will retry against leader hint endpoint " +
                            *redirected_endpoint + " (attempt " +
                            std::to_string(attempt + 2) + "/" +
                            std::to_string(kMaxMetadataNotLeaderAttempts) + ")");
                    call_result.result.diagnostics.push_back(
                        std::move(diagnostic));
                    redirected_stub = MakeMetadataStubForEndpoint(
                        *redirected_endpoint,
                        config_);
                    current_endpoint = *redirected_endpoint;
                    continue;
                }

                if (redirected_endpoint.has_value())
                {
                    retry_reason = "MetadataService kept returning NOT_LEADER after " +
                                   std::to_string(kMaxMetadataNotLeaderAttempts) +
                                   " attempts; latest leader hint endpoint=" +
                                   *redirected_endpoint;
                }
                diagnostic.message = AppendMessageDetail(diagnostic.message,
                                                         retry_reason);
                call_result.result.summary.message = diagnostic.message;
                call_result.result.diagnostics.push_back(
                    std::move(diagnostic));
                return call_result;
            }

            call_result.result.created_pending = call_result.result.summary.ok();
            if (call_result.result.summary.ok())
            {
                TransferWritePlan write_plan;
                write_plan.request_id = request.request_id;
                write_plan.bucket = request.bucket;
                write_plan.object_key = request.object_key;
                write_plan.object_id = request.object_id;
                write_plan.version = proto_response.object().version();
                write_plan.chunk_size_bytes = request.chunk_size;
                write_plan.replica_count = request.desired_replica_count;
                write_plan.minimum_successful_writes =
                    request.minimum_successful_writes;
                write_plan.total_chunks = 0;
                write_plan.placement_epoch = 0;
                write_plan.object_checksum = request.expected_object_checksum;
                if (!proto_response.object().etag().empty())
                {
                    write_plan.object_checksum.etag =
                        proto_response.object().etag();
                }
                if (proto_response.object().size() != 0)
                {
                    write_plan.object_checksum.size =
                        proto_response.object().size();
                }
                if (!write_plan.object_checksum.checksum.IsSet() &&
                    LooksLikeSha256Hex(write_plan.object_checksum.etag))
                {
                    write_plan.object_checksum.checksum =
                        InferChunkChecksum(write_plan.object_checksum.size,
                                           write_plan.object_checksum.etag);
                }
                write_plan.created_at_unix_ms =
                    proto_response.object().create_time();
                write_plan.expires_at_unix_ms = 0;
                call_result.result.write_plan = std::move(write_plan);
            }

            if (redirected_stub != nullptr)
            {
                stub_ = std::move(redirected_stub);
                target_endpoint_ = current_endpoint;
            }

            if (!call_result.result.summary.ok() ||
                call_result.result.summary.leader_hint.has_value())
            {
                call_result.result.diagnostics.push_back(
                    MakeDiagnostic(call_result.result.summary, call_result.rpc));
            }
            return call_result;
        }

        return call_result;
    }

    MetadataTransferClientCommitObjectCallResult
    MetadataTransferClient::CommitObject(
        const MetadataTransferCommitObjectRequest &request,
        MetadataTransferClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.commit_object_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        MetadataTransferClientCommitObjectCallResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.bucket,
                                             request.object_key,
                                             request.object_id,
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        std::string validation_error;
        if (!ValidateCommitObjectRequest(request, &validation_error))
        {
            FillLocalValidationFailureSummary(request.request_id,
                                             request.bucket,
                                             request.object_key,
                                             request.object_id,
                                             validation_error,
                                             &call_result.result.summary);
            call_result.result.diagnostics.push_back(
                MakeDiagnostic(call_result.result.summary, call_result.rpc));
            return call_result;
        }

        raft::CommitObjectRequest proto_request;
        proto_request.set_request_id(request.request_id);
        proto_request.set_bucket(request.bucket);
        proto_request.set_object_key(request.object_key);
        proto_request.set_object_id(request.object_id);
        proto_request.set_version(request.version);
        proto_request.set_size(request.object_checksum.size);
        proto_request.set_etag(request.object_checksum.etag);
        proto_request.set_client_time_unix_ms(request.client_time_unix_ms);

        for (const auto &chunk : request.committed_chunks)
        {
            auto *proto_chunk = proto_request.add_chunks();
            proto_chunk->set_chunk_id(chunk.identity.chunk_id);
            proto_chunk->set_offset(chunk.identity.offset);
            proto_chunk->set_size(chunk.size);
            for (const auto &node_id : chunk.replica_nodes)
            {
                proto_chunk->add_replica_nodes(node_id);
            }
            proto_chunk->set_checksum(chunk.checksum.value);
        }

        std::string current_endpoint = target_endpoint_;
        std::unique_ptr<raft::MetadataService::StubInterface> redirected_stub;

        for (int attempt = 0; attempt < kMaxMetadataNotLeaderAttempts; ++attempt)
        {
            call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                                 request.bucket,
                                                 request.object_key,
                                                 request.object_id,
                                                 current_endpoint,
                                                 effective_timeout,
                                                 wait_for_ready);

            grpc::ClientContext context;
            ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

            raft::CommitObjectResponse proto_response;
            auto *stub = redirected_stub != nullptr ? redirected_stub.get()
                                                    : stub_.get();
            const grpc::Status grpc_status =
                stub->CommitObject(&context, proto_request, &proto_response);
            if (!grpc_status.ok())
            {
                FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
                FillTransportFailureSummary(call_result.rpc,
                                            "CommitObject RPC failed: " +
                                                grpc_status.error_message(),
                                            &call_result.result.summary);
                call_result.result.diagnostics.push_back(
                    MakeDiagnostic(call_result.result.summary, call_result.rpc));
                return call_result;
            }

            FillSummaryFromProto(proto_response.summary(),
                                 &call_result.result.summary);
            call_result.result.idempotent =
                call_result.result.summary.status ==
                MetadataTransferStatusCode::kIdempotentReplay;

            if (call_result.result.summary.status ==
                MetadataTransferStatusCode::kNotLeader)
            {
                std::string retry_reason;
                const auto redirected_endpoint =
                    ResolveRedirectedMetadataEndpoint(call_result.result.summary,
                                                      current_endpoint,
                                                      &retry_reason);
                MetadataTransferDiagnostic diagnostic =
                    MakeDiagnostic(call_result.result.summary, call_result.rpc);
                if (redirected_endpoint.has_value() &&
                    attempt + 1 < kMaxMetadataNotLeaderAttempts)
                {
                    diagnostic.message = AppendMessageDetail(
                        diagnostic.message,
                        "metadata_transfer_client will retry against leader hint endpoint " +
                            *redirected_endpoint + " (attempt " +
                            std::to_string(attempt + 2) + "/" +
                            std::to_string(kMaxMetadataNotLeaderAttempts) + ")");
                    call_result.result.diagnostics.push_back(
                        std::move(diagnostic));
                    redirected_stub = MakeMetadataStubForEndpoint(
                        *redirected_endpoint,
                        config_);
                    current_endpoint = *redirected_endpoint;
                    continue;
                }

                if (redirected_endpoint.has_value())
                {
                    retry_reason = "MetadataService kept returning NOT_LEADER after " +
                                   std::to_string(kMaxMetadataNotLeaderAttempts) +
                                   " attempts; latest leader hint endpoint=" +
                                   *redirected_endpoint;
                }
                diagnostic.message = AppendMessageDetail(diagnostic.message,
                                                         retry_reason);
                call_result.result.summary.message = diagnostic.message;
                call_result.result.diagnostics.push_back(
                    std::move(diagnostic));
                return call_result;
            }

            if (call_result.result.summary.ok() &&
                proto_response.object().state() ==
                    raft::METADATA_OBJECT_STATE_COMMITTED)
            {
                call_result.result.committed_manifest =
                    BuildCommittedManifest(proto_response.object());
                call_result.result.committed = true;
                call_result.result.visible_for_read = true;
            }

            if (redirected_stub != nullptr)
            {
                stub_ = std::move(redirected_stub);
                target_endpoint_ = current_endpoint;
            }

            if (!call_result.result.summary.ok() ||
                call_result.result.summary.leader_hint.has_value())
            {
                call_result.result.diagnostics.push_back(
                    MakeDiagnostic(call_result.result.summary, call_result.rpc));
            }
            return call_result;
        }

        return call_result;
    }

    MetadataTransferClientHeadObjectCallResult
    MetadataTransferClient::HeadObject(
        const MetadataTransferHeadObjectRequest &request,
        MetadataTransferClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.head_object_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        MetadataTransferClientHeadObjectCallResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.bucket,
                                             request.object_key,
                                             request.object_id,
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        std::string validation_error;
        if (!ValidateHeadLikeRequest(request.bucket,
                                     request.object_key,
                                     &validation_error))
        {
            FillLocalValidationFailureSummary(request.request_id,
                                             request.bucket,
                                             request.object_key,
                                             request.object_id,
                                             validation_error,
                                             &call_result.result.summary);
            call_result.result.diagnostics.push_back(
                MakeDiagnostic(call_result.result.summary, call_result.rpc));
            return call_result;
        }

        raft::HeadObjectRequest proto_request;
        proto_request.set_bucket(request.bucket);
        proto_request.set_object_key(request.object_key);
        if (!request.object_id.empty())
        {
            proto_request.set_object_id(request.object_id);
        }
        if (request.version.has_value())
        {
            proto_request.set_version(*request.version);
        }

        std::string current_endpoint = target_endpoint_;
        std::unique_ptr<raft::MetadataService::StubInterface> redirected_stub;

        for (int attempt = 0; attempt < kMaxMetadataNotLeaderAttempts; ++attempt)
        {
            call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                                 request.bucket,
                                                 request.object_key,
                                                 request.object_id,
                                                 current_endpoint,
                                                 effective_timeout,
                                                 wait_for_ready);

            grpc::ClientContext context;
            ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

            raft::HeadObjectResponse proto_response;
            auto *stub = redirected_stub != nullptr ? redirected_stub.get()
                                                    : stub_.get();
            const grpc::Status grpc_status =
                stub->HeadObject(&context, proto_request, &proto_response);
            if (!grpc_status.ok())
            {
                FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
                FillTransportFailureSummary(call_result.rpc,
                                            "HeadObject RPC failed: " +
                                                grpc_status.error_message(),
                                            &call_result.result.summary);
                call_result.result.diagnostics.push_back(
                    MakeDiagnostic(call_result.result.summary, call_result.rpc));
                return call_result;
            }

            FillSummaryFromProto(proto_response.summary(),
                                 &call_result.result.summary);
            if (call_result.result.summary.status ==
                MetadataTransferStatusCode::kNotLeader)
            {
                std::string retry_reason;
                const auto redirected_endpoint =
                    ResolveRedirectedMetadataEndpoint(call_result.result.summary,
                                                      current_endpoint,
                                                      &retry_reason);
                MetadataTransferDiagnostic diagnostic =
                    MakeDiagnostic(call_result.result.summary, call_result.rpc);
                if (redirected_endpoint.has_value() &&
                    attempt + 1 < kMaxMetadataNotLeaderAttempts)
                {
                    diagnostic.message = AppendMessageDetail(
                        diagnostic.message,
                        "metadata_transfer_client will retry against leader hint endpoint " +
                            *redirected_endpoint + " (attempt " +
                            std::to_string(attempt + 2) + "/" +
                            std::to_string(kMaxMetadataNotLeaderAttempts) + ")");
                    call_result.result.diagnostics.push_back(
                        std::move(diagnostic));
                    redirected_stub = MakeMetadataStubForEndpoint(
                        *redirected_endpoint,
                        config_);
                    current_endpoint = *redirected_endpoint;
                    continue;
                }

                if (redirected_endpoint.has_value())
                {
                    retry_reason = "MetadataService kept returning NOT_LEADER after " +
                                   std::to_string(kMaxMetadataNotLeaderAttempts) +
                                   " attempts; latest leader hint endpoint=" +
                                   *redirected_endpoint;
                }
                diagnostic.message = AppendMessageDetail(diagnostic.message,
                                                         retry_reason);
                call_result.result.summary.message = diagnostic.message;
                call_result.result.diagnostics.push_back(
                    std::move(diagnostic));
                return call_result;
            }

            call_result.result.found = proto_response.found();
            if (proto_response.found())
            {
                call_result.result.object =
                    BuildTransferObjectHead(proto_response.object());
                call_result.result.visible_for_read =
                    call_result.result.object->state ==
                    raftdemo::ObjectState::COMMITTED;
            }

            if (redirected_stub != nullptr)
            {
                stub_ = std::move(redirected_stub);
                target_endpoint_ = current_endpoint;
            }

            if (!call_result.result.summary.ok() ||
                call_result.result.summary.leader_hint.has_value())
            {
                call_result.result.diagnostics.push_back(
                    MakeDiagnostic(call_result.result.summary, call_result.rpc));
            }

            (void)request.require_committed_visible;
            return call_result;
        }

        (void)request.require_committed_visible;
        return call_result;
    }

    MetadataTransferClientGetObjectManifestCallResult
    MetadataTransferClient::GetObjectManifest(
        const MetadataTransferGetObjectManifestRequest &request,
        MetadataTransferClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.get_manifest_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        MetadataTransferClientGetObjectManifestCallResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.bucket,
                                             request.object_key,
                                             request.object_id,
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        std::string validation_error;
        if (!ValidateHeadLikeRequest(request.bucket,
                                     request.object_key,
                                     &validation_error))
        {
            FillLocalValidationFailureSummary(request.request_id,
                                             request.bucket,
                                             request.object_key,
                                             request.object_id,
                                             validation_error,
                                             &call_result.result.summary);
            call_result.result.diagnostics.push_back(
                MakeDiagnostic(call_result.result.summary, call_result.rpc));
            return call_result;
        }

        raft::HeadObjectRequest proto_request;
        proto_request.set_bucket(request.bucket);
        proto_request.set_object_key(request.object_key);
        if (!request.object_id.empty())
        {
            proto_request.set_object_id(request.object_id);
        }
        if (request.version.has_value())
        {
            proto_request.set_version(*request.version);
        }

        std::string current_endpoint = target_endpoint_;
        std::unique_ptr<raft::MetadataService::StubInterface> redirected_stub;

        for (int attempt = 0; attempt < kMaxMetadataNotLeaderAttempts; ++attempt)
        {
            call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                                 request.bucket,
                                                 request.object_key,
                                                 request.object_id,
                                                 current_endpoint,
                                                 effective_timeout,
                                                 wait_for_ready);

            grpc::ClientContext context;
            ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

            raft::HeadObjectResponse proto_response;
            auto *stub = redirected_stub != nullptr ? redirected_stub.get()
                                                    : stub_.get();
            const grpc::Status grpc_status =
                stub->HeadObject(&context, proto_request, &proto_response);
            if (!grpc_status.ok())
            {
                FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
                FillTransportFailureSummary(call_result.rpc,
                                            "HeadObject RPC failed: " +
                                                grpc_status.error_message(),
                                            &call_result.result.summary);
                call_result.result.diagnostics.push_back(
                    MakeDiagnostic(call_result.result.summary, call_result.rpc));
                return call_result;
            }

            FillSummaryFromProto(proto_response.summary(),
                                 &call_result.result.summary);
            if (call_result.result.summary.status ==
                MetadataTransferStatusCode::kNotLeader)
            {
                std::string retry_reason;
                const auto redirected_endpoint =
                    ResolveRedirectedMetadataEndpoint(call_result.result.summary,
                                                      current_endpoint,
                                                      &retry_reason);
                MetadataTransferDiagnostic diagnostic =
                    MakeDiagnostic(call_result.result.summary, call_result.rpc);
                if (redirected_endpoint.has_value() &&
                    attempt + 1 < kMaxMetadataNotLeaderAttempts)
                {
                    diagnostic.message = AppendMessageDetail(
                        diagnostic.message,
                        "metadata_transfer_client will retry against leader hint endpoint " +
                            *redirected_endpoint + " (attempt " +
                            std::to_string(attempt + 2) + "/" +
                            std::to_string(kMaxMetadataNotLeaderAttempts) + ")");
                    call_result.result.diagnostics.push_back(
                        std::move(diagnostic));
                    redirected_stub = MakeMetadataStubForEndpoint(
                        *redirected_endpoint,
                        config_);
                    current_endpoint = *redirected_endpoint;
                    continue;
                }

                if (redirected_endpoint.has_value())
                {
                    retry_reason = "MetadataService kept returning NOT_LEADER after " +
                                   std::to_string(kMaxMetadataNotLeaderAttempts) +
                                   " attempts; latest leader hint endpoint=" +
                                   *redirected_endpoint;
                }
                diagnostic.message = AppendMessageDetail(diagnostic.message,
                                                         retry_reason);
                call_result.result.summary.message = diagnostic.message;
                call_result.result.diagnostics.push_back(
                    std::move(diagnostic));
                return call_result;
            }

            call_result.result.found = proto_response.found();
            if (proto_response.found())
            {
                call_result.result.manifest =
                    BuildCommittedManifest(proto_response.object());
                call_result.result.visible_for_read = true;
                call_result.result.diagnostics.push_back(
                    MakeCommittedManifestBoundaryDiagnostic(
                        call_result.result.summary,
                        call_result.rpc));
            }

            if (redirected_stub != nullptr)
            {
                stub_ = std::move(redirected_stub);
                target_endpoint_ = current_endpoint;
            }

            if (!call_result.result.summary.ok() ||
                call_result.result.summary.leader_hint.has_value())
            {
                call_result.result.diagnostics.push_back(
                    MakeDiagnostic(call_result.result.summary, call_result.rpc));
            }

            (void)request.require_committed_visible;
            return call_result;
        }

        (void)request.require_committed_visible;
        return call_result;
    }

    std::string_view MetadataTransferClient::target_endpoint() const
    {
        return target_endpoint_;
    }

    const MetadataTransferClientConfig &MetadataTransferClient::config() const
    {
        return config_;
    }

    std::shared_ptr<MetadataTransferClient> CreateGrpcMetadataTransferClient(
        std::string target_endpoint,
        MetadataTransferClientConfig config)
    {
        auto channel = grpc::CreateChannel(target_endpoint,
                                           ResolveCredentials(config));
        return std::make_shared<MetadataTransferClient>(std::move(channel),
                                                        std::move(target_endpoint),
                                                        std::move(config));
    }
}
