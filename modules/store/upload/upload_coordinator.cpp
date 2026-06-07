#include "store/upload/upload_coordinator.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace storedemo
{
    namespace
    {
        struct BoundedSha256State
        {
            std::array<std::uint32_t, 8> words{
                0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
            std::array<std::uint8_t, 64> buffer{};
            std::size_t buffer_size{0};
            std::uint64_t total_bytes{0};
        };

        constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
            0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
            0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
            0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
            0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
            0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
            0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
            0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

        std::string JoinChunkRequestId(std::string_view base_request_id,
                                       std::string_view suffix)
        {
            return std::string(base_request_id) + "/" + std::string(suffix);
        }

        std::uint32_t RotateRight(const std::uint32_t value,
                                  const std::uint32_t bits)
        {
            return (value >> bits) | (value << (32U - bits));
        }

        std::uint32_t LoadBigEndianWord(const std::uint8_t *bytes)
        {
            return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                   (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                   (static_cast<std::uint32_t>(bytes[2]) << 8U) |
                   static_cast<std::uint32_t>(bytes[3]);
        }

        void StoreBigEndianWord(const std::uint32_t value,
                                std::uint8_t *out_bytes)
        {
            out_bytes[0] = static_cast<std::uint8_t>(value >> 24U);
            out_bytes[1] = static_cast<std::uint8_t>(value >> 16U);
            out_bytes[2] = static_cast<std::uint8_t>(value >> 8U);
            out_bytes[3] = static_cast<std::uint8_t>(value);
        }

        void ProcessSha256Block(BoundedSha256State *state,
                                const std::uint8_t *block)
        {
            std::array<std::uint32_t, 64> schedule{};
            for (std::size_t index = 0; index < 16; ++index)
            {
                schedule[index] = LoadBigEndianWord(block + index * 4U);
            }

            for (std::size_t index = 16; index < schedule.size(); ++index)
            {
                const std::uint32_t s0 =
                    RotateRight(schedule[index - 15U], 7U) ^
                    RotateRight(schedule[index - 15U], 18U) ^
                    (schedule[index - 15U] >> 3U);
                const std::uint32_t s1 =
                    RotateRight(schedule[index - 2U], 17U) ^
                    RotateRight(schedule[index - 2U], 19U) ^
                    (schedule[index - 2U] >> 10U);
                schedule[index] = schedule[index - 16U] + s0 +
                                  schedule[index - 7U] + s1;
            }

            std::uint32_t a = state->words[0];
            std::uint32_t b = state->words[1];
            std::uint32_t c = state->words[2];
            std::uint32_t d = state->words[3];
            std::uint32_t e = state->words[4];
            std::uint32_t f = state->words[5];
            std::uint32_t g = state->words[6];
            std::uint32_t h = state->words[7];

            for (std::size_t index = 0; index < schedule.size(); ++index)
            {
                const std::uint32_t sigma1 =
                    RotateRight(e, 6U) ^ RotateRight(e, 11U) ^
                    RotateRight(e, 25U);
                const std::uint32_t choice = (e & f) ^ (~e & g);
                const std::uint32_t temp1 =
                    h + sigma1 + choice + kSha256RoundConstants[index] +
                    schedule[index];
                const std::uint32_t sigma0 =
                    RotateRight(a, 2U) ^ RotateRight(a, 13U) ^
                    RotateRight(a, 22U);
                const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                const std::uint32_t temp2 = sigma0 + majority;

                h = g;
                g = f;
                f = e;
                e = d + temp1;
                d = c;
                c = b;
                b = a;
                a = temp1 + temp2;
            }

            state->words[0] += a;
            state->words[1] += b;
            state->words[2] += c;
            state->words[3] += d;
            state->words[4] += e;
            state->words[5] += f;
            state->words[6] += g;
            state->words[7] += h;
        }

        void UpdateSha256(BoundedSha256State *state,
                          const std::uint8_t *data,
                          const std::size_t size)
        {
            if (size == 0)
            {
                return;
            }

            state->total_bytes += size;

            std::size_t offset = 0;
            while (offset < size)
            {
                const std::size_t remaining = size - offset;
                const std::size_t copy_size = std::min<std::size_t>(
                    remaining,
                    state->buffer.size() - state->buffer_size);
                std::copy_n(data + offset,
                            copy_size,
                            state->buffer.begin() + static_cast<std::ptrdiff_t>(
                                                       state->buffer_size));
                state->buffer_size += copy_size;
                offset += copy_size;

                if (state->buffer_size == state->buffer.size())
                {
                    ProcessSha256Block(state, state->buffer.data());
                    state->buffer_size = 0;
                }
            }
        }

        std::array<std::uint8_t, kSha256DigestBytes> FinalizeSha256(
            BoundedSha256State state)
        {
            const std::uint64_t total_bits = state.total_bytes * 8U;

            state.buffer[state.buffer_size++] = 0x80U;
            if (state.buffer_size > 56U)
            {
                std::fill(state.buffer.begin() +
                              static_cast<std::ptrdiff_t>(state.buffer_size),
                          state.buffer.end(),
                          static_cast<std::uint8_t>(0));
                ProcessSha256Block(&state, state.buffer.data());
                state.buffer_size = 0;
            }

            std::fill(state.buffer.begin() +
                          static_cast<std::ptrdiff_t>(state.buffer_size),
                      state.buffer.begin() + 56,
                      static_cast<std::uint8_t>(0));

            for (std::size_t index = 0; index < 8U; ++index)
            {
                state.buffer[63U - index] =
                    static_cast<std::uint8_t>(total_bits >> (index * 8U));
            }
            ProcessSha256Block(&state, state.buffer.data());

            std::array<std::uint8_t, kSha256DigestBytes> digest{};
            for (std::size_t index = 0; index < state.words.size(); ++index)
            {
                StoreBigEndianWord(state.words[index],
                                   digest.data() + index * 4U);
            }
            return digest;
        }

        std::string EncodeLowerHex(const std::uint8_t *bytes,
                                   const std::size_t size)
        {
            static constexpr char kHexDigits[] = "0123456789abcdef";

            std::string encoded(size * 2U, '\0');
            for (std::size_t index = 0; index < size; ++index)
            {
                encoded[index * 2U] =
                    kHexDigits[(bytes[index] >> 4U) & 0x0fU];
                encoded[index * 2U + 1U] =
                    kHexDigits[bytes[index] & 0x0fU];
            }
            return encoded;
        }

        StorageNodeStatusCode ValidateProvidedObjectChecksum(
            const UploadObjectChecksumFacts &object_checksum,
            std::string *error_detail)
        {
            if (!object_checksum.checksum.IsSet())
            {
                return StorageNodeStatusCode::kOk;
            }

            if (object_checksum.checksum.algorithm !=
                ChunkChecksumAlgorithm::kSha256)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "object checksum algorithm is not supported";
                }
                return StorageNodeStatusCode::kUnsupported;
            }

            if (object_checksum.checksum.value.size() !=
                kSha256DigestHexChars)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "object checksum value must be 64 hex chars";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (object_checksum.size != 0 &&
                object_checksum.checksum.size_bytes != 0 &&
                object_checksum.size != object_checksum.checksum.size_bytes)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "object checksum size must match object_checksum.size";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return StorageNodeStatusCode::kOk;
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

        StorageNodeStatusCode ResolveObjectChecksumFacts(
            const UploadCoordinatorRequest &request,
            UploadObjectChecksumFacts *facts,
            std::string *error_detail)
        {
            if (facts == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "object checksum facts output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            auto status = ValidateProvidedObjectChecksum(request.object_checksum,
                                                        error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            BoundedSha256State state;
            std::uint64_t object_size = 0;
            for (const auto &chunk : request.chunks)
            {
                object_size += static_cast<std::uint64_t>(chunk.payload.size());
                const auto *bytes = reinterpret_cast<const std::uint8_t *>(
                    chunk.payload.data());
                UpdateSha256(&state, bytes, chunk.payload.size());
            }

            if (request.object_checksum.size != 0 &&
                request.object_checksum.size != object_size)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "object_checksum.size must match summed chunk payload size";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            ChunkChecksum computed_checksum;
            computed_checksum.algorithm = ChunkChecksumAlgorithm::kSha256;
            computed_checksum.size_bytes = object_size;
            computed_checksum.computed_at = 0;
            const auto digest = FinalizeSha256(state);
            computed_checksum.value =
                EncodeLowerHex(digest.data(), digest.size());

            if (request.object_checksum.checksum.IsSet())
            {
                if (request.object_checksum.checksum.size_bytes != object_size ||
                    request.object_checksum.checksum.value !=
                        computed_checksum.value)
                {
                    if (error_detail != nullptr)
                    {
                        *error_detail = "object checksum mismatch";
                    }
                    return StorageNodeStatusCode::kChecksumMismatch;
                }
            }

            facts->size = object_size;
            facts->checksum = computed_checksum;
            if (!request.object_checksum.etag.empty())
            {
                facts->etag = request.object_checksum.etag;
            }
            else if (!request.etag.empty())
            {
                facts->etag = request.etag;
            }
            else
            {
                facts->etag = computed_checksum.value;
            }

            return StorageNodeStatusCode::kOk;
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

        UploadCommittedChunk BuildDurableChunkFacts(
            const ChunkIdentity &identity,
            const std::uint64_t offset,
            const std::uint64_t expected_size,
            const ChunkChecksum &expected_checksum,
            const WriteChunkResponse &first_durable_response,
            std::vector<StorageNodeId> durable_replicas)
        {
            UploadCommittedChunk committed_chunk;
            committed_chunk.identity = identity;
            committed_chunk.offset = offset;
            committed_chunk.size = first_durable_response.metadata.size == 0
                                       ? expected_size
                                       : first_durable_response.metadata.size;
            committed_chunk.checksum =
                first_durable_response.metadata.checksum.IsSet()
                    ? first_durable_response.metadata.checksum
                    : expected_checksum;
            committed_chunk.replica_nodes = std::move(durable_replicas);
            return committed_chunk;
        }

        void AppendCleanupCandidate(const UploadCommittedChunk &chunk,
                                    std::string reason,
                                    std::vector<UploadCleanupCandidate> *candidates)
        {
            if (candidates == nullptr)
            {
                return;
            }

            const auto existing = std::find_if(
                candidates->begin(),
                candidates->end(),
                [&](const UploadCleanupCandidate &candidate)
                {
                    return candidate.chunk.identity.chunk_id == chunk.identity.chunk_id;
                });
            if (existing != candidates->end())
            {
                return;
            }

            candidates->push_back(UploadCleanupCandidate{
                .chunk = chunk,
                .reason = std::move(reason)});
        }

        void AppendCleanupCandidates(const std::vector<UploadCommittedChunk> &chunks,
                                     const std::string &reason,
                                     std::vector<UploadCleanupCandidate> *candidates)
        {
            if (candidates == nullptr)
            {
                return;
            }

            for (const auto &chunk : chunks)
            {
                AppendCleanupCandidate(chunk, reason, candidates);
            }
        }

        void SortCleanupCandidates(std::vector<UploadCleanupCandidate> *candidates)
        {
            if (candidates == nullptr)
            {
                return;
            }

            std::sort(candidates->begin(),
                      candidates->end(),
                      [](const UploadCleanupCandidate &lhs,
                         const UploadCleanupCandidate &rhs)
                      {
                          if (lhs.chunk.offset != rhs.chunk.offset)
                          {
                              return lhs.chunk.offset < rhs.chunk.offset;
                          }
                          return lhs.chunk.identity.chunk_index <
                                 rhs.chunk.identity.chunk_index;
                      });
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

        UploadObjectChecksumFacts object_checksum;
        result.status =
            ResolveObjectChecksumFacts(request,
                                       &object_checksum,
                                       &result.error_detail);
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
                .size = object_checksum.size,
                .etag = object_checksum.etag,
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
            const auto expected_size = ResolveExpectedSize(chunk);

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
                        .expected_size = expected_size,
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
                if (chunk_execution.durable_success_count > 0)
                {
                    AppendCleanupCandidates(
                        result.committed_chunks,
                        "upload failed before CommitObject; durable chunk requires cleanup candidate",
                        &result.cleanup_candidates);
                    AppendCleanupCandidate(
                        BuildDurableChunkFacts(chunk_execution.identity,
                                               chunk.offset,
                                               expected_size,
                                               expected_checksum,
                                               first_durable_response,
                                               std::move(durable_replicas)),
                        "minimum_successful_writes not reached; durable replica requires cleanup candidate",
                        &result.cleanup_candidates);
                    SortCleanupCandidates(&result.cleanup_candidates);
                }
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

            auto committed_chunk = BuildDurableChunkFacts(chunk_execution.identity,
                                                          chunk.offset,
                                                          expected_size,
                                                          expected_checksum,
                                                          first_durable_response,
                                                          std::move(durable_replicas));
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
                .size = object_checksum.size,
                .etag = object_checksum.etag,
                .chunks = result.committed_chunks,
                .client_time_unix_ms = request.client_time_unix_ms});

        if (!commit_result.ok())
        {
            result.status = commit_result.status;
            result.error_detail = "CommitObject failed: " +
                                  commit_result.error_detail;
            result.orphan_chunk_possible = !result.committed_chunks.empty();
            AppendCleanupCandidates(
                result.committed_chunks,
                "CommitObject failed after durable write; chunk requires cleanup candidate",
                &result.cleanup_candidates);
            SortCleanupCandidates(&result.cleanup_candidates);
            return result;
        }

        result.status = StorageNodeStatusCode::kOk;
        result.error_detail.clear();
        result.committed = true;
        result.pending_object_possible = false;
        result.orphan_chunk_possible = false;
        result.cleanup_candidates.clear();
        return result;
    }
}
