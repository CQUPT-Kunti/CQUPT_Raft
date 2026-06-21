#include "store/transfer/storage_transfer_client.h"

#include "store/node/storage_node_client.h"

#include "store/transfer/object_transfer.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
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

        using TransferClock = std::chrono::steady_clock;

        [[nodiscard]] bool HasDeadline(const StorageTaskContext &context)
        {
            return context.timeout_ms != 0;
        }

        [[nodiscard]] TransferClock::time_point ResolveAbsoluteDeadline(
            const StorageTaskContext &context)
        {
            return HasDeadline(context)
                       ? TransferClock::now() +
                             std::chrono::milliseconds(context.timeout_ms)
                       : TransferClock::time_point::max();
        }

        [[nodiscard]] bool HasDeadlineExpired(
            const StorageTaskContext &context,
            const TransferClock::time_point absolute_deadline)
        {
            return HasDeadline(context) && TransferClock::now() >= absolute_deadline;
        }

        [[nodiscard]] std::uint64_t RemainingTimeoutMs(
            const StorageTaskContext &context,
            const TransferClock::time_point absolute_deadline)
        {
            if (!HasDeadline(context))
            {
                return 0;
            }

            const auto now = TransferClock::now();
            if (now >= absolute_deadline)
            {
                return 1;
            }

            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    absolute_deadline - now)
                    .count();
            return static_cast<std::uint64_t>(std::max<std::int64_t>(1, remaining));
        }

        [[nodiscard]] StorageTaskContext MakeAttemptContext(
            const StorageTaskContext &base_context,
            const TransferClock::time_point absolute_deadline)
        {
            StorageTaskContext context = base_context;
            if (HasDeadline(base_context))
            {
                context.timeout_ms =
                    RemainingTimeoutMs(base_context, absolute_deadline);
            }
            return context;
        }

        [[nodiscard]] std::uint64_t ComputeBackoffMs(
            const StorageTransferClientConfig &config,
            const std::uint64_t retry_after_ms,
            const std::uint32_t failure_index)
        {
            const std::uint64_t cap = config.max_backoff_ms;
            const std::uint64_t base = config.initial_backoff_ms;
            if (cap == 0)
            {
                return 0;
            }

            std::uint64_t delay = base;
            if (delay != 0)
            {
                const std::uint32_t shift = std::min<std::uint32_t>(failure_index, 10);
                for (std::uint32_t i = 0; i < shift && delay < cap; ++i)
                {
                    delay = std::min<std::uint64_t>(cap, delay * 2);
                }
            }
            if (retry_after_ms != 0)
            {
                delay = std::max(delay,
                                 std::min<std::uint64_t>(cap, retry_after_ms));
            }
            return std::min(delay, cap);
        }

        template <typename Result>
        void AnnotateAttemptFailure(Result *result,
                                    const std::string_view operation,
                                    const std::string &request_id,
                                    const StorageTransferTarget &target,
                                    const ChunkId &chunk_id,
                                    const std::uint32_t attempts_used,
                                    const std::uint32_t max_retries)
        {
            if (result == nullptr)
            {
                return;
            }

            std::string detail = operation == "WriteChunk"
                                     ? "WriteChunk transient failure"
                                     : "ReadChunk transient failure";
            detail += " after ";
            detail += std::to_string(attempts_used);
            detail += " attempt(s)";
            detail += " (max_retries=";
            detail += std::to_string(max_retries);
            detail += ", request_id=";
            detail += request_id;
            detail += ", chunk_id=";
            detail += chunk_id;
            detail += ", node_id=";
            detail += target.node_id.empty() ? "<unknown>" : target.node_id;
            detail += ", endpoint=";
            detail += target.endpoint.empty() ? "<unknown>" : target.endpoint;
            detail += ", status=";
            detail += ToString(result->status);
            detail += ")";
            if (!result->error_detail.empty())
            {
                detail += ": ";
                detail += result->error_detail;
            }
            result->error_detail = std::move(detail);
        }

        template <typename Result>
        Result MakeDeadlineExceededResult(const Result &last_result,
                                          const std::string_view operation,
                                          const std::string &request_id,
                                          const StorageTransferTarget &target,
                                          const ChunkId &chunk_id,
                                          const std::uint32_t attempts_used)
        {
            Result result = last_result;
            result.status = StorageNodeStatusCode::kTimeout;
            result.retryable = true;
            result.target = target;
            result.error_detail =
                std::string(operation == "WriteChunk"
                                ? "WriteChunk retry deadline expired"
                                : "ReadChunk retry deadline expired") +
                " before attempt " + std::to_string(attempts_used + 1) +
                " (request_id=" + request_id + ", chunk_id=" + chunk_id +
                ", node_id=" +
                (target.node_id.empty() ? std::string("<unknown>") : target.node_id) +
                ", endpoint=" +
                (target.endpoint.empty() ? std::string("<unknown>") : target.endpoint) +
                ")";
            return result;
        }

        [[nodiscard]] bool SleepForRetryBackoff(
            const std::uint64_t backoff_ms,
            const StorageTaskContext &context,
            const TransferClock::time_point absolute_deadline)
        {
            if (backoff_ms == 0)
            {
                return true;
            }

            std::uint64_t bounded_backoff_ms = backoff_ms;
            if (HasDeadline(context))
            {
                const auto now = TransferClock::now();
                if (now >= absolute_deadline)
                {
                    return false;
                }

                const auto remaining_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        absolute_deadline - now)
                        .count();
                if (remaining_ms <= 0)
                {
                    return false;
                }
                bounded_backoff_ms = std::min<std::uint64_t>(
                    bounded_backoff_ms,
                    static_cast<std::uint64_t>(remaining_ms));
            }

            if (bounded_backoff_ms == 0)
            {
                return true;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(bounded_backoff_ms));
            return true;
        }

        [[nodiscard]] int ResolveGrpcMessageLimitBytes(
            const StorageTransferClientConfig &config)
        {
            constexpr std::uint64_t kGrpcEnvelopeHeadroomBytes = 1024ULL * 1024ULL;
            constexpr std::uint64_t kGrpcMinimumMessageBytes = 4ULL * 1024ULL * 1024ULL;

            std::uint64_t message_limit = config.grpc_message_limit_bytes;
            if (message_limit == 0)
            {
                message_limit =
                    static_cast<std::uint64_t>(kProductionChunkSizeBytes) +
                    kGrpcEnvelopeHeadroomBytes;
            }
            if (message_limit < kGrpcMinimumMessageBytes)
            {
                message_limit = kGrpcMinimumMessageBytes;
            }
            if (message_limit >
                static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
            {
                message_limit =
                    static_cast<std::uint64_t>(std::numeric_limits<int>::max());
            }
            return static_cast<int>(message_limit);
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

            const auto absolute_deadline = ResolveAbsoluteDeadline(request.context);
            const std::uint32_t max_retries = config_.max_transient_write_retries;
            StorageTransferWriteResult last_result;
            last_result.target = request.target;

            for (std::uint32_t attempt_index = 0;; ++attempt_index)
            {
                if (HasDeadlineExpired(request.context, absolute_deadline))
                {
                    return MakeDeadlineExceededResult(last_result,
                                                     "WriteChunk",
                                                     request.request_id,
                                                     request.target,
                                                     node_request.identity.chunk_id,
                                                     attempt_index);
                }

                StorageNodeClientWriteChunkOptions options;
                options.context =
                    MakeAttemptContext(request.context, absolute_deadline);
                options.durability = StorageNodeWriteDurability::kPublish;

                const WriteChunkResponse response =
                    MakeNodeClient(request.target.endpoint)
                        .WriteChunk(node_request, options);
                last_result = TranslateWriteResult(request, response);
                if (!last_result.retryable || attempt_index >= max_retries)
                {
                    if (last_result.retryable)
                    {
                        AnnotateAttemptFailure(&last_result,
                                               "WriteChunk",
                                               request.request_id,
                                               last_result.target,
                                               node_request.identity.chunk_id,
                                               attempt_index + 1,
                                               max_retries);
                    }
                    return last_result;
                }

                const auto backoff_ms =
                    ComputeBackoffMs(config_,
                                     last_result.retry_after_ms,
                                     attempt_index);
                if (!SleepForRetryBackoff(backoff_ms,
                                          request.context,
                                          absolute_deadline))
                {
                    return MakeDeadlineExceededResult(last_result,
                                                     "WriteChunk",
                                                     request.request_id,
                                                     last_result.target,
                                                     node_request.identity.chunk_id,
                                                     attempt_index + 1);
                }
            }
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

            const auto absolute_deadline = ResolveAbsoluteDeadline(request.context);
            const std::uint32_t max_retries = config_.max_transient_read_retries;
            StorageTransferReadResult last_result;
            last_result.target = request.target;

            for (std::uint32_t attempt_index = 0;; ++attempt_index)
            {
                if (HasDeadlineExpired(request.context, absolute_deadline))
                {
                    return MakeDeadlineExceededResult(last_result,
                                                     "ReadChunk",
                                                     request.request_id,
                                                     request.target,
                                                     normalized_identity.chunk_id,
                                                     attempt_index);
                }

                StorageNodeClientReadChunkOptions options;
                options.context =
                    MakeAttemptContext(request.context, absolute_deadline);

                const ReadChunkResponse response =
                    MakeNodeClient(request.target.endpoint)
                        .ReadChunk(node_request, options);
                last_result = TranslateReadResult(request, response);
                if (!last_result.retryable || attempt_index >= max_retries)
                {
                    if (last_result.retryable)
                    {
                        AnnotateAttemptFailure(&last_result,
                                               "ReadChunk",
                                               request.request_id,
                                               last_result.target,
                                               normalized_identity.chunk_id,
                                               attempt_index + 1,
                                               max_retries);
                    }
                    return last_result;
                }

                const auto backoff_ms =
                    ComputeBackoffMs(config_,
                                     last_result.retry_after_ms,
                                     attempt_index);
                if (!SleepForRetryBackoff(backoff_ms,
                                          request.context,
                                          absolute_deadline))
                {
                    return MakeDeadlineExceededResult(last_result,
                                                     "ReadChunk",
                                                     request.request_id,
                                                     last_result.target,
                                                     normalized_identity.chunk_id,
                                                     attempt_index + 1);
                }
            }
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

            grpc::ChannelArguments arguments;
            const int message_limit_bytes = ResolveGrpcMessageLimitBytes(config_);
            arguments.SetMaxReceiveMessageSize(message_limit_bytes);
            arguments.SetMaxSendMessageSize(message_limit_bytes);
            auto channel = grpc::CreateCustomChannel(std::string(endpoint),
                                                     ResolveCredentials(),
                                                     arguments);
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
