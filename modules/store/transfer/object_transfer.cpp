#include "store/transfer/object_transfer.h"

#include "store/transfer/metadata_transfer_client.h"
#include "store/transfer/storage_transfer_client.h"
#include "view/view_client.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace storedemo
{
    namespace
    {
        constexpr std::array<std::uint32_t, 8> kSha256InitialState = {
            0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

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

        struct Sha256State
        {
            std::array<std::uint32_t, 8> words{kSha256InitialState};
            std::array<std::uint8_t, 64> buffer{};
            std::size_t buffer_size{0};
            std::uint64_t total_bytes{0};
        };

        [[nodiscard]] bool IsFinishedStage(const ObjectTransferStage stage)
        {
            return stage == ObjectTransferStage::kCompleted ||
                   stage == ObjectTransferStage::kFailed ||
                   stage == ObjectTransferStage::kCancelled;
        }

        void SetErrorDetail(std::string *error_detail, const std::string &detail)
        {
            if (error_detail != nullptr)
            {
                *error_detail = detail;
            }
        }

        [[nodiscard]] std::uint32_t RotateRight(const std::uint32_t value,
                                                const std::uint32_t bits)
        {
            return (value >> bits) | (value << (32U - bits));
        }

        [[nodiscard]] std::uint32_t LoadBigEndianWord(const std::uint8_t *bytes)
        {
            return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
                   (static_cast<std::uint32_t>(bytes[1]) << 16U) |
                   (static_cast<std::uint32_t>(bytes[2]) << 8U) |
                   static_cast<std::uint32_t>(bytes[3]);
        }

        void StoreBigEndianWord(const std::uint32_t value, std::uint8_t *out_bytes)
        {
            out_bytes[0] = static_cast<std::uint8_t>(value >> 24U);
            out_bytes[1] = static_cast<std::uint8_t>(value >> 16U);
            out_bytes[2] = static_cast<std::uint8_t>(value >> 8U);
            out_bytes[3] = static_cast<std::uint8_t>(value);
        }

        void ProcessSha256Block(Sha256State *state, const std::uint8_t *block)
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
                    RotateRight(e, 6U) ^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
                const std::uint32_t choice = (e & f) ^ (~e & g);
                const std::uint32_t temp1 =
                    h + sigma1 + choice + kSha256RoundConstants[index] + schedule[index];
                const std::uint32_t sigma0 =
                    RotateRight(a, 2U) ^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
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

        void UpdateSha256(Sha256State *state,
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
                const std::size_t copy_size =
                    std::min<std::size_t>(remaining, state->buffer.size() - state->buffer_size);
                std::copy_n(data + offset,
                            copy_size,
                            state->buffer.begin() +
                                static_cast<std::ptrdiff_t>(state->buffer_size));
                state->buffer_size += copy_size;
                offset += copy_size;

                if (state->buffer_size == state->buffer.size())
                {
                    ProcessSha256Block(state, state->buffer.data());
                    state->buffer_size = 0;
                }
            }
        }

        [[nodiscard]] std::array<std::uint8_t, kSha256DigestBytes> FinalizeSha256(
            Sha256State state)
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
                StoreBigEndianWord(state.words[index], digest.data() + index * 4U);
            }
            return digest;
        }

        [[nodiscard]] std::string EncodeLowerHex(const std::uint8_t *bytes,
                                                 const std::size_t size)
        {
            static constexpr char kHexDigits[] = "0123456789abcdef";

            std::string encoded(size * 2U, '\0');
            for (std::size_t index = 0; index < size; ++index)
            {
                encoded[index * 2U] = kHexDigits[(bytes[index] >> 4U) & 0x0fU];
                encoded[index * 2U + 1U] = kHexDigits[bytes[index] & 0x0fU];
            }
            return encoded;
        }

        [[nodiscard]] TransferObjectChecksumFacts MakeObjectChecksumFacts(
            const ChunkChecksum &checksum,
            const std::uint64_t size)
        {
            TransferObjectChecksumFacts facts;
            facts.size = size;
            facts.checksum = checksum;
            facts.etag = checksum.value;
            return facts;
        }

        [[nodiscard]] ObjectTransferDiagnostic MakeDiagnostic(
            const ObjectTransferStatusCode status,
            const std::string &message,
            const std::string &request_id,
            const std::uint32_t chunk_index = 0,
            const std::uint64_t offset = 0,
            const ChunkId &chunk_id = ChunkId(),
            const bool retryable = false)
        {
            ObjectTransferDiagnostic diagnostic;
            diagnostic.status = status;
            diagnostic.message = message;
            diagnostic.request_id = request_id;
            diagnostic.chunk_index = chunk_index;
            diagnostic.offset = offset;
            diagnostic.chunk_id = chunk_id;
            diagnostic.retryable = retryable;
            return diagnostic;
        }

        [[nodiscard]] ObjectTransferStatusCode MapViewStatus(
            const viewdemo::ViewRegistryStatusCode status)
        {
            switch (status)
            {
            case viewdemo::ViewRegistryStatusCode::kOk:
            case viewdemo::ViewRegistryStatusCode::kIdempotentReplay:
                return ObjectTransferStatusCode::kOk;
            case viewdemo::ViewRegistryStatusCode::kInvalidArgument:
                return ObjectTransferStatusCode::kInvalidArgument;
            case viewdemo::ViewRegistryStatusCode::kNotFound:
                return ObjectTransferStatusCode::kNotFound;
            case viewdemo::ViewRegistryStatusCode::kTimeout:
                return ObjectTransferStatusCode::kTimeout;
            case viewdemo::ViewRegistryStatusCode::kOverloaded:
            case viewdemo::ViewRegistryStatusCode::kServiceUnavailable:
                return ObjectTransferStatusCode::kDiscoveryUnavailable;
            case viewdemo::ViewRegistryStatusCode::kConflict:
                return ObjectTransferStatusCode::kConflict;
            case viewdemo::ViewRegistryStatusCode::kUnsupported:
                return ObjectTransferStatusCode::kUnsupported;
            case viewdemo::ViewRegistryStatusCode::kStaleIgnored:
            case viewdemo::ViewRegistryStatusCode::kInternalError:
            default:
                return ObjectTransferStatusCode::kDiscoveryUnavailable;
            }
        }

        [[nodiscard]] ObjectTransferStatusCode MapMetadataStatus(
            const MetadataTransferStatusCode status)
        {
            switch (status)
            {
            case MetadataTransferStatusCode::kOk:
            case MetadataTransferStatusCode::kIdempotentReplay:
                return ObjectTransferStatusCode::kOk;
            case MetadataTransferStatusCode::kInvalidArgument:
                return ObjectTransferStatusCode::kInvalidArgument;
            case MetadataTransferStatusCode::kNotFound:
                return ObjectTransferStatusCode::kNotFound;
            case MetadataTransferStatusCode::kNotLeader:
                return ObjectTransferStatusCode::kMetadataNotLeader;
            case MetadataTransferStatusCode::kTimeout:
                return ObjectTransferStatusCode::kTimeout;
            case MetadataTransferStatusCode::kUnsupported:
                return ObjectTransferStatusCode::kUnsupported;
            case MetadataTransferStatusCode::kServiceUnavailable:
            case MetadataTransferStatusCode::kOverloaded:
            case MetadataTransferStatusCode::kQuorumUnavailable:
                return ObjectTransferStatusCode::kMetadataRejected;
            case MetadataTransferStatusCode::kIdempotencyConflict:
            case MetadataTransferStatusCode::kStateConflict:
                return ObjectTransferStatusCode::kConflict;
            case MetadataTransferStatusCode::kObjectNotVisible:
                return ObjectTransferStatusCode::kNotFound;
            case MetadataTransferStatusCode::kInternalError:
            default:
                return ObjectTransferStatusCode::kInternalError;
            }
        }

        [[nodiscard]] std::string SelectMetadataEndpoint(
            const viewdemo::DiscoverMetadataResult &discovery)
        {
            if (discovery.leader_hint.has_value() &&
                !discovery.leader_hint->endpoint.empty())
            {
                return discovery.leader_hint->endpoint;
            }

            for (const auto &snapshot : discovery.metadata_nodes)
            {
                if (!snapshot.control_plane_endpoint.empty())
                {
                    return snapshot.control_plane_endpoint;
                }
                if (!snapshot.endpoint.empty())
                {
                    return snapshot.endpoint;
                }
            }

            return {};
        }

        [[nodiscard]] StorageTransferTarget MakeStorageTargetFromSnapshot(
            const viewdemo::ViewNodeSnapshot &snapshot)
        {
            StorageTransferTarget target;
            target.node_id = snapshot.node_id;
            if (!snapshot.data_plane_endpoint.empty())
            {
                target.endpoint = snapshot.data_plane_endpoint;
            }
            else
            {
                target.endpoint = snapshot.endpoint;
            }
            return target;
        }

        template <typename DiagnosticsContainer>
        void AppendViewDiagnostics(
            const std::vector<viewdemo::ViewRegistryDiagnostic> &view_diagnostics,
            const std::string &request_id,
            DiagnosticsContainer *diagnostics)
        {
            if (diagnostics == nullptr)
            {
                return;
            }

            for (const auto &item : view_diagnostics)
            {
                ObjectTransferDiagnostic diagnostic;
                diagnostic.status = ObjectTransferStatusCode::kDiscoveryUnavailable;
                diagnostic.message = item.message;
                diagnostic.request_id = request_id;
                diagnostic.node_id = item.node_id;
                diagnostic.endpoint = item.endpoint;
                diagnostics->push_back(std::move(diagnostic));
            }
        }

        template <typename DiagnosticsContainer>
        void AppendMetadataDiagnostics(
            const std::vector<MetadataTransferDiagnostic> &metadata_diagnostics,
            DiagnosticsContainer *diagnostics)
        {
            if (diagnostics == nullptr)
            {
                return;
            }

            for (const auto &item : metadata_diagnostics)
            {
                ObjectTransferDiagnostic diagnostic;
                diagnostic.status = MapMetadataStatus(item.status);
                diagnostic.message = item.message;
                diagnostic.request_id = item.request_id;
                diagnostic.endpoint = item.endpoint;
                diagnostic.retryable = item.retryable;
                diagnostics->push_back(std::move(diagnostic));
            }
        }

        [[nodiscard]] std::shared_ptr<MetadataTransferClient> DiscoverMetadataClient(
            const std::string &request_id,
            const std::string &cluster_id,
            const std::shared_ptr<MetadataTransferClient> &metadata_client_seed,
            const std::shared_ptr<viewdemo::ViewNodeClient> &view_client,
            std::vector<ObjectTransferDiagnostic> *diagnostics,
            ObjectTransferStatusCode *status,
            std::string *error_detail)
        {
            if (view_client == nullptr)
            {
                if (status != nullptr)
                {
                    *status = ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(error_detail,
                               "ViewNode discovery is required but view_client is null");
                return nullptr;
            }

            const auto discovery_call = view_client->DiscoverMetadata(
                {.request_id = request_id,
                 .cluster_id = cluster_id,
                 .prefer_leader = true,
                 .live_only = true,
                 .limit = 3});
            AppendViewDiagnostics(discovery_call.result.diagnostics,
                                  request_id,
                                  diagnostics);

            if (!discovery_call.transport_ok() || !discovery_call.result.ok())
            {
                if (status != nullptr)
                {
                    *status = discovery_call.transport_ok()
                                  ? MapViewStatus(
                                        discovery_call.result.summary.status)
                                  : ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(
                    error_detail,
                    discovery_call.transport_ok()
                        ? "ViewNode DiscoverMetadata failed: " +
                              discovery_call.result.summary.message
                        : "ViewNode DiscoverMetadata RPC failed: " +
                              discovery_call.rpc.grpc_error_message);
                return nullptr;
            }

            const std::string endpoint = SelectMetadataEndpoint(
                discovery_call.result);
            if (endpoint.empty())
            {
                if (status != nullptr)
                {
                    *status = ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(error_detail,
                               "ViewNode returned no usable MetadataNode endpoint");
                return nullptr;
            }

            if (diagnostics != nullptr)
            {
                diagnostics->push_back(MakeDiagnostic(
                    ObjectTransferStatusCode::kOk,
                    discovery_call.result.leader_hint.has_value()
                            ? "ViewNode leader hint selected as MetadataNode endpoint candidate; MetadataService remains authority"
                            : "ViewNode metadata snapshot selected endpoint candidate; MetadataService remains authority",
                    request_id,
                    0,
                    0,
                    ChunkId(),
                    false));
                diagnostics->back().endpoint = endpoint;
                if (discovery_call.result.leader_hint.has_value())
                {
                    diagnostics->back().node_id =
                        discovery_call.result.leader_hint->node_id;
                }
            }

            MetadataTransferClientConfig config;
            if (metadata_client_seed != nullptr)
            {
                config = metadata_client_seed->config();
            }
            if (status != nullptr)
            {
                *status = ObjectTransferStatusCode::kOk;
            }
            return CreateGrpcMetadataTransferClient(endpoint, std::move(config));
        }

        [[nodiscard]] std::unordered_map<StorageNodeId, StorageTransferTarget>
        DiscoverStorageTargets(
            const std::string &request_id,
            const std::string &cluster_id,
            const std::uint64_t minimum_available_capacity_bytes,
            const std::uint32_t limit,
            const bool require_writable,
            const std::shared_ptr<viewdemo::ViewNodeClient> &view_client,
            std::vector<ObjectTransferDiagnostic> *diagnostics,
            ObjectTransferStatusCode *status,
            std::string *error_detail)
        {
            std::unordered_map<StorageNodeId, StorageTransferTarget> targets;
            if (view_client == nullptr)
            {
                if (status != nullptr)
                {
                    *status = ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(error_detail,
                               "ViewNode discovery is required but view_client is null");
                return targets;
            }

            const auto discovery_call = view_client->DiscoverStorage(
                {.request_id = request_id,
                 .cluster_id = cluster_id,
                 .live_only = true,
                 .minimum_available_capacity_bytes =
                     minimum_available_capacity_bytes,
                 .limit = limit,
                 .require_writable = require_writable});
            AppendViewDiagnostics(discovery_call.result.diagnostics,
                                  request_id,
                                  diagnostics);

            if (!discovery_call.transport_ok() || !discovery_call.result.ok())
            {
                if (status != nullptr)
                {
                    *status = discovery_call.transport_ok()
                                  ? MapViewStatus(
                                        discovery_call.result.summary.status)
                                  : ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(
                    error_detail,
                    discovery_call.transport_ok()
                        ? "ViewNode DiscoverStorage failed: " +
                              discovery_call.result.summary.message
                        : "ViewNode DiscoverStorage RPC failed: " +
                              discovery_call.rpc.grpc_error_message);
                return targets;
            }

            for (const auto &snapshot : discovery_call.result.storage_nodes)
            {
                StorageTransferTarget target = MakeStorageTargetFromSnapshot(snapshot);
                if (target.node_id.empty() || target.endpoint.empty())
                {
                    continue;
                }
                targets.emplace(target.node_id, target);

                if (diagnostics != nullptr)
                {
                    auto diagnostic = MakeDiagnostic(
                        ObjectTransferStatusCode::kOk,
                        "ViewNode storage snapshot recorded for data-plane endpoint resolution only; it does not imply object visibility",
                        request_id);
                    diagnostic.node_id = target.node_id;
                    diagnostic.endpoint = target.endpoint;
                    diagnostics->push_back(std::move(diagnostic));
                }
            }

            if (targets.empty())
            {
                if (status != nullptr)
                {
                    *status = ObjectTransferStatusCode::kDiscoveryUnavailable;
                }
                SetErrorDetail(error_detail,
                               "ViewNode returned no usable StorageNode endpoint");
                return targets;
            }

            if (status != nullptr)
            {
                *status = ObjectTransferStatusCode::kOk;
            }
            return targets;
        }

        [[nodiscard]] ObjectTransferStatusCode MapStorageStatus(
            const StorageNodeStatusCode status)
        {
            switch (status)
            {
            case StorageNodeStatusCode::kOk:
                return ObjectTransferStatusCode::kOk;
            case StorageNodeStatusCode::kNotFound:
                return ObjectTransferStatusCode::kNotFound;
            case StorageNodeStatusCode::kInvalidArgument:
                return ObjectTransferStatusCode::kInvalidArgument;
            case StorageNodeStatusCode::kChecksumMismatch:
            case StorageNodeStatusCode::kCorrupted:
                return ObjectTransferStatusCode::kChecksumMismatch;
            case StorageNodeStatusCode::kTimeout:
                return ObjectTransferStatusCode::kTimeout;
            case StorageNodeStatusCode::kCancelled:
                return ObjectTransferStatusCode::kCancelled;
            case StorageNodeStatusCode::kUnsupported:
                return ObjectTransferStatusCode::kUnsupported;
            case StorageNodeStatusCode::kConflict:
                return ObjectTransferStatusCode::kConflict;
            case StorageNodeStatusCode::kAlreadyExists:
            case StorageNodeStatusCode::kDiskFull:
            case StorageNodeStatusCode::kPermissionDenied:
            case StorageNodeStatusCode::kIoError:
            case StorageNodeStatusCode::kOverloaded:
            case StorageNodeStatusCode::kNodeUnavailable:
            default:
                return ObjectTransferStatusCode::kStorageRejected;
            }
        }

        [[nodiscard]] std::string SanitizePathToken(std::string_view value)
        {
            std::string sanitized;
            sanitized.reserve(value.size());
            for (const unsigned char ch : value)
            {
                if (std::isalnum(ch) != 0 || ch == '-' || ch == '_')
                {
                    sanitized.push_back(static_cast<char>(ch));
                }
                else
                {
                    sanitized.push_back('_');
                }
            }
            if (sanitized.empty())
            {
                sanitized = "request";
            }
            return sanitized;
        }

        [[nodiscard]] std::filesystem::path MakeTemporaryDownloadPath(
            const std::filesystem::path &destination_path,
            const std::string &request_id)
        {
            auto temp_path = destination_path;
            temp_path += ".";
            temp_path += SanitizePathToken(request_id);
            temp_path += ".part";
            return temp_path;
        }

        void RemovePathIfExists(const std::filesystem::path &path)
        {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }

        [[nodiscard]] std::optional<std::string> ValidateObjectChecksumFacts(
            const TransferObjectChecksumFacts &actual,
            const TransferObjectChecksumFacts &expected)
        {
            if (expected.size != 0 && actual.size != expected.size)
            {
                return "downloaded object size does not match expected checksum facts";
            }
            if (expected.checksum.IsSet())
            {
                if (actual.checksum.algorithm != expected.checksum.algorithm ||
                    actual.checksum.value != expected.checksum.value)
                {
                    return "downloaded object checksum does not match expected checksum facts";
                }
            }
            if (!expected.etag.empty() && actual.etag != expected.etag)
            {
                return "downloaded object etag does not match expected checksum facts";
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::string> ValidateManifestLayout(
            const TransferCommittedManifest &manifest,
            std::vector<TransferCommittedChunk> *ordered_chunks)
        {
            if (ordered_chunks == nullptr)
            {
                return std::string("ordered_chunks output must not be null");
            }

            *ordered_chunks = manifest.chunks;
            std::sort(ordered_chunks->begin(),
                      ordered_chunks->end(),
                      [](const TransferCommittedChunk &lhs,
                         const TransferCommittedChunk &rhs)
                      {
                          if (lhs.identity.offset != rhs.identity.offset)
                          {
                              return lhs.identity.offset < rhs.identity.offset;
                          }
                          return lhs.identity.chunk_index < rhs.identity.chunk_index;
                      });

            if (!manifest.object_checksum.checksum.IsSet())
            {
                return "manifest does not contain verifiable object checksum facts";
            }

            std::uint64_t expected_offset = 0;
            for (const auto &chunk : *ordered_chunks)
            {
                if (chunk.identity.offset != expected_offset)
                {
                    return "manifest chunk offsets are not contiguous";
                }
                if (chunk.size == 0 && manifest.object_checksum.size != 0)
                {
                    return "manifest contains zero-sized chunk for non-empty object";
                }
                if (chunk.size != 0 && !chunk.checksum.IsSet())
                {
                    return "manifest chunk is missing checksum facts";
                }
                if (chunk.size != 0 && chunk.replica_nodes.empty())
                {
                    return "manifest chunk does not contain replica node ids";
                }
                expected_offset += chunk.size;
            }

            if (manifest.object_checksum.size != 0 &&
                expected_offset != manifest.object_checksum.size)
            {
                return "manifest chunk sizes do not match object size";
            }
            if (manifest.object_checksum.size == 0 && !ordered_chunks->empty())
            {
                return "empty object manifest must not contain chunks";
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<StorageTransferTarget> ResolveReplicaTarget(
            const TransferCommittedChunk &chunk,
            const std::unordered_map<StorageNodeId, StorageTransferTarget> &storage_targets)
        {
            for (const auto &replica_node_id : chunk.replica_nodes)
            {
                const auto it = storage_targets.find(replica_node_id);
                if (it != storage_targets.end() && !it->second.endpoint.empty())
                {
                    return it->second;
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::vector<StorageTransferTarget> SortedStorageTargets(
            const std::unordered_map<StorageNodeId, StorageTransferTarget> &storage_targets)
        {
            std::vector<StorageTransferTarget> targets;
            targets.reserve(storage_targets.size());
            for (const auto &[node_id, target] : storage_targets)
            {
                (void)node_id;
                if (target.node_id.empty() || target.endpoint.empty())
                {
                    continue;
                }
                targets.push_back(target);
            }

            std::sort(targets.begin(),
                      targets.end(),
                      [](const StorageTransferTarget &lhs,
                         const StorageTransferTarget &rhs)
                      {
                          if (lhs.node_id != rhs.node_id)
                          {
                              return lhs.node_id < rhs.node_id;
                          }
                          return lhs.endpoint < rhs.endpoint;
                      });
            return targets;
        }

        [[nodiscard]] TransferCommittedChunk BuildDurableChunkFacts(
            const ChunkIdentity &identity,
            const std::uint64_t expected_size,
            const ChunkChecksum &expected_checksum,
            const StorageTransferWriteResult &first_durable_result,
            std::vector<StorageNodeId> durable_replicas)
        {
            TransferCommittedChunk committed_chunk;
            committed_chunk.identity = identity;
            committed_chunk.size = first_durable_result.metadata.size == 0
                                       ? expected_size
                                       : first_durable_result.metadata.size;
            committed_chunk.checksum =
                first_durable_result.metadata.checksum.IsSet()
                    ? first_durable_result.metadata.checksum
                    : expected_checksum;
            committed_chunk.replica_nodes = std::move(durable_replicas);
            return committed_chunk;
        }

        [[nodiscard]] std::vector<CleanupCandidate>
        BuildFailedUploadCleanupCandidatesFromChunks(
            const std::string &bucket,
            const std::string &object_key,
            const std::string &object_id,
            const std::uint64_t version,
            const std::uint64_t created_at_unix_ms,
            const std::vector<TransferCommittedChunk> &durable_chunks)
        {
            std::vector<CleanupChunkFact> cleanup_chunk_facts;
            cleanup_chunk_facts.reserve(durable_chunks.size());
            for (const auto &chunk : durable_chunks)
            {
                CleanupChunkFact fact;
                fact.identity = chunk.identity;
                fact.identity.offset = chunk.identity.offset;
                fact.size = chunk.size;
                fact.checksum = chunk.checksum;
                fact.replica_nodes = chunk.replica_nodes;
                cleanup_chunk_facts.push_back(std::move(fact));
            }

            FailedUploadCleanupRequest cleanup_request;
            cleanup_request.bucket = bucket;
            cleanup_request.object_key = object_key;
            cleanup_request.object_id = object_id;
            cleanup_request.version = version;
            cleanup_request.object_state = CleanupObjectState::kPending;
            cleanup_request.created_at_unix_ms = created_at_unix_ms;
            cleanup_request.durable_chunks = std::move(cleanup_chunk_facts);
            return BuildFailedUploadCleanupCandidates(cleanup_request);
        }

        void AppendStorageWriteDiagnostic(const std::string &request_id,
                                         const StorageTransferWriteResult &write_result,
                                         const ChunkIdentity &identity,
                                         const std::uint32_t chunk_index,
                                         const std::uint64_t offset,
                                         std::vector<ObjectTransferDiagnostic> *diagnostics)
        {
            if (diagnostics == nullptr)
            {
                return;
            }

            ObjectTransferDiagnostic diagnostic;
            diagnostic.status = MapStorageStatus(write_result.status);
            diagnostic.message =
                write_result.ok()
                    ? "StorageNode WriteChunk recorded durable chunk facts; object visibility still depends on CommitObject"
                    : "StorageNode WriteChunk failed: " + write_result.error_detail;
            diagnostic.request_id = request_id;
            diagnostic.node_id = write_result.target.node_id;
            diagnostic.endpoint = write_result.target.endpoint;
            diagnostic.chunk_id = identity.chunk_id;
            diagnostic.chunk_index = chunk_index;
            diagnostic.offset = offset;
            diagnostic.retryable = write_result.retryable;
            diagnostics->push_back(std::move(diagnostic));
        }

        [[nodiscard]] TransferCommittedManifest BuildFallbackCommittedManifest(
            const TransferWritePlan &write_plan,
            const TransferObjectChecksumFacts &object_checksum,
            const std::vector<TransferCommittedChunk> &committed_chunks,
            const std::uint64_t committed_at_unix_ms)
        {
            TransferCommittedManifest manifest;
            manifest.bucket = write_plan.bucket;
            manifest.object_key = write_plan.object_key;
            manifest.object_id = write_plan.object_id;
            manifest.version = write_plan.version;
            manifest.object_checksum = object_checksum;
            manifest.chunks = committed_chunks;
            manifest.committed_at_unix_ms = committed_at_unix_ms;
            return manifest;
        }

        class FileTransferChunkReader final : public TransferChunkReader
        {
        public:
            ObjectTransferStatusCode Open(const TransferChunkReaderOpenRequest &request,
                                          std::string *error_detail) override
            {
                Close();

                if (request.source_path.empty())
                {
                    SetErrorDetail(error_detail, "source_path is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request.chunk_size == 0)
                {
                    SetErrorDetail(error_detail, "chunk_size must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request.start_offset % request.chunk_size != 0)
                {
                    SetErrorDetail(error_detail,
                                   "start_offset must align with chunk_size boundary");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }

                std::error_code stat_ec;
                if (!std::filesystem::exists(request.source_path, stat_ec))
                {
                    SetErrorDetail(error_detail,
                                   stat_ec ? "failed to stat source_path: " +
                                                 stat_ec.message()
                                           : "source_path does not exist");
                    return stat_ec ? ObjectTransferStatusCode::kIoError
                                   : ObjectTransferStatusCode::kNotFound;
                }
                if (!std::filesystem::is_regular_file(request.source_path, stat_ec))
                {
                    SetErrorDetail(error_detail,
                                   stat_ec ? "failed to verify regular file: " +
                                                 stat_ec.message()
                                           : "source_path must be a regular file");
                    return stat_ec ? ObjectTransferStatusCode::kIoError
                                   : ObjectTransferStatusCode::kInvalidArgument;
                }

                const auto file_size = std::filesystem::file_size(request.source_path,
                                                                  stat_ec);
                if (stat_ec)
                {
                    SetErrorDetail(error_detail,
                                   "failed to read source file size: " + stat_ec.message());
                    return ObjectTransferStatusCode::kIoError;
                }
                if (request.start_offset > file_size)
                {
                    SetErrorDetail(error_detail,
                                   "start_offset exceeds source file size");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request.chunk_size >
                    static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
                {
                    SetErrorDetail(error_detail,
                                   "chunk_size exceeds supported stream buffer size");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }

                input_.open(request.source_path, std::ios::binary);
                if (!input_.is_open())
                {
                    SetErrorDetail(error_detail,
                                   "failed to open source file for reading");
                    return ObjectTransferStatusCode::kIoError;
                }

                input_.seekg(static_cast<std::streamoff>(request.start_offset),
                             std::ios::beg);
                if (!input_)
                {
                    Close();
                    SetErrorDetail(error_detail, "failed to seek source file");
                    return ObjectTransferStatusCode::kIoError;
                }

                request_ = request;
                file_size_ = file_size;
                next_chunk_index_ =
                    static_cast<std::uint32_t>(request.start_offset / request.chunk_size);
                next_offset_ = request.start_offset;
                opened_ = true;
                return ObjectTransferStatusCode::kOk;
            }

            TransferChunkReadResult ReadNextChunk() override
            {
                TransferChunkReadResult result;
                if (!opened_)
                {
                    result.status = ObjectTransferStatusCode::kInvalidArgument;
                    result.error_detail = "chunk reader is not open";
                    return result;
                }

                result.chunk_index = next_chunk_index_;
                result.offset = next_offset_;
                if (next_offset_ >= file_size_)
                {
                    result.eof = true;
                    result.last_chunk = true;
                    return result;
                }

                const std::uint64_t remaining = file_size_ - next_offset_;
                const std::uint64_t to_read =
                    std::min<std::uint64_t>(request_.chunk_size, remaining);
                result.payload.resize(static_cast<std::size_t>(to_read));
                input_.read(result.payload.data(),
                            static_cast<std::streamsize>(to_read));
                const auto bytes_read =
                    static_cast<std::uint64_t>(input_.gcount());
                if (bytes_read != to_read)
                {
                    result.status = ObjectTransferStatusCode::kIoError;
                    result.error_detail = "failed to read full bounded chunk from source file";
                    result.payload.resize(static_cast<std::size_t>(bytes_read));
                    return result;
                }
                if (input_.bad())
                {
                    result.status = ObjectTransferStatusCode::kIoError;
                    result.error_detail = "source file stream entered bad state";
                    return result;
                }

                next_offset_ += to_read;
                ++next_chunk_index_;
                result.last_chunk = next_offset_ >= file_size_;
                result.eof = result.last_chunk;
                return result;
            }

            void Close() override
            {
                if (input_.is_open())
                {
                    input_.close();
                }
                request_ = TransferChunkReaderOpenRequest{};
                file_size_ = 0;
                next_chunk_index_ = 0;
                next_offset_ = 0;
                opened_ = false;
            }

        private:
            TransferChunkReaderOpenRequest request_{};
            std::ifstream input_;
            std::uint64_t file_size_{0};
            std::uint32_t next_chunk_index_{0};
            std::uint64_t next_offset_{0};
            bool opened_{false};
        };

        class IncrementalTransferChecksumState final : public TransferChecksumState
        {
        public:
            TransferChecksumUpdateResult Append(
                const TransferChecksumUpdateRequest &request) override
            {
                TransferChecksumUpdateResult result;
                if (finalized_)
                {
                    result.status = ObjectTransferStatusCode::kConflict;
                    result.error_detail =
                        "checksum state is finalized and must be reset before append";
                    return result;
                }

                ChunkChecksum chunk_checksum;
                std::string error_detail;
                const auto compute_status =
                    ComputeChunkChecksum(request.payload, &chunk_checksum, &error_detail);
                if (compute_status != StorageNodeStatusCode::kOk)
                {
                    result.status = ObjectTransferStatusCode::kInternalError;
                    result.error_detail = "failed to compute chunk checksum: " +
                                          error_detail;
                    return result;
                }

                if (request.expected_chunk_checksum.has_value())
                {
                    const auto verify_status = VerifyChunkChecksum(
                        request.payload,
                        *request.expected_chunk_checksum,
                        &chunk_checksum,
                        &error_detail);
                    if (verify_status != StorageNodeStatusCode::kOk)
                    {
                        result.status = verify_status == StorageNodeStatusCode::kChecksumMismatch
                                            ? ObjectTransferStatusCode::kChecksumMismatch
                                            : ObjectTransferStatusCode::kInternalError;
                        result.error_detail = "chunk checksum verification failed: " +
                                              error_detail;
                        return result;
                    }
                    result.chunk_checksum_verified = true;
                }

                const auto *bytes =
                    reinterpret_cast<const std::uint8_t *>(request.payload.data());
                UpdateSha256(&object_checksum_state_, bytes, request.payload.size());
                bytes_processed_ += static_cast<std::uint64_t>(request.payload.size());
                ++chunks_processed_;

                result.chunk_checksum = std::move(chunk_checksum);
                result.bytes_processed = bytes_processed_;
                result.chunks_processed = chunks_processed_;
                return result;
            }

            TransferChecksumFinalizeResult Finalize() override
            {
                TransferChecksumFinalizeResult result;
                if (finalized_)
                {
                    if (final_object_checksum_.has_value())
                    {
                        result.object_checksum = *final_object_checksum_;
                    }
                    return result;
                }

                finalized_ = true;
                const auto digest = FinalizeSha256(object_checksum_state_);
                ChunkChecksum checksum;
                checksum.algorithm = ChunkChecksumAlgorithm::kSha256;
                checksum.value = EncodeLowerHex(digest.data(), digest.size());
                checksum.size_bytes = bytes_processed_;
                checksum.computed_at = 0;

                final_object_checksum_ =
                    MakeObjectChecksumFacts(checksum, bytes_processed_);
                result.object_checksum = *final_object_checksum_;
                return result;
            }

            TransferChecksumSnapshot Snapshot() const override
            {
                TransferChecksumSnapshot snapshot;
                snapshot.bytes_processed = bytes_processed_;
                snapshot.chunks_processed = chunks_processed_;
                snapshot.finalized = finalized_;
                snapshot.object_checksum = final_object_checksum_;
                return snapshot;
            }

            void Reset() override
            {
                object_checksum_state_ = Sha256State{};
                bytes_processed_ = 0;
                chunks_processed_ = 0;
                finalized_ = false;
                final_object_checksum_.reset();
            }

        private:
            Sha256State object_checksum_state_{};
            std::uint64_t bytes_processed_{0};
            std::uint32_t chunks_processed_{0};
            bool finalized_{false};
            std::optional<TransferObjectChecksumFacts> final_object_checksum_;
        };

        class BasicTransferSession
        {
        public:
            explicit BasicTransferSession(TransferSessionSnapshot snapshot)
                : snapshot_(std::move(snapshot))
            {
            }

            [[nodiscard]] TransferSessionSnapshot Snapshot() const
            {
                return snapshot_;
            }

            [[nodiscard]] bool finished() const
            {
                return IsFinishedStage(snapshot_.stage);
            }

        protected:
            void SetStage(const ObjectTransferStage stage)
            {
                snapshot_.stage = stage;
            }

            void SetFailure(const TransferFailureSummary &failure)
            {
                snapshot_.failure = failure;
                snapshot_.stage = ObjectTransferStage::kFailed;
            }

            void MarkCompleted()
            {
                snapshot_.stage = ObjectTransferStage::kCompleted;
            }

            TransferSessionSnapshot &mutable_snapshot()
            {
                return snapshot_;
            }

            const TransferSessionSnapshot &snapshot_ref() const
            {
                return snapshot_;
            }

        private:
            TransferSessionSnapshot snapshot_;
        };

        class BasicUploadTransferSession final : public UploadTransferSession,
                                                 private BasicTransferSession
        {
        public:
            BasicUploadTransferSession(
                UploadObjectRequest request,
                std::shared_ptr<MetadataTransferClient> metadata_client,
                std::shared_ptr<StorageTransferClient> storage_client,
                std::shared_ptr<viewdemo::ViewNodeClient> view_client)
                : BasicTransferSession(MakeInitialSnapshot(request)),
                  request_(std::move(request)),
                  metadata_client_(std::move(metadata_client)),
                  storage_client_(std::move(storage_client)),
                  view_client_(std::move(view_client))
            {
            }

            [[nodiscard]] ObjectTransferDirection direction() const override
            {
                return ObjectTransferDirection::kUpload;
            }

            [[nodiscard]] TransferSessionSnapshot Snapshot() const override
            {
                return snapshot_ref();
            }

            [[nodiscard]] bool finished() const override
            {
                return BasicTransferSession::finished();
            }

            [[nodiscard]] const UploadObjectRequest &request() const override
            {
                return request_;
            }

            UploadObjectResult Execute(TransferChunkReader &reader,
                                       TransferChecksumState &checksum_state) override
            {
                UploadObjectResult result;
                result.session = Snapshot();
                current_write_plan_.reset();

                const auto validation_status = ValidateRequest(&result.error_detail);
                if (validation_status != ObjectTransferStatusCode::kOk)
                {
                    Fail(&result, validation_status, result.error_detail);
                    return result;
                }

                SetStage(ObjectTransferStage::kPreparing);
                result.session = Snapshot();

                checksum_state.Reset();
                std::string open_error;
                const auto open_status = reader.Open(
                    {.source_path = request_.source_path,
                     .chunk_size = request_.chunk_size,
                     .start_offset = 0},
                    &open_error);
                if (open_status != ObjectTransferStatusCode::kOk)
                {
                    Fail(&result, open_status, std::move(open_error));
                    return result;
                }

                std::uint64_t expected_offset = 0;
                std::uint32_t expected_chunk_index = 0;
                while (true)
                {
                    TransferChunkReadResult chunk = reader.ReadNextChunk();
                    if (!chunk.ok())
                    {
                        reader.Close();
                        Fail(&result,
                             chunk.status,
                             chunk.error_detail,
                             chunk.chunk_index,
                             chunk.offset);
                        return result;
                    }

                    if (chunk.offset != expected_offset)
                    {
                        reader.Close();
                        Fail(&result,
                             ObjectTransferStatusCode::kConflict,
                             "chunk reader returned non-contiguous offset",
                             chunk.chunk_index,
                             chunk.offset);
                        return result;
                    }
                    if (chunk.chunk_index != expected_chunk_index)
                    {
                        reader.Close();
                        Fail(&result,
                             ObjectTransferStatusCode::kConflict,
                             "chunk reader returned unexpected chunk index",
                             chunk.chunk_index,
                             chunk.offset);
                        return result;
                    }

                    if (!chunk.payload.empty())
                    {
                        if (chunk.payload.size() > request_.chunk_size)
                        {
                            reader.Close();
                            Fail(&result,
                                 ObjectTransferStatusCode::kConflict,
                                 "chunk reader exceeded bounded chunk_size",
                                 chunk.chunk_index,
                                 chunk.offset);
                            return result;
                        }

                        const auto checksum_update = checksum_state.Append(
                            {.chunk_index = chunk.chunk_index,
                             .offset = chunk.offset,
                             .payload = chunk.payload});
                        if (!checksum_update.ok())
                        {
                            reader.Close();
                            Fail(&result,
                                 checksum_update.status,
                                 checksum_update.error_detail,
                                 chunk.chunk_index,
                                 chunk.offset);
                            return result;
                        }

                        result.prepared_chunks.push_back(
                            {.chunk_index = chunk.chunk_index,
                             .offset = chunk.offset,
                             .size = static_cast<std::uint64_t>(chunk.payload.size()),
                             .checksum = checksum_update.chunk_checksum});

                        mutable_snapshot().bytes_completed +=
                            static_cast<std::uint64_t>(chunk.payload.size());
                        mutable_snapshot().chunks_completed += 1;
                        mutable_snapshot().total_bytes = mutable_snapshot().bytes_completed;
                        mutable_snapshot().total_chunks =
                            mutable_snapshot().chunks_completed;

                        expected_offset +=
                            static_cast<std::uint64_t>(chunk.payload.size());
                        ++expected_chunk_index;
                    }

                    if (chunk.last_chunk)
                    {
                        break;
                    }
                    if (chunk.eof && chunk.payload.empty())
                    {
                        break;
                    }
                }
                reader.Close();

                const auto finalize_result = checksum_state.Finalize();
                if (!finalize_result.ok())
                {
                    Fail(&result,
                         finalize_result.status,
                         finalize_result.error_detail,
                         expected_chunk_index,
                         expected_offset);
                    return result;
                }

                mutable_snapshot().final_checksum_verified = true;
                mutable_snapshot().total_bytes = finalize_result.object_checksum.size;
                result.session = Snapshot();

                if (request_.expected_object_checksum.has_value())
                {
                    const auto mismatch_reason = ValidateExpectedObjectChecksum(
                        finalize_result.object_checksum,
                        *request_.expected_object_checksum);
                    if (mismatch_reason.has_value())
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kChecksumMismatch,
                             *mismatch_reason,
                             expected_chunk_index,
                             expected_offset);
                        return result;
                    }
                }

                SetStage(ObjectTransferStage::kDiscoveringMetadata);
                result.session = Snapshot();

                ObjectTransferStatusCode discovery_status =
                    ObjectTransferStatusCode::kOk;
                std::string discovery_error;
                const auto discovered_metadata_client = DiscoverMetadataClient(
                    request_.request_id,
                    request_.cluster_id,
                    metadata_client_,
                    view_client_,
                    &result.diagnostics,
                    &discovery_status,
                    &discovery_error);
                if (discovered_metadata_client == nullptr)
                {
                    Fail(&result, discovery_status, std::move(discovery_error));
                    return result;
                }

                SetStage(ObjectTransferStage::kPlanningWrite);
                result.session = Snapshot();

                const auto create_plan_call =
                    discovered_metadata_client->CreateWritePlan(
                        {.request_id = request_.request_id,
                         .bucket = request_.bucket,
                         .object_key = request_.object_key,
                         .object_id = request_.object_id,
                         .expected_object_checksum =
                             finalize_result.object_checksum,
                         .chunk_size = request_.chunk_size,
                         .desired_replica_count =
                             request_.desired_replica_count,
                         .minimum_successful_writes =
                             request_.minimum_successful_writes,
                         .client_time_unix_ms =
                             request_.client_time_unix_ms});
                AppendMetadataDiagnostics(create_plan_call.result.diagnostics,
                                          &result.diagnostics);
                if (!create_plan_call.transport_ok() ||
                    !create_plan_call.result.ok() ||
                    !create_plan_call.result.write_plan.has_value())
                {
                    Fail(&result,
                         !create_plan_call.transport_ok()
                             ? ObjectTransferStatusCode::kMetadataRejected
                             : MapMetadataStatus(
                                   create_plan_call.result.summary.status),
                         !create_plan_call.transport_ok()
                             ? "Metadata CreateWritePlan RPC failed: " +
                                   create_plan_call.rpc.grpc_error_message
                             : "Metadata CreateWritePlan failed: " +
                                   create_plan_call.result.summary.message);
                    return result;
                }
                result.write_plan = create_plan_call.result.write_plan;
                current_write_plan_ = result.write_plan;

                const std::string resolved_object_id =
                    !result.write_plan->object_id.empty()
                        ? result.write_plan->object_id
                        : (!create_plan_call.result.summary.object_id.empty()
                               ? create_plan_call.result.summary.object_id
                               : request_.object_id);
                if (resolved_object_id.empty())
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kMetadataRejected,
                         "Metadata CreateWritePlan did not return a usable object_id");
                    return result;
                }
                if (result.write_plan->version == 0)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kMetadataRejected,
                         "Metadata CreateWritePlan did not return a usable version");
                    return result;
                }
                result.write_plan->object_id = resolved_object_id;
                mutable_snapshot().object_id = resolved_object_id;
                mutable_snapshot().version = result.write_plan->version;
                result.session = Snapshot();

                std::string storage_discovery_error;
                const auto storage_targets = DiscoverStorageTargets(
                    request_.request_id,
                    request_.cluster_id,
                    finalize_result.object_checksum.size,
                    request_.desired_replica_count,
                    true,
                    view_client_,
                    &result.diagnostics,
                    &discovery_status,
                    &storage_discovery_error);
                if (storage_targets.empty())
                {
                    Fail(&result,
                         discovery_status,
                         std::move(storage_discovery_error));
                    return result;
                }
                if (storage_client_ == nullptr)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kInternalError,
                         "StorageTransferClient is required for upload chunk writes");
                    return result;
                }

                std::vector<TransferCommittedChunk> durable_chunks;
                durable_chunks.reserve(result.prepared_chunks.size());
                bool uncertain_cleanup_possible = false;

                if (!result.prepared_chunks.empty())
                {
                    SetStage(ObjectTransferStage::kUploadingChunks);
                    result.session = Snapshot();

                    open_error.clear();
                    const auto reopen_status = reader.Open(
                        {.source_path = request_.source_path,
                         .chunk_size = request_.chunk_size,
                         .start_offset = 0},
                        &open_error);
                    if (reopen_status != ObjectTransferStatusCode::kOk)
                    {
                        Fail(&result, reopen_status, std::move(open_error));
                        return result;
                    }

                    std::size_t prepared_index = 0;
                    while (true)
                    {
                        TransferChunkReadResult chunk = reader.ReadNextChunk();
                        if (!chunk.ok())
                        {
                            reader.Close();
                            BuildCleanupCandidates(&result,
                                                   durable_chunks,
                                                   uncertain_cleanup_possible);
                            Fail(&result,
                                 chunk.status,
                                 chunk.error_detail,
                                 chunk.chunk_index,
                                 chunk.offset);
                            return result;
                        }

                        if (chunk.payload.empty())
                        {
                            if (chunk.last_chunk || chunk.eof)
                            {
                                if (prepared_index == result.prepared_chunks.size())
                                {
                                    break;
                                }

                                reader.Close();
                                BuildCleanupCandidates(&result,
                                                       durable_chunks,
                                                       uncertain_cleanup_possible);
                                Fail(&result,
                                     ObjectTransferStatusCode::kConflict,
                                     "upload second pass observed fewer chunks than prepared checksum facts",
                                     static_cast<std::uint32_t>(prepared_index),
                                     prepared_index < result.prepared_chunks.size()
                                         ? result.prepared_chunks[prepared_index].offset
                                         : 0);
                                return result;
                            }
                        }

                        if (prepared_index >= result.prepared_chunks.size())
                        {
                            reader.Close();
                            BuildCleanupCandidates(&result,
                                                   durable_chunks,
                                                   uncertain_cleanup_possible);
                            Fail(&result,
                                 ObjectTransferStatusCode::kConflict,
                                 "upload second pass observed more chunks than prepared checksum facts",
                                 chunk.chunk_index,
                                 chunk.offset);
                            return result;
                        }

                        const auto &prepared_chunk = result.prepared_chunks[prepared_index];
                        if (chunk.chunk_index != prepared_chunk.chunk_index ||
                            chunk.offset != prepared_chunk.offset)
                        {
                            reader.Close();
                            BuildCleanupCandidates(&result,
                                                   durable_chunks,
                                                   uncertain_cleanup_possible);
                            Fail(&result,
                                 ObjectTransferStatusCode::kConflict,
                                 "upload second pass chunk order does not match prepared checksum facts",
                                 chunk.chunk_index,
                                 chunk.offset);
                            return result;
                        }
                        if (static_cast<std::uint64_t>(chunk.payload.size()) !=
                            prepared_chunk.size)
                        {
                            reader.Close();
                            BuildCleanupCandidates(&result,
                                                   durable_chunks,
                                                   uncertain_cleanup_possible);
                            Fail(&result,
                                 ObjectTransferStatusCode::kConflict,
                                 "upload second pass chunk size does not match prepared checksum facts",
                                 chunk.chunk_index,
                                 chunk.offset);
                            return result;
                        }

                        ChunkChecksum verified_checksum;
                        std::string verify_error;
                        const auto verify_status = VerifyChunkChecksum(
                            chunk.payload,
                            prepared_chunk.checksum,
                            &verified_checksum,
                            &verify_error);
                        if (verify_status != StorageNodeStatusCode::kOk)
                        {
                            reader.Close();
                            BuildCleanupCandidates(&result,
                                                   durable_chunks,
                                                   uncertain_cleanup_possible);
                            Fail(&result,
                                 verify_status ==
                                         StorageNodeStatusCode::kChecksumMismatch
                                     ? ObjectTransferStatusCode::kChecksumMismatch
                                     : ObjectTransferStatusCode::kInternalError,
                                 "upload second pass chunk checksum verification failed: " +
                                     verify_error,
                                 chunk.chunk_index,
                                 chunk.offset);
                            return result;
                        }

                        std::string identity_error;
                        const ChunkIdentity identity = BuildChunkIdentity(
                            prepared_chunk,
                            resolved_object_id,
                            result.write_plan->version,
                            &identity_error);
                        if (identity.chunk_id.empty())
                        {
                            reader.Close();
                            BuildCleanupCandidates(&result,
                                                   durable_chunks,
                                                   uncertain_cleanup_possible);
                            Fail(&result,
                                 ObjectTransferStatusCode::kInternalError,
                                 "failed to build upload chunk identity: " +
                                     identity_error,
                                 chunk.chunk_index,
                                 chunk.offset);
                            return result;
                        }

                        std::string resolve_targets_error;
                        const auto desired_replica_count =
                            ResolveDesiredReplicaCount(prepared_chunk.chunk_index);
                        const auto minimum_successful_writes =
                            ResolveMinimumSuccessfulWrites(prepared_chunk.chunk_index);
                        const auto chunk_targets = ResolveChunkTargets(
                            prepared_chunk,
                            storage_targets,
                            desired_replica_count,
                            &resolve_targets_error);
                        if (chunk_targets.empty())
                        {
                            reader.Close();
                            BuildCleanupCandidates(&result,
                                                   durable_chunks,
                                                   uncertain_cleanup_possible);
                            Fail(&result,
                                 ObjectTransferStatusCode::kDiscoveryUnavailable,
                                 std::move(resolve_targets_error),
                                 chunk.chunk_index,
                                 chunk.offset,
                                 identity.chunk_id);
                            return result;
                        }

                        std::vector<StorageNodeId> durable_replicas;
                        durable_replicas.reserve(chunk_targets.size());
                        StorageTransferWriteResult first_durable_result;
                        bool have_durable_result = false;
                        ObjectTransferStatusCode last_failure_status =
                            ObjectTransferStatusCode::kStorageRejected;
                        std::string last_failure_message =
                            "StorageNode WriteChunk did not reach minimum_successful_writes";
                        StorageTransferTarget last_failure_target;
                        bool last_failure_retryable = false;

                        for (const auto &target : chunk_targets)
                        {
                            const auto write_result = storage_client_->WriteChunk(
                                {.request_id = request_.request_id + "/chunk-" +
                                                 std::to_string(prepared_chunk.chunk_index) +
                                                 "/node-" + target.node_id,
                                 .target = target,
                                 .identity = identity,
                                 .offset = prepared_chunk.offset,
                                 .expected_size = prepared_chunk.size,
                                 .expected_checksum = prepared_chunk.checksum,
                                 .payload = chunk.payload});
                            AppendStorageWriteDiagnostic(request_.request_id,
                                                         write_result,
                                                         identity,
                                                         prepared_chunk.chunk_index,
                                                         prepared_chunk.offset,
                                                         &result.diagnostics);

                            if (write_result.ok())
                            {
                                if (!have_durable_result)
                                {
                                    first_durable_result = write_result;
                                    have_durable_result = true;
                                }
                                durable_replicas.push_back(
                                    write_result.target.node_id.empty()
                                        ? target.node_id
                                        : write_result.target.node_id);
                                continue;
                            }

                            last_failure_status = MapStorageStatus(write_result.status);
                            last_failure_message =
                                "StorageNode WriteChunk failed: " +
                                write_result.error_detail;
                            last_failure_target = write_result.target.endpoint.empty()
                                                      ? target
                                                      : write_result.target;
                            last_failure_retryable = write_result.retryable;
                            uncertain_cleanup_possible =
                                uncertain_cleanup_possible || write_result.retryable;
                        }

                        if (durable_replicas.size() < minimum_successful_writes)
                        {
                            reader.Close();
                            if (have_durable_result)
                            {
                                durable_chunks.push_back(BuildDurableChunkFacts(
                                    identity,
                                    prepared_chunk.size,
                                    prepared_chunk.checksum,
                                    first_durable_result,
                                    durable_replicas));
                            }
                            BuildCleanupCandidates(
                                &result,
                                durable_chunks,
                                uncertain_cleanup_possible,
                                last_failure_retryable
                                    ? "upload failure left uncertain chunk placement facts; cleanup_candidate_possible stays true even if candidate list is partial"
                                    : std::string());
                            Fail(&result,
                                 last_failure_status,
                                 "chunk " + identity.chunk_id +
                                     " did not reach minimum_successful_writes=" +
                                     std::to_string(minimum_successful_writes) +
                                     "; " + last_failure_message,
                                 prepared_chunk.chunk_index,
                                 prepared_chunk.offset,
                                 identity.chunk_id,
                                 last_failure_target.node_id,
                                 last_failure_target.endpoint,
                                 last_failure_retryable);
                            return result;
                        }

                        auto durable_chunk = BuildDurableChunkFacts(
                            identity,
                            prepared_chunk.size,
                            prepared_chunk.checksum,
                            first_durable_result,
                            durable_replicas);
                        result.committed_chunks.push_back(durable_chunk);
                        durable_chunks.push_back(std::move(durable_chunk));
                        ++prepared_index;

                        if (chunk.last_chunk ||
                            (chunk.eof &&
                             prepared_index == result.prepared_chunks.size()))
                        {
                            break;
                        }
                    }
                    reader.Close();
                }

                SetStage(ObjectTransferStage::kCommittingObject);
                mutable_snapshot().metadata_commit_attempted = true;
                result.commit_attempted = true;
                result.session = Snapshot();

                const auto commit_call = discovered_metadata_client->CommitObject(
                    {.request_id = request_.request_id,
                     .bucket = request_.bucket,
                     .object_key = request_.object_key,
                     .object_id = result.write_plan->object_id,
                     .version = result.write_plan->version,
                     .object_checksum = finalize_result.object_checksum,
                     .committed_chunks = result.committed_chunks,
                     .client_time_unix_ms = request_.client_time_unix_ms});
                AppendMetadataDiagnostics(commit_call.result.diagnostics,
                                          &result.diagnostics);
                if (!commit_call.transport_ok() || !commit_call.result.ok() ||
                    !commit_call.result.committed)
                {
                    BuildCleanupCandidates(
                        &result,
                        durable_chunks,
                        !durable_chunks.empty(),
                        durable_chunks.empty()
                            ? std::string()
                            : "CommitObject failed after chunk writes; durable chunks remain cleanup candidates until metadata authority confirms otherwise");
                    Fail(&result,
                         !commit_call.transport_ok()
                             ? ObjectTransferStatusCode::kMetadataRejected
                             : MapMetadataStatus(commit_call.result.summary.status),
                         !commit_call.transport_ok()
                             ? "Metadata CommitObject RPC failed: " +
                                   commit_call.rpc.grpc_error_message
                             : "Metadata CommitObject failed: " +
                                   commit_call.result.summary.message);
                    return result;
                }

                result.committed = true;
                mutable_snapshot().committed_visible = true;
                if (commit_call.result.committed_manifest.has_value())
                {
                    result.committed_manifest =
                        commit_call.result.committed_manifest;
                }
                else
                {
                    result.committed_manifest = BuildFallbackCommittedManifest(
                        *result.write_plan,
                        finalize_result.object_checksum,
                        result.committed_chunks,
                        request_.client_time_unix_ms);
                }
                result.cleanup_candidates.clear();
                result.cleanup_candidate_possible = false;

                MarkCompleted();
                result.status = ObjectTransferStatusCode::kOk;
                result.session = Snapshot();
                result.diagnostics.push_back(
                    MakeDiagnostic(
                        ObjectTransferStatusCode::kOk,
                        "upload session completed bounded chunk writes and CommitObject; COMMITTED visibility still comes only from MetadataNode authority",
                        request_.request_id));
                return result;
            }

        private:
            static TransferSessionSnapshot MakeInitialSnapshot(
                const UploadObjectRequest &request)
            {
                TransferSessionSnapshot snapshot;
                snapshot.direction = ObjectTransferDirection::kUpload;
                snapshot.stage = ObjectTransferStage::kPreparing;
                snapshot.request_id = request.request_id;
                snapshot.cluster_id = request.cluster_id;
                snapshot.bucket = request.bucket;
                snapshot.object_key = request.object_key;
                snapshot.object_id = request.object_id;
                snapshot.source_path = request.source_path;
                snapshot.chunk_size = request.chunk_size;
                snapshot.concurrency = request.concurrency;
                return snapshot;
            }

            [[nodiscard]] ObjectTransferStatusCode ValidateRequest(
                std::string *error_detail) const
            {
                if (request_.request_id.empty())
                {
                    SetErrorDetail(error_detail, "request_id is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.cluster_id.empty())
                {
                    SetErrorDetail(error_detail, "cluster_id is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.bucket.empty())
                {
                    SetErrorDetail(error_detail, "bucket is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.object_key.empty())
                {
                    SetErrorDetail(error_detail, "object_key is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.source_path.empty())
                {
                    SetErrorDetail(error_detail, "source_path is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.chunk_size == 0)
                {
                    SetErrorDetail(error_detail, "chunk_size must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.concurrency == 0)
                {
                    SetErrorDetail(error_detail, "concurrency must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.desired_replica_count == 0)
                {
                    SetErrorDetail(error_detail,
                                   "desired_replica_count must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.minimum_successful_writes == 0 ||
                    request_.minimum_successful_writes >
                        request_.desired_replica_count)
                {
                    SetErrorDetail(error_detail,
                                   "minimum_successful_writes must be in [1, desired_replica_count]");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                return ObjectTransferStatusCode::kOk;
            }

            [[nodiscard]] std::optional<std::string> ValidateExpectedObjectChecksum(
                const TransferObjectChecksumFacts &actual,
                const TransferObjectChecksumFacts &expected) const
            {
                if (expected.size != 0 && actual.size != expected.size)
                {
                    return "object size does not match expected checksum facts";
                }
                if (expected.checksum.IsSet())
                {
                    if (actual.checksum.algorithm != expected.checksum.algorithm ||
                        actual.checksum.value != expected.checksum.value)
                    {
                        return "object checksum does not match expected checksum facts";
                    }
                }
                if (!expected.etag.empty() && actual.etag != expected.etag)
                {
                    return "object etag does not match expected checksum facts";
                }
                return std::nullopt;
            }

            void BuildCleanupCandidates(
                UploadObjectResult *result,
                const std::vector<TransferCommittedChunk> &durable_chunks,
                const bool uncertain_cleanup_possible,
                const std::string &uncertain_reason = std::string()) const
            {
                if (result == nullptr)
                {
                    return;
                }

                if (!durable_chunks.empty())
                {
                    const auto object_id =
                        !snapshot_ref().object_id.empty()
                            ? snapshot_ref().object_id
                            : (result->write_plan.has_value()
                                       ? result->write_plan->object_id
                                       : request_.object_id);
                    const auto version =
                        snapshot_ref().version != 0
                            ? snapshot_ref().version
                            : (result->write_plan.has_value()
                                   ? result->write_plan->version
                                   : 0);
                    const auto created_at_unix_ms =
                        result->write_plan.has_value()
                            ? result->write_plan->created_at_unix_ms
                            : request_.client_time_unix_ms;
                    result->cleanup_candidates =
                        BuildFailedUploadCleanupCandidatesFromChunks(
                            request_.bucket,
                            request_.object_key,
                            object_id,
                            version,
                            created_at_unix_ms,
                            durable_chunks);
                }

                result->cleanup_candidate_possible =
                    uncertain_cleanup_possible || !result->cleanup_candidates.empty();
                if (uncertain_cleanup_possible && !uncertain_reason.empty())
                {
                    auto diagnostic = MakeDiagnostic(
                        ObjectTransferStatusCode::kStorageRejected,
                        uncertain_reason,
                        request_.request_id);
                    result->diagnostics.push_back(std::move(diagnostic));
                }
            }

            [[nodiscard]] std::vector<StorageTransferTarget> ResolveChunkTargets(
                const TransferPreparedChunk &chunk,
                const std::unordered_map<StorageNodeId, StorageTransferTarget> &storage_targets,
                const std::uint32_t desired_replica_count,
                std::string *error_detail) const
            {
                std::vector<StorageTransferTarget> targets;
                if (result_write_plan_has_chunk_targets(chunk))
                {
                    const auto *chunk_plan = FindChunkPlan(chunk.chunk_index);
                    if (chunk_plan != nullptr)
                    {
                        for (const auto &node_id : chunk_plan->candidate_nodes)
                        {
                            const auto it = storage_targets.find(node_id);
                            if (it == storage_targets.end() ||
                                it->second.endpoint.empty())
                            {
                                continue;
                            }
                            const auto duplicate =
                                std::find_if(targets.begin(),
                                             targets.end(),
                                             [&](const StorageTransferTarget &target)
                                             {
                                                 return target.node_id == it->second.node_id;
                                             });
                            if (duplicate == targets.end())
                            {
                                targets.push_back(it->second);
                            }
                        }
                    }
                }

                if (targets.empty())
                {
                    targets = SortedStorageTargets(storage_targets);
                }

                if (targets.size() > desired_replica_count)
                {
                    targets.resize(desired_replica_count);
                }
                if (targets.size() < desired_replica_count)
                {
                    SetErrorDetail(
                        error_detail,
                        "ViewNode returned fewer writable StorageNode targets than desired_replica_count");
                    return {};
                }
                return targets;
            }

            [[nodiscard]] const TransferChunkPlan *FindChunkPlan(
                const std::uint32_t chunk_index) const
            {
                if (!current_write_plan_.has_value())
                {
                    return nullptr;
                }
                for (const auto &chunk_plan : current_write_plan_->chunks)
                {
                    if (chunk_plan.identity.chunk_index == chunk_index)
                    {
                        return &chunk_plan;
                    }
                }
                return nullptr;
            }

            [[nodiscard]] bool result_write_plan_has_chunk_targets(
                const TransferPreparedChunk &chunk) const
            {
                const auto *chunk_plan = FindChunkPlan(chunk.chunk_index);
                return chunk_plan != nullptr && !chunk_plan->candidate_nodes.empty();
            }

            [[nodiscard]] std::uint32_t ResolveMinimumSuccessfulWrites(
                const std::uint32_t chunk_index) const
            {
                const auto *chunk_plan = FindChunkPlan(chunk_index);
                if (chunk_plan != nullptr && chunk_plan->minimum_successful_writes != 0)
                {
                    return chunk_plan->minimum_successful_writes;
                }
                return request_.minimum_successful_writes;
            }

            [[nodiscard]] std::uint32_t ResolveDesiredReplicaCount(
                const std::uint32_t chunk_index) const
            {
                const auto *chunk_plan = FindChunkPlan(chunk_index);
                if (chunk_plan != nullptr && chunk_plan->required_replica_count != 0)
                {
                    return chunk_plan->required_replica_count;
                }
                return request_.desired_replica_count;
            }

            [[nodiscard]] ChunkIdentity BuildChunkIdentity(
                const TransferPreparedChunk &chunk,
                const std::string &object_id,
                const std::uint64_t version,
                std::string *error_detail) const
            {
                ChunkIdentity identity;
                identity.object_id = object_id;
                identity.version = version;
                identity.chunk_index = chunk.chunk_index;
                identity.offset = chunk.offset;
                const auto status = MakeChunkId(identity.object_id,
                                                identity.version,
                                                identity.chunk_index,
                                                &identity.chunk_id,
                                                error_detail);
                if (status != StorageNodeStatusCode::kOk)
                {
                    return {};
                }
                return identity;
            }

            void Fail(UploadObjectResult *result,
                      const ObjectTransferStatusCode status,
                      std::string message,
                      const std::uint32_t chunk_index = 0,
                      const std::uint64_t offset = 0,
                      const ChunkId &chunk_id = ChunkId(),
                      const std::string &node_id = std::string(),
                      const std::string &endpoint = std::string(),
                      const std::optional<bool> retryable_override = std::nullopt)
            {
                if (result == nullptr)
                {
                    return;
                }

                TransferFailureSummary failure;
                failure.status = status;
                failure.error_detail = message;
                failure.node_id = node_id;
                failure.endpoint = endpoint;
                failure.chunk_id = chunk_id;
                failure.chunk_index = chunk_index;
                failure.offset = offset;
                failure.retryable = retryable_override.value_or(
                    status == ObjectTransferStatusCode::kIoError ||
                    status == ObjectTransferStatusCode::kTimeout ||
                    status == ObjectTransferStatusCode::kDiscoveryUnavailable ||
                    status == ObjectTransferStatusCode::kMetadataNotLeader);
                SetFailure(failure);

                result->status = status;
                result->error_detail = std::move(message);
                result->session = Snapshot();
                auto diagnostic = MakeDiagnostic(result->status,
                                                 result->error_detail,
                                                 request_.request_id,
                                                 chunk_index,
                                                 offset,
                                                 chunk_id,
                                                 failure.retryable);
                diagnostic.node_id = node_id;
                diagnostic.endpoint = endpoint;
                result->diagnostics.push_back(std::move(diagnostic));
            }

            UploadObjectRequest request_;
            std::shared_ptr<MetadataTransferClient> metadata_client_;
            std::shared_ptr<StorageTransferClient> storage_client_;
            std::shared_ptr<viewdemo::ViewNodeClient> view_client_;
            std::optional<TransferWritePlan> current_write_plan_;
        };

        class BasicDownloadTransferSession final : public DownloadTransferSession,
                                                   private BasicTransferSession
        {
        public:
            BasicDownloadTransferSession(
                DownloadObjectRequest request,
                std::shared_ptr<MetadataTransferClient> metadata_client,
                std::shared_ptr<StorageTransferClient> storage_client,
                std::shared_ptr<viewdemo::ViewNodeClient> view_client)
                : BasicTransferSession(MakeInitialSnapshot(request)),
                  request_(std::move(request)),
                  metadata_client_(std::move(metadata_client)),
                  storage_client_(std::move(storage_client)),
                  view_client_(std::move(view_client))
            {
            }

            [[nodiscard]] ObjectTransferDirection direction() const override
            {
                return ObjectTransferDirection::kDownload;
            }

            [[nodiscard]] TransferSessionSnapshot Snapshot() const override
            {
                return snapshot_ref();
            }

            [[nodiscard]] bool finished() const override
            {
                return BasicTransferSession::finished();
            }

            [[nodiscard]] const DownloadObjectRequest &request() const override
            {
                return request_;
            }

            DownloadObjectResult Execute(
                TransferChecksumState &checksum_state) override
            {
                DownloadObjectResult result;
                result.session = Snapshot();

                const auto validation_status = ValidateRequest(&result.error_detail);
                if (validation_status != ObjectTransferStatusCode::kOk)
                {
                    Fail(&result, validation_status, result.error_detail);
                    return result;
                }

                SetStage(ObjectTransferStage::kDiscoveringMetadata);
                result.session = Snapshot();

                ObjectTransferStatusCode discovery_status =
                    ObjectTransferStatusCode::kOk;
                std::string discovery_error;
                const auto discovered_metadata_client = DiscoverMetadataClient(
                    request_.request_id,
                    request_.cluster_id,
                    metadata_client_,
                    view_client_,
                    &result.diagnostics,
                    &discovery_status,
                    &discovery_error);
                if (discovered_metadata_client == nullptr)
                {
                    TransferFailureSummary failure;
                    failure.status = discovery_status;
                    failure.error_detail = discovery_error;
                    failure.retryable = discovery_status ==
                                            ObjectTransferStatusCode::kDiscoveryUnavailable;
                    SetFailure(failure);
                    result.status = discovery_status;
                    result.error_detail = std::move(discovery_error);
                    result.session = Snapshot();
                    return result;
                }

                SetStage(ObjectTransferStage::kFetchingManifest);
                result.session = Snapshot();

                const auto manifest_call = discovered_metadata_client->GetObjectManifest(
                    {.request_id = request_.request_id,
                     .bucket = request_.bucket,
                     .object_key = request_.object_key,
                     .object_id = request_.object_id,
                     .version = request_.version,
                     .require_committed_visible = true});
                AppendMetadataDiagnostics(manifest_call.result.diagnostics,
                                          &result.diagnostics);
                if (!manifest_call.transport_ok() || !manifest_call.result.ok() ||
                    !manifest_call.result.manifest.has_value())
                {
                    const auto status = !manifest_call.transport_ok()
                                            ? ObjectTransferStatusCode::kMetadataRejected
                                            : MapMetadataStatus(
                                                  manifest_call.result.summary.status);
                    const std::string error =
                        !manifest_call.transport_ok()
                            ? "Metadata GetObjectManifest RPC failed: " +
                                  manifest_call.rpc.grpc_error_message
                            : "Metadata GetObjectManifest failed: " +
                                  manifest_call.result.summary.message;
                    TransferFailureSummary failure;
                    failure.status = status;
                    failure.error_detail = error;
                    failure.retryable = status ==
                                            ObjectTransferStatusCode::kMetadataNotLeader;
                    SetFailure(failure);
                    result.status = status;
                    result.error_detail = error;
                    result.session = Snapshot();
                    return result;
                }

                result.manifest = manifest_call.result.manifest;
                mutable_snapshot().object_id = result.manifest->object_id;
                mutable_snapshot().version = result.manifest->version;
                mutable_snapshot().committed_visible = true;
                mutable_snapshot().total_bytes =
                    result.manifest->object_checksum.size;
                mutable_snapshot().total_chunks = static_cast<std::uint32_t>(
                    result.manifest->chunks.size());

                std::vector<TransferCommittedChunk> ordered_chunks;
                if (const auto layout_error = ValidateManifestLayout(
                        *result.manifest,
                        &ordered_chunks);
                    layout_error.has_value())
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kConflict,
                         "COMMITTED manifest layout is invalid: " + *layout_error);
                    return result;
                }

                std::string storage_discovery_error;
                const auto storage_targets = DiscoverStorageTargets(
                    request_.request_id,
                    request_.cluster_id,
                    0,
                    0,
                    false,
                    view_client_,
                    &result.diagnostics,
                    &discovery_status,
                    &storage_discovery_error);
                if (storage_targets.empty())
                {
                    Fail(&result, discovery_status, std::move(storage_discovery_error));
                    return result;
                }

                const auto temp_path = MakeTemporaryDownloadPath(
                    request_.destination_path,
                    request_.request_id);
                const auto cleanup_temp = [&temp_path]()
                {
                    RemovePathIfExists(temp_path);
                };

                std::error_code path_ec;
                const auto parent_path = request_.destination_path.parent_path();
                if (!parent_path.empty() &&
                    !std::filesystem::exists(parent_path, path_ec))
                {
                    Fail(&result,
                         path_ec ? ObjectTransferStatusCode::kIoError
                                 : ObjectTransferStatusCode::kNotFound,
                         path_ec ? "failed to access destination directory: " +
                                       path_ec.message()
                                 : "destination directory does not exist");
                    return result;
                }
                if (path_ec)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kIoError,
                         "failed to access destination directory: " +
                             path_ec.message());
                    return result;
                }
                if (std::filesystem::exists(request_.destination_path, path_ec))
                {
                    if (path_ec)
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kIoError,
                             "failed to inspect destination_path: " +
                                 path_ec.message());
                    }
                    else
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kConflict,
                             "destination_path already exists");
                    }
                    return result;
                }
                RemovePathIfExists(temp_path);

                checksum_state.Reset();
                SetStage(ObjectTransferStage::kDownloadingChunks);
                result.session = Snapshot();

                std::ofstream output(temp_path,
                                     std::ios::binary | std::ios::trunc);
                if (!output.is_open())
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kIoError,
                         "failed to open temporary download file for writing");
                    cleanup_temp();
                    return result;
                }
                const auto close_output = [&output]()
                {
                    if (output.is_open())
                    {
                        output.close();
                    }
                };

                for (const auto &chunk : ordered_chunks)
                {
                    const auto target = ResolveReplicaTarget(chunk, storage_targets);
                    if (!target.has_value())
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kDiscoveryUnavailable,
                             "ViewNode did not provide a readable endpoint for manifest replica_nodes",
                             chunk.identity.chunk_index,
                             chunk.identity.offset);
                        close_output();
                        cleanup_temp();
                        return result;
                    }

                    const auto read_result = storage_client_->ReadChunk(
                        {.request_id = request_.request_id,
                         .target = *target,
                         .identity = chunk.identity,
                         .expected_checksum = chunk.checksum,
                         .verify_checksum = true});
                    if (!read_result.ok())
                    {
                        const auto status = MapStorageStatus(read_result.status);
                        Fail(&result,
                             status,
                             "StorageNode ReadChunk failed: " +
                                 read_result.error_detail,
                             chunk.identity.chunk_index,
                             chunk.identity.offset,
                             target->node_id,
                             target->endpoint,
                             chunk.identity.chunk_id,
                             read_result.retryable);
                        close_output();
                        cleanup_temp();
                        return result;
                    }

                    if (static_cast<std::uint64_t>(read_result.payload.size()) !=
                        chunk.size)
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kConflict,
                             "StorageNode returned payload size inconsistent with COMMITTED manifest",
                             chunk.identity.chunk_index,
                             chunk.identity.offset,
                             target->node_id,
                             target->endpoint,
                             chunk.identity.chunk_id,
                             false);
                        close_output();
                        cleanup_temp();
                        return result;
                    }

                    const auto checksum_update = checksum_state.Append(
                        {.chunk_index = chunk.identity.chunk_index,
                         .offset = chunk.identity.offset,
                         .payload = read_result.payload,
                         .expected_chunk_checksum = chunk.checksum});
                    if (!checksum_update.ok())
                    {
                        Fail(&result,
                             checksum_update.status,
                             checksum_update.error_detail,
                             chunk.identity.chunk_index,
                             chunk.identity.offset,
                             target->node_id,
                             target->endpoint,
                             chunk.identity.chunk_id,
                             false);
                        close_output();
                        cleanup_temp();
                        return result;
                    }

                    output.seekp(static_cast<std::streamoff>(chunk.identity.offset),
                                 std::ios::beg);
                    if (!output)
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kIoError,
                             "failed to seek temporary download file",
                             chunk.identity.chunk_index,
                             chunk.identity.offset,
                             target->node_id,
                             target->endpoint,
                             chunk.identity.chunk_id,
                             false);
                        close_output();
                        cleanup_temp();
                        return result;
                    }

                    output.write(read_result.payload.data(),
                                 static_cast<std::streamsize>(
                                     read_result.payload.size()));
                    if (!output)
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kIoError,
                             "failed to write chunk payload into temporary download file",
                             chunk.identity.chunk_index,
                             chunk.identity.offset,
                             target->node_id,
                             target->endpoint,
                             chunk.identity.chunk_id,
                             false);
                        close_output();
                        cleanup_temp();
                        return result;
                    }

                    mutable_snapshot().bytes_completed += chunk.size;
                    mutable_snapshot().chunks_completed += 1;
                    result.session = Snapshot();
                }

                output.flush();
                if (!output)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kIoError,
                         "failed to flush temporary download file");
                    close_output();
                    cleanup_temp();
                    return result;
                }
                close_output();
                if (!output)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kIoError,
                         "failed to finalize temporary download file");
                    cleanup_temp();
                    return result;
                }

                SetStage(ObjectTransferStage::kVerifyingChecksums);
                result.session = Snapshot();

                const auto finalize_result = checksum_state.Finalize();
                if (!finalize_result.ok())
                {
                    Fail(&result,
                         finalize_result.status,
                         finalize_result.error_detail);
                    close_output();
                    cleanup_temp();
                    return result;
                }

                result.downloaded_object_checksum = finalize_result.object_checksum;
                if (const auto mismatch_reason = ValidateObjectChecksumFacts(
                        finalize_result.object_checksum,
                        result.manifest->object_checksum);
                    mismatch_reason.has_value())
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kChecksumMismatch,
                         *mismatch_reason);
                    close_output();
                    cleanup_temp();
                    return result;
                }
                if (request_.expected_object_checksum.has_value())
                {
                    if (const auto mismatch_reason = ValidateObjectChecksumFacts(
                            finalize_result.object_checksum,
                            *request_.expected_object_checksum);
                        mismatch_reason.has_value())
                    {
                        Fail(&result,
                             ObjectTransferStatusCode::kChecksumMismatch,
                             *mismatch_reason);
                        close_output();
                        cleanup_temp();
                        return result;
                    }
                }

                std::filesystem::rename(temp_path,
                                        request_.destination_path,
                                        path_ec);
                if (path_ec)
                {
                    Fail(&result,
                         ObjectTransferStatusCode::kIoError,
                         "failed to publish temporary download file: " +
                             path_ec.message());
                    cleanup_temp();
                    return result;
                }

                mutable_snapshot().final_checksum_verified = true;
                result.checksum_verified = true;
                result.status = ObjectTransferStatusCode::kOk;
                MarkCompleted();
                result.session = Snapshot();
                result.diagnostics.push_back(
                    MakeDiagnostic(
                        ObjectTransferStatusCode::kOk,
                        "download reconstructed from MetadataNode COMMITTED manifest with per-chunk and final object checksum verification",
                        request_.request_id));
                return result;
            }

        private:
            static TransferSessionSnapshot MakeInitialSnapshot(
                const DownloadObjectRequest &request)
            {
                TransferSessionSnapshot snapshot;
                snapshot.direction = ObjectTransferDirection::kDownload;
                snapshot.stage = ObjectTransferStage::kPreparing;
                snapshot.request_id = request.request_id;
                snapshot.cluster_id = request.cluster_id;
                snapshot.bucket = request.bucket;
                snapshot.object_key = request.object_key;
                snapshot.object_id = request.object_id;
                snapshot.version = request.version.value_or(0);
                snapshot.destination_path = request.destination_path;
                snapshot.concurrency = request.concurrency;
                return snapshot;
            }

            [[nodiscard]] ObjectTransferStatusCode ValidateRequest(
                std::string *error_detail) const
            {
                if (request_.request_id.empty())
                {
                    SetErrorDetail(error_detail, "request_id is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.cluster_id.empty())
                {
                    SetErrorDetail(error_detail, "cluster_id is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.bucket.empty())
                {
                    SetErrorDetail(error_detail, "bucket is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.object_key.empty())
                {
                    SetErrorDetail(error_detail, "object_key is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.destination_path.empty())
                {
                    SetErrorDetail(error_detail, "destination_path is required");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (request_.concurrency == 0)
                {
                    SetErrorDetail(error_detail, "concurrency must be greater than 0");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                if (storage_client_ == nullptr)
                {
                    SetErrorDetail(error_detail,
                                   "storage_client is required for manifest-driven download");
                    return ObjectTransferStatusCode::kInvalidArgument;
                }
                return ObjectTransferStatusCode::kOk;
            }

            void Fail(DownloadObjectResult *result,
                      const ObjectTransferStatusCode status,
                      std::string message,
                      const std::uint32_t chunk_index = 0,
                      const std::uint64_t offset = 0,
                      std::string node_id = {},
                      std::string endpoint = {},
                      ChunkId chunk_id = {},
                      const bool retryable = false)
            {
                if (result == nullptr)
                {
                    return;
                }

                TransferFailureSummary failure;
                failure.status = status;
                failure.error_detail = message;
                failure.node_id = node_id;
                failure.endpoint = endpoint;
                failure.chunk_id = chunk_id;
                failure.chunk_index = chunk_index;
                failure.offset = offset;
                failure.retryable = retryable;
                SetFailure(failure);

                result->status = status;
                result->error_detail = std::move(message);
                result->session = Snapshot();

                auto diagnostic = MakeDiagnostic(result->status,
                                                 result->error_detail,
                                                 request_.request_id,
                                                 chunk_index,
                                                 offset,
                                                 chunk_id,
                                                 retryable);
                diagnostic.node_id = std::move(node_id);
                diagnostic.endpoint = std::move(endpoint);
                result->diagnostics.push_back(std::move(diagnostic));
            }

            DownloadObjectRequest request_;
            std::shared_ptr<MetadataTransferClient> metadata_client_;
            std::shared_ptr<StorageTransferClient> storage_client_;
            std::shared_ptr<viewdemo::ViewNodeClient> view_client_;
        };
    } // namespace

    TransferChunkReader::~TransferChunkReader() = default;

    std::unique_ptr<TransferChunkReader> CreateFileTransferChunkReader()
    {
        return std::make_unique<FileTransferChunkReader>();
    }

    TransferChecksumState::~TransferChecksumState() = default;

    std::unique_ptr<TransferChecksumState> CreateTransferChecksumState()
    {
        return std::make_unique<IncrementalTransferChecksumState>();
    }

    TransferSession::~TransferSession() = default;

    UploadTransferSession::~UploadTransferSession() = default;

    DownloadTransferSession::~DownloadTransferSession() = default;

    ObjectTransfer::ObjectTransfer(
        std::shared_ptr<MetadataTransferClient> metadata_client,
        std::shared_ptr<StorageTransferClient> storage_client,
        std::shared_ptr<viewdemo::ViewNodeClient> view_client)
        : metadata_client_(std::move(metadata_client)),
          storage_client_(std::move(storage_client)),
          view_client_(std::move(view_client))
    {
    }

    ObjectTransfer::~ObjectTransfer() = default;

    ObjectTransfer::ObjectTransfer(ObjectTransfer &&) noexcept = default;

    ObjectTransfer &ObjectTransfer::operator=(ObjectTransfer &&) noexcept = default;

    std::unique_ptr<UploadTransferSession> ObjectTransfer::StartUploadSession(
        const UploadObjectRequest &request) const
    {
        return std::make_unique<BasicUploadTransferSession>(request,
                                                            metadata_client_,
                                                            storage_client_,
                                                            view_client_);
    }

    std::unique_ptr<DownloadTransferSession> ObjectTransfer::StartDownloadSession(
        const DownloadObjectRequest &request) const
    {
        return std::make_unique<BasicDownloadTransferSession>(request,
                                                              metadata_client_,
                                                              storage_client_,
                                                              view_client_);
    }

    const std::shared_ptr<MetadataTransferClient> &ObjectTransfer::metadata_client()
        const
    {
        return metadata_client_;
    }

    const std::shared_ptr<StorageTransferClient> &ObjectTransfer::storage_client()
        const
    {
        return storage_client_;
    }

    const std::shared_ptr<viewdemo::ViewNodeClient> &ObjectTransfer::view_client()
        const
    {
        return view_client_;
    }
} // namespace storedemo
