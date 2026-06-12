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

        struct ScrubChunkRequestContext
        {
            StatChunkRequest stat_request;
            ChunkIdentity identity;
            ChunkChecksum expected_checksum;
            std::uint64_t expected_size{0};
        };

        struct ScrubChunkServiceResult : ChunkStoreResult
        {
            ChunkMetadata metadata;
            ChunkState state_before{ChunkState::kMissing};
            ChunkState state_after{ChunkState::kMissing};
            ChunkChecksum expected_checksum;
            ChunkChecksum observed_checksum;
            std::uint64_t expected_size{0};
            std::uint64_t observed_size{0};
            bool checksum_verified{false};
            bool known_corrupted{false};
            bool known_missing{false};
            bool quarantined{false};
            bool repair_required{false};
            bool retryable{false};
        };

        struct RepairChunkRequestContext
        {
            WriteChunkRequest write_request;
            ChunkIdentity identity;
            ChunkChecksum expected_checksum;
            ChunkChecksum source_checksum;
            StorageNodeId source_node_id;
            ChunkState source_state{ChunkState::kMissing};
            std::uint64_t expected_size{0};
            std::uint64_t source_size{0};
            bool source_checksum_verified{false};
        };

        struct RepairChunkServiceResult : ChunkStoreResult
        {
            ChunkMetadata metadata;
            StorageNodeId source_node_id;
            ChunkState source_state{ChunkState::kMissing};
            ChunkState target_state{ChunkState::kMissing};
            ChunkChecksum expected_checksum;
            ChunkChecksum observed_checksum;
            std::uint64_t expected_size{0};
            std::uint64_t observed_size{0};
            bool source_checksum_verified{false};
            bool source_unavailable{false};
            bool target_durable{false};
            bool already_exists{false};
            bool repaired{false};
            bool retryable{false};
        };

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
            out_health->set_writable(health.writable);
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
            out_snapshot->set_incarnation_id(snapshot.incarnation_id);
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
            out_health->writable = proto_health.writable();
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

        StorageNodeStatusCode ResolveScrubRequestChunkId(const std::string_view chunk_id,
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
                    *error_detail = "scrub chunk_id output must not be null";
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
                            "ScrubChunk chunk_id does not match object identity";
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
                        "ScrubChunk requires chunk_id or object identity";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return MakeChunkId(object_id,
                               version,
                               chunk_index,
                               out_chunk_id,
                               error_detail);
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

        StorageNodeStatusCode ResolveRepairRequestChunkId(const std::string_view chunk_id,
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
                    *error_detail = "repair chunk_id output must not be null";
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
                            "RepairChunk chunk_id does not match object identity";
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
                        "RepairChunk requires chunk_id or object identity";
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

        std::string ResolveResponseChunkId(const storage::ScrubChunkRequest &request,
                                           const ScrubChunkServiceResult &store_response)
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

        std::string ResolveResponseChunkId(const storage::RepairChunkRequest &request,
                                           const RepairChunkServiceResult &store_response)
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

        void FillSummary(const storage::ScrubChunkRequest &request,
                         const ScrubChunkServiceResult &store_response,
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

        void FillSummary(const storage::RepairChunkRequest &request,
                         const RepairChunkServiceResult &store_response,
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
            registry_request->incarnation_id = request.incarnation_id();
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
            registry_request->incarnation_id = heartbeat.incarnation_id();
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
            registry_request->incarnation_id = request.incarnation_id();
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
            registry_request->incarnation_id = request.incarnation_id();
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
            registry_request->incarnation_id = request.incarnation_id();
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

        ScrubChunkServiceResult MakeScrubValidationError(
            const StorageNodeStatusCode status,
            std::string error_detail)
        {
            ScrubChunkServiceResult response;
            response.status = status;
            response.error_detail = std::move(error_detail);
            response.retryable =
                response.status != StorageNodeStatusCode::kOk &&
                IsRetriableStatus(response.status);
            return response;
        }

        RepairChunkServiceResult MakeRepairValidationError(
            const StorageNodeStatusCode status,
            std::string error_detail)
        {
            RepairChunkServiceResult response;
            response.status = status;
            response.error_detail = std::move(error_detail);
            response.retryable =
                response.status != StorageNodeStatusCode::kOk &&
                IsRetriableStatus(response.status);
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

        ScrubChunkServiceResult TranslateScrubRequest(
            const storage::ScrubChunkRequest &request,
            ScrubChunkRequestContext *request_context)
        {
            if (request_context == nullptr)
            {
                return MakeScrubValidationError(
                    StorageNodeStatusCode::kInvalidArgument,
                    "store scrub request output must not be null");
            }

            if (request.request_id().empty())
            {
                return MakeScrubValidationError(
                    StorageNodeStatusCode::kInvalidArgument,
                    "ScrubChunk request_id must not be empty");
            }

            request_context->stat_request.request_id = request.request_id();
            request_context->stat_request.include_quarantine = true;
            request_context->stat_request.verify_checksum = false;
            request_context->expected_size = request.expected_size();
            request_context->identity.chunk_id = request.chunk_id();
            request_context->identity.object_id = request.object_id();
            request_context->identity.version = request.version();
            request_context->identity.chunk_index = request.chunk_index();

            std::string error_detail;
            const auto chunk_status = ResolveScrubRequestChunkId(request.chunk_id(),
                                                                 request.object_id(),
                                                                 request.version(),
                                                                 request.chunk_index(),
                                                                 &request_context->stat_request.chunk_id,
                                                                 &error_detail);
            if (chunk_status != StorageNodeStatusCode::kOk)
            {
                return MakeScrubValidationError(chunk_status, std::move(error_detail));
            }

            request_context->identity.chunk_id = request_context->stat_request.chunk_id;

            const auto checksum_status = FromProtoChecksum(request.expected_checksum(),
                                                           &request_context->expected_checksum,
                                                           &error_detail);
            if (checksum_status != StorageNodeStatusCode::kOk)
            {
                return MakeScrubValidationError(checksum_status, std::move(error_detail));
            }

            return {};
        }

        RepairChunkServiceResult TranslateRepairRequest(
            const storage::RepairChunkRequest &request,
            RepairChunkRequestContext *request_context)
        {
            if (request_context == nullptr)
            {
                return MakeRepairValidationError(
                    StorageNodeStatusCode::kInvalidArgument,
                    "store repair request output must not be null");
            }

            if (request.request_id().empty())
            {
                return MakeRepairValidationError(
                    StorageNodeStatusCode::kInvalidArgument,
                    "RepairChunk request_id must not be empty");
            }

            request_context->write_request.request_id = request.request_id();
            request_context->write_request.payload = request.payload();
            request_context->expected_size = request.expected_size();
            request_context->source_node_id = request.source_node_id();
            request_context->source_size = request.source_size();
            request_context->source_state = ToStoreChunkState(request.source_state());
            request_context->source_checksum_verified =
                request.source_checksum_verified();
            request_context->identity.chunk_id = request.chunk_id();
            request_context->identity.object_id = request.object_id();
            request_context->identity.version = request.version();
            request_context->identity.chunk_index = request.chunk_index();
            request_context->identity.offset = request.offset();

            std::string error_detail;
            const auto chunk_status = ResolveRepairRequestChunkId(request.chunk_id(),
                                                                  request.object_id(),
                                                                  request.version(),
                                                                  request.chunk_index(),
                                                                  &request_context->write_request.identity.chunk_id,
                                                                  &error_detail);
            if (chunk_status != StorageNodeStatusCode::kOk)
            {
                return MakeRepairValidationError(chunk_status, std::move(error_detail));
            }

            request_context->identity.chunk_id =
                request_context->write_request.identity.chunk_id;
            request_context->write_request.identity = request_context->identity;

            const auto expected_checksum_status =
                FromProtoChecksum(request.expected_checksum(),
                                  &request_context->expected_checksum,
                                  &error_detail);
            if (expected_checksum_status != StorageNodeStatusCode::kOk)
            {
                return MakeRepairValidationError(expected_checksum_status,
                                                 std::move(error_detail));
            }

            const auto source_checksum_status =
                FromProtoChecksum(request.source_checksum(),
                                  &request_context->source_checksum,
                                  &error_detail);
            if (source_checksum_status != StorageNodeStatusCode::kOk)
            {
                return MakeRepairValidationError(source_checksum_status,
                                                 std::move(error_detail));
            }

            request_context->write_request.expected_size = request.expected_size();
            request_context->write_request.expected_checksum =
                request_context->expected_checksum;
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

        StorageNodeStatusCode CompareScrubExpectedChecksum(
            const ChunkChecksum &expected_checksum,
            const ChunkChecksum &observed_checksum,
            std::string *error_detail)
        {
            if (!expected_checksum.IsSet())
            {
                return StorageNodeStatusCode::kOk;
            }

            if (!observed_checksum.IsSet())
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "ScrubChunk did not produce an observed checksum";
                }
                return StorageNodeStatusCode::kChecksumMismatch;
            }

            if (expected_checksum.algorithm != observed_checksum.algorithm)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "ScrubChunk observed checksum algorithm does not match expected checksum";
                }
                return StorageNodeStatusCode::kChecksumMismatch;
            }

            if (expected_checksum.value != observed_checksum.value)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "ScrubChunk observed checksum does not match expected checksum";
                }
                return StorageNodeStatusCode::kChecksumMismatch;
            }

            if (expected_checksum.size_bytes != 0 &&
                expected_checksum.size_bytes != observed_checksum.size_bytes)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "ScrubChunk observed checksum size does not match expected checksum size";
                }
                return StorageNodeStatusCode::kChecksumMismatch;
            }

            return StorageNodeStatusCode::kOk;
        }

        bool IsScrubCorruptedState(const ChunkState state)
        {
            return state == ChunkState::kQuarantined ||
                   state == ChunkState::kCorrupted;
        }

        void FillScrubFact(const storage::ScrubChunkRequest &request,
                           const ScrubChunkServiceResult &store_response,
                           storage::ScrubChunkFact *fact)
        {
            if (fact == nullptr)
            {
                return;
            }

            fact->set_chunk_id(ResolveResponseChunkId(request, store_response));
            fact->set_expected_size(store_response.expected_size);
            fact->set_observed_size(store_response.observed_size);
            FillProtoChecksum(store_response.expected_checksum,
                              fact->mutable_expected_checksum());
            FillProtoChecksum(store_response.observed_checksum,
                              fact->mutable_observed_checksum());
            fact->set_state_before(ToProtoChunkState(store_response.state_before));
            fact->set_state_after(ToProtoChunkState(store_response.state_after));
            fact->set_checksum_verified(store_response.checksum_verified);
            fact->set_known_corrupted(store_response.known_corrupted);
            fact->set_known_missing(store_response.known_missing);
            fact->set_quarantined(store_response.quarantined);
        }

        void FillScrubResponse(const storage::ScrubChunkRequest &request,
                               const ScrubChunkServiceResult &store_response,
                               const std::string &configured_node_id,
                               storage::ScrubChunkResponse *response)
        {
            if (response == nullptr)
            {
                return;
            }

            FillSummary(request,
                        store_response,
                        configured_node_id,
                        response->mutable_summary());
            FillScrubFact(request,
                          store_response,
                          response->mutable_result()->mutable_fact());
            response->mutable_result()->set_repair_required(
                store_response.repair_required);
            response->mutable_result()->set_retryable(store_response.retryable);
        }

        void FillRepairFact(const storage::RepairChunkRequest &request,
                            const RepairChunkServiceResult &store_response,
                            const std::string &configured_node_id,
                            storage::RepairChunkFact *fact)
        {
            if (fact == nullptr)
            {
                return;
            }

            fact->set_chunk_id(ResolveResponseChunkId(request, store_response));
            fact->set_source_node_id(store_response.source_node_id);
            fact->set_target_node_id(!store_response.metadata.node_id.empty()
                                         ? store_response.metadata.node_id
                                         : configured_node_id);
            fact->set_expected_size(store_response.expected_size);
            fact->set_observed_size(store_response.observed_size);
            FillProtoChecksum(store_response.expected_checksum,
                              fact->mutable_expected_checksum());
            FillProtoChecksum(store_response.observed_checksum,
                              fact->mutable_observed_checksum());
            fact->set_source_state(ToProtoChunkState(store_response.source_state));
            fact->set_target_state(ToProtoChunkState(store_response.target_state));
            fact->set_source_checksum_verified(
                store_response.source_checksum_verified);
            fact->set_source_unavailable(store_response.source_unavailable);
            fact->set_target_durable(store_response.target_durable);
            fact->set_already_exists(store_response.already_exists);
        }

        void FillRepairResponse(const storage::RepairChunkRequest &request,
                                const RepairChunkServiceResult &store_response,
                                const std::string &configured_node_id,
                                storage::RepairChunkResponse *response)
        {
            if (response == nullptr)
            {
                return;
            }

            FillSummary(request,
                        store_response,
                        configured_node_id,
                        response->mutable_summary());
            FillRepairFact(request,
                           store_response,
                           configured_node_id,
                           response->mutable_result()->mutable_fact());
            response->mutable_result()->set_repaired(store_response.repaired);
            response->mutable_result()->set_retryable(store_response.retryable);
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
        : chunk_store_(std::move(chunk_store)), node_id_(std::move(node_id)), storage_node_registry_(std::move(storage_node_registry))
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

    grpc::ServerUnaryReactor *StorageNodeService::ScrubChunk(
        grpc::CallbackServerContext *context,
        const storage::ScrubChunkRequest *request,
        storage::ScrubChunkResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "ScrubChunk request/response must not be null"));
            return reactor;
        }

        (void)request->timeout_ms();
        (void)request->best_effort_cancel();
        (void)request->quarantine_on_corruption();

        ScrubChunkRequestContext request_context;
        auto store_response = TranslateScrubRequest(*request, &request_context);
        if (store_response.status == StorageNodeStatusCode::kOk)
        {
            store_response.expected_size = request_context.expected_size;
            store_response.expected_checksum = request_context.expected_checksum;
            store_response.metadata.identity = request_context.identity;

            auto pre_stat_request = request_context.stat_request;
            pre_stat_request.verify_checksum = false;
            const auto pre_stat_response = chunk_store_->StatChunk(pre_stat_request);

            if (pre_stat_response.metadata.identity.chunk_id.empty())
            {
                store_response.metadata.identity = request_context.identity;
            }
            else
            {
                store_response.metadata.identity = pre_stat_response.metadata.identity;
            }

            if (!pre_stat_response.metadata.node_id.empty())
            {
                store_response.metadata.node_id = pre_stat_response.metadata.node_id;
            }

            store_response.state_before = pre_stat_response.metadata.state;
            store_response.state_after = pre_stat_response.metadata.state;
            store_response.metadata.state = pre_stat_response.metadata.state;
            store_response.metadata.size = pre_stat_response.metadata.size;
            store_response.metadata.checksum = pre_stat_response.metadata.checksum;

            if (pre_stat_response.status == StorageNodeStatusCode::kNotFound)
            {
                store_response.status = StorageNodeStatusCode::kNotFound;
                store_response.error_detail = pre_stat_response.error_detail;
                store_response.retry_after_ms = pre_stat_response.retry_after_ms;
                store_response.known_missing = true;
                store_response.repair_required = true;
                store_response.state_before = ChunkState::kMissing;
                store_response.state_after = ChunkState::kMissing;
                store_response.metadata.state = ChunkState::kMissing;
            }
            else if (!pre_stat_response.ok())
            {
                store_response.status = pre_stat_response.status;
                store_response.error_detail = pre_stat_response.error_detail;
                store_response.retry_after_ms = pre_stat_response.retry_after_ms;
            }
            else if (IsScrubCorruptedState(pre_stat_response.metadata.state))
            {
                store_response.status = StorageNodeStatusCode::kCorrupted;
                store_response.error_detail =
                    pre_stat_response.metadata.state == ChunkState::kQuarantined
                        ? "chunk is quarantined"
                        : "chunk is corrupted";
                store_response.known_corrupted = true;
                store_response.quarantined =
                    pre_stat_response.metadata.state == ChunkState::kQuarantined;
                store_response.repair_required = true;
            }
            else if (!IsReadableChunkState(pre_stat_response.metadata.state))
            {
                store_response.status = StorageNodeStatusCode::kConflict;
                store_response.error_detail =
                    std::string("ScrubChunk cannot verify non-LIVE chunk state: ") +
                    ToString(pre_stat_response.metadata.state);
            }
            else
            {
                auto verify_request = request_context.stat_request;
                verify_request.verify_checksum = true;
                const auto verify_response = chunk_store_->StatChunk(verify_request);
                const auto post_verify_response = chunk_store_->StatChunk(pre_stat_request);

                if (!post_verify_response.metadata.identity.chunk_id.empty())
                {
                    store_response.metadata = post_verify_response.metadata;
                }
                else
                {
                    store_response.metadata.identity = request_context.identity;
                }

                if (post_verify_response.ok())
                {
                    store_response.state_after = post_verify_response.metadata.state;
                }
                else if (post_verify_response.status == StorageNodeStatusCode::kNotFound)
                {
                    store_response.state_after = ChunkState::kMissing;
                }

                if (verify_response.ok())
                {
                    store_response.status = StorageNodeStatusCode::kOk;
                    store_response.error_detail.clear();
                    store_response.retry_after_ms = verify_response.retry_after_ms;
                    store_response.metadata = verify_response.metadata;
                    store_response.checksum_verified = verify_response.verified;
                    store_response.observed_size = verify_response.metadata.size;
                    store_response.observed_checksum = verify_response.metadata.checksum;

                    std::string mismatch_detail;
                    if (request_context.expected_size != 0 &&
                        verify_response.metadata.size != request_context.expected_size)
                    {
                        store_response.status =
                            StorageNodeStatusCode::kChecksumMismatch;
                        mismatch_detail =
                            "ScrubChunk observed size does not match expected size";
                    }
                    else
                    {
                        store_response.status = CompareScrubExpectedChecksum(
                            request_context.expected_checksum,
                            verify_response.metadata.checksum,
                            &mismatch_detail);
                    }

                    if (store_response.status != StorageNodeStatusCode::kOk)
                    {
                        store_response.error_detail = std::move(mismatch_detail);
                        store_response.repair_required = true;
                    }
                }
                else if (verify_response.status == StorageNodeStatusCode::kNotFound ||
                         post_verify_response.status == StorageNodeStatusCode::kNotFound)
                {
                    store_response.status = StorageNodeStatusCode::kNotFound;
                    store_response.error_detail = verify_response.error_detail;
                    if (store_response.error_detail.empty())
                    {
                        store_response.error_detail = post_verify_response.error_detail;
                    }
                    store_response.retry_after_ms = verify_response.retry_after_ms;
                    store_response.known_missing = true;
                    store_response.repair_required = true;
                    store_response.state_after = ChunkState::kMissing;
                    store_response.metadata.state = ChunkState::kMissing;
                }
                else if (verify_response.status == StorageNodeStatusCode::kCorrupted ||
                         IsScrubCorruptedState(store_response.state_after))
                {
                    store_response.status = StorageNodeStatusCode::kCorrupted;
                    store_response.error_detail = verify_response.error_detail;
                    store_response.retry_after_ms = verify_response.retry_after_ms;
                    store_response.known_corrupted = true;
                    store_response.quarantined =
                        store_response.state_after == ChunkState::kQuarantined;
                    store_response.repair_required = true;
                }
                else
                {
                    store_response.status = verify_response.status;
                    store_response.error_detail = verify_response.error_detail;
                    store_response.retry_after_ms = verify_response.retry_after_ms;
                }
            }

            store_response.retryable =
                store_response.status != StorageNodeStatusCode::kOk &&
                IsRetriableStatus(store_response.status);
            store_response.metadata.last_error = store_response.status;
        }

        FillScrubResponse(*request, store_response, node_id_, response);
        reactor->Finish(grpc::Status::OK);
        return reactor;
    }

    grpc::ServerUnaryReactor *StorageNodeService::RepairChunk(
        grpc::CallbackServerContext *context,
        const storage::RepairChunkRequest *request,
        storage::RepairChunkResponse *response)
    {
        auto *reactor = context->DefaultReactor();

        if (request == nullptr || response == nullptr)
        {
            reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                         "RepairChunk request/response must not be null"));
            return reactor;
        }

        (void)request->timeout_ms();
        (void)request->best_effort_cancel();
        (void)request->durability();

        RepairChunkRequestContext request_context;
        auto store_response = TranslateRepairRequest(*request, &request_context);
        if (store_response.status == StorageNodeStatusCode::kOk)
        {
            store_response.metadata.identity = request_context.identity;
            store_response.source_node_id = request_context.source_node_id;
            store_response.source_state = request_context.source_state;
            store_response.target_state = ChunkState::kMissing;
            store_response.expected_size = request_context.expected_size;
            store_response.expected_checksum = request_context.expected_checksum;
            store_response.observed_size = static_cast<std::uint64_t>(
                request_context.write_request.payload.size());
            store_response.source_checksum_verified =
                request_context.source_checksum_verified;

            if (request_context.source_node_id.empty())
            {
                store_response = MakeRepairValidationError(
                    StorageNodeStatusCode::kInvalidArgument,
                    "RepairChunk source_node_id must not be empty");
            }
            else if (!request_context.expected_checksum.IsSet())
            {
                store_response = MakeRepairValidationError(
                    StorageNodeStatusCode::kInvalidArgument,
                    "RepairChunk expected_checksum must be set");
            }
            else if (!request_context.source_checksum.IsSet())
            {
                store_response = MakeRepairValidationError(
                    StorageNodeStatusCode::kInvalidArgument,
                    "RepairChunk source_checksum must be set");
            }
            else if (request_context.source_state == ChunkState::kMissing ||
                     request_context.source_state == ChunkState::kDeleted)
            {
                store_response.status = StorageNodeStatusCode::kNotFound;
                store_response.error_detail = "RepairChunk source chunk is missing";
                store_response.source_unavailable = true;
            }
            else if (request_context.source_state == ChunkState::kQuarantined ||
                     request_context.source_state == ChunkState::kCorrupted)
            {
                store_response.status = StorageNodeStatusCode::kCorrupted;
                store_response.error_detail =
                    request_context.source_state == ChunkState::kQuarantined
                        ? "RepairChunk source chunk is quarantined"
                        : "RepairChunk source chunk is corrupted";
            }
            else if (request_context.source_state != ChunkState::kLive)
            {
                store_response.status = StorageNodeStatusCode::kConflict;
                store_response.error_detail =
                    std::string("RepairChunk source state is not readable: ") +
                    ToString(request_context.source_state);
            }
            else if (!request_context.source_checksum_verified)
            {
                store_response.status = StorageNodeStatusCode::kChecksumMismatch;
                store_response.error_detail =
                    "RepairChunk source checksum must be verified before target write";
            }
            else if (request_context.expected_size != store_response.observed_size)
            {
                store_response.status = StorageNodeStatusCode::kInvalidArgument;
                store_response.error_detail =
                    "RepairChunk payload size does not match expected_size";
            }
            else if (request_context.source_size != 0 &&
                     request_context.source_size != request_context.expected_size)
            {
                store_response.status = StorageNodeStatusCode::kChecksumMismatch;
                store_response.error_detail =
                    "RepairChunk source_size does not match expected_size";
            }
            else if (request_context.source_checksum.algorithm !=
                          request_context.expected_checksum.algorithm ||
                     request_context.source_checksum.value !=
                          request_context.expected_checksum.value ||
                     request_context.source_checksum.size_bytes !=
                          request_context.expected_checksum.size_bytes)
            {
                store_response.status = StorageNodeStatusCode::kChecksumMismatch;
                store_response.error_detail =
                    "RepairChunk source checksum does not match expected checksum";
            }
            else
            {
                ChunkChecksum actual_checksum;
                std::string checksum_error;
                const auto checksum_status = VerifyChunkChecksum(
                    request_context.write_request.payload,
                    request_context.expected_checksum,
                    &actual_checksum,
                    &checksum_error);
                store_response.observed_checksum = actual_checksum;
                if (checksum_status != StorageNodeStatusCode::kOk)
                {
                    store_response.status = checksum_status;
                    store_response.error_detail = std::move(checksum_error);
                }
                else if (request_context.source_checksum.algorithm !=
                              actual_checksum.algorithm ||
                         request_context.source_checksum.value != actual_checksum.value ||
                         request_context.source_checksum.size_bytes !=
                              actual_checksum.size_bytes)
                {
                    store_response.status = StorageNodeStatusCode::kChecksumMismatch;
                    store_response.error_detail =
                        "RepairChunk payload does not match source checksum";
                }
                else
                {
                    const auto write_response =
                        chunk_store_->WriteChunk(request_context.write_request);
                    store_response.status = write_response.status;
                    store_response.error_detail = write_response.error_detail;
                    store_response.retry_after_ms = write_response.retry_after_ms;
                    store_response.metadata = write_response.metadata;
                    store_response.target_state = write_response.metadata.state;
                    store_response.observed_size = write_response.metadata.size;
                    if (write_response.metadata.checksum.IsSet())
                    {
                        store_response.observed_checksum =
                            write_response.metadata.checksum;
                    }
                    store_response.target_durable = write_response.durable;
                    store_response.already_exists = write_response.already_exists;
                    store_response.repaired =
                        write_response.status == StorageNodeStatusCode::kOk &&
                        write_response.durable;

                    if (store_response.status == StorageNodeStatusCode::kOk &&
                        !store_response.target_durable)
                    {
                        store_response.status = StorageNodeStatusCode::kIoError;
                        store_response.error_detail =
                            "RepairChunk target write did not reach durable boundary";
                        store_response.repaired = false;
                    }
                }
            }

            store_response.retryable =
                store_response.status != StorageNodeStatusCode::kOk &&
                IsRetriableStatus(store_response.status);
            store_response.metadata.last_error = store_response.status;
        }

        FillRepairResponse(*request, store_response, node_id_, response);
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
