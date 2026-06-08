#include "store/transfer/storage_transfer_client.h"

#include "store/node/storage_node_client.h"

#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace storedemo
{
    namespace
    {
        StorageTransferWriteResult MakeInvalidWriteResult(
            const StorageTransferWriteRequest &request,
            std::string message)
        {
            StorageTransferWriteResult result;
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = std::move(message);
            result.retryable = false;
            result.target = request.target;
            return result;
        }

        StorageTransferReadResult MakeInvalidReadResult(
            const StorageTransferReadRequest &request,
            std::string message)
        {
            StorageTransferReadResult result;
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = std::move(message);
            result.retryable = false;
            result.target = request.target;
            return result;
        }

        StorageTransferTarget ResolveTarget(const StorageTransferTarget &request_target,
                                            const ChunkMetadata &metadata)
        {
            StorageTransferTarget target = request_target;
            if (target.node_id.empty() && !metadata.node_id.empty())
            {
                target.node_id = metadata.node_id;
            }
            return target;
        }

        StorageNodeStatusCode NormalizeIdentity(const ChunkIdentity &input,
                                                const std::uint64_t offset,
                                                ChunkIdentity *output,
                                                std::string *error_detail)
        {
            if (output == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk identity output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            ChunkIdentity normalized = input;
            if (normalized.chunk_id.empty())
            {
                if (!normalized.HasChunkKey())
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail =
                            "chunk identity must provide chunk_id or object_id/version/chunk_index";
                    }
                    return StorageNodeStatusCode::kInvalidArgument;
                }

                const auto status = MakeChunkId(normalized.object_id,
                                                normalized.version,
                                                normalized.chunk_index,
                                                &normalized.chunk_id,
                                                error_detail);
                if (status != StorageNodeStatusCode::kOk)
                {
                    return status;
                }
            }

            if (offset != 0 && normalized.offset != 0 && normalized.offset != offset)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "request offset does not match chunk identity offset";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
            if (normalized.offset == 0)
            {
                normalized.offset = offset;
            }

            *output = std::move(normalized);
            return StorageNodeStatusCode::kOk;
        }

        StorageTransferWriteResult TranslateWriteResult(
            const StorageTransferWriteRequest &request,
            const WriteChunkResponse &response)
        {
            StorageTransferWriteResult result;
            result.status = response.status;
            result.error_detail = response.error_detail;
            result.retry_after_ms = response.retry_after_ms;
            result.retryable = IsRetriableStatus(response.status);
            result.target = ResolveTarget(request.target, response.metadata);
            result.metadata = response.metadata;
            result.durable = response.durable;
            result.already_exists = response.already_exists;
            return result;
        }

        StorageTransferReadResult TranslateReadResult(
            const StorageTransferReadRequest &request,
            const ReadChunkResponse &response)
        {
            StorageTransferReadResult result;
            result.status = response.status;
            result.error_detail = response.error_detail;
            result.retry_after_ms = response.retry_after_ms;
            result.retryable = IsRetriableStatus(response.status);
            result.target = ResolveTarget(request.target, response.metadata);
            result.metadata = response.metadata;
            result.actual_checksum = response.actual_checksum;
            result.payload = response.payload;
            result.verified = response.verified;
            return result;
        }
    } // namespace

    class GrpcStorageTransferClient final : public StorageTransferClient
    {
    public:
        explicit GrpcStorageTransferClient(StorageTransferClientConfig config)
            : config_(std::move(config))
        {
        }

        StorageTransferWriteResult WriteChunk(
            const StorageTransferWriteRequest &request) override
        {
            if (request.request_id.empty())
            {
                return MakeInvalidWriteResult(request,
                                              "WriteChunk request_id must not be empty");
            }
            if (request.target.endpoint.empty())
            {
                return MakeInvalidWriteResult(
                    request,
                    "WriteChunk target endpoint must not be empty");
            }
            if (request.expected_size.has_value() &&
                *request.expected_size != request.payload.size())
            {
                return MakeInvalidWriteResult(
                    request,
                    "WriteChunk payload size does not match expected_size");
            }

            ChunkIdentity normalized_identity;
            std::string error_detail;
            const auto identity_status = NormalizeIdentity(request.identity,
                                                           request.offset,
                                                           &normalized_identity,
                                                           &error_detail);
            if (identity_status != StorageNodeStatusCode::kOk)
            {
                return MakeInvalidWriteResult(request, std::move(error_detail));
            }

            WriteChunkRequest node_request;
            node_request.request_id = request.request_id;
            node_request.identity = std::move(normalized_identity);
            node_request.expected_size = request.expected_size;
            node_request.expected_checksum = request.expected_checksum;
            node_request.payload = request.payload;

            StorageNodeClientWriteChunkOptions options;
            options.context = request.context;
            options.durability = StorageNodeWriteDurability::kPublish;

            const WriteChunkResponse response =
                MakeNodeClient(request.target.endpoint)
                    .WriteChunk(node_request, options);
            return TranslateWriteResult(request, response);
        }

        StorageTransferReadResult ReadChunk(
            const StorageTransferReadRequest &request) override
        {
            if (request.request_id.empty())
            {
                return MakeInvalidReadResult(request,
                                             "ReadChunk request_id must not be empty");
            }
            if (request.target.endpoint.empty())
            {
                return MakeInvalidReadResult(
                    request,
                    "ReadChunk target endpoint must not be empty");
            }

            ChunkIdentity normalized_identity;
            std::string error_detail;
            const auto identity_status = NormalizeIdentity(request.identity,
                                                           request.identity.offset,
                                                           &normalized_identity,
                                                           &error_detail);
            if (identity_status != StorageNodeStatusCode::kOk)
            {
                return MakeInvalidReadResult(request, std::move(error_detail));
            }

            ReadChunkRequest node_request;
            node_request.request_id = request.request_id;
            node_request.chunk_id = normalized_identity.chunk_id;
            node_request.range = request.range;
            node_request.expected_checksum = request.expected_checksum;
            node_request.verify_checksum = request.verify_checksum;

            StorageNodeClientReadChunkOptions options;
            options.context = request.context;

            const ReadChunkResponse response =
                MakeNodeClient(request.target.endpoint)
                    .ReadChunk(node_request, options);
            return TranslateReadResult(request, response);
        }

    private:
        std::shared_ptr<grpc::ChannelCredentials> ResolveCredentials() const
        {
            if (config_.channel_credentials != nullptr)
            {
                return config_.channel_credentials;
            }
            return grpc::InsecureChannelCredentials();
        }

        std::shared_ptr<grpc::Channel> ResolveChannel(
            const std::string_view endpoint)
        {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = channels_.find(std::string(endpoint));
                if (it != channels_.end())
                {
                    return it->second;
                }
            }

            auto channel = grpc::CreateChannel(std::string(endpoint),
                                               ResolveCredentials());
            std::lock_guard<std::mutex> lock(mutex_);
            auto [it, inserted] =
                channels_.emplace(std::string(endpoint), std::move(channel));
            return it->second;
        }

        StorageNodeClient MakeNodeClient(const std::string_view endpoint)
        {
            StorageNodeClientConfig config;
            config.max_write_retries = config_.max_write_retries;
            return StorageNodeClient(ResolveChannel(endpoint), config);
        }

        StorageTransferClientConfig config_;
        std::mutex mutex_;
        std::unordered_map<std::string, std::shared_ptr<grpc::Channel>> channels_;
    };

    std::shared_ptr<StorageTransferClient> CreateGrpcStorageTransferClient(
        StorageTransferClientConfig config)
    {
        return std::make_shared<GrpcStorageTransferClient>(std::move(config));
    }

} // namespace storedemo
