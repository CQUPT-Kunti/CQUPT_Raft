#include "store/node/storage_node_service.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "store/chunk/chunk_store.h"
#include "store/node/storage_node_registry.h"

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

        storage::StorageNodeHealth ToProtoNodeHealth(const StorageNodeHealth health)
        {
            switch (health)
            {
            case StorageNodeHealth::kHealthy:
                return storage::STORAGE_NODE_HEALTH_HEALTHY;
            case StorageNodeHealth::kDegraded:
                return storage::STORAGE_NODE_HEALTH_DEGRADED;
            case StorageNodeHealth::kReadOnly:
                return storage::STORAGE_NODE_HEALTH_READ_ONLY;
            case StorageNodeHealth::kUnavailable:
                return storage::STORAGE_NODE_HEALTH_UNAVAILABLE;
            case StorageNodeHealth::kDraining:
                return storage::STORAGE_NODE_HEALTH_DRAINING;
            default:
                return storage::STORAGE_NODE_HEALTH_UNSPECIFIED;
            }
        }

        storage::StorageNodeDiskPressure ToProtoDiskPressure(
            const StorageNodeDiskPressure pressure)
        {
            switch (pressure)
            {
            case StorageNodeDiskPressure::kLow:
                return storage::STORAGE_NODE_DISK_PRESSURE_LOW;
            case StorageNodeDiskPressure::kMedium:
                return storage::STORAGE_NODE_DISK_PRESSURE_MEDIUM;
            case StorageNodeDiskPressure::kHigh:
                return storage::STORAGE_NODE_DISK_PRESSURE_HIGH;
            case StorageNodeDiskPressure::kFull:
                return storage::STORAGE_NODE_DISK_PRESSURE_FULL;
            default:
                return storage::STORAGE_NODE_DISK_PRESSURE_UNSPECIFIED;
            }
        }

        storage::StorageNodeLivenessState ToProtoLiveness(
            const StorageNodeRegistryLiveness liveness)
        {
            switch (liveness)
            {
            case StorageNodeRegistryLiveness::kLive:
                return storage::STORAGE_NODE_LIVENESS_STATE_LIVE;
            case StorageNodeRegistryLiveness::kStale:
                return storage::STORAGE_NODE_LIVENESS_STATE_STALE;
            case StorageNodeRegistryLiveness::kDead:
                return storage::STORAGE_NODE_LIVENESS_STATE_DEAD;
            default:
                return storage::STORAGE_NODE_LIVENESS_STATE_UNSPECIFIED;
            }
        }

        StorageNodeStatusCode FromProtoNodeHealth(const storage::StorageNodeHealth health,
                                                  StorageNodeHealth *out_health,
                                                  std::string *error_detail)
        {
            if (out_health == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "health output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            switch (health)
            {
            case storage::STORAGE_NODE_HEALTH_UNSPECIFIED:
            case storage::STORAGE_NODE_HEALTH_HEALTHY:
                *out_health = StorageNodeHealth::kHealthy;
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_NODE_HEALTH_DEGRADED:
                *out_health = StorageNodeHealth::kDegraded;
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_NODE_HEALTH_READ_ONLY:
                *out_health = StorageNodeHealth::kReadOnly;
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_NODE_HEALTH_UNAVAILABLE:
                *out_health = StorageNodeHealth::kUnavailable;
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_NODE_HEALTH_DRAINING:
                *out_health = StorageNodeHealth::kDraining;
                return StorageNodeStatusCode::kOk;
            default:
                if (error_detail != nullptr)
                {
                    *error_detail = "storage node health is not supported";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
        }

        StorageNodeStatusCode FromProtoDiskPressure(
            const storage::StorageNodeDiskPressure pressure,
            StorageNodeDiskPressure *out_pressure,
            std::string *error_detail)
        {
            if (out_pressure == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "disk pressure output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            switch (pressure)
            {
            case storage::STORAGE_NODE_DISK_PRESSURE_UNSPECIFIED:
            case storage::STORAGE_NODE_DISK_PRESSURE_LOW:
                *out_pressure = StorageNodeDiskPressure::kLow;
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_NODE_DISK_PRESSURE_MEDIUM:
                *out_pressure = StorageNodeDiskPressure::kMedium;
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_NODE_DISK_PRESSURE_HIGH:
                *out_pressure = StorageNodeDiskPressure::kHigh;
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_NODE_DISK_PRESSURE_FULL:
                *out_pressure = StorageNodeDiskPressure::kFull;
                return StorageNodeStatusCode::kOk;
            default:
                if (error_detail != nullptr)
                {
                    *error_detail = "storage node disk pressure is not supported";
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

        void FillProtoCapacityReport(
            const StorageNodeRegistryCapacityFacts &capacity,
            storage::StorageNodeCapacityReport *out_capacity)
        {
            if (out_capacity == nullptr)
            {
                return;
            }

            out_capacity->set_total_capacity_bytes(capacity.total_capacity_bytes);
            out_capacity->set_used_capacity_bytes(capacity.used_capacity_bytes);
            out_capacity->set_available_capacity_bytes(capacity.available_capacity_bytes);
            out_capacity->set_chunk_count(capacity.chunk_count);
        }

        void FillProtoHealthReport(
            const StorageNodeRegistryHealthFacts &health,
            storage::StorageNodeHealthReport *out_health)
        {
            if (out_health == nullptr)
            {
                return;
            }

            out_health->set_health(ToProtoNodeHealth(health.health));
            out_health->set_disk_pressure(ToProtoDiskPressure(health.disk_pressure));
            out_health->set_io_error_count(health.io_error_count);
        }

        void FillProtoLoadReport(const StorageNodeRegistryLoadFacts &load,
                                 storage::StorageNodeLoadReport *out_load)
        {
            if (out_load == nullptr)
            {
                return;
            }

            out_load->set_active_reads(load.load.active_reads);
            out_load->set_active_writes(load.load.active_writes);
            out_load->set_queued_ops(load.load.queued_ops);
            out_load->set_write_admission_overloaded(load.write_admission_overloaded);
            out_load->set_read_admission_overloaded(load.read_admission_overloaded);
        }

        void FillProtoFacts(const StorageNodeRegistryFacts &facts,
                            storage::StorageNodeFacts *out_facts)
        {
            if (out_facts == nullptr)
            {
                return;
            }

            FillProtoCapacityReport(facts.capacity, out_facts->mutable_capacity());
            FillProtoHealthReport(facts.health, out_facts->mutable_health());
            FillProtoLoadReport(facts.load, out_facts->mutable_load());
            out_facts->mutable_failure_domain()->set_zone(facts.failure_domain.zone);
            out_facts->mutable_failure_domain()->set_rack(facts.failure_domain.rack);
        }

        void FillProtoRegistrySnapshot(
            const StorageNodeRegistryNodeSnapshot &snapshot,
            storage::StorageNodeRegistrySnapshot *out_snapshot)
        {
            if (out_snapshot == nullptr)
            {
                return;
            }

            out_snapshot->set_node_id(snapshot.node_id);
            out_snapshot->set_endpoint(snapshot.endpoint);
            out_snapshot->set_last_sequence(snapshot.last_sequence);
            out_snapshot->set_last_seen_unix_ms(snapshot.last_seen_unix_ms);
            out_snapshot->set_liveness(ToProtoLiveness(snapshot.liveness));
            FillProtoFacts(snapshot.facts, out_snapshot->mutable_facts());
        }

        StorageNodeStatusCode FromProtoCapacityReport(
            const storage::StorageNodeCapacityReport &proto_capacity,
            StorageNodeRegistryCapacityFacts *out_capacity,
            std::string *error_detail)
        {
            if (out_capacity == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "capacity output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            out_capacity->total_capacity_bytes = proto_capacity.total_capacity_bytes();
            out_capacity->used_capacity_bytes = proto_capacity.used_capacity_bytes();
            out_capacity->available_capacity_bytes = proto_capacity.available_capacity_bytes();
            out_capacity->chunk_count = proto_capacity.chunk_count();
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode FromProtoHealthReport(
            const storage::StorageNodeHealthReport &proto_health,
            StorageNodeRegistryHealthFacts *out_health,
            std::string *error_detail)
        {
            if (out_health == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "health output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            auto status = FromProtoNodeHealth(proto_health.health(),
                                              &out_health->health,
                                              error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            status = FromProtoDiskPressure(proto_health.disk_pressure(),
                                           &out_health->disk_pressure,
                                           error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            out_health->io_error_count = proto_health.io_error_count();
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode FromProtoLoadReport(const storage::StorageNodeLoadReport &proto_load,
                                                  StorageNodeRegistryLoadFacts *out_load,
                                                  std::string *error_detail)
        {
            if (out_load == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "load output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            out_load->load.active_reads = proto_load.active_reads();
            out_load->load.active_writes = proto_load.active_writes();
            out_load->load.queued_ops = proto_load.queued_ops();
            out_load->write_admission_overloaded = proto_load.write_admission_overloaded();
            out_load->read_admission_overloaded = proto_load.read_admission_overloaded();
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode FromProtoFacts(const storage::StorageNodeFacts &proto_facts,
                                             StorageNodeRegistryFacts *out_facts,
                                             std::string *error_detail)
        {
            if (out_facts == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "facts output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            auto status = FromProtoCapacityReport(proto_facts.capacity(),
                                                  &out_facts->capacity,
                                                  error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            status = FromProtoHealthReport(proto_facts.health(),
                                           &out_facts->health,
                                           error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            status = FromProtoLoadReport(proto_facts.load(), &out_facts->load, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            out_facts->failure_domain.zone = proto_facts.failure_domain().zone();
            out_facts->failure_domain.rack = proto_facts.failure_domain().rack();
            return StorageNodeStatusCode::kOk;
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

        std::string ResolveRegistrySummaryNodeId(
            const std::string_view request_node_id,
            const StorageNodeRegistryNodeSnapshot &snapshot)
        {
            if (!snapshot.node_id.empty())
            {
                return snapshot.node_id;
            }
            return std::string(request_node_id);
        }

        void FillRegistrySummary(const StorageNodeStatusCode status,
                                 const std::string_view error_detail,
                                 const std::string_view request_id,
                                 const std::string_view request_node_id,
                                 const StorageNodeRegistryNodeSnapshot &snapshot,
                                 storage::StorageNodeResponseSummary *summary)
        {
            if (summary == nullptr)
            {
                return;
            }

            summary->set_code(ToProtoStatusCode(status));
            summary->set_message(std::string(error_detail));
            summary->set_request_id(std::string(request_id));
            summary->set_node_id(ResolveRegistrySummaryNodeId(request_node_id, snapshot));
            summary->set_chunk_id("");
            summary->set_retry_after_ms(0);
        }

        RegisterStorageNodeResult MakeRegisterValidationResult(
            const StorageNodeStatusCode status,
            std::string error_detail)
        {
            RegisterStorageNodeResult result;
            result.status = status;
            result.error_detail = std::move(error_detail);
            return result;
        }

        StorageNodeRegistryUpdateResult MakeRegistryUpdateValidationResult(
            const StorageNodeStatusCode status,
            std::string error_detail)
        {
            StorageNodeRegistryUpdateResult result;
            result.status = status;
            result.error_detail = std::move(error_detail);
            return result;
        }

        WriteChunkResponse MakeValidationError(const StorageNodeStatusCode status,
                                               std::string error_detail)
        {
            WriteChunkResponse response;
            response.status = status;
            response.error_detail = std::move(error_detail);
            return response;
        }

        RegisterStorageNodeResult TranslateRegisterRequest(
            const storage::RegisterStorageNodeRequest &request,
            RegisterStorageNodeRequest *registry_request)
        {
            if (registry_request == nullptr)
            {
                return MakeRegisterValidationResult(
                    StorageNodeStatusCode::kInvalidArgument,
                    "registry register request output must not be null");
            }

            registry_request->node_id = request.node_id();
            registry_request->endpoint = request.endpoint();
            registry_request->observed_at_unix_ms = request.observed_at_unix_ms();

            std::string error_detail;
            const auto status =
                FromProtoFacts(request.facts(), &registry_request->facts, &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return MakeRegisterValidationResult(status, std::move(error_detail));
            }

            return {};
        }

        StorageNodeRegistryUpdateResult TranslateHeartbeatRequest(
            const storage::UpdateStorageNodeHeartbeatRequest &request,
            UpdateStorageNodeHeartbeatRequest *registry_request)
        {
            if (registry_request == nullptr)
            {
                return MakeRegistryUpdateValidationResult(
                    StorageNodeStatusCode::kInvalidArgument,
                    "registry heartbeat request output must not be null");
            }

            if (!request.has_heartbeat())
            {
                return MakeRegistryUpdateValidationResult(
                    StorageNodeStatusCode::kInvalidArgument,
                    "UpdateStorageNodeHeartbeat requires heartbeat");
            }

            const auto &heartbeat = request.heartbeat();
            registry_request->node_id = heartbeat.node_id();
            registry_request->endpoint = heartbeat.endpoint();
            registry_request->sequence = heartbeat.sequence();
            registry_request->observed_at_unix_ms = heartbeat.observed_at_unix_ms();

            std::string error_detail;
            const auto status =
                FromProtoFacts(heartbeat.facts(), &registry_request->facts, &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return MakeRegistryUpdateValidationResult(status,
                                                          std::move(error_detail));
            }

            return {};
        }

        StorageNodeRegistryUpdateResult TranslateHealthReportRequest(
            const storage::ReportHealthRequest &request,
            ReportHealthRequest *registry_request)
        {
            if (registry_request == nullptr)
            {
                return MakeRegistryUpdateValidationResult(
                    StorageNodeStatusCode::kInvalidArgument,
                    "registry health report output must not be null");
            }

            registry_request->node_id = request.node_id();
            registry_request->endpoint = request.endpoint();
            registry_request->sequence = request.sequence();
            registry_request->observed_at_unix_ms = request.observed_at_unix_ms();

            std::string error_detail;
            const auto status =
                FromProtoHealthReport(request.health(), &registry_request->health, &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return MakeRegistryUpdateValidationResult(status,
                                                          std::move(error_detail));
            }

            return {};
        }

        StorageNodeRegistryUpdateResult TranslateCapacityReportRequest(
            const storage::ReportCapacityRequest &request,
            ReportCapacityRequest *registry_request)
        {
            if (registry_request == nullptr)
            {
                return MakeRegistryUpdateValidationResult(
                    StorageNodeStatusCode::kInvalidArgument,
                    "registry capacity report output must not be null");
            }

            registry_request->node_id = request.node_id();
            registry_request->endpoint = request.endpoint();
            registry_request->sequence = request.sequence();
            registry_request->observed_at_unix_ms = request.observed_at_unix_ms();

            std::string error_detail;
            const auto status = FromProtoCapacityReport(request.capacity(),
                                                        &registry_request->capacity,
                                                        &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return MakeRegistryUpdateValidationResult(status,
                                                          std::move(error_detail));
            }

            return {};
        }

        StorageNodeRegistryUpdateResult TranslateLoadReportRequest(
            const storage::ReportLoadRequest &request,
            ReportLoadRequest *registry_request)
        {
            if (registry_request == nullptr)
            {
                return MakeRegistryUpdateValidationResult(
                    StorageNodeStatusCode::kInvalidArgument,
                    "registry load report output must not be null");
            }

            registry_request->node_id = request.node_id();
            registry_request->endpoint = request.endpoint();
            registry_request->sequence = request.sequence();
            registry_request->observed_at_unix_ms = request.observed_at_unix_ms();

            std::string error_detail;
            const auto status = FromProtoLoadReport(request.load(),
                                                    &registry_request->load,
                                                    &error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return MakeRegistryUpdateValidationResult(status,
                                                          std::move(error_detail));
            }

            return {};
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

        void FillRegisterResponse(const storage::RegisterStorageNodeRequest &request,
                                  const RegisterStorageNodeResult &result,
                                  storage::RegisterStorageNodeResponse *response)
        {
            if (response == nullptr)
            {
                return;
            }

            FillRegistrySummary(result.status,
                                result.error_detail,
                                request.request_id(),
                                request.node_id(),
                                result.snapshot,
                                response->mutable_summary());
            response->set_created(result.created);
            response->set_idempotent(result.idempotent);
            FillProtoRegistrySnapshot(result.snapshot, response->mutable_snapshot());
        }

        void FillFactUpdateResponse(const std::string_view request_id,
                                    const std::string_view request_node_id,
                                    const StorageNodeRegistryUpdateResult &result,
                                    storage::StorageNodeFactUpdateResponse *response)
        {
            if (response == nullptr)
            {
                return;
            }

            FillRegistrySummary(result.status,
                                result.error_detail,
                                request_id,
                                request_node_id,
                                result.snapshot,
                                response->mutable_summary());
            response->set_accepted_sequence(result.accepted_sequence);
            response->set_applied(result.applied);
            response->set_idempotent(result.idempotent);
            response->set_stale_ignored(result.stale_ignored);
            FillProtoRegistrySnapshot(result.snapshot, response->mutable_snapshot());
        }
    }

    StorageNodeService::StorageNodeService(
        std::shared_ptr<ChunkStore> chunk_store,
        std::string node_id,
        std::shared_ptr<StorageNodeRegistry> storage_node_registry)
        : chunk_store_(std::move(chunk_store))
        , node_id_(std::move(node_id))
        , storage_node_registry_(std::move(storage_node_registry))
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

    grpc::ServerUnaryReactor *StorageNodeService::RegisterStorageNode(
        grpc::CallbackServerContext *context,
        const storage::RegisterStorageNodeRequest *request,
        storage::RegisterStorageNodeResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "RegisterStorageNode request/response must not be null"));
            return reactor;
        }

        if (storage_node_registry_ == nullptr)
        {
            FillRegisterResponse(*request,
                                 MakeRegisterValidationResult(
                                     StorageNodeStatusCode::kUnsupported,
                                     "StorageNodeRegistry is not configured"),
                                 response);
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }

        RegisterStorageNodeRequest registry_request;
        auto result = TranslateRegisterRequest(*request, &registry_request);
        if (result.status == StorageNodeStatusCode::kOk)
        {
            result = storage_node_registry_->RegisterStorageNode(registry_request);
        }

        FillRegisterResponse(*request, result, response);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }

    grpc::ServerUnaryReactor *StorageNodeService::UpdateStorageNodeHeartbeat(
        grpc::CallbackServerContext *context,
        const storage::UpdateStorageNodeHeartbeatRequest *request,
        storage::StorageNodeFactUpdateResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(
                grpc::StatusCode::INVALID_ARGUMENT,
                "UpdateStorageNodeHeartbeat request/response must not be null"));
            return reactor;
        }

        if (storage_node_registry_ == nullptr)
        {
            FillFactUpdateResponse(request->request_id(),
                                   request->has_heartbeat() ? request->heartbeat().node_id()
                                                            : std::string_view{},
                                   MakeRegistryUpdateValidationResult(
                                       StorageNodeStatusCode::kUnsupported,
                                       "StorageNodeRegistry is not configured"),
                                   response);
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }

        UpdateStorageNodeHeartbeatRequest registry_request;
        auto result = TranslateHeartbeatRequest(*request, &registry_request);
        if (result.status == StorageNodeStatusCode::kOk)
        {
            result = storage_node_registry_->UpdateStorageNodeHeartbeat(registry_request);
        }

        FillFactUpdateResponse(request->request_id(),
                               request->has_heartbeat() ? request->heartbeat().node_id()
                                                        : std::string_view{},
                               result,
                               response);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }

    grpc::ServerUnaryReactor *StorageNodeService::ReportHealth(
        grpc::CallbackServerContext *context,
        const storage::ReportHealthRequest *request,
        storage::StorageNodeFactUpdateResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "ReportHealth request/response must not be null"));
            return reactor;
        }

        if (storage_node_registry_ == nullptr)
        {
            FillFactUpdateResponse(request->request_id(),
                                   request->node_id(),
                                   MakeRegistryUpdateValidationResult(
                                       StorageNodeStatusCode::kUnsupported,
                                       "StorageNodeRegistry is not configured"),
                                   response);
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }

        ReportHealthRequest registry_request;
        auto result = TranslateHealthReportRequest(*request, &registry_request);
        if (result.status == StorageNodeStatusCode::kOk)
        {
            result = storage_node_registry_->ReportHealth(registry_request);
        }

        FillFactUpdateResponse(request->request_id(), request->node_id(), result, response);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }

    grpc::ServerUnaryReactor *StorageNodeService::ReportCapacity(
        grpc::CallbackServerContext *context,
        const storage::ReportCapacityRequest *request,
        storage::StorageNodeFactUpdateResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "ReportCapacity request/response must not be null"));
            return reactor;
        }

        if (storage_node_registry_ == nullptr)
        {
            FillFactUpdateResponse(request->request_id(),
                                   request->node_id(),
                                   MakeRegistryUpdateValidationResult(
                                       StorageNodeStatusCode::kUnsupported,
                                       "StorageNodeRegistry is not configured"),
                                   response);
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }

        ReportCapacityRequest registry_request;
        auto result = TranslateCapacityReportRequest(*request, &registry_request);
        if (result.status == StorageNodeStatusCode::kOk)
        {
            result = storage_node_registry_->ReportCapacity(registry_request);
        }

        FillFactUpdateResponse(request->request_id(), request->node_id(), result, response);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }

    grpc::ServerUnaryReactor *StorageNodeService::ReportLoad(
        grpc::CallbackServerContext *context,
        const storage::ReportLoadRequest *request,
        storage::StorageNodeFactUpdateResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "ReportLoad request/response must not be null"));
            return reactor;
        }

        if (storage_node_registry_ == nullptr)
        {
            FillFactUpdateResponse(request->request_id(),
                                   request->node_id(),
                                   MakeRegistryUpdateValidationResult(
                                       StorageNodeStatusCode::kUnsupported,
                                       "StorageNodeRegistry is not configured"),
                                   response);
            reactor->Finish(grpc::Status::OK);
            return reactor;
        }

        ReportLoadRequest registry_request;
        auto result = TranslateLoadReportRequest(*request, &registry_request);
        if (result.status == StorageNodeStatusCode::kOk)
        {
            result = storage_node_registry_->ReportLoad(registry_request);
        }

        FillFactUpdateResponse(request->request_id(), request->node_id(), result, response);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }
}
