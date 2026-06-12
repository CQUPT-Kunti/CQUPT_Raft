#include "store/node/storage_node_client.h"

#include <chrono>
#include <string_view>
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
            default:
                return storage::STORAGE_CHUNK_STATE_MISSING;
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

        StorageNodeStatusCode FromProtoLiveness(
            const storage::StorageNodeLivenessState liveness,
            StorageNodeRegistryLiveness *out_liveness,
            std::string *error_detail)
        {
            if (out_liveness == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "liveness output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            switch (liveness)
            {
            case storage::STORAGE_NODE_LIVENESS_STATE_UNSPECIFIED:
            case storage::STORAGE_NODE_LIVENESS_STATE_LIVE:
                *out_liveness = StorageNodeRegistryLiveness::kLive;
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_NODE_LIVENESS_STATE_STALE:
            case storage::STORAGE_NODE_LIVENESS_STATE_SUSPECT:
                *out_liveness = StorageNodeRegistryLiveness::kStale;
                return StorageNodeStatusCode::kOk;
            case storage::STORAGE_NODE_LIVENESS_STATE_DEAD:
                *out_liveness = StorageNodeRegistryLiveness::kDead;
                return StorageNodeStatusCode::kOk;
            default:
                if (error_detail != nullptr)
                {
                    *error_detail = "storage node liveness is not supported";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
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

        void FillProtoLoadReport(
            const StorageNodeRegistryLoadFacts &load,
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

        StorageNodeStatusCode FillCapacityFromProto(
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

        StorageNodeStatusCode FillHealthFromProto(
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

        StorageNodeStatusCode FillLoadFromProto(
            const storage::StorageNodeLoadReport &proto_load,
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

        StorageNodeStatusCode FillFactsFromProto(const storage::StorageNodeFacts &proto_facts,
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

            auto status = FillCapacityFromProto(proto_facts.capacity(),
                                                &out_facts->capacity,
                                                error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            status = FillHealthFromProto(proto_facts.health(),
                                         &out_facts->health,
                                         error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            status = FillLoadFromProto(proto_facts.load(), &out_facts->load, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            out_facts->failure_domain.zone = proto_facts.failure_domain().zone();
            out_facts->failure_domain.rack = proto_facts.failure_domain().rack();
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode FillSnapshotFromProto(
            const storage::StorageNodeRegistrySnapshot &proto_snapshot,
            StorageNodeRegistryNodeSnapshot *out_snapshot,
            std::string *error_detail)
        {
            if (out_snapshot == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "snapshot output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            StorageNodeRegistryNodeSnapshot snapshot;
            snapshot.node_id = proto_snapshot.node_id();
            snapshot.endpoint = proto_snapshot.endpoint();
            snapshot.incarnation_id = proto_snapshot.incarnation_id();
            snapshot.last_sequence = proto_snapshot.last_sequence();
            snapshot.last_seen_unix_ms = proto_snapshot.last_seen_unix_ms();

            auto status = FromProtoLiveness(proto_snapshot.liveness(),
                                            &snapshot.liveness,
                                            error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            status = FillFactsFromProto(proto_snapshot.facts(), &snapshot.facts, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            *out_snapshot = std::move(snapshot);
            return StorageNodeStatusCode::kOk;
        }

        void FillProtoRegisterRequest(
            const StorageNodeClientRegisterStorageNodeRequest &request,
            const StorageNodeClientRegistryOptions & /*options*/,
            storage::RegisterStorageNodeRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            proto_request->set_node_id(request.node_id);
            proto_request->set_endpoint(request.endpoint);
            proto_request->set_incarnation_id(request.incarnation_id);
            proto_request->set_observed_at_unix_ms(request.observed_at_unix_ms);
            FillProtoFacts(request.facts, proto_request->mutable_facts());
        }

        void FillProtoHeartbeatRequest(
            const StorageNodeClientHeartbeatRequest &request,
            const StorageNodeClientRegistryOptions & /*options*/,
            storage::UpdateStorageNodeHeartbeatRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            auto *heartbeat = proto_request->mutable_heartbeat();
            heartbeat->set_node_id(request.node_id);
            heartbeat->set_endpoint(request.endpoint);
            heartbeat->set_incarnation_id(request.incarnation_id);
            heartbeat->set_sequence(request.sequence);
            heartbeat->set_observed_at_unix_ms(request.observed_at_unix_ms);
            FillProtoFacts(request.facts, heartbeat->mutable_facts());
        }

        void FillProtoHealthReportRequest(
            const StorageNodeClientHealthReportRequest &request,
            const StorageNodeClientRegistryOptions & /*options*/,
            storage::ReportHealthRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            proto_request->set_node_id(request.node_id);
            proto_request->set_endpoint(request.endpoint);
            proto_request->set_incarnation_id(request.incarnation_id);
            proto_request->set_sequence(request.sequence);
            proto_request->set_observed_at_unix_ms(request.observed_at_unix_ms);
            FillProtoHealthReport(request.health, proto_request->mutable_health());
        }

        void FillProtoCapacityReportRequest(
            const StorageNodeClientCapacityReportRequest &request,
            const StorageNodeClientRegistryOptions & /*options*/,
            storage::ReportCapacityRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            proto_request->set_node_id(request.node_id);
            proto_request->set_endpoint(request.endpoint);
            proto_request->set_incarnation_id(request.incarnation_id);
            proto_request->set_sequence(request.sequence);
            proto_request->set_observed_at_unix_ms(request.observed_at_unix_ms);
            FillProtoCapacityReport(request.capacity, proto_request->mutable_capacity());
        }

        void FillProtoLoadReportRequest(
            const StorageNodeClientLoadReportRequest &request,
            const StorageNodeClientRegistryOptions & /*options*/,
            storage::ReportLoadRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            proto_request->set_node_id(request.node_id);
            proto_request->set_endpoint(request.endpoint);
            proto_request->set_incarnation_id(request.incarnation_id);
            proto_request->set_sequence(request.sequence);
            proto_request->set_observed_at_unix_ms(request.observed_at_unix_ms);
            FillProtoLoadReport(request.load, proto_request->mutable_load());
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

        void FillProtoScrubRequest(
            const StorageNodeClientScrubChunkRequest &request,
            const StorageNodeClientScrubChunkOptions &options,
            storage::ScrubChunkRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            proto_request->set_chunk_id(request.chunk_id);
            proto_request->set_object_id(request.object_id);
            proto_request->set_version(request.version);
            proto_request->set_chunk_index(request.chunk_index);
            proto_request->set_expected_size(request.expected_size);
            FillProtoChecksum(request.expected_checksum,
                              proto_request->mutable_expected_checksum());
            proto_request->set_timeout_ms(options.context.timeout_ms);
            proto_request->set_best_effort_cancel(options.context.best_effort_cancel);
            proto_request->set_verify_checksum(request.verify_checksum);
            proto_request->set_quarantine_on_corruption(
                request.quarantine_on_corruption);
        }

        void FillProtoRepairRequest(
            const StorageNodeClientRepairChunkRequest &request,
            const StorageNodeClientRepairChunkOptions &options,
            storage::RepairChunkRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            proto_request->set_chunk_id(request.chunk_id);
            proto_request->set_object_id(request.object_id);
            proto_request->set_version(request.version);
            proto_request->set_chunk_index(request.chunk_index);
            proto_request->set_offset(request.offset);
            proto_request->set_expected_size(request.expected_size);
            FillProtoChecksum(request.expected_checksum,
                              proto_request->mutable_expected_checksum());
            proto_request->set_source_node_id(request.source_node_id);
            proto_request->set_source_size(request.source_size);
            FillProtoChecksum(request.source_checksum,
                              proto_request->mutable_source_checksum());
            proto_request->set_source_state(ToProtoChunkState(request.source_state));
            proto_request->set_source_checksum_verified(
                request.source_checksum_verified);
            proto_request->set_payload(request.payload);
            proto_request->set_timeout_ms(options.context.timeout_ms);
            proto_request->set_best_effort_cancel(options.context.best_effort_cancel);
            proto_request->set_durability(ToProtoDurability(request.durability));
        }

        StorageNodeClientDeleteChunkRequest ToClientDeleteRequest(
            const DeleteChunkRequest &request)
        {
            StorageNodeClientDeleteChunkRequest client_request;
            client_request.request_id = request.request_id;
            client_request.chunk_id = request.chunk_id;
            client_request.expected_checksum = request.expected_checksum;
            client_request.reason = request.reason;
            client_request.metadata_boundary = request.metadata_boundary;
            return client_request;
        }

        StorageNodeClientDeleteChunkRequest ToClientDeleteRequest(
            const std::string &request_id,
            const StorageNodeClientBatchDeleteChunkRequest &request)
        {
            StorageNodeClientDeleteChunkRequest client_request;
            client_request.request_id = request_id;
            client_request.chunk_id = request.chunk_id;
            client_request.object_id = request.object_id;
            client_request.version = request.version;
            client_request.chunk_index = request.chunk_index;
            client_request.expected_checksum = request.expected_checksum;
            client_request.reason = request.reason;
            client_request.metadata_boundary = request.metadata_boundary;
            return client_request;
        }

        void FillProtoDeleteRequest(
            const StorageNodeClientDeleteChunkRequest &request,
            const StorageNodeClientDeleteChunkOptions &options,
            storage::DeleteChunkRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            proto_request->set_chunk_id(request.chunk_id);
            proto_request->set_object_id(request.object_id);
            proto_request->set_version(request.version);
            proto_request->set_chunk_index(request.chunk_index);
            FillProtoChecksum(request.expected_checksum,
                              proto_request->mutable_expected_checksum());
            proto_request->set_reason(request.reason);
            proto_request->set_metadata_boundary(request.metadata_boundary);
            proto_request->set_timeout_ms(options.context.timeout_ms);
            proto_request->set_best_effort_cancel(options.context.best_effort_cancel);
        }

        void FillProtoBatchDeleteChunkRequest(
            const StorageNodeClientBatchDeleteChunkRequest &request,
            storage::BatchDeleteChunkRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_chunk_id(request.chunk_id);
            proto_request->set_object_id(request.object_id);
            proto_request->set_version(request.version);
            proto_request->set_chunk_index(request.chunk_index);
            FillProtoChecksum(request.expected_checksum,
                              proto_request->mutable_expected_checksum());
            proto_request->set_reason(request.reason);
            proto_request->set_metadata_boundary(request.metadata_boundary);
        }

        void FillProtoBatchDeleteRequest(
            const StorageNodeClientBatchDeleteChunksRequest &request,
            const StorageNodeClientDeleteChunkOptions &options,
            storage::BatchDeleteChunksRequest *proto_request)
        {
            if (proto_request == nullptr)
            {
                return;
            }

            proto_request->set_request_id(request.request_id);
            proto_request->set_timeout_ms(options.context.timeout_ms);
            proto_request->set_best_effort_cancel(options.context.best_effort_cancel);
            for (const auto &chunk : request.chunks)
            {
                FillProtoBatchDeleteChunkRequest(chunk, proto_request->add_chunks());
            }
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

        StorageNodeStatusCode ResolveDeleteResponseIdentity(
            const std::string_view requested_chunk_id,
            const std::string_view requested_object_id,
            const std::uint64_t requested_version,
            const std::uint32_t requested_chunk_index,
            const std::string_view summary_chunk_id,
            const std::string_view response_chunk_id,
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

            const std::string candidate_chunk_id =
                !response_chunk_id.empty()
                    ? std::string(response_chunk_id)
                    : std::string(summary_chunk_id);

            if (!candidate_chunk_id.empty())
            {
                ChunkIdentity parsed_identity;
                const auto parse_status =
                    ParseChunkId(candidate_chunk_id, &parsed_identity, error_detail);
                if (parse_status != StorageNodeStatusCode::kOk)
                {
                    return parse_status;
                }

                *out_identity = std::move(parsed_identity);
                return StorageNodeStatusCode::kOk;
            }

            if (!requested_chunk_id.empty())
            {
                ChunkIdentity identity;
                identity.chunk_id = std::string(requested_chunk_id);

                ChunkIdentity parsed_identity;
                const auto parse_status =
                    ParseChunkId(requested_chunk_id, &parsed_identity, error_detail);
                if (parse_status == StorageNodeStatusCode::kOk)
                {
                    identity = std::move(parsed_identity);
                }

                *out_identity = std::move(identity);
                return StorageNodeStatusCode::kOk;
            }

            if (!requested_object_id.empty() && requested_version != 0)
            {
                ChunkId derived_chunk_id;
                const auto make_status = MakeChunkId(requested_object_id,
                                                     requested_version,
                                                     requested_chunk_index,
                                                     &derived_chunk_id,
                                                     error_detail);
                if (make_status != StorageNodeStatusCode::kOk)
                {
                    return make_status;
                }

                ChunkIdentity identity;
                identity.chunk_id = std::move(derived_chunk_id);
                identity.object_id = std::string(requested_object_id);
                identity.version = requested_version;
                identity.chunk_index = requested_chunk_index;
                *out_identity = std::move(identity);
                return StorageNodeStatusCode::kOk;
            }

            *out_identity = ChunkIdentity{};
            return StorageNodeStatusCode::kOk;
        }

        bool HasObjectIdentity(const std::string_view object_id,
                               const std::uint64_t version,
                               const std::uint32_t chunk_index)
        {
            return !object_id.empty() || version != 0 || chunk_index != 0;
        }

        StorageNodeStatusCode ResolveScrubResponseIdentity(
            const StorageNodeClientScrubChunkRequest &request,
            const storage::ScrubChunkResponse &proto_response,
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

            const std::string candidate_chunk_id =
                !proto_response.result().fact().chunk_id().empty()
                    ? proto_response.result().fact().chunk_id()
                    : proto_response.summary().chunk_id();

            if (!candidate_chunk_id.empty())
            {
                ChunkIdentity parsed_identity;
                const auto parse_status =
                    ParseChunkId(candidate_chunk_id, &parsed_identity, error_detail);
                if (parse_status == StorageNodeStatusCode::kOk)
                {
                    *out_identity = std::move(parsed_identity);
                    return StorageNodeStatusCode::kOk;
                }

                if (error_detail != nullptr)
                {
                    *error_detail = "invalid ScrubChunk response chunk identity: " +
                                    *error_detail;
                }
                return parse_status;
            }

            if (!request.chunk_id.empty())
            {
                ChunkIdentity identity;
                identity.chunk_id = request.chunk_id;

                ChunkIdentity parsed_identity;
                std::string parse_error;
                const auto parse_status =
                    ParseChunkId(request.chunk_id, &parsed_identity, &parse_error);
                if (parse_status == StorageNodeStatusCode::kOk)
                {
                    identity = std::move(parsed_identity);
                }
                else
                {
                    identity.object_id = request.object_id;
                    identity.version = request.version;
                    identity.chunk_index = request.chunk_index;
                }

                *out_identity = std::move(identity);
                return StorageNodeStatusCode::kOk;
            }

            if (HasObjectIdentity(request.object_id, request.version, request.chunk_index))
            {
                ChunkId chunk_id;
                const auto make_status = MakeChunkId(request.object_id,
                                                     request.version,
                                                     request.chunk_index,
                                                     &chunk_id,
                                                     error_detail);
                if (make_status != StorageNodeStatusCode::kOk)
                {
                    return make_status;
                }

                ChunkIdentity identity;
                identity.chunk_id = std::move(chunk_id);
                identity.object_id = request.object_id;
                identity.version = request.version;
                identity.chunk_index = request.chunk_index;
                *out_identity = std::move(identity);
                return StorageNodeStatusCode::kOk;
            }

            *out_identity = ChunkIdentity{};
            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ResolveRepairResponseIdentity(
            const StorageNodeClientRepairChunkRequest &request,
            const storage::RepairChunkResponse &proto_response,
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

            const std::string candidate_chunk_id =
                !proto_response.result().fact().chunk_id().empty()
                    ? proto_response.result().fact().chunk_id()
                    : proto_response.summary().chunk_id();

            if (!candidate_chunk_id.empty())
            {
                ChunkIdentity parsed_identity;
                const auto parse_status =
                    ParseChunkId(candidate_chunk_id, &parsed_identity, error_detail);
                if (parse_status == StorageNodeStatusCode::kOk)
                {
                    *out_identity = std::move(parsed_identity);
                    return StorageNodeStatusCode::kOk;
                }

                if (error_detail != nullptr)
                {
                    *error_detail =
                        "invalid RepairChunk response chunk identity: " + *error_detail;
                }
                return parse_status;
            }

            if (!request.chunk_id.empty())
            {
                ChunkIdentity identity;
                identity.chunk_id = request.chunk_id;

                ChunkIdentity parsed_identity;
                std::string parse_error;
                const auto parse_status =
                    ParseChunkId(request.chunk_id, &parsed_identity, &parse_error);
                if (parse_status == StorageNodeStatusCode::kOk)
                {
                    identity = std::move(parsed_identity);
                }
                else
                {
                    identity.object_id = request.object_id;
                    identity.version = request.version;
                    identity.chunk_index = request.chunk_index;
                    identity.offset = request.offset;
                }

                *out_identity = std::move(identity);
                return StorageNodeStatusCode::kOk;
            }

            if (HasObjectIdentity(request.object_id, request.version, request.chunk_index))
            {
                ChunkId chunk_id;
                const auto make_status = MakeChunkId(request.object_id,
                                                     request.version,
                                                     request.chunk_index,
                                                     &chunk_id,
                                                     error_detail);
                if (make_status != StorageNodeStatusCode::kOk)
                {
                    return make_status;
                }

                ChunkIdentity identity;
                identity.chunk_id = std::move(chunk_id);
                identity.object_id = request.object_id;
                identity.version = request.version;
                identity.chunk_index = request.chunk_index;
                identity.offset = request.offset;
                *out_identity = std::move(identity);
                return StorageNodeStatusCode::kOk;
            }

            *out_identity = ChunkIdentity{};
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

        StorageNodeClientScrubChunkResponse TranslateProtoScrubResponse(
            const StorageNodeClientScrubChunkRequest &request,
            const storage::ScrubChunkResponse &proto_response)
        {
            StorageNodeClientScrubChunkResponse response;
            response.status = FromProtoStatusCode(proto_response.summary().code());
            response.error_detail = proto_response.summary().message();
            response.retry_after_ms = proto_response.summary().retry_after_ms();
            response.expected_size = proto_response.result().fact().expected_size();
            response.observed_size = proto_response.result().fact().observed_size();
            response.checksum_verified =
                proto_response.result().fact().checksum_verified();
            response.known_corrupted =
                proto_response.result().fact().known_corrupted();
            response.known_missing = proto_response.result().fact().known_missing();
            response.quarantined = proto_response.result().fact().quarantined();
            response.repair_required = proto_response.result().repair_required();
            response.retryable =
                response.status != StorageNodeStatusCode::kOk &&
                (proto_response.result().retryable() ||
                 IsRetriableStatus(response.status));

            response.metadata.node_id = proto_response.summary().node_id();
            response.metadata.size = response.observed_size;
            response.metadata.last_error = response.status;

            if (response.status == StorageNodeStatusCode::kIoError &&
                proto_response.summary().code() ==
                    storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED &&
                response.error_detail.empty())
            {
                response.error_detail = "ScrubChunk response status is unspecified";
            }

            std::string error_detail;
            const auto identity_status = ResolveScrubResponseIdentity(request,
                                                                      proto_response,
                                                                      &response.metadata.identity,
                                                                      &error_detail);
            if (identity_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = error_detail;
                response.retryable = false;
                return response;
            }

            const auto expected_checksum_status = FillChecksumFromProto(
                proto_response.result().fact().expected_checksum(),
                &response.expected_checksum,
                &error_detail);
            if (expected_checksum_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail =
                    "invalid ScrubChunk response expected checksum: " + error_detail;
                response.retryable = false;
                return response;
            }

            const auto observed_checksum_status = FillChecksumFromProto(
                proto_response.result().fact().observed_checksum(),
                &response.observed_checksum,
                &error_detail);
            if (observed_checksum_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail =
                    "invalid ScrubChunk response observed checksum: " + error_detail;
                response.retryable = false;
                return response;
            }

            const auto state_before_status = FromProtoChunkState(
                proto_response.result().fact().state_before());
            if (state_before_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail =
                    "invalid ScrubChunk response state_before";
                response.retryable = false;
                return response;
            }

            const auto state_after_status = FromProtoChunkState(
                proto_response.result().fact().state_after());
            if (state_after_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid ScrubChunk response state_after";
                response.retryable = false;
                return response;
            }

            response.state_before =
                ToStoreChunkState(proto_response.result().fact().state_before());
            response.state_after =
                ToStoreChunkState(proto_response.result().fact().state_after());
            response.metadata.state = response.state_after;
            response.metadata.checksum = response.observed_checksum;
            return response;
        }

        StorageNodeClientRepairChunkResponse TranslateProtoRepairResponse(
            const StorageNodeClientRepairChunkRequest &request,
            const storage::RepairChunkResponse &proto_response)
        {
            StorageNodeClientRepairChunkResponse response;
            response.status = FromProtoStatusCode(proto_response.summary().code());
            response.error_detail = proto_response.summary().message();
            response.retry_after_ms = proto_response.summary().retry_after_ms();
            response.source_node_id = proto_response.result().fact().source_node_id();
            response.expected_size = proto_response.result().fact().expected_size();
            response.observed_size = proto_response.result().fact().observed_size();
            response.source_checksum_verified =
                proto_response.result().fact().source_checksum_verified();
            response.source_unavailable =
                proto_response.result().fact().source_unavailable();
            response.target_durable = proto_response.result().fact().target_durable();
            response.already_exists = proto_response.result().fact().already_exists();
            response.repaired = proto_response.result().repaired();
            response.retryable =
                response.status != StorageNodeStatusCode::kOk &&
                (proto_response.result().retryable() ||
                 IsRetriableStatus(response.status));

            response.metadata.node_id = proto_response.summary().node_id();
            response.metadata.size = response.observed_size;
            response.metadata.last_error = response.status;

            if (response.status == StorageNodeStatusCode::kIoError &&
                proto_response.summary().code() ==
                    storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED &&
                response.error_detail.empty())
            {
                response.error_detail = "RepairChunk response status is unspecified";
            }

            std::string error_detail;
            const auto identity_status = ResolveRepairResponseIdentity(request,
                                                                       proto_response,
                                                                       &response.metadata.identity,
                                                                       &error_detail);
            if (identity_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = error_detail;
                response.retryable = false;
                return response;
            }

            const auto expected_checksum_status = FillChecksumFromProto(
                proto_response.result().fact().expected_checksum(),
                &response.expected_checksum,
                &error_detail);
            if (expected_checksum_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail =
                    "invalid RepairChunk response expected checksum: " + error_detail;
                response.retryable = false;
                return response;
            }

            const auto observed_checksum_status = FillChecksumFromProto(
                proto_response.result().fact().observed_checksum(),
                &response.observed_checksum,
                &error_detail);
            if (observed_checksum_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail =
                    "invalid RepairChunk response observed checksum: " + error_detail;
                response.retryable = false;
                return response;
            }

            const auto source_state_status = FromProtoChunkState(
                proto_response.result().fact().source_state());
            if (source_state_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid RepairChunk response source_state";
                response.retryable = false;
                return response;
            }

            const auto target_state_status = FromProtoChunkState(
                proto_response.result().fact().target_state());
            if (target_state_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid RepairChunk response target_state";
                response.retryable = false;
                return response;
            }

            response.source_state =
                ToStoreChunkState(proto_response.result().fact().source_state());
            response.target_state =
                ToStoreChunkState(proto_response.result().fact().target_state());
            response.metadata.state = response.target_state;
            response.metadata.checksum = response.observed_checksum;
            return response;
        }

        StorageNodeClientDeleteChunkResponse TranslateProtoDeleteResponse(
            const StorageNodeClientDeleteChunkRequest &request,
            const storage::DeleteChunkResponse &proto_response)
        {
            StorageNodeClientDeleteChunkResponse response;
            response.status = FromProtoStatusCode(proto_response.summary().code());
            response.error_detail = proto_response.summary().message();
            response.retry_after_ms = proto_response.summary().retry_after_ms();
            response.deleted = proto_response.deleted();
            response.already_missing = proto_response.already_missing();

            response.metadata.node_id = proto_response.summary().node_id();
            response.metadata.size = proto_response.size();
            response.metadata.state = ToStoreChunkState(proto_response.state());
            response.metadata.last_error = response.status;

            if (response.status == StorageNodeStatusCode::kIoError &&
                proto_response.summary().code() ==
                    storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED &&
                response.error_detail.empty())
            {
                response.error_detail = "DeleteChunk response status is unspecified";
            }

            std::string error_detail;
            const auto identity_status = ResolveDeleteResponseIdentity(
                request.chunk_id,
                request.object_id,
                request.version,
                request.chunk_index,
                proto_response.summary().chunk_id(),
                proto_response.chunk_id(),
                &response.metadata.identity,
                &error_detail);
            if (identity_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid DeleteChunk response chunk identity: " +
                                        error_detail;
                response.retryable = false;
                return response;
            }

            const auto checksum_status =
                FillChecksumFromProto(proto_response.checksum(),
                                      &response.metadata.checksum,
                                      &error_detail);
            if (checksum_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid DeleteChunk response checksum: " +
                                        error_detail;
                response.retryable = false;
                return response;
            }

            const auto state_status = FromProtoChunkState(proto_response.state());
            if (state_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail = "invalid DeleteChunk response chunk state";
                response.retryable = false;
                return response;
            }

            response.already_deleted =
                proto_response.already_deleted() ||
                (response.already_missing &&
                 response.metadata.state == ChunkState::kDeleted);
            response.retryable =
                response.status != StorageNodeStatusCode::kOk &&
                (proto_response.retryable() || IsRetriableStatus(response.status));
            return response;
        }

        StorageNodeClientBatchDeleteChunkResult TranslateProtoBatchDeleteResult(
            const StorageNodeClientBatchDeleteChunksRequest &request,
            const std::size_t index,
            const storage::BatchDeleteChunkResult &proto_result)
        {
            storage::DeleteChunkResponse single_response;
            *single_response.mutable_summary() = proto_result.summary();
            single_response.set_chunk_id(proto_result.chunk_id());
            single_response.set_size(proto_result.size());
            *single_response.mutable_checksum() = proto_result.checksum();
            single_response.set_state(proto_result.state());
            single_response.set_deleted(proto_result.deleted());
            single_response.set_already_missing(proto_result.already_missing());
            single_response.set_already_deleted(proto_result.already_deleted());
            single_response.set_retryable(proto_result.retryable());

            return TranslateProtoDeleteResponse(
                ToClientDeleteRequest(request.request_id + "/item/" +
                                          std::to_string(index),
                                      request.chunks[index]),
                single_response);
        }

        bool IsDeleteIdempotentSuccess(
            const StorageNodeClientBatchDeleteChunkResult &result)
        {
            return result.status == StorageNodeStatusCode::kOk &&
                   result.already_missing;
        }

        bool IsDeleteRetryableFailure(
            const StorageNodeClientBatchDeleteChunkResult &result)
        {
            return result.status != StorageNodeStatusCode::kOk &&
                   (result.retryable || IsRetriableStatus(result.status));
        }

        StorageNodeClientBatchDeleteChunksResponse TranslateProtoBatchDeleteResponse(
            const StorageNodeClientBatchDeleteChunksRequest &request,
            const storage::BatchDeleteChunksResponse &proto_response)
        {
            StorageNodeClientBatchDeleteChunksResponse response;
            response.status = FromProtoStatusCode(proto_response.summary().code());
            response.error_detail = proto_response.summary().message();
            response.retry_after_ms = proto_response.summary().retry_after_ms();
            response.success_count = proto_response.success_count();
            response.idempotent_count = proto_response.idempotent_count();
            response.retryable_failure_count =
                proto_response.retryable_failure_count();
            response.non_retryable_failure_count =
                proto_response.non_retryable_failure_count();
            response.partial_failure = proto_response.partial_failure();

            if (response.status == StorageNodeStatusCode::kIoError &&
                proto_response.summary().code() ==
                    storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED &&
                response.error_detail.empty())
            {
                response.error_detail = "BatchDeleteChunks response status is unspecified";
            }

            if (proto_response.results_size() !=
                static_cast<int>(request.chunks.size()))
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail =
                    "invalid BatchDeleteChunks response result count";
                return response;
            }

            response.results.reserve(request.chunks.size());
            for (int index = 0; index < proto_response.results_size(); ++index)
            {
                response.results.push_back(TranslateProtoBatchDeleteResult(
                    request,
                    static_cast<std::size_t>(index),
                    proto_response.results(index)));
            }

            if (response.status != StorageNodeStatusCode::kOk)
            {
                return response;
            }

            std::uint32_t computed_success_count = 0;
            std::uint32_t computed_idempotent_count = 0;
            std::uint32_t computed_retryable_failure_count = 0;
            std::uint32_t computed_non_retryable_failure_count = 0;
            for (const auto &result : response.results)
            {
                if (result.status == StorageNodeStatusCode::kOk)
                {
                    if (IsDeleteIdempotentSuccess(result))
                    {
                        ++computed_idempotent_count;
                    }
                    else
                    {
                        ++computed_success_count;
                    }
                    continue;
                }

                if (IsDeleteRetryableFailure(result))
                {
                    ++computed_retryable_failure_count;
                }
                else
                {
                    ++computed_non_retryable_failure_count;
                }
            }

            const bool computed_partial_failure =
                (computed_retryable_failure_count != 0 ||
                 computed_non_retryable_failure_count != 0) &&
                (computed_success_count != 0 || computed_idempotent_count != 0);
            if (computed_success_count != response.success_count ||
                computed_idempotent_count != response.idempotent_count ||
                computed_retryable_failure_count !=
                    response.retryable_failure_count ||
                computed_non_retryable_failure_count !=
                    response.non_retryable_failure_count ||
                computed_partial_failure != response.partial_failure)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail =
                    "invalid BatchDeleteChunks response aggregate facts";
            }

            return response;
        }

        StorageNodeClientRegisterStorageNodeResponse TranslateProtoRegisterResponse(
            const storage::RegisterStorageNodeResponse &proto_response)
        {
            StorageNodeClientRegisterStorageNodeResponse response;
            response.status = FromProtoStatusCode(proto_response.summary().code());
            response.error_detail = proto_response.summary().message();
            response.retry_after_ms = proto_response.summary().retry_after_ms();
            response.created = proto_response.created();
            response.idempotent = proto_response.idempotent();

            if (response.status == StorageNodeStatusCode::kIoError &&
                proto_response.summary().code() ==
                    storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED &&
                response.error_detail.empty())
            {
                response.error_detail =
                    "RegisterStorageNode response status is unspecified";
            }

            std::string error_detail;
            const auto snapshot_status = FillSnapshotFromProto(proto_response.snapshot(),
                                                              &response.snapshot,
                                                              &error_detail);
            if (snapshot_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail =
                    "invalid RegisterStorageNode response snapshot: " + error_detail;
                response.created = false;
                response.idempotent = false;
            }

            return response;
        }

        StorageNodeClientFactUpdateResponse TranslateProtoFactUpdateResponse(
            const storage::StorageNodeFactUpdateResponse &proto_response,
            const std::string_view operation_name)
        {
            StorageNodeClientFactUpdateResponse response;
            response.status = FromProtoStatusCode(proto_response.summary().code());
            response.error_detail = proto_response.summary().message();
            response.retry_after_ms = proto_response.summary().retry_after_ms();
            response.accepted_sequence = proto_response.accepted_sequence();
            response.applied = proto_response.applied();
            response.idempotent = proto_response.idempotent();
            response.stale_ignored = proto_response.stale_ignored();

            if (response.status == StorageNodeStatusCode::kIoError &&
                proto_response.summary().code() ==
                    storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED &&
                response.error_detail.empty())
            {
                response.error_detail =
                    std::string(operation_name) + " response status is unspecified";
            }

            std::string error_detail;
            const auto snapshot_status = FillSnapshotFromProto(proto_response.snapshot(),
                                                              &response.snapshot,
                                                              &error_detail);
            if (snapshot_status != StorageNodeStatusCode::kOk)
            {
                response.status = StorageNodeStatusCode::kIoError;
                response.error_detail =
                    std::string("invalid ") + std::string(operation_name) +
                    " response snapshot: " + error_detail;
                response.applied = false;
                response.idempotent = false;
                response.stale_ignored = false;
                response.accepted_sequence = 0;
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

        StorageNodeClientScrubChunkResponse MakeGrpcScrubFailureResponse(
            const grpc::Status &status)
        {
            StorageNodeClientScrubChunkResponse response;
            response.status = MapGrpcStatusCode(status.error_code());
            response.error_detail = status.error_message();
            response.retryable =
                response.status != StorageNodeStatusCode::kOk &&
                IsRetriableStatus(response.status);
            return response;
        }

        StorageNodeClientRepairChunkResponse MakeGrpcRepairFailureResponse(
            const grpc::Status &status)
        {
            StorageNodeClientRepairChunkResponse response;
            response.status = MapGrpcStatusCode(status.error_code());
            response.error_detail = status.error_message();
            response.retryable =
                response.status != StorageNodeStatusCode::kOk &&
                IsRetriableStatus(response.status);
            return response;
        }

        StorageNodeClientDeleteChunkResponse MakeGrpcDeleteFailureResponse(
            const grpc::Status &status)
        {
            StorageNodeClientDeleteChunkResponse response;
            response.status = MapGrpcStatusCode(status.error_code());
            response.error_detail = status.error_message();
            response.retryable =
                response.status != StorageNodeStatusCode::kOk &&
                IsRetriableStatus(response.status);
            return response;
        }

        StorageNodeClientRegisterStorageNodeResponse MakeGrpcRegisterFailureResponse(
            const grpc::Status &status)
        {
            StorageNodeClientRegisterStorageNodeResponse response;
            response.status = MapGrpcStatusCode(status.error_code());
            response.error_detail = status.error_message();
            return response;
        }

        StorageNodeClientFactUpdateResponse MakeGrpcFactUpdateFailureResponse(
            const grpc::Status &status)
        {
            StorageNodeClientFactUpdateResponse response;
            response.status = MapGrpcStatusCode(status.error_code());
            response.error_detail = status.error_message();
            return response;
        }

        StorageNodeClientBatchDeleteChunksResponse MakeGrpcBatchDeleteFailureResponse(
            const grpc::Status &status)
        {
            StorageNodeClientBatchDeleteChunksResponse response;
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

        int ReplicaFailurePriority(const StorageNodeStatusCode status)
        {
            switch (status)
            {
            case StorageNodeStatusCode::kChecksumMismatch:
                return 0;
            case StorageNodeStatusCode::kCorrupted:
                return 1;
            case StorageNodeStatusCode::kConflict:
                return 2;
            case StorageNodeStatusCode::kNotFound:
                return 3;
            case StorageNodeStatusCode::kTimeout:
            case StorageNodeStatusCode::kNodeUnavailable:
            case StorageNodeStatusCode::kOverloaded:
            case StorageNodeStatusCode::kIoError:
                return 4;
            case StorageNodeStatusCode::kCancelled:
                return 5;
            case StorageNodeStatusCode::kUnsupported:
                return 6;
            case StorageNodeStatusCode::kPermissionDenied:
                return 7;
            case StorageNodeStatusCode::kInvalidArgument:
                return 8;
            case StorageNodeStatusCode::kDiskFull:
                return 9;
            case StorageNodeStatusCode::kAlreadyExists:
                return 10;
            case StorageNodeStatusCode::kOk:
            default:
                return 11;
            }
        }

        ReadChunkResponse SelectDominantReplicaFailure(
            const std::vector<ReadReplicaAttempt> &attempts,
            const std::vector<ReadChunkResponse> &responses)
        {
            if (attempts.empty() || attempts.size() != responses.size())
            {
                ReadChunkResponse response;
                response.status = StorageNodeStatusCode::kInvalidArgument;
                response.error_detail =
                    "replica fallback attempts are inconsistent";
                return response;
            }

            std::size_t best_index = responses.size() - 1;
            int best_priority =
                ReplicaFailurePriority(responses[best_index].status);
            for (std::size_t index = 0; index + 1 < responses.size(); ++index)
            {
                const int priority = ReplicaFailurePriority(responses[index].status);
                if (priority < best_priority)
                {
                    best_priority = priority;
                    best_index = index;
                }
            }

            ReadChunkResponse dominant = responses[best_index];
            if (dominant.error_detail.empty())
            {
                dominant.error_detail =
                    "all replicas failed after " + std::to_string(attempts.size()) +
                    " attempts";
            }
            else
            {
                dominant.error_detail +=
                    "; all replicas failed after " +
                    std::to_string(attempts.size()) + " attempts";
            }
            return dominant;
        }
    }

    ReadChunkRequest MakeReadChunkRequestForCommittedManifestReplica(
        const std::string_view request_id,
        const std::string_view chunk_id,
        const std::uint64_t expected_size,
        const std::string_view expected_checksum)
    {
        ReadChunkRequest request;
        request.request_id = std::string(request_id);
        request.chunk_id = std::string(chunk_id);
        request.expected_checksum.algorithm = ChunkChecksumAlgorithm::kSha256;
        request.expected_checksum.value = std::string(expected_checksum);
        request.expected_checksum.size_bytes = expected_size;
        request.verify_checksum = true;
        return request;
    }

    ReadReplicaFailureAction ClassifyReadReplicaFailure(
        const ReadChunkResponse &response)
    {
        switch (response.status)
        {
        case StorageNodeStatusCode::kOk:
            return ReadReplicaFailureAction::kStop;
        case StorageNodeStatusCode::kTimeout:
        case StorageNodeStatusCode::kNodeUnavailable:
        case StorageNodeStatusCode::kOverloaded:
        case StorageNodeStatusCode::kIoError:
        case StorageNodeStatusCode::kNotFound:
        case StorageNodeStatusCode::kConflict:
        case StorageNodeStatusCode::kChecksumMismatch:
        case StorageNodeStatusCode::kCorrupted:
            return ReadReplicaFailureAction::kTryNext;
        case StorageNodeStatusCode::kAlreadyExists:
        case StorageNodeStatusCode::kDiskFull:
        case StorageNodeStatusCode::kPermissionDenied:
        case StorageNodeStatusCode::kCancelled:
        case StorageNodeStatusCode::kUnsupported:
        case StorageNodeStatusCode::kInvalidArgument:
        default:
            return ReadReplicaFailureAction::kStop;
        }
    }

    ReadReplicaFallbackResult ReadChunkWithReplicaFallback(
        const std::span<const StorageNodeId> replica_nodes,
        const ReadChunkRequest &request,
        const StorageNodeClientReadChunkOptions options,
        const ReadChunkReplicaInvoker &invoker)
    {
        ReadReplicaFallbackResult result;
        if (replica_nodes.empty())
        {
            result.response.status = StorageNodeStatusCode::kInvalidArgument;
            result.response.error_detail =
                "replica fallback requires at least one replica node";
            return result;
        }

        if (!invoker)
        {
            result.response.status = StorageNodeStatusCode::kInvalidArgument;
            result.response.error_detail =
                "replica fallback requires a non-null invoker";
            return result;
        }

        std::vector<ReadChunkResponse> failed_responses;
        failed_responses.reserve(replica_nodes.size());
        result.attempts.reserve(replica_nodes.size());

        for (const auto &node_id : replica_nodes)
        {
            const ReadChunkResponse response = invoker(node_id, request, options);
            const ReadReplicaFailureAction action =
                ClassifyReadReplicaFailure(response);

            result.attempts.push_back(ReadReplicaAttempt{
                .node_id = node_id,
                .status = response.status,
                .error_detail = response.error_detail,
                .action = action});

            if (response.status == StorageNodeStatusCode::kOk)
            {
                result.response = response;
                result.selected_node_id = node_id;
                return result;
            }

            failed_responses.push_back(response);
            if (action == ReadReplicaFailureAction::kStop)
            {
                result.response = response;
                return result;
            }
        }

        result.response =
            SelectDominantReplicaFailure(result.attempts, failed_responses);
        return result;
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

    StorageNodeClientScrubChunkResponse StorageNodeClient::ScrubChunk(
        const StorageNodeClientScrubChunkRequest &request,
        StorageNodeClientScrubChunkOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        if (HasDeadlineExpired(options.context, absolute_deadline))
        {
            StorageNodeClientScrubChunkResponse response;
            response.status = StorageNodeStatusCode::kTimeout;
            response.error_detail = "ScrubChunk client-side deadline expired";
            response.retryable = true;
            return response;
        }

        grpc::ClientContext context;
        ApplyDeadlineToContext(options.context, absolute_deadline, &context);

        storage::ScrubChunkRequest proto_request;
        FillProtoScrubRequest(request, options, &proto_request);

        storage::ScrubChunkResponse proto_response;
        const grpc::Status grpc_status =
            stub_->ScrubChunk(&context, proto_request, &proto_response);

        if (!grpc_status.ok())
        {
            return MakeGrpcScrubFailureResponse(grpc_status);
        }

        return TranslateProtoScrubResponse(request, proto_response);
    }

    StorageNodeClientRepairChunkResponse StorageNodeClient::RepairChunk(
        const StorageNodeClientRepairChunkRequest &request,
        StorageNodeClientRepairChunkOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        if (HasDeadlineExpired(options.context, absolute_deadline))
        {
            StorageNodeClientRepairChunkResponse response;
            response.status = StorageNodeStatusCode::kTimeout;
            response.error_detail = "RepairChunk client-side deadline expired";
            response.retryable = true;
            return response;
        }

        grpc::ClientContext context;
        ApplyDeadlineToContext(options.context, absolute_deadline, &context);

        storage::RepairChunkRequest proto_request;
        FillProtoRepairRequest(request, options, &proto_request);

        storage::RepairChunkResponse proto_response;
        const grpc::Status grpc_status =
            stub_->RepairChunk(&context, proto_request, &proto_response);

        if (!grpc_status.ok())
        {
            return MakeGrpcRepairFailureResponse(grpc_status);
        }

        return TranslateProtoRepairResponse(request, proto_response);
    }

    StorageNodeClientDeleteChunkResponse StorageNodeClient::DeleteChunk(
        const StorageNodeClientDeleteChunkRequest &request,
        StorageNodeClientDeleteChunkOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        if (HasDeadlineExpired(options.context, absolute_deadline))
        {
            StorageNodeClientDeleteChunkResponse response;
            response.status = StorageNodeStatusCode::kTimeout;
            response.error_detail = "DeleteChunk client-side deadline expired";
            response.retryable = true;
            return response;
        }

        grpc::ClientContext context;
        ApplyDeadlineToContext(options.context, absolute_deadline, &context);

        storage::DeleteChunkRequest proto_request;
        FillProtoDeleteRequest(request, options, &proto_request);

        storage::DeleteChunkResponse proto_response;
        const grpc::Status grpc_status =
            stub_->DeleteChunk(&context, proto_request, &proto_response);

        if (!grpc_status.ok())
        {
            return MakeGrpcDeleteFailureResponse(grpc_status);
        }

        return TranslateProtoDeleteResponse(request, proto_response);
    }

    StorageNodeClientDeleteChunkResponse StorageNodeClient::DeleteChunk(
        const DeleteChunkRequest &request,
        StorageNodeClientDeleteChunkOptions options)
    {
        return DeleteChunk(ToClientDeleteRequest(request), options);
    }

    StorageNodeClientBatchDeleteChunksResponse StorageNodeClient::BatchDeleteChunks(
        const StorageNodeClientBatchDeleteChunksRequest &request,
        StorageNodeClientDeleteChunkOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        if (HasDeadlineExpired(options.context, absolute_deadline))
        {
            StorageNodeClientBatchDeleteChunksResponse response;
            response.status = StorageNodeStatusCode::kTimeout;
            response.error_detail =
                "BatchDeleteChunks client-side deadline expired";
            return response;
        }

        grpc::ClientContext context;
        ApplyDeadlineToContext(options.context, absolute_deadline, &context);

        storage::BatchDeleteChunksRequest proto_request;
        FillProtoBatchDeleteRequest(request, options, &proto_request);

        storage::BatchDeleteChunksResponse proto_response;
        const grpc::Status grpc_status =
            stub_->BatchDeleteChunks(&context, proto_request, &proto_response);

        if (!grpc_status.ok())
        {
            return MakeGrpcBatchDeleteFailureResponse(grpc_status);
        }

        return TranslateProtoBatchDeleteResponse(request, proto_response);
    }

    StorageNodeClientRegisterStorageNodeResponse StorageNodeClient::RegisterStorageNode(
        const StorageNodeClientRegisterStorageNodeRequest &request,
        StorageNodeClientRegistryOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        if (HasDeadlineExpired(options.context, absolute_deadline))
        {
            StorageNodeClientRegisterStorageNodeResponse response;
            response.status = StorageNodeStatusCode::kTimeout;
            response.error_detail =
                "RegisterStorageNode client-side deadline expired";
            return response;
        }

        grpc::ClientContext context;
        ApplyDeadlineToContext(options.context, absolute_deadline, &context);

        storage::RegisterStorageNodeRequest proto_request;
        FillProtoRegisterRequest(request, options, &proto_request);

        storage::RegisterStorageNodeResponse proto_response;
        const grpc::Status grpc_status =
            stub_->RegisterStorageNode(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            return MakeGrpcRegisterFailureResponse(grpc_status);
        }

        return TranslateProtoRegisterResponse(proto_response);
    }

    StorageNodeClientFactUpdateResponse StorageNodeClient::UpdateStorageNodeHeartbeat(
        const StorageNodeClientHeartbeatRequest &request,
        StorageNodeClientRegistryOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        if (HasDeadlineExpired(options.context, absolute_deadline))
        {
            StorageNodeClientFactUpdateResponse response;
            response.status = StorageNodeStatusCode::kTimeout;
            response.error_detail =
                "UpdateStorageNodeHeartbeat client-side deadline expired";
            return response;
        }

        grpc::ClientContext context;
        ApplyDeadlineToContext(options.context, absolute_deadline, &context);

        storage::UpdateStorageNodeHeartbeatRequest proto_request;
        FillProtoHeartbeatRequest(request, options, &proto_request);

        storage::StorageNodeFactUpdateResponse proto_response;
        const grpc::Status grpc_status =
            stub_->UpdateStorageNodeHeartbeat(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            return MakeGrpcFactUpdateFailureResponse(grpc_status);
        }

        return TranslateProtoFactUpdateResponse(proto_response,
                                               "UpdateStorageNodeHeartbeat");
    }

    StorageNodeClientFactUpdateResponse StorageNodeClient::ReportHealth(
        const StorageNodeClientHealthReportRequest &request,
        StorageNodeClientRegistryOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        if (HasDeadlineExpired(options.context, absolute_deadline))
        {
            StorageNodeClientFactUpdateResponse response;
            response.status = StorageNodeStatusCode::kTimeout;
            response.error_detail = "ReportHealth client-side deadline expired";
            return response;
        }

        grpc::ClientContext context;
        ApplyDeadlineToContext(options.context, absolute_deadline, &context);

        storage::ReportHealthRequest proto_request;
        FillProtoHealthReportRequest(request, options, &proto_request);

        storage::StorageNodeFactUpdateResponse proto_response;
        const grpc::Status grpc_status =
            stub_->ReportHealth(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            return MakeGrpcFactUpdateFailureResponse(grpc_status);
        }

        return TranslateProtoFactUpdateResponse(proto_response, "ReportHealth");
    }

    StorageNodeClientFactUpdateResponse StorageNodeClient::ReportCapacity(
        const StorageNodeClientCapacityReportRequest &request,
        StorageNodeClientRegistryOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        if (HasDeadlineExpired(options.context, absolute_deadline))
        {
            StorageNodeClientFactUpdateResponse response;
            response.status = StorageNodeStatusCode::kTimeout;
            response.error_detail =
                "ReportCapacity client-side deadline expired";
            return response;
        }

        grpc::ClientContext context;
        ApplyDeadlineToContext(options.context, absolute_deadline, &context);

        storage::ReportCapacityRequest proto_request;
        FillProtoCapacityReportRequest(request, options, &proto_request);

        storage::StorageNodeFactUpdateResponse proto_response;
        const grpc::Status grpc_status =
            stub_->ReportCapacity(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            return MakeGrpcFactUpdateFailureResponse(grpc_status);
        }

        return TranslateProtoFactUpdateResponse(proto_response, "ReportCapacity");
    }

    StorageNodeClientFactUpdateResponse StorageNodeClient::ReportLoad(
        const StorageNodeClientLoadReportRequest &request,
        StorageNodeClientRegistryOptions options)
    {
        const auto start_time = std::chrono::system_clock::now();
        const auto absolute_deadline =
            ResolveAbsoluteDeadline(options.context, start_time);

        if (HasDeadlineExpired(options.context, absolute_deadline))
        {
            StorageNodeClientFactUpdateResponse response;
            response.status = StorageNodeStatusCode::kTimeout;
            response.error_detail = "ReportLoad client-side deadline expired";
            return response;
        }

        grpc::ClientContext context;
        ApplyDeadlineToContext(options.context, absolute_deadline, &context);

        storage::ReportLoadRequest proto_request;
        FillProtoLoadReportRequest(request, options, &proto_request);

        storage::StorageNodeFactUpdateResponse proto_response;
        const grpc::Status grpc_status =
            stub_->ReportLoad(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            return MakeGrpcFactUpdateFailureResponse(grpc_status);
        }

        return TranslateProtoFactUpdateResponse(proto_response, "ReportLoad");
    }

    const StorageNodeClientConfig &StorageNodeClient::config() const
    {
        return config_;
    }
}
