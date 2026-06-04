#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "raft/common/metadata_command.h"
#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/node/storage_node_client.h"
#include "store/node/storage_node_service.h"
#include "support/metadata_test_utils.h"
#include "support/store_test_utils.h"

namespace
{
    using namespace std::chrono_literals;

    struct FixtureBinaryPayload
    {
        std::string payload;
        std::filesystem::path source_path;
    };

    std::filesystem::path RepoRoot()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path().lexically_normal();
    }

    FixtureBinaryPayload LoadFixtureBinaryPayload()
    {
        const std::filesystem::path fixture_path =
            RepoRoot() / "tests" / "test_file" / "test_file.deb";
        if (!std::filesystem::exists(fixture_path))
        {
            throw std::runtime_error("missing binary fixture: " + fixture_path.string());
        }

        std::ifstream input(fixture_path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open binary fixture: " +
                                     fixture_path.string());
        }

        return FixtureBinaryPayload{
            .payload = std::string(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()),
            .source_path = fixture_path};
    }

    FixtureBinaryPayload LoadDeleteFixtureBinaryPayload()
    {
        const std::filesystem::path fixture_path =
            RepoRoot() / "tests" / "test_file" / "test_file.zip";
        if (!std::filesystem::exists(fixture_path))
        {
            throw std::runtime_error("missing delete binary fixture: " +
                                     fixture_path.string());
        }

        std::ifstream input(fixture_path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open delete binary fixture: " +
                                     fixture_path.string());
        }

        return FixtureBinaryPayload{
            .payload = std::string(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()),
            .source_path = fixture_path};
    }

    storedemo::ChunkChecksum ComputeStoreChecksumOrThrow(const std::string_view payload)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute store checksum: " + error_detail);
        }
        return checksum;
    }

    storedemo::ChunkIdentity MakeStoreIdentityOrThrow(const std::string_view object_id,
                                                      const std::uint64_t version,
                                                      const std::uint32_t chunk_index,
                                                      const std::uint64_t offset = 0)
    {
        storedemo::ChunkId chunk_id;
        std::string error_detail;
        const auto status = storedemo::MakeChunkId(object_id,
                                                   version,
                                                   chunk_index,
                                                   &chunk_id,
                                                   &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to build chunk id: " + error_detail);
        }

        storedemo::ChunkIdentity identity;
        identity.chunk_id = std::move(chunk_id);
        identity.object_id = std::string(object_id);
        identity.version = version;
        identity.chunk_index = chunk_index;
        identity.offset = offset;
        return identity;
    }

    storedemo::WriteChunkRequest MakeWriteRequest(const storedemo::ChunkIdentity &identity,
                                                  const std::string &payload,
                                                  const std::string &request_id)
    {
        return storedemo::WriteChunkRequest{
            .request_id = request_id,
            .identity = identity,
            .expected_size = static_cast<std::uint64_t>(payload.size()),
            .expected_checksum = ComputeStoreChecksumOrThrow(payload),
            .payload = payload};
    }

    storedemo::ReadChunkRequest MakeReadRequest(const storedemo::ChunkId &chunk_id,
                                                const std::string &request_id)
    {
        return storedemo::ReadChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id};
    }

    storedemo::StorageNodeClientDeleteChunkRequest MakeDeleteRequest(
        const storedemo::ChunkIdentity &identity,
        const std::string &request_id)
    {
        storedemo::StorageNodeClientDeleteChunkRequest request;
        request.request_id = request_id;
        request.chunk_id = identity.chunk_id;
        request.object_id = identity.object_id;
        request.version = identity.version;
        request.chunk_index = identity.chunk_index;
        request.reason = "client delete test";
        request.metadata_boundary = "metadata-first-boundary";
        return request;
    }

    storedemo::StorageNodeClientBatchDeleteChunkRequest MakeBatchDeleteChunkRequest(
        const storedemo::ChunkIdentity &identity)
    {
        storedemo::StorageNodeClientBatchDeleteChunkRequest request;
        request.chunk_id = identity.chunk_id;
        request.object_id = identity.object_id;
        request.version = identity.version;
        request.chunk_index = identity.chunk_index;
        request.reason = "client batch delete test";
        request.metadata_boundary = "metadata-first-boundary";
        return request;
    }

    raftdemo::MetadataCommand MakeCreateObjectCommandWithSize(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id,
        const std::uint64_t size,
        const std::string &etag,
        const std::uint64_t create_time = 1712000001)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateObject;
        command.request_id = request_id;
        command.create_object = raftdemo::CreateObjectCommandPayload{
            raftdemo::ObjectRecord{bucket,
                                   object_key,
                                   object_id,
                                   1,
                                   size,
                                   etag,
                                   raftdemo::ObjectState::PENDING,
                                   {},
                                   create_time,
                                   std::nullopt,
                                   std::nullopt}};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kCreateObject,
            bucket,
            object_key,
            "accepted",
            0,
            create_time,
            std::nullopt};
        return command;
    }

    raftdemo::MetadataCommand MakeCommitObjectCommandWithChunks(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id,
        const std::uint64_t size,
        const std::string &etag,
        std::vector<raftdemo::ChunkRef> chunks,
        const std::uint64_t commit_time = 1712000002)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCommitObject;
        command.request_id = request_id;
        command.commit_object = raftdemo::CommitObjectCommandPayload{
            bucket,
            object_key,
            object_id,
            1,
            size,
            etag,
            std::move(chunks),
            commit_time};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kCommitObject,
            bucket,
            object_key,
            "accepted",
            0,
            commit_time,
            std::nullopt};
        return command;
    }

    void FillProtoChecksum(const storedemo::ChunkChecksum &checksum,
                           storage::StorageChunkChecksum *proto_checksum)
    {
        ASSERT_NE(proto_checksum, nullptr);
        switch (checksum.algorithm)
        {
        case storedemo::ChunkChecksumAlgorithm::kSha256:
            proto_checksum->set_algorithm(storage::STORAGE_CHECKSUM_ALGORITHM_SHA256);
            break;
        case storedemo::ChunkChecksumAlgorithm::kUnknown:
        default:
            proto_checksum->set_algorithm(storage::STORAGE_CHECKSUM_ALGORITHM_UNSPECIFIED);
            break;
        }
        proto_checksum->set_value(checksum.value);
        proto_checksum->set_size_bytes(checksum.size_bytes);
        proto_checksum->set_computed_at_unix_ms(checksum.computed_at);
    }

    storage::StorageNodeHealth ToProtoNodeHealth(
        const storedemo::StorageNodeHealth health)
    {
        switch (health)
        {
        case storedemo::StorageNodeHealth::kHealthy:
            return storage::STORAGE_NODE_HEALTH_HEALTHY;
        case storedemo::StorageNodeHealth::kDegraded:
            return storage::STORAGE_NODE_HEALTH_DEGRADED;
        case storedemo::StorageNodeHealth::kReadOnly:
            return storage::STORAGE_NODE_HEALTH_READ_ONLY;
        case storedemo::StorageNodeHealth::kUnavailable:
            return storage::STORAGE_NODE_HEALTH_UNAVAILABLE;
        case storedemo::StorageNodeHealth::kDraining:
            return storage::STORAGE_NODE_HEALTH_DRAINING;
        default:
            return storage::STORAGE_NODE_HEALTH_UNSPECIFIED;
        }
    }

    storage::StorageNodeDiskPressure ToProtoDiskPressure(
        const storedemo::StorageNodeDiskPressure pressure)
    {
        switch (pressure)
        {
        case storedemo::StorageNodeDiskPressure::kLow:
            return storage::STORAGE_NODE_DISK_PRESSURE_LOW;
        case storedemo::StorageNodeDiskPressure::kMedium:
            return storage::STORAGE_NODE_DISK_PRESSURE_MEDIUM;
        case storedemo::StorageNodeDiskPressure::kHigh:
            return storage::STORAGE_NODE_DISK_PRESSURE_HIGH;
        case storedemo::StorageNodeDiskPressure::kFull:
            return storage::STORAGE_NODE_DISK_PRESSURE_FULL;
        default:
            return storage::STORAGE_NODE_DISK_PRESSURE_UNSPECIFIED;
        }
    }

    storage::StorageNodeLivenessState ToProtoLiveness(
        const storedemo::StorageNodeRegistryLiveness liveness)
    {
        switch (liveness)
        {
        case storedemo::StorageNodeRegistryLiveness::kLive:
            return storage::STORAGE_NODE_LIVENESS_STATE_LIVE;
        case storedemo::StorageNodeRegistryLiveness::kStale:
            return storage::STORAGE_NODE_LIVENESS_STATE_STALE;
        case storedemo::StorageNodeRegistryLiveness::kDead:
            return storage::STORAGE_NODE_LIVENESS_STATE_DEAD;
        default:
            return storage::STORAGE_NODE_LIVENESS_STATE_UNSPECIFIED;
        }
    }

    storedemo::StorageNodeRegistryFacts MakeRegistryFacts(
        const std::size_t index,
        const std::uint64_t total_capacity_bytes = 8'192,
        const std::uint64_t used_capacity_bytes = 2'048,
        const storedemo::StorageNodeHealth health =
            storedemo::StorageNodeHealth::kHealthy,
        const storedemo::StorageNodeDiskPressure disk_pressure =
            storedemo::StorageNodeDiskPressure::kLow)
    {
        storedemo::StorageNodeRegistryFacts facts;
        facts.capacity.total_capacity_bytes = total_capacity_bytes;
        facts.capacity.used_capacity_bytes = used_capacity_bytes;
        facts.capacity.available_capacity_bytes =
            total_capacity_bytes >= used_capacity_bytes
                ? total_capacity_bytes - used_capacity_bytes
                : 0;
        facts.capacity.chunk_count = 10 + index;
        facts.health.health = health;
        facts.health.disk_pressure = disk_pressure;
        facts.health.io_error_count = index;
        facts.load.load.active_reads = static_cast<std::uint32_t>(index);
        facts.load.load.active_writes = static_cast<std::uint32_t>(index + 1);
        facts.load.load.queued_ops = static_cast<std::uint32_t>(index + 2);
        facts.failure_domain.zone = "zone-" + std::to_string(index % 2);
        facts.failure_domain.rack = "rack-" + std::to_string(index);
        return facts;
    }

    storedemo::StorageNodeClientRegisterStorageNodeRequest MakeClientRegisterRequest(
        const std::size_t index,
        const std::uint64_t observed_at_unix_ms)
    {
        storedemo::StorageNodeClientRegisterStorageNodeRequest request;
        request.request_id = "register-client-" + std::to_string(index);
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.endpoint = "127.0.0.1:" + std::to_string(7200 + index);
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.facts = MakeRegistryFacts(index);
        return request;
    }

    storedemo::StorageNodeClientHeartbeatRequest MakeClientHeartbeatRequest(
        const std::size_t index,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        storedemo::StorageNodeClientHeartbeatRequest request;
        request.request_id = "heartbeat-client-" + std::to_string(index) + "-" +
                             std::to_string(sequence);
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.endpoint = "127.0.0.1:" + std::to_string(7200 + index);
        request.sequence = sequence;
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.facts = MakeRegistryFacts(index);
        return request;
    }

    storedemo::StorageNodeClientHealthReportRequest MakeClientHealthReportRequest(
        const std::size_t index,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        storedemo::StorageNodeClientHealthReportRequest request;
        request.request_id = "health-client-" + std::to_string(index) + "-" +
                             std::to_string(sequence);
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.endpoint = "127.0.0.1:" + std::to_string(7200 + index);
        request.sequence = sequence;
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.health = MakeRegistryFacts(index).health;
        return request;
    }

    storedemo::StorageNodeClientCapacityReportRequest MakeClientCapacityReportRequest(
        const std::size_t index,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        storedemo::StorageNodeClientCapacityReportRequest request;
        request.request_id = "capacity-client-" + std::to_string(index) + "-" +
                             std::to_string(sequence);
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.endpoint = "127.0.0.1:" + std::to_string(7200 + index);
        request.sequence = sequence;
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.capacity = MakeRegistryFacts(index).capacity;
        return request;
    }

    storedemo::StorageNodeClientLoadReportRequest MakeClientLoadReportRequest(
        const std::size_t index,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        storedemo::StorageNodeClientLoadReportRequest request;
        request.request_id = "load-client-" + std::to_string(index) + "-" +
                             std::to_string(sequence);
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.endpoint = "127.0.0.1:" + std::to_string(7200 + index);
        request.sequence = sequence;
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.load = MakeRegistryFacts(index).load;
        return request;
    }

    void FillProtoRegistryFacts(const storedemo::StorageNodeRegistryFacts &facts,
                                storage::StorageNodeFacts *proto_facts)
    {
        ASSERT_NE(proto_facts, nullptr);
        proto_facts->mutable_capacity()->set_total_capacity_bytes(
            facts.capacity.total_capacity_bytes);
        proto_facts->mutable_capacity()->set_used_capacity_bytes(
            facts.capacity.used_capacity_bytes);
        proto_facts->mutable_capacity()->set_available_capacity_bytes(
            facts.capacity.available_capacity_bytes);
        proto_facts->mutable_capacity()->set_chunk_count(facts.capacity.chunk_count);
        proto_facts->mutable_health()->set_health(ToProtoNodeHealth(facts.health.health));
        proto_facts->mutable_health()->set_disk_pressure(
            ToProtoDiskPressure(facts.health.disk_pressure));
        proto_facts->mutable_health()->set_io_error_count(facts.health.io_error_count);
        proto_facts->mutable_load()->set_active_reads(facts.load.load.active_reads);
        proto_facts->mutable_load()->set_active_writes(facts.load.load.active_writes);
        proto_facts->mutable_load()->set_queued_ops(facts.load.load.queued_ops);
        proto_facts->mutable_load()->set_write_admission_overloaded(
            facts.load.write_admission_overloaded);
        proto_facts->mutable_load()->set_read_admission_overloaded(
            facts.load.read_admission_overloaded);
        proto_facts->mutable_failure_domain()->set_zone(facts.failure_domain.zone);
        proto_facts->mutable_failure_domain()->set_rack(facts.failure_domain.rack);
    }

    void FillProtoRegistrySnapshot(
        const storedemo::StorageNodeId &node_id,
        const std::string &endpoint,
        const std::uint64_t last_sequence,
        const std::uint64_t last_seen_unix_ms,
        const storedemo::StorageNodeRegistryLiveness liveness,
        const storedemo::StorageNodeRegistryFacts &facts,
        storage::StorageNodeRegistrySnapshot *proto_snapshot)
    {
        ASSERT_NE(proto_snapshot, nullptr);
        proto_snapshot->set_node_id(node_id);
        proto_snapshot->set_endpoint(endpoint);
        proto_snapshot->set_last_sequence(last_sequence);
        proto_snapshot->set_last_seen_unix_ms(last_seen_unix_ms);
        proto_snapshot->set_liveness(ToProtoLiveness(liveness));
        FillProtoRegistryFacts(facts, proto_snapshot->mutable_facts());
    }

    storage::RegisterStorageNodeResponse MakeProtoRegisterNodeResponse(
        const storedemo::StorageNodeStatusCode status,
        const storedemo::StorageNodeClientRegisterStorageNodeRequest &request,
        const bool created,
        const bool idempotent,
        const std::string &message = {},
        const std::uint64_t retry_after_ms = 0)
    {
        storage::RegisterStorageNodeResponse response;
        response.mutable_summary()->set_code(
            status == storedemo::StorageNodeStatusCode::kOk
                ? storage::STORAGE_NODE_STATUS_CODE_OK
                : status == storedemo::StorageNodeStatusCode::kConflict
                      ? storage::STORAGE_NODE_STATUS_CODE_CONFLICT
                      : status == storedemo::StorageNodeStatusCode::kInvalidArgument
                            ? storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT
                            : storage::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS);
        response.mutable_summary()->set_message(message);
        response.mutable_summary()->set_request_id(request.request_id);
        response.mutable_summary()->set_node_id(request.node_id);
        response.mutable_summary()->set_retry_after_ms(retry_after_ms);
        response.set_created(created);
        response.set_idempotent(idempotent);
        FillProtoRegistrySnapshot(request.node_id,
                                  request.endpoint,
                                  0,
                                  request.observed_at_unix_ms,
                                  storedemo::StorageNodeRegistryLiveness::kLive,
                                  request.facts,
                                  response.mutable_snapshot());
        return response;
    }

    storage::StorageNodeFactUpdateResponse MakeProtoFactUpdateResponse(
        const storedemo::StorageNodeStatusCode status,
        const std::string &request_id,
        const storedemo::StorageNodeId &node_id,
        const std::string &endpoint,
        const std::uint64_t accepted_sequence,
        const std::uint64_t last_seen_unix_ms,
        const bool applied,
        const bool idempotent,
        const bool stale_ignored,
        const storedemo::StorageNodeRegistryFacts &facts,
        const std::string &message = {},
        const std::uint64_t retry_after_ms = 0)
    {
        storage::StorageNodeFactUpdateResponse response;
        response.mutable_summary()->set_code(
            status == storedemo::StorageNodeStatusCode::kOk
                ? storage::STORAGE_NODE_STATUS_CODE_OK
                : status == storedemo::StorageNodeStatusCode::kInvalidArgument
                      ? storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT
                      : status == storedemo::StorageNodeStatusCode::kConflict
                            ? storage::STORAGE_NODE_STATUS_CODE_CONFLICT
                            : storage::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS);
        response.mutable_summary()->set_message(message);
        response.mutable_summary()->set_request_id(request_id);
        response.mutable_summary()->set_node_id(node_id);
        response.mutable_summary()->set_retry_after_ms(retry_after_ms);
        response.set_accepted_sequence(accepted_sequence);
        response.set_applied(applied);
        response.set_idempotent(idempotent);
        response.set_stale_ignored(stale_ignored);
        FillProtoRegistrySnapshot(node_id,
                                  endpoint,
                                  accepted_sequence,
                                  last_seen_unix_ms,
                                  storedemo::StorageNodeRegistryLiveness::kLive,
                                  facts,
                                  response.mutable_snapshot());
        return response;
    }

    storage::WriteChunkResponse MakeProtoWriteResponse(
        const storedemo::StorageNodeStatusCode status,
        const storedemo::ChunkIdentity &identity,
        const std::string &node_id,
        const storedemo::ChunkChecksum &checksum,
        const std::uint64_t size,
        const storedemo::ChunkState state,
        const bool durable,
        const bool already_exists,
        const std::string &message = {},
        const std::uint64_t retry_after_ms = 0)
    {
        storage::WriteChunkResponse response;

        storage::StorageNodeStatusCode proto_status =
            storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED;
        switch (status)
        {
        case storedemo::StorageNodeStatusCode::kOk:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_OK;
            break;
        case storedemo::StorageNodeStatusCode::kAlreadyExists:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS;
            break;
        case storedemo::StorageNodeStatusCode::kConflict:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_CONFLICT;
            break;
        case storedemo::StorageNodeStatusCode::kChecksumMismatch:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH;
            break;
        case storedemo::StorageNodeStatusCode::kOverloaded:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_OVERLOADED;
            break;
        case storedemo::StorageNodeStatusCode::kInvalidArgument:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT;
            break;
        default:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_IO_ERROR;
            break;
        }

        response.mutable_summary()->set_code(proto_status);
        response.mutable_summary()->set_message(message);
        response.mutable_summary()->set_request_id("proto-request-id");
        response.mutable_summary()->set_node_id(node_id);
        response.mutable_summary()->set_chunk_id(identity.chunk_id);
        response.mutable_summary()->set_retry_after_ms(retry_after_ms);
        response.set_size(size);
        FillProtoChecksum(checksum, response.mutable_checksum());

        switch (state)
        {
        case storedemo::ChunkState::kStaging:
            response.set_state(storage::STORAGE_CHUNK_STATE_STAGING);
            break;
        case storedemo::ChunkState::kLive:
            response.set_state(storage::STORAGE_CHUNK_STATE_LIVE);
            break;
        case storedemo::ChunkState::kDeleting:
            response.set_state(storage::STORAGE_CHUNK_STATE_DELETING);
            break;
        case storedemo::ChunkState::kDeleted:
            response.set_state(storage::STORAGE_CHUNK_STATE_DELETED);
            break;
        case storedemo::ChunkState::kQuarantined:
            response.set_state(storage::STORAGE_CHUNK_STATE_QUARANTINED);
            break;
        case storedemo::ChunkState::kCorrupted:
            response.set_state(storage::STORAGE_CHUNK_STATE_CORRUPTED);
            break;
        case storedemo::ChunkState::kMissing:
        default:
            response.set_state(storage::STORAGE_CHUNK_STATE_MISSING);
            break;
        }

        response.set_durable(durable);
        response.set_already_exists(already_exists);
        return response;
    }

    storage::ReadChunkResponse MakeProtoReadResponse(
        const storedemo::StorageNodeStatusCode status,
        const storedemo::ChunkIdentity &identity,
        const std::string &node_id,
        const storedemo::ChunkChecksum &checksum,
        const std::uint64_t size,
        const storedemo::ChunkState state,
        const std::string &payload,
        const std::uint64_t offset,
        const bool complete,
        const bool full_read,
        const std::string &message = {},
        const std::uint64_t retry_after_ms = 0)
    {
        storage::ReadChunkResponse response;

        storage::StorageNodeStatusCode proto_status =
            storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED;
        switch (status)
        {
        case storedemo::StorageNodeStatusCode::kOk:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_OK;
            break;
        case storedemo::StorageNodeStatusCode::kNotFound:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_NOT_FOUND;
            break;
        case storedemo::StorageNodeStatusCode::kConflict:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_CONFLICT;
            break;
        case storedemo::StorageNodeStatusCode::kChecksumMismatch:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH;
            break;
        case storedemo::StorageNodeStatusCode::kCorrupted:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_CORRUPTED;
            break;
        case storedemo::StorageNodeStatusCode::kUnsupported:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_UNSUPPORTED;
            break;
        case storedemo::StorageNodeStatusCode::kInvalidArgument:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT;
            break;
        default:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_IO_ERROR;
            break;
        }

        response.mutable_summary()->set_code(proto_status);
        response.mutable_summary()->set_message(message);
        response.mutable_summary()->set_request_id("proto-read-request-id");
        response.mutable_summary()->set_node_id(node_id);
        response.mutable_summary()->set_chunk_id(identity.chunk_id);
        response.mutable_summary()->set_retry_after_ms(retry_after_ms);
        response.set_chunk_id(identity.chunk_id);
        response.set_payload(payload);
        response.set_size(size);
        FillProtoChecksum(checksum, response.mutable_checksum());

        switch (state)
        {
        case storedemo::ChunkState::kStaging:
            response.set_state(storage::STORAGE_CHUNK_STATE_STAGING);
            break;
        case storedemo::ChunkState::kLive:
            response.set_state(storage::STORAGE_CHUNK_STATE_LIVE);
            break;
        case storedemo::ChunkState::kDeleting:
            response.set_state(storage::STORAGE_CHUNK_STATE_DELETING);
            break;
        case storedemo::ChunkState::kDeleted:
            response.set_state(storage::STORAGE_CHUNK_STATE_DELETED);
            break;
        case storedemo::ChunkState::kQuarantined:
            response.set_state(storage::STORAGE_CHUNK_STATE_QUARANTINED);
            break;
        case storedemo::ChunkState::kCorrupted:
            response.set_state(storage::STORAGE_CHUNK_STATE_CORRUPTED);
            break;
        case storedemo::ChunkState::kMissing:
        default:
            response.set_state(storage::STORAGE_CHUNK_STATE_MISSING);
            break;
        }

        response.set_offset(offset);
        response.set_complete(complete);
        response.set_full_read(full_read);
        return response;
    }

    storage::DeleteChunkResponse MakeProtoDeleteResponse(
        const storedemo::StorageNodeStatusCode status,
        const storedemo::ChunkIdentity &identity,
        const std::string &node_id,
        const storedemo::ChunkChecksum &checksum,
        const std::uint64_t size,
        const storedemo::ChunkState state,
        const bool deleted,
        const bool already_missing,
        const bool already_deleted,
        const bool retryable,
        const std::string &message = {},
        const std::uint64_t retry_after_ms = 0)
    {
        storage::DeleteChunkResponse response;

        storage::StorageNodeStatusCode proto_status =
            storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED;
        switch (status)
        {
        case storedemo::StorageNodeStatusCode::kOk:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_OK;
            break;
        case storedemo::StorageNodeStatusCode::kChecksumMismatch:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH;
            break;
        case storedemo::StorageNodeStatusCode::kTimeout:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_TIMEOUT;
            break;
        case storedemo::StorageNodeStatusCode::kInvalidArgument:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT;
            break;
        case storedemo::StorageNodeStatusCode::kNotFound:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_NOT_FOUND;
            break;
        default:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_IO_ERROR;
            break;
        }

        response.mutable_summary()->set_code(proto_status);
        response.mutable_summary()->set_message(message);
        response.mutable_summary()->set_request_id("proto-delete-request-id");
        response.mutable_summary()->set_node_id(node_id);
        response.mutable_summary()->set_chunk_id(identity.chunk_id);
        response.mutable_summary()->set_retry_after_ms(retry_after_ms);
        response.set_chunk_id(identity.chunk_id);
        response.set_size(size);
        FillProtoChecksum(checksum, response.mutable_checksum());

        switch (state)
        {
        case storedemo::ChunkState::kStaging:
            response.set_state(storage::STORAGE_CHUNK_STATE_STAGING);
            break;
        case storedemo::ChunkState::kLive:
            response.set_state(storage::STORAGE_CHUNK_STATE_LIVE);
            break;
        case storedemo::ChunkState::kDeleting:
            response.set_state(storage::STORAGE_CHUNK_STATE_DELETING);
            break;
        case storedemo::ChunkState::kDeleted:
            response.set_state(storage::STORAGE_CHUNK_STATE_DELETED);
            break;
        case storedemo::ChunkState::kQuarantined:
            response.set_state(storage::STORAGE_CHUNK_STATE_QUARANTINED);
            break;
        case storedemo::ChunkState::kCorrupted:
            response.set_state(storage::STORAGE_CHUNK_STATE_CORRUPTED);
            break;
        case storedemo::ChunkState::kMissing:
        default:
            response.set_state(storage::STORAGE_CHUNK_STATE_MISSING);
            break;
        }

        response.set_deleted(deleted);
        response.set_already_missing(already_missing);
        response.set_already_deleted(already_deleted);
        response.set_retryable(retryable);
        return response;
    }

    storage::BatchDeleteChunkResult MakeProtoBatchDeleteResult(
        const storedemo::StorageNodeStatusCode status,
        const storedemo::ChunkIdentity &identity,
        const std::string &node_id,
        const storedemo::ChunkChecksum &checksum,
        const std::uint64_t size,
        const storedemo::ChunkState state,
        const bool deleted,
        const bool already_missing,
        const bool already_deleted,
        const bool retryable,
        const std::string &message = {},
        const std::uint64_t retry_after_ms = 0)
    {
        const auto single_response = MakeProtoDeleteResponse(status,
                                                             identity,
                                                             node_id,
                                                             checksum,
                                                             size,
                                                             state,
                                                             deleted,
                                                             already_missing,
                                                             already_deleted,
                                                             retryable,
                                                             message,
                                                             retry_after_ms);

        storage::BatchDeleteChunkResult result;
        *result.mutable_summary() = single_response.summary();
        result.set_chunk_id(single_response.chunk_id());
        result.set_size(single_response.size());
        *result.mutable_checksum() = single_response.checksum();
        result.set_state(single_response.state());
        result.set_deleted(single_response.deleted());
        result.set_already_missing(single_response.already_missing());
        result.set_already_deleted(single_response.already_deleted());
        result.set_retryable(single_response.retryable());
        return result;
    }

    class FakeStorageNodeStub final : public storage::StorageNodeService::StubInterface
    {
    public:
        grpc::Status WriteChunk(grpc::ClientContext *context,
                                const storage::WriteChunkRequest &request,
                                storage::WriteChunkResponse *response) override
        {
            ++write_calls;
            last_request = request;
            call_observed_at = std::chrono::system_clock::now();
            observed_deadline = context->deadline();

            if (write_handler)
            {
                return write_handler(context, request, response);
            }

            return grpc::Status::OK;
        }

        grpc::Status ReadChunk(grpc::ClientContext *context,
                               const storage::ReadChunkRequest &request,
                               storage::ReadChunkResponse *response) override
        {
            ++read_calls;
            last_read_request = request;
            read_call_observed_at = std::chrono::system_clock::now();
            read_observed_deadline = context->deadline();

            if (read_handler)
            {
                return read_handler(context, request, response);
            }

            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "ReadChunk is not implemented in this fake stub");
        }

        grpc::Status DeleteChunk(grpc::ClientContext *context,
                                 const storage::DeleteChunkRequest &request,
                                 storage::DeleteChunkResponse *response) override
        {
            ++delete_calls;
            last_delete_request = request;
            delete_call_observed_at = std::chrono::system_clock::now();
            delete_observed_deadline = context->deadline();

            if (delete_handler)
            {
                return delete_handler(context, request, response);
            }

            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "DeleteChunk is not implemented in this fake stub");
        }

        grpc::Status BatchDeleteChunks(grpc::ClientContext *context,
                                       const storage::BatchDeleteChunksRequest &request,
                                       storage::BatchDeleteChunksResponse *response) override
        {
            ++batch_delete_calls;
            last_batch_delete_request = request;
            batch_delete_call_observed_at = std::chrono::system_clock::now();
            batch_delete_observed_deadline = context->deadline();

            if (batch_delete_handler)
            {
                return batch_delete_handler(context, request, response);
            }

            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "BatchDeleteChunks is not implemented in this fake stub");
        }

        grpc::Status ScrubChunk(grpc::ClientContext *context,
                                const storage::ScrubChunkRequest &request,
                                storage::ScrubChunkResponse *response) override
        {
            ++scrub_calls;
            last_scrub_request = request;
            scrub_call_observed_at = std::chrono::system_clock::now();
            scrub_observed_deadline = context->deadline();

            if (scrub_handler)
            {
                return scrub_handler(context, request, response);
            }

            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "ScrubChunk is not implemented in this fake stub");
        }

        grpc::Status RepairChunk(grpc::ClientContext *context,
                                 const storage::RepairChunkRequest &request,
                                 storage::RepairChunkResponse *response) override
        {
            ++repair_calls;
            last_repair_request = request;
            repair_call_observed_at = std::chrono::system_clock::now();
            repair_observed_deadline = context->deadline();

            if (repair_handler)
            {
                return repair_handler(context, request, response);
            }

            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "RepairChunk is not implemented in this fake stub");
        }

        grpc::Status RegisterStorageNode(
            grpc::ClientContext *context,
            const storage::RegisterStorageNodeRequest &request,
            storage::RegisterStorageNodeResponse *response) override
        {
            ++register_calls;
            last_register_request = request;
            register_call_observed_at = std::chrono::system_clock::now();
            register_observed_deadline = context->deadline();

            if (register_handler)
            {
                return register_handler(context, request, response);
            }

            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "RegisterStorageNode is not implemented in this fake stub");
        }

        grpc::Status UpdateStorageNodeHeartbeat(
            grpc::ClientContext *context,
            const storage::UpdateStorageNodeHeartbeatRequest &request,
            storage::StorageNodeFactUpdateResponse *response) override
        {
            ++heartbeat_calls;
            last_heartbeat_request = request;
            heartbeat_call_observed_at = std::chrono::system_clock::now();
            heartbeat_observed_deadline = context->deadline();

            if (heartbeat_handler)
            {
                return heartbeat_handler(context, request, response);
            }

            return grpc::Status(
                grpc::StatusCode::UNIMPLEMENTED,
                "UpdateStorageNodeHeartbeat is not implemented in this fake stub");
        }

        grpc::Status ReportHealth(grpc::ClientContext *context,
                                  const storage::ReportHealthRequest &request,
                                  storage::StorageNodeFactUpdateResponse *response) override
        {
            ++health_report_calls;
            last_health_report_request = request;
            health_report_call_observed_at = std::chrono::system_clock::now();
            health_report_observed_deadline = context->deadline();

            if (health_report_handler)
            {
                return health_report_handler(context, request, response);
            }

            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "ReportHealth is not implemented in this fake stub");
        }

        grpc::Status ReportCapacity(grpc::ClientContext *context,
                                    const storage::ReportCapacityRequest &request,
                                    storage::StorageNodeFactUpdateResponse *response) override
        {
            ++capacity_report_calls;
            last_capacity_report_request = request;
            capacity_report_call_observed_at = std::chrono::system_clock::now();
            capacity_report_observed_deadline = context->deadline();

            if (capacity_report_handler)
            {
                return capacity_report_handler(context, request, response);
            }

            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "ReportCapacity is not implemented in this fake stub");
        }

        grpc::Status ReportLoad(grpc::ClientContext *context,
                                const storage::ReportLoadRequest &request,
                                storage::StorageNodeFactUpdateResponse *response) override
        {
            ++load_report_calls;
            last_load_report_request = request;
            load_report_call_observed_at = std::chrono::system_clock::now();
            load_report_observed_deadline = context->deadline();

            if (load_report_handler)
            {
                return load_report_handler(context, request, response);
            }

            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "ReportLoad is not implemented in this fake stub");
        }

        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::WriteChunkRequest &,
                                   storage::WriteChunkResponse *)>
            write_handler;
        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::ReadChunkRequest &,
                                   storage::ReadChunkResponse *)>
            read_handler;
        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::DeleteChunkRequest &,
                                   storage::DeleteChunkResponse *)>
            delete_handler;
        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::BatchDeleteChunksRequest &,
                                   storage::BatchDeleteChunksResponse *)>
            batch_delete_handler;
        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::ScrubChunkRequest &,
                                   storage::ScrubChunkResponse *)>
            scrub_handler;
        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::RepairChunkRequest &,
                                   storage::RepairChunkResponse *)>
            repair_handler;
        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::RegisterStorageNodeRequest &,
                                   storage::RegisterStorageNodeResponse *)>
            register_handler;
        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::UpdateStorageNodeHeartbeatRequest &,
                                   storage::StorageNodeFactUpdateResponse *)>
            heartbeat_handler;
        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::ReportHealthRequest &,
                                   storage::StorageNodeFactUpdateResponse *)>
            health_report_handler;
        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::ReportCapacityRequest &,
                                   storage::StorageNodeFactUpdateResponse *)>
            capacity_report_handler;
        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::ReportLoadRequest &,
                                   storage::StorageNodeFactUpdateResponse *)>
            load_report_handler;
        storage::WriteChunkRequest last_request;
        storage::ReadChunkRequest last_read_request;
        storage::DeleteChunkRequest last_delete_request;
        storage::BatchDeleteChunksRequest last_batch_delete_request;
        storage::ScrubChunkRequest last_scrub_request;
        storage::RepairChunkRequest last_repair_request;
        storage::RegisterStorageNodeRequest last_register_request;
        storage::UpdateStorageNodeHeartbeatRequest last_heartbeat_request;
        storage::ReportHealthRequest last_health_report_request;
        storage::ReportCapacityRequest last_capacity_report_request;
        storage::ReportLoadRequest last_load_report_request;
        std::size_t write_calls{0};
        std::size_t read_calls{0};
        std::size_t delete_calls{0};
        std::size_t batch_delete_calls{0};
        std::size_t scrub_calls{0};
        std::size_t repair_calls{0};
        std::size_t register_calls{0};
        std::size_t heartbeat_calls{0};
        std::size_t health_report_calls{0};
        std::size_t capacity_report_calls{0};
        std::size_t load_report_calls{0};
        std::chrono::system_clock::time_point call_observed_at{};
        std::chrono::system_clock::time_point observed_deadline{};
        std::chrono::system_clock::time_point read_call_observed_at{};
        std::chrono::system_clock::time_point read_observed_deadline{};
        std::chrono::system_clock::time_point delete_call_observed_at{};
        std::chrono::system_clock::time_point delete_observed_deadline{};
        std::chrono::system_clock::time_point batch_delete_call_observed_at{};
        std::chrono::system_clock::time_point batch_delete_observed_deadline{};
        std::chrono::system_clock::time_point scrub_call_observed_at{};
        std::chrono::system_clock::time_point scrub_observed_deadline{};
        std::chrono::system_clock::time_point repair_call_observed_at{};
        std::chrono::system_clock::time_point repair_observed_deadline{};
        std::chrono::system_clock::time_point register_call_observed_at{};
        std::chrono::system_clock::time_point register_observed_deadline{};
        std::chrono::system_clock::time_point heartbeat_call_observed_at{};
        std::chrono::system_clock::time_point heartbeat_observed_deadline{};
        std::chrono::system_clock::time_point health_report_call_observed_at{};
        std::chrono::system_clock::time_point health_report_observed_deadline{};
        std::chrono::system_clock::time_point capacity_report_call_observed_at{};
        std::chrono::system_clock::time_point capacity_report_observed_deadline{};
        std::chrono::system_clock::time_point load_report_call_observed_at{};
        std::chrono::system_clock::time_point load_report_observed_deadline{};

    private:
        grpc::ClientAsyncResponseReaderInterface<storage::WriteChunkResponse> *
        AsyncWriteChunkRaw(grpc::ClientContext *,
                           const storage::WriteChunkRequest &,
                           grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::WriteChunkResponse> *
        PrepareAsyncWriteChunkRaw(grpc::ClientContext *,
                                  const storage::WriteChunkRequest &,
                                  grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::ReadChunkResponse> *
        AsyncReadChunkRaw(grpc::ClientContext *,
                          const storage::ReadChunkRequest &,
                          grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::ReadChunkResponse> *
        PrepareAsyncReadChunkRaw(grpc::ClientContext *,
                                 const storage::ReadChunkRequest &,
                                 grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::DeleteChunkResponse> *
        AsyncDeleteChunkRaw(grpc::ClientContext *,
                            const storage::DeleteChunkRequest &,
                            grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::DeleteChunkResponse> *
        PrepareAsyncDeleteChunkRaw(grpc::ClientContext *,
                                   const storage::DeleteChunkRequest &,
                                   grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::BatchDeleteChunksResponse> *
        AsyncBatchDeleteChunksRaw(grpc::ClientContext *,
                                  const storage::BatchDeleteChunksRequest &,
                                  grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::BatchDeleteChunksResponse> *
        PrepareAsyncBatchDeleteChunksRaw(grpc::ClientContext *,
                                         const storage::BatchDeleteChunksRequest &,
                                         grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::ScrubChunkResponse> *
        AsyncScrubChunkRaw(grpc::ClientContext *,
                           const storage::ScrubChunkRequest &,
                           grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::ScrubChunkResponse> *
        PrepareAsyncScrubChunkRaw(grpc::ClientContext *,
                                  const storage::ScrubChunkRequest &,
                                  grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::RepairChunkResponse> *
        AsyncRepairChunkRaw(grpc::ClientContext *,
                            const storage::RepairChunkRequest &,
                            grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::RepairChunkResponse> *
        PrepareAsyncRepairChunkRaw(grpc::ClientContext *,
                                   const storage::RepairChunkRequest &,
                                   grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::RegisterStorageNodeResponse> *
        AsyncRegisterStorageNodeRaw(grpc::ClientContext *,
                                    const storage::RegisterStorageNodeRequest &,
                                    grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::RegisterStorageNodeResponse> *
        PrepareAsyncRegisterStorageNodeRaw(
            grpc::ClientContext *,
            const storage::RegisterStorageNodeRequest &,
            grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::StorageNodeFactUpdateResponse> *
        AsyncUpdateStorageNodeHeartbeatRaw(
            grpc::ClientContext *,
            const storage::UpdateStorageNodeHeartbeatRequest &,
            grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::StorageNodeFactUpdateResponse> *
        PrepareAsyncUpdateStorageNodeHeartbeatRaw(
            grpc::ClientContext *,
            const storage::UpdateStorageNodeHeartbeatRequest &,
            grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::StorageNodeFactUpdateResponse> *
        AsyncReportHealthRaw(grpc::ClientContext *,
                             const storage::ReportHealthRequest &,
                             grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::StorageNodeFactUpdateResponse> *
        PrepareAsyncReportHealthRaw(grpc::ClientContext *,
                                    const storage::ReportHealthRequest &,
                                    grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::StorageNodeFactUpdateResponse> *
        AsyncReportCapacityRaw(grpc::ClientContext *,
                               const storage::ReportCapacityRequest &,
                               grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::StorageNodeFactUpdateResponse> *
        PrepareAsyncReportCapacityRaw(grpc::ClientContext *,
                                      const storage::ReportCapacityRequest &,
                                      grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::StorageNodeFactUpdateResponse> *
        AsyncReportLoadRaw(grpc::ClientContext *,
                           const storage::ReportLoadRequest &,
                           grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::StorageNodeFactUpdateResponse> *
        PrepareAsyncReportLoadRaw(grpc::ClientContext *,
                                  const storage::ReportLoadRequest &,
                                  grpc::CompletionQueue *) override
        {
            return nullptr;
        }
    };

    class RunningStorageNodeService
    {
    public:
        explicit RunningStorageNodeService(std::shared_ptr<storedemo::StorageNodeService> service)
            : service_(std::move(service))
        {
            constexpr int kMaxMessageBytes = 32 * 1024 * 1024;
            grpc::ServerBuilder builder;
            builder.SetMaxReceiveMessageSize(kMaxMessageBytes);
            builder.SetMaxSendMessageSize(kMaxMessageBytes);
            builder.AddListeningPort("127.0.0.1:0",
                                     grpc::InsecureServerCredentials(),
                                     &selected_port_);
            builder.RegisterService(service_.get());
            server_ = builder.BuildAndStart();
            if (server_ == nullptr || selected_port_ <= 0)
            {
                throw std::runtime_error("failed to start in-process storage node service");
            }

            grpc::ChannelArguments channel_arguments;
            channel_arguments.SetMaxReceiveMessageSize(kMaxMessageBytes);
            channel_arguments.SetMaxSendMessageSize(kMaxMessageBytes);
            channel_ = grpc::CreateCustomChannel(
                "127.0.0.1:" + std::to_string(selected_port_),
                grpc::InsecureChannelCredentials(),
                channel_arguments);
            if (!channel_->WaitForConnected(std::chrono::system_clock::now() + 5s))
            {
                throw std::runtime_error("storage node test channel did not connect");
            }
        }

        ~RunningStorageNodeService()
        {
            if (server_ != nullptr)
            {
                server_->Shutdown();
                server_->Wait();
            }
        }

        RunningStorageNodeService(const RunningStorageNodeService &) = delete;
        RunningStorageNodeService &operator=(const RunningStorageNodeService &) = delete;

        [[nodiscard]] std::shared_ptr<grpc::Channel> channel() const
        {
            return channel_;
        }

    private:
        std::shared_ptr<storedemo::StorageNodeService> service_;
        std::unique_ptr<grpc::Server> server_;
        std::shared_ptr<grpc::Channel> channel_;
        int selected_port_{0};
    };

    class StorageNodeClientTest : public ::testing::Test
    {
    protected:
        static storedemo::LocalDiskChunkStoreConfig MakeStoreConfig(
            const std::filesystem::path &data_dir,
            const std::size_t index)
        {
            return storedemo::LocalDiskChunkStoreConfig{
                .data_dir = data_dir,
                .node_id = storedemo::test::MakeStorageNodeIdFixture(index)};
        }
    };

    TEST_F(StorageNodeClientTest, ConstructingWithoutStubThrows)
    {
        EXPECT_THROW(storedemo::StorageNodeClient(
                         std::unique_ptr<storage::StorageNodeService::StubInterface>()),
                     std::invalid_argument);
    }

    TEST_F(StorageNodeClientTest, RegisterStorageNodeMapsRequestFieldsAndSuccessResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto request = MakeClientRegisterRequest(64, 100);

        stub_ptr->register_handler =
            [request](grpc::ClientContext *,
                      const storage::RegisterStorageNodeRequest &,
                      storage::RegisterStorageNodeResponse *response)
        {
            *response = MakeProtoRegisterNodeResponse(storedemo::StorageNodeStatusCode::kOk,
                                                      request,
                                                      true,
                                                      false);
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};

        const auto response = client.RegisterStorageNode(
            request,
            {.context = {.timeout_ms = 900, .best_effort_cancel = true}});

        ASSERT_EQ(stub_ptr->register_calls, 1U);
        EXPECT_EQ(stub_ptr->last_register_request.request_id(), request.request_id);
        EXPECT_EQ(stub_ptr->last_register_request.node_id(), request.node_id);
        EXPECT_EQ(stub_ptr->last_register_request.endpoint(), request.endpoint);
        EXPECT_EQ(stub_ptr->last_register_request.observed_at_unix_ms(),
                  request.observed_at_unix_ms);
        EXPECT_EQ(stub_ptr->last_register_request.facts().capacity().total_capacity_bytes(),
                  request.facts.capacity.total_capacity_bytes);
        EXPECT_EQ(stub_ptr->last_register_request.facts().health().health(),
                  ToProtoNodeHealth(request.facts.health.health));
        EXPECT_EQ(stub_ptr->last_register_request.facts().load().active_writes(),
                  request.facts.load.load.active_writes);
        EXPECT_EQ(stub_ptr->last_register_request.facts().failure_domain().zone(),
                  request.facts.failure_domain.zone);

        const auto deadline_delta =
            stub_ptr->register_observed_deadline - stub_ptr->register_call_observed_at;
        EXPECT_GT(deadline_delta, 0ms);
        EXPECT_LE(deadline_delta, 1200ms);

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(response.created);
        EXPECT_FALSE(response.idempotent);
        EXPECT_EQ(response.snapshot.node_id, request.node_id);
        EXPECT_EQ(response.snapshot.endpoint, request.endpoint);
        EXPECT_EQ(response.snapshot.last_seen_unix_ms, request.observed_at_unix_ms);
        EXPECT_EQ(response.snapshot.facts.capacity.total_capacity_bytes,
                  request.facts.capacity.total_capacity_bytes);
        EXPECT_EQ(response.snapshot.facts.health.health, request.facts.health.health);
        EXPECT_EQ(response.snapshot.facts.load.load.queued_ops,
                  request.facts.load.load.queued_ops);
    }

    TEST_F(StorageNodeClientTest, HeartbeatAndReportsMapRequestFieldsAndSuccessResponses)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        auto heartbeat_request = MakeClientHeartbeatRequest(65, 7, 160);
        heartbeat_request.facts = MakeRegistryFacts(65,
                                                    16'384,
                                                    4'096,
                                                    storedemo::StorageNodeHealth::kDegraded,
                                                    storedemo::StorageNodeDiskPressure::kMedium);
        heartbeat_request.facts.load.write_admission_overloaded = true;

        auto health_request = MakeClientHealthReportRequest(65, 8, 170);
        health_request.health.health = storedemo::StorageNodeHealth::kReadOnly;
        health_request.health.disk_pressure = storedemo::StorageNodeDiskPressure::kHigh;
        health_request.health.io_error_count = 9;

        auto capacity_request = MakeClientCapacityReportRequest(65, 9, 180);
        capacity_request.capacity.total_capacity_bytes = 32'768;
        capacity_request.capacity.used_capacity_bytes = 12'288;
        capacity_request.capacity.available_capacity_bytes = 20'480;
        capacity_request.capacity.chunk_count = 88;

        auto load_request = MakeClientLoadReportRequest(65, 10, 190);
        load_request.load.load.active_reads = 11;
        load_request.load.load.active_writes = 12;
        load_request.load.load.queued_ops = 13;
        load_request.load.write_admission_overloaded = true;
        load_request.load.read_admission_overloaded = true;

        stub_ptr->heartbeat_handler =
            [heartbeat_request](grpc::ClientContext *,
                                const storage::UpdateStorageNodeHeartbeatRequest &,
                                storage::StorageNodeFactUpdateResponse *response)
        {
            *response = MakeProtoFactUpdateResponse(storedemo::StorageNodeStatusCode::kOk,
                                                    heartbeat_request.request_id,
                                                    heartbeat_request.node_id,
                                                    heartbeat_request.endpoint,
                                                    heartbeat_request.sequence,
                                                    heartbeat_request.observed_at_unix_ms,
                                                    true,
                                                    false,
                                                    false,
                                                    heartbeat_request.facts);
            return grpc::Status::OK;
        };
        stub_ptr->health_report_handler =
            [health_request](grpc::ClientContext *,
                             const storage::ReportHealthRequest &,
                             storage::StorageNodeFactUpdateResponse *response)
        {
            *response = MakeProtoFactUpdateResponse(storedemo::StorageNodeStatusCode::kOk,
                                                    health_request.request_id,
                                                    health_request.node_id,
                                                    health_request.endpoint,
                                                    health_request.sequence,
                                                    health_request.observed_at_unix_ms,
                                                    true,
                                                    false,
                                                    false,
                                                    MakeRegistryFacts(65));
            response->mutable_snapshot()->mutable_facts()->mutable_health()->set_health(
                ToProtoNodeHealth(health_request.health.health));
            response->mutable_snapshot()->mutable_facts()->mutable_health()->set_disk_pressure(
                ToProtoDiskPressure(health_request.health.disk_pressure));
            response->mutable_snapshot()->mutable_facts()->mutable_health()->set_io_error_count(
                health_request.health.io_error_count);
            return grpc::Status::OK;
        };
        stub_ptr->capacity_report_handler =
            [capacity_request](grpc::ClientContext *,
                               const storage::ReportCapacityRequest &,
                               storage::StorageNodeFactUpdateResponse *response)
        {
            *response = MakeProtoFactUpdateResponse(storedemo::StorageNodeStatusCode::kOk,
                                                    capacity_request.request_id,
                                                    capacity_request.node_id,
                                                    capacity_request.endpoint,
                                                    capacity_request.sequence,
                                                    capacity_request.observed_at_unix_ms,
                                                    true,
                                                    false,
                                                    false,
                                                    MakeRegistryFacts(65));
            response->mutable_snapshot()->mutable_facts()->mutable_capacity()->set_total_capacity_bytes(
                capacity_request.capacity.total_capacity_bytes);
            response->mutable_snapshot()->mutable_facts()->mutable_capacity()->set_used_capacity_bytes(
                capacity_request.capacity.used_capacity_bytes);
            response->mutable_snapshot()->mutable_facts()->mutable_capacity()->set_available_capacity_bytes(
                capacity_request.capacity.available_capacity_bytes);
            response->mutable_snapshot()->mutable_facts()->mutable_capacity()->set_chunk_count(
                capacity_request.capacity.chunk_count);
            return grpc::Status::OK;
        };
        stub_ptr->load_report_handler =
            [load_request](grpc::ClientContext *,
                           const storage::ReportLoadRequest &,
                           storage::StorageNodeFactUpdateResponse *response)
        {
            *response = MakeProtoFactUpdateResponse(storedemo::StorageNodeStatusCode::kOk,
                                                    load_request.request_id,
                                                    load_request.node_id,
                                                    load_request.endpoint,
                                                    load_request.sequence,
                                                    load_request.observed_at_unix_ms,
                                                    true,
                                                    false,
                                                    false,
                                                    MakeRegistryFacts(65));
            response->mutable_snapshot()->mutable_facts()->mutable_load()->set_active_reads(
                load_request.load.load.active_reads);
            response->mutable_snapshot()->mutable_facts()->mutable_load()->set_active_writes(
                load_request.load.load.active_writes);
            response->mutable_snapshot()->mutable_facts()->mutable_load()->set_queued_ops(
                load_request.load.load.queued_ops);
            response->mutable_snapshot()->mutable_facts()->mutable_load()->set_write_admission_overloaded(
                load_request.load.write_admission_overloaded);
            response->mutable_snapshot()->mutable_facts()->mutable_load()->set_read_admission_overloaded(
                load_request.load.read_admission_overloaded);
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};

        const auto heartbeat_response = client.UpdateStorageNodeHeartbeat(
            heartbeat_request,
            {.context = {.timeout_ms = 1000}});
        const auto health_response = client.ReportHealth(
            health_request,
            {.context = {.timeout_ms = 1000, .best_effort_cancel = true}});
        const auto capacity_response = client.ReportCapacity(
            capacity_request,
            {.context = {.timeout_ms = 1000}});
        const auto load_response = client.ReportLoad(
            load_request,
            {.context = {.timeout_ms = 1000}});

        ASSERT_EQ(stub_ptr->heartbeat_calls, 1U);
        EXPECT_EQ(stub_ptr->last_heartbeat_request.request_id(),
                  heartbeat_request.request_id);
        EXPECT_EQ(stub_ptr->last_heartbeat_request.heartbeat().node_id(),
                  heartbeat_request.node_id);
        EXPECT_EQ(stub_ptr->last_heartbeat_request.heartbeat().endpoint(),
                  heartbeat_request.endpoint);
        EXPECT_EQ(stub_ptr->last_heartbeat_request.heartbeat().sequence(),
                  heartbeat_request.sequence);
        EXPECT_EQ(
            stub_ptr->last_heartbeat_request.heartbeat().facts().capacity().chunk_count(),
            heartbeat_request.facts.capacity.chunk_count);
        EXPECT_TRUE(
            stub_ptr->last_heartbeat_request.heartbeat().facts().load().write_admission_overloaded());

        ASSERT_EQ(stub_ptr->health_report_calls, 1U);
        EXPECT_EQ(stub_ptr->last_health_report_request.request_id(),
                  health_request.request_id);
        EXPECT_EQ(stub_ptr->last_health_report_request.health().health(),
                  ToProtoNodeHealth(health_request.health.health));
        EXPECT_EQ(stub_ptr->last_health_report_request.health().disk_pressure(),
                  ToProtoDiskPressure(health_request.health.disk_pressure));
        EXPECT_EQ(stub_ptr->last_health_report_request.health().io_error_count(),
                  health_request.health.io_error_count);

        ASSERT_EQ(stub_ptr->capacity_report_calls, 1U);
        EXPECT_EQ(stub_ptr->last_capacity_report_request.request_id(),
                  capacity_request.request_id);
        EXPECT_EQ(stub_ptr->last_capacity_report_request.capacity().total_capacity_bytes(),
                  capacity_request.capacity.total_capacity_bytes);
        EXPECT_EQ(stub_ptr->last_capacity_report_request.capacity().available_capacity_bytes(),
                  capacity_request.capacity.available_capacity_bytes);

        ASSERT_EQ(stub_ptr->load_report_calls, 1U);
        EXPECT_EQ(stub_ptr->last_load_report_request.request_id(), load_request.request_id);
        EXPECT_EQ(stub_ptr->last_load_report_request.load().active_reads(),
                  load_request.load.load.active_reads);
        EXPECT_TRUE(
            stub_ptr->last_load_report_request.load().write_admission_overloaded());
        EXPECT_TRUE(
            stub_ptr->last_load_report_request.load().read_admission_overloaded());

        EXPECT_EQ(heartbeat_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(heartbeat_response.applied);
        EXPECT_EQ(heartbeat_response.accepted_sequence, heartbeat_request.sequence);
        EXPECT_EQ(heartbeat_response.snapshot.facts.capacity.chunk_count,
                  heartbeat_request.facts.capacity.chunk_count);

        EXPECT_EQ(health_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(health_response.snapshot.facts.health.health,
                  health_request.health.health);
        EXPECT_EQ(health_response.snapshot.facts.health.disk_pressure,
                  health_request.health.disk_pressure);

        EXPECT_EQ(capacity_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(capacity_response.snapshot.facts.capacity.total_capacity_bytes,
                  capacity_request.capacity.total_capacity_bytes);

        EXPECT_EQ(load_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(load_response.snapshot.facts.load.load.active_writes,
                  load_request.load.load.active_writes);
        EXPECT_TRUE(load_response.snapshot.facts.load.write_admission_overloaded);
    }

    TEST_F(StorageNodeClientTest, RegisterStorageNodeMapsGrpcFailureStatuses)
    {
        struct StatusCase
        {
            grpc::StatusCode grpc_code;
            storedemo::StorageNodeStatusCode expected_status;
            const char *message;
        };

        const std::vector<StatusCase> cases{
            {grpc::StatusCode::DEADLINE_EXCEEDED,
             storedemo::StorageNodeStatusCode::kTimeout,
             "deadline"},
            {grpc::StatusCode::CANCELLED,
             storedemo::StorageNodeStatusCode::kCancelled,
             "cancelled"},
            {grpc::StatusCode::UNAVAILABLE,
             storedemo::StorageNodeStatusCode::kNodeUnavailable,
             "unavailable"},
            {grpc::StatusCode::INVALID_ARGUMENT,
             storedemo::StorageNodeStatusCode::kInvalidArgument,
             "invalid"},
            {grpc::StatusCode::INTERNAL,
             storedemo::StorageNodeStatusCode::kIoError,
             "internal"}};

        for (const auto &status_case : cases)
        {
            SCOPED_TRACE(status_case.message);

            auto *stub_ptr = new FakeStorageNodeStub();
            stub_ptr->register_handler =
                [status_case](grpc::ClientContext *,
                              const storage::RegisterStorageNodeRequest &,
                              storage::RegisterStorageNodeResponse *)
            {
                return grpc::Status(status_case.grpc_code, status_case.message);
            };

            storedemo::StorageNodeClient client{
                std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
            const auto response = client.RegisterStorageNode(
                MakeClientRegisterRequest(66, 100),
                {.context = {.timeout_ms = 500}});

            EXPECT_EQ(response.status, status_case.expected_status);
            EXPECT_EQ(response.error_detail, status_case.message);
        }
    }

    TEST_F(StorageNodeClientTest,
           RegisterAndHeartbeatMatchRealServiceRegistrySemantics)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_client_registry");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 64));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto registry = std::make_shared<storedemo::StorageNodeRegistry>();
        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id,
            registry);
        RunningStorageNodeService server(service);
        storedemo::StorageNodeClient client{server.channel()};

        const auto original = MakeClientRegisterRequest(67, 100);
        const auto created = client.RegisterStorageNode(original);
        ASSERT_EQ(created.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(created.created);
        EXPECT_FALSE(created.idempotent);

        auto duplicate = original;
        duplicate.observed_at_unix_ms = 140;
        duplicate.facts.capacity.total_capacity_bytes = 16'384;
        duplicate.facts.capacity.available_capacity_bytes = 14'336;
        const auto duplicate_response = client.RegisterStorageNode(duplicate);
        EXPECT_EQ(duplicate_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_FALSE(duplicate_response.created);
        EXPECT_TRUE(duplicate_response.idempotent);
        EXPECT_EQ(duplicate_response.snapshot.last_seen_unix_ms,
                  original.observed_at_unix_ms);
        EXPECT_EQ(duplicate_response.snapshot.facts.capacity.total_capacity_bytes,
                  original.facts.capacity.total_capacity_bytes);

        auto conflict = original;
        conflict.endpoint = "127.0.0.1:7999";
        const auto conflict_response = client.RegisterStorageNode(conflict);
        EXPECT_EQ(conflict_response.status, storedemo::StorageNodeStatusCode::kConflict);

        auto heartbeat = MakeClientHeartbeatRequest(67, 7, 160);
        heartbeat.facts = MakeRegistryFacts(67,
                                            32'768,
                                            12'288,
                                            storedemo::StorageNodeHealth::kDegraded,
                                            storedemo::StorageNodeDiskPressure::kMedium);
        heartbeat.facts.load.load.active_reads = 8;
        heartbeat.facts.load.load.active_writes = 3;
        heartbeat.facts.load.load.queued_ops = 11;
        heartbeat.facts.load.write_admission_overloaded = true;
        heartbeat.facts.capacity.chunk_count = 77;
        const auto applied = client.UpdateStorageNodeHeartbeat(heartbeat);
        ASSERT_EQ(applied.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(applied.applied);
        EXPECT_EQ(applied.accepted_sequence, 7U);

        auto stale = MakeClientHeartbeatRequest(67, 6, 170);
        stale.facts = MakeRegistryFacts(67, 4'096, 1'024);
        const auto stale_response = client.UpdateStorageNodeHeartbeat(stale);
        EXPECT_EQ(stale_response.status,
                  storedemo::StorageNodeStatusCode::kAlreadyExists);
        EXPECT_TRUE(stale_response.stale_ignored);
        EXPECT_EQ(stale_response.accepted_sequence, 7U);

        auto duplicate_heartbeat = heartbeat;
        duplicate_heartbeat.observed_at_unix_ms = 180;
        duplicate_heartbeat.facts = MakeRegistryFacts(67, 8'192, 4'096);
        const auto duplicate_heartbeat_response =
            client.UpdateStorageNodeHeartbeat(duplicate_heartbeat);
        EXPECT_EQ(duplicate_heartbeat_response.status,
                  storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(duplicate_heartbeat_response.idempotent);
        EXPECT_FALSE(duplicate_heartbeat_response.applied);

        const auto lookup = registry->LookupNode(original.node_id, 181);
        ASSERT_EQ(lookup.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup.snapshot.last_sequence, 7U);
        EXPECT_EQ(lookup.snapshot.last_seen_unix_ms, 160U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.total_capacity_bytes, 32'768U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.chunk_count, 77U);
        EXPECT_EQ(lookup.snapshot.facts.health.health,
                  storedemo::StorageNodeHealth::kDegraded);
        EXPECT_TRUE(lookup.snapshot.facts.load.write_admission_overloaded);
    }

    TEST_F(StorageNodeClientTest,
           ReportHealthCapacityAndLoadMergeThroughRealServiceAndInvalidRequestsMapClearly)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_client_reports");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 65));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto registry = std::make_shared<storedemo::StorageNodeRegistry>();
        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id,
            registry);
        RunningStorageNodeService server(service);
        storedemo::StorageNodeClient client{server.channel()};

        const auto register_request = MakeClientRegisterRequest(68, 100);
        ASSERT_EQ(client.RegisterStorageNode(register_request).status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto health_request = MakeClientHealthReportRequest(68, 2, 120);
        health_request.health.health = storedemo::StorageNodeHealth::kReadOnly;
        health_request.health.disk_pressure = storedemo::StorageNodeDiskPressure::kHigh;
        health_request.health.io_error_count = 5;
        const auto health_response = client.ReportHealth(health_request);
        ASSERT_EQ(health_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(health_response.applied);

        auto capacity_request = MakeClientCapacityReportRequest(68, 3, 130);
        capacity_request.capacity.total_capacity_bytes = 65'536;
        capacity_request.capacity.used_capacity_bytes = 16'384;
        capacity_request.capacity.available_capacity_bytes = 49'152;
        capacity_request.capacity.chunk_count = 99;
        const auto capacity_response = client.ReportCapacity(capacity_request);
        ASSERT_EQ(capacity_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(capacity_response.accepted_sequence, 3U);

        auto load_request = MakeClientLoadReportRequest(68, 4, 140);
        load_request.load.load.active_reads = 21;
        load_request.load.load.active_writes = 22;
        load_request.load.load.queued_ops = 23;
        load_request.load.write_admission_overloaded = true;
        load_request.load.read_admission_overloaded = false;
        const auto load_response = client.ReportLoad(load_request);
        ASSERT_EQ(load_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(load_response.accepted_sequence, 4U);

        const auto lookup = registry->LookupNode(register_request.node_id, 145);
        ASSERT_EQ(lookup.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup.snapshot.facts.health.health,
                  storedemo::StorageNodeHealth::kReadOnly);
        EXPECT_EQ(lookup.snapshot.facts.health.disk_pressure,
                  storedemo::StorageNodeDiskPressure::kHigh);
        EXPECT_EQ(lookup.snapshot.facts.capacity.total_capacity_bytes, 65'536U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.chunk_count, 99U);
        EXPECT_EQ(lookup.snapshot.facts.load.load.active_writes, 22U);
        EXPECT_TRUE(lookup.snapshot.facts.load.write_admission_overloaded);

        auto invalid_register = MakeClientRegisterRequest(69, 200);
        invalid_register.endpoint = "invalid-endpoint";
        const auto invalid_register_response = client.RegisterStorageNode(invalid_register);
        EXPECT_EQ(invalid_register_response.status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);

        auto invalid_capacity = MakeClientCapacityReportRequest(68, 5, 150);
        invalid_capacity.capacity.total_capacity_bytes = 10;
        invalid_capacity.capacity.used_capacity_bytes = 20;
        invalid_capacity.capacity.available_capacity_bytes = 0;
        const auto invalid_capacity_response = client.ReportCapacity(invalid_capacity);
        EXPECT_EQ(invalid_capacity_response.status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsRequestFieldsAndSuccessResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-success", 9, 2, 2048);
        const auto payload = storedemo::test::MakeChunkPayload(180, "t032-success");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(storedemo::StorageNodeStatusCode::kOk,
                                               identity,
                                               "client-node-t032",
                                               checksum,
                                               checksum.size_bytes,
                                               storedemo::ChunkState::kLive,
                                               true,
                                               false);
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};

        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-success-t032"),
            {.context = {.timeout_ms = 1200, .best_effort_cancel = true}});

        ASSERT_EQ(stub_ptr->write_calls, 1U);
        EXPECT_EQ(stub_ptr->last_request.request_id(), "write-success-t032");
        EXPECT_EQ(stub_ptr->last_request.chunk_id(), identity.chunk_id);
        EXPECT_EQ(stub_ptr->last_request.object_id(), identity.object_id);
        EXPECT_EQ(stub_ptr->last_request.version(), identity.version);
        EXPECT_EQ(stub_ptr->last_request.chunk_index(), identity.chunk_index);
        EXPECT_EQ(stub_ptr->last_request.offset(), identity.offset);
        EXPECT_EQ(stub_ptr->last_request.expected_size(),
                  static_cast<std::uint64_t>(payload.size()));
        EXPECT_EQ(stub_ptr->last_request.expected_checksum().value(), checksum.value);
        EXPECT_EQ(stub_ptr->last_request.payload(), payload);
        EXPECT_EQ(stub_ptr->last_request.timeout_ms(), 1200U);
        EXPECT_TRUE(stub_ptr->last_request.best_effort_cancel());
        EXPECT_EQ(stub_ptr->last_request.durability(),
                  storage::WRITE_CHUNK_DURABILITY_PUBLISH);

        const auto deadline_delta = stub_ptr->observed_deadline - stub_ptr->call_observed_at;
        EXPECT_GT(deadline_delta, 0ms);
        EXPECT_LE(deadline_delta, 1500ms);

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(response.durable);
        EXPECT_FALSE(response.already_exists);
        EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);
        EXPECT_EQ(response.metadata.identity.object_id, identity.object_id);
        EXPECT_EQ(response.metadata.identity.version, identity.version);
        EXPECT_EQ(response.metadata.identity.chunk_index, identity.chunk_index);
        EXPECT_EQ(response.metadata.identity.offset, identity.offset);
        EXPECT_EQ(response.metadata.node_id, "client-node-t032");
        EXPECT_EQ(response.metadata.size, checksum.size_bytes);
        EXPECT_EQ(response.metadata.checksum.value, checksum.value);
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsAlreadyExistsResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity =
            MakeStoreIdentityOrThrow("obj-t032-already-exists", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(64, "t032-already-exists");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(
                storedemo::StorageNodeStatusCode::kAlreadyExists,
                identity,
                "client-node-t032",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kLive,
                true,
                true,
                "already durable");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.WriteChunk(MakeWriteRequest(identity, payload, "write-already-t032"));

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kAlreadyExists);
        EXPECT_TRUE(response.durable);
        EXPECT_TRUE(response.already_exists);
        EXPECT_EQ(response.error_detail, "already durable");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsConflictResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-conflict", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(72, "t032-conflict");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(storedemo::StorageNodeStatusCode::kConflict,
                                               identity,
                                               "client-node-t032",
                                               checksum,
                                               checksum.size_bytes,
                                               storedemo::ChunkState::kLive,
                                               false,
                                               false,
                                               "content conflict");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.WriteChunk(MakeWriteRequest(identity, payload, "write-conflict-t032"));

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kConflict);
        EXPECT_FALSE(response.durable);
        EXPECT_EQ(response.error_detail, "content conflict");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsChecksumMismatchResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-checksum", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(80, "t032-checksum");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(
                storedemo::StorageNodeStatusCode::kChecksumMismatch,
                identity,
                "client-node-t032",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kMissing,
                false,
                false,
                "checksum mismatch");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.WriteChunk(MakeWriteRequest(identity, payload, "write-checksum-t032"));

        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_FALSE(response.durable);
        EXPECT_EQ(response.error_detail, "checksum mismatch");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsOverloadedResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-overloaded", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(96, "t032-overloaded");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(storedemo::StorageNodeStatusCode::kOverloaded,
                                               identity,
                                               "client-node-t032",
                                               checksum,
                                               checksum.size_bytes,
                                               storedemo::ChunkState::kMissing,
                                               false,
                                               false,
                                               "queue full",
                                               55);
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.WriteChunk(MakeWriteRequest(identity, payload, "write-overloaded-t032"));

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOverloaded);
        EXPECT_EQ(response.retry_after_ms, 55U);
        EXPECT_EQ(response.error_detail, "queue full");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsGrpcDeadlineExceededToTimeout)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        stub_ptr->write_handler =
            [](grpc::ClientContext *,
               const storage::WriteChunkRequest &,
               storage::WriteChunkResponse *)
        {
            return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                "rpc deadline exceeded");
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-timeout", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "t032-timeout");

        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-timeout-t032"),
            {.context = {.timeout_ms = 50}});

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kTimeout);
        EXPECT_EQ(response.error_detail, "rpc deadline exceeded");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsGrpcCancelledToCancelled)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        stub_ptr->write_handler =
            [](grpc::ClientContext *,
               const storage::WriteChunkRequest &,
               storage::WriteChunkResponse *)
        {
            return grpc::Status(grpc::StatusCode::CANCELLED,
                                "cancelled by caller");
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-cancelled", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "t032-cancelled");

        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-cancelled-t032"),
            {.context = {.timeout_ms = 300, .best_effort_cancel = true}});

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kCancelled);
        EXPECT_EQ(response.error_detail, "cancelled by caller");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsGrpcUnavailableToNodeUnavailable)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        stub_ptr->write_handler =
            [](grpc::ClientContext *,
               const storage::WriteChunkRequest &,
               storage::WriteChunkResponse *)
        {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "remote unavailable");
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-unavailable", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "t032-unavailable");

        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-unavailable-t032"));

        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_EQ(response.error_detail, "remote unavailable");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsInvalidArgumentResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity =
            MakeStoreIdentityOrThrow("obj-t032-invalid-argument", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(48, "t032-invalid-argument");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(
                storedemo::StorageNodeStatusCode::kInvalidArgument,
                identity,
                "client-node-t032",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kMissing,
                false,
                false,
                "request invalid");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-invalid-argument-t032"));

        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);
        EXPECT_EQ(response.error_detail, "request invalid");
    }

    TEST_F(StorageNodeClientTest, WriteChunkRetriesRetryableUnavailableWithinBudget)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-retry", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "t032-retry");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum, attempts = 0](grpc::ClientContext *,
                                               const storage::WriteChunkRequest &,
                                               storage::WriteChunkResponse *response) mutable
        {
            ++attempts;
            if (attempts == 1)
            {
                return grpc::Status(grpc::StatusCode::UNAVAILABLE, "first unavailable");
            }

            *response = MakeProtoWriteResponse(storedemo::StorageNodeStatusCode::kOk,
                                               identity,
                                               "client-node-t032",
                                               checksum,
                                               checksum.size_bytes,
                                               storedemo::ChunkState::kLive,
                                               true,
                                               false);
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr),
            {.max_write_retries = 1}};
        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-retry-t032"));

        EXPECT_EQ(stub_ptr->write_calls, 2U);
        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(response.durable);
    }

    TEST_F(StorageNodeClientTest, WriteChunkDoesNotRetryNonRetryableConflict)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-no-retry", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "t032-no-retry");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(storedemo::StorageNodeStatusCode::kConflict,
                                               identity,
                                               "client-node-t032",
                                               checksum,
                                               checksum.size_bytes,
                                               storedemo::ChunkState::kLive,
                                               false,
                                               false,
                                               "conflict");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr),
            {.max_write_retries = 3}};
        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-no-retry-t032"));

        EXPECT_EQ(stub_ptr->write_calls, 1U);
        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kConflict);
    }

    TEST_F(StorageNodeClientTest, WriteChunkBinaryPayloadUsesFixtureThroughRealService)
    {
        const auto fixture = LoadFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_client_binary");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 32));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        storedemo::StorageNodeClient client{server.channel()};
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-binary", 1, 0, 0);
        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-binary-t032"),
            {.context = {.timeout_ms = 1500, .best_effort_cancel = true}});

        ASSERT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk)
            << response.error_detail;
        EXPECT_TRUE(response.durable);
        EXPECT_FALSE(response.already_exists);
        EXPECT_EQ(response.metadata.node_id, store->config().node_id);
        EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);

        const auto read_response =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-binary-t032"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, fixture.payload);
    }

    TEST_F(StorageNodeClientTest, ReadChunkMapsRequestFieldsAndFullReadSuccessResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t044-success", 9, 2, 2048);
        const auto payload = storedemo::test::MakeChunkPayload(180, "t044-success");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->read_handler =
            [identity, payload, checksum](grpc::ClientContext *,
                                          const storage::ReadChunkRequest &,
                                          storage::ReadChunkResponse *response)
        {
            *response = MakeProtoReadResponse(storedemo::StorageNodeStatusCode::kOk,
                                              identity,
                                              "client-node-t044",
                                              checksum,
                                              checksum.size_bytes,
                                              storedemo::ChunkState::kLive,
                                              payload,
                                              identity.offset,
                                              true,
                                              true);
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};

        auto request = MakeReadRequest(identity.chunk_id, "read-success-t044");
        request.expected_checksum = checksum;
        request.verify_checksum = true;
        const auto response = client.ReadChunk(
            request,
            {.context = {.timeout_ms = 1200, .best_effort_cancel = true}});

        ASSERT_EQ(stub_ptr->read_calls, 1U);
        EXPECT_EQ(stub_ptr->last_read_request.request_id(), "read-success-t044");
        EXPECT_EQ(stub_ptr->last_read_request.chunk_id(), identity.chunk_id);
        EXPECT_EQ(stub_ptr->last_read_request.offset(), 0U);
        EXPECT_EQ(stub_ptr->last_read_request.length(), 0U);
        EXPECT_EQ(stub_ptr->last_read_request.expected_checksum().value(), checksum.value);
        EXPECT_EQ(stub_ptr->last_read_request.timeout_ms(), 1200U);
        EXPECT_TRUE(stub_ptr->last_read_request.best_effort_cancel());
        EXPECT_TRUE(stub_ptr->last_read_request.verify_checksum());

        const auto deadline_delta =
            stub_ptr->read_observed_deadline - stub_ptr->read_call_observed_at;
        EXPECT_GT(deadline_delta, 0ms);
        EXPECT_LE(deadline_delta, 1500ms);

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);
        EXPECT_EQ(response.metadata.identity.object_id, identity.object_id);
        EXPECT_EQ(response.metadata.identity.version, identity.version);
        EXPECT_EQ(response.metadata.identity.chunk_index, identity.chunk_index);
        EXPECT_EQ(response.metadata.identity.offset, identity.offset);
        EXPECT_EQ(response.metadata.node_id, "client-node-t044");
        EXPECT_EQ(response.metadata.size, checksum.size_bytes);
        EXPECT_EQ(response.metadata.checksum.value, checksum.value);
        EXPECT_EQ(response.actual_checksum.value, checksum.value);
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kLive);
        EXPECT_EQ(response.payload, payload);
        EXPECT_TRUE(response.verified);
    }

    TEST_F(StorageNodeClientTest, ReadChunkMapsChecksumMismatchResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t044-checksum", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(80, "t044-checksum");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->read_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::ReadChunkRequest &,
                                 storage::ReadChunkResponse *response)
        {
            *response = MakeProtoReadResponse(
                storedemo::StorageNodeStatusCode::kChecksumMismatch,
                identity,
                "client-node-t044",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kLive,
                {},
                identity.offset,
                false,
                false,
                "checksum mismatch");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        auto request = MakeReadRequest(identity.chunk_id, "read-checksum-t044");
        request.expected_checksum = ComputeStoreChecksumOrThrow("different-payload");
        request.verify_checksum = true;
        const auto response = client.ReadChunk(request);

        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_EQ(response.error_detail, "checksum mismatch");
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kLive);
        EXPECT_EQ(response.actual_checksum.value, checksum.value);
        EXPECT_TRUE(response.verified);
    }

    TEST_F(StorageNodeClientTest, ReadChunkMapsMissingChunkResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t044-missing", 1, 0, 0);
        const storedemo::ChunkChecksum checksum;

        stub_ptr->read_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::ReadChunkRequest &,
                                 storage::ReadChunkResponse *response)
        {
            *response = MakeProtoReadResponse(storedemo::StorageNodeStatusCode::kNotFound,
                                              identity,
                                              "client-node-t044",
                                              checksum,
                                              0,
                                              storedemo::ChunkState::kMissing,
                                              {},
                                              0,
                                              false,
                                              false,
                                              "missing chunk");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.ReadChunk(MakeReadRequest(identity.chunk_id, "read-missing-t044"));

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(response.error_detail, "missing chunk");
        EXPECT_TRUE(response.payload.empty());
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kMissing);
        EXPECT_FALSE(response.verified);
    }

    TEST_F(StorageNodeClientTest, ReadChunkMapsNonLiveCorruptedResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t044-non-live", 1, 0, 0);
        const auto checksum =
            ComputeStoreChecksumOrThrow(storedemo::test::MakeChunkPayload(48, "t044-non-live"));

        stub_ptr->read_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::ReadChunkRequest &,
                                 storage::ReadChunkResponse *response)
        {
            *response = MakeProtoReadResponse(storedemo::StorageNodeStatusCode::kCorrupted,
                                              identity,
                                              "client-node-t044",
                                              checksum,
                                              checksum.size_bytes,
                                              storedemo::ChunkState::kQuarantined,
                                              {},
                                              0,
                                              false,
                                              false,
                                              "quarantined chunk");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.ReadChunk(MakeReadRequest(identity.chunk_id, "read-non-live-t044"));

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kCorrupted);
        EXPECT_EQ(response.error_detail, "quarantined chunk");
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kQuarantined);
        EXPECT_FALSE(response.verified);
    }

    TEST_F(StorageNodeClientTest, ReadChunkMapsRangeBoundaryResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t044-range", 1, 0, 0);
        const auto checksum =
            ComputeStoreChecksumOrThrow(storedemo::test::MakeChunkPayload(48, "t044-range"));

        stub_ptr->read_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::ReadChunkRequest &request,
                                 storage::ReadChunkResponse *response)
        {
            EXPECT_EQ(request.offset(), 7U);
            EXPECT_EQ(request.length(), 19U);
            *response = MakeProtoReadResponse(storedemo::StorageNodeStatusCode::kUnsupported,
                                              identity,
                                              "client-node-t044",
                                              checksum,
                                              checksum.size_bytes,
                                              storedemo::ChunkState::kLive,
                                              {},
                                              0,
                                              false,
                                              false,
                                              "range unsupported");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        auto request = MakeReadRequest(identity.chunk_id, "read-range-t044");
        request.range = storedemo::ChunkReadRange{.offset = 7, .length = 19};
        request.expected_checksum = checksum;
        const auto response = client.ReadChunk(request);

        EXPECT_EQ(stub_ptr->read_calls, 1U);
        EXPECT_EQ(stub_ptr->last_read_request.offset(), 7U);
        EXPECT_EQ(stub_ptr->last_read_request.length(), 19U);
        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kUnsupported);
        EXPECT_EQ(response.error_detail, "range unsupported");
        EXPECT_TRUE(response.payload.empty());
    }

    TEST_F(StorageNodeClientTest, ReadChunkMapsGrpcDeadlineExceededToTimeout)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        stub_ptr->read_handler =
            [](grpc::ClientContext *,
               const storage::ReadChunkRequest &,
               storage::ReadChunkResponse *)
        {
            return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                "rpc deadline exceeded");
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response = client.ReadChunk(
            MakeReadRequest("obj-t044-timeout~1~0", "read-timeout-t044"),
            {.context = {.timeout_ms = 50}});

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kTimeout);
        EXPECT_EQ(response.error_detail, "rpc deadline exceeded");
    }

    TEST_F(StorageNodeClientTest, ReadChunkMapsGrpcCancelledToCancelled)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        stub_ptr->read_handler =
            [](grpc::ClientContext *,
               const storage::ReadChunkRequest &,
               storage::ReadChunkResponse *)
        {
            return grpc::Status(grpc::StatusCode::CANCELLED,
                                "cancelled by caller");
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response = client.ReadChunk(
            MakeReadRequest("obj-t044-cancelled~1~0", "read-cancelled-t044"),
            {.context = {.timeout_ms = 300, .best_effort_cancel = true}});

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kCancelled);
        EXPECT_EQ(response.error_detail, "cancelled by caller");
    }

    TEST_F(StorageNodeClientTest, ReadChunkMapsGrpcUnavailableToNodeUnavailable)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        stub_ptr->read_handler =
            [](grpc::ClientContext *,
               const storage::ReadChunkRequest &,
               storage::ReadChunkResponse *)
        {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "remote unavailable");
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response = client.ReadChunk(
            MakeReadRequest("obj-t044-unavailable~1~0", "read-unavailable-t044"));

        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_EQ(response.error_detail, "remote unavailable");
    }

    TEST_F(StorageNodeClientTest, ReadChunkMapsGrpcInternalToIoError)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        stub_ptr->read_handler =
            [](grpc::ClientContext *,
               const storage::ReadChunkRequest &,
               storage::ReadChunkResponse *)
        {
            return grpc::Status(grpc::StatusCode::INTERNAL, "backend io error");
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.ReadChunk(MakeReadRequest("obj-t044-io~1~0", "read-io-t044"));

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kIoError);
        EXPECT_EQ(response.error_detail, "backend io error");
    }

    TEST_F(StorageNodeClientTest, ReadChunkMapsInvalidArgumentResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t044-invalid", 1, 0, 0);
        const storedemo::ChunkChecksum checksum;

        stub_ptr->read_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::ReadChunkRequest &,
                                 storage::ReadChunkResponse *response)
        {
            *response = MakeProtoReadResponse(
                storedemo::StorageNodeStatusCode::kInvalidArgument,
                identity,
                "client-node-t044",
                checksum,
                0,
                storedemo::ChunkState::kMissing,
                {},
                0,
                false,
                false,
                "request invalid");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.ReadChunk(MakeReadRequest(identity.chunk_id, "read-invalid-t044"));

        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);
        EXPECT_EQ(response.error_detail, "request invalid");
    }

    TEST_F(StorageNodeClientTest, ReadChunkBinaryPayloadUsesFixtureThroughRealServiceWithoutCommittingMetadata)
    {
        const auto fixture = LoadFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t044-read",
                            "create-bucket-t044-read"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t044-read", 1, 0, 4096);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t044-read",
                                                        "objects/test_file.deb",
                                                        identity.object_id,
                                                        "create-object-t044-read",
                                                        fixture.payload.size(),
                                                        "etag-t044-read"))
                        .Ok);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_client_read_binary");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 44));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-read-binary-t044",
                          .identity = identity,
                          .expected_size =
                              static_cast<std::uint64_t>(fixture.payload.size()),
                          .expected_checksum =
                              ComputeStoreChecksumOrThrow(fixture.payload),
                          .payload = fixture.payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        storedemo::StorageNodeClient client{server.channel()};
        auto request = MakeReadRequest(identity.chunk_id, "read-binary-t044");
        request.expected_checksum = ComputeStoreChecksumOrThrow(fixture.payload);
        request.verify_checksum = true;
        const auto response = client.ReadChunk(
            request,
            {.context = {.timeout_ms = 1500, .best_effort_cancel = true}});

        ASSERT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk)
            << response.error_detail;
        EXPECT_EQ(response.metadata.node_id, store->config().node_id);
        EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);
        EXPECT_EQ(response.metadata.identity.offset, identity.offset);
        EXPECT_EQ(response.payload, fixture.payload);
        EXPECT_EQ(response.metadata.checksum.value, request.expected_checksum.value);
        EXPECT_TRUE(response.verified);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t044-read", .object_key = "objects/test_file.deb"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());
    }

    TEST_F(StorageNodeClientTest, DeleteChunkMapsRequestFieldsAndSuccessResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t053-success", 9, 2, 0);
        const auto payload = storedemo::test::MakeChunkPayload(180, "t053-success");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->delete_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::DeleteChunkRequest &,
                                 storage::DeleteChunkResponse *response)
        {
            *response = MakeProtoDeleteResponse(storedemo::StorageNodeStatusCode::kOk,
                                                identity,
                                                "client-node-t053",
                                                checksum,
                                                checksum.size_bytes,
                                                storedemo::ChunkState::kDeleted,
                                                true,
                                                false,
                                                false,
                                                false);
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};

        auto request = MakeDeleteRequest(identity, "delete-success-t053");
        request.expected_checksum = checksum;
        const auto response = client.DeleteChunk(
            request,
            {.context = {.timeout_ms = 1200, .best_effort_cancel = true}});

        ASSERT_EQ(stub_ptr->delete_calls, 1U);
        EXPECT_EQ(stub_ptr->last_delete_request.request_id(), "delete-success-t053");
        EXPECT_EQ(stub_ptr->last_delete_request.chunk_id(), identity.chunk_id);
        EXPECT_EQ(stub_ptr->last_delete_request.object_id(), identity.object_id);
        EXPECT_EQ(stub_ptr->last_delete_request.version(), identity.version);
        EXPECT_EQ(stub_ptr->last_delete_request.chunk_index(), identity.chunk_index);
        EXPECT_EQ(stub_ptr->last_delete_request.expected_checksum().value(),
                  checksum.value);
        EXPECT_EQ(stub_ptr->last_delete_request.reason(), "client delete test");
        EXPECT_EQ(stub_ptr->last_delete_request.metadata_boundary(),
                  "metadata-first-boundary");
        EXPECT_EQ(stub_ptr->last_delete_request.timeout_ms(), 1200U);
        EXPECT_TRUE(stub_ptr->last_delete_request.best_effort_cancel());

        const auto deadline_delta =
            stub_ptr->delete_observed_deadline - stub_ptr->delete_call_observed_at;
        EXPECT_GT(deadline_delta, 0ms);
        EXPECT_LE(deadline_delta, 1500ms);

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(response.deleted);
        EXPECT_FALSE(response.already_missing);
        EXPECT_FALSE(response.already_deleted);
        EXPECT_FALSE(response.retryable);
        EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);
        EXPECT_EQ(response.metadata.identity.object_id, identity.object_id);
        EXPECT_EQ(response.metadata.identity.version, identity.version);
        EXPECT_EQ(response.metadata.identity.chunk_index, identity.chunk_index);
        EXPECT_EQ(response.metadata.node_id, "client-node-t053");
        EXPECT_EQ(response.metadata.size, checksum.size_bytes);
        EXPECT_EQ(response.metadata.checksum.value, checksum.value);
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kDeleted);
    }

    TEST_F(StorageNodeClientTest, DeleteChunkMapsMissingAndDeletedIdempotentResponses)
    {
        const auto missing_identity = MakeStoreIdentityOrThrow("obj-t053-missing", 1, 0, 0);
        const auto deleted_identity = MakeStoreIdentityOrThrow("obj-t053-deleted", 1, 0, 0);
        const auto checksum =
            ComputeStoreChecksumOrThrow(storedemo::test::MakeChunkPayload(96, "t053-delete"));

        {
            auto *stub_ptr = new FakeStorageNodeStub();
            stub_ptr->delete_handler =
                [missing_identity](grpc::ClientContext *,
                                   const storage::DeleteChunkRequest &,
                                   storage::DeleteChunkResponse *response)
            {
                *response = MakeProtoDeleteResponse(
                    storedemo::StorageNodeStatusCode::kOk,
                    missing_identity,
                    "client-node-t053",
                    {},
                    0,
                    storedemo::ChunkState::kMissing,
                    false,
                    true,
                    false,
                    false,
                    "already missing");
                return grpc::Status::OK;
            };

            storedemo::StorageNodeClient client{
                std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
            const auto response =
                client.DeleteChunk(MakeDeleteRequest(missing_identity, "delete-missing-t053"));

            EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk);
            EXPECT_FALSE(response.deleted);
            EXPECT_TRUE(response.already_missing);
            EXPECT_FALSE(response.already_deleted);
            EXPECT_FALSE(response.retryable);
            EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kMissing);
        }

        {
            auto *stub_ptr = new FakeStorageNodeStub();
            stub_ptr->delete_handler =
                [deleted_identity, checksum](grpc::ClientContext *,
                                             const storage::DeleteChunkRequest &,
                                             storage::DeleteChunkResponse *response)
            {
                *response = MakeProtoDeleteResponse(
                    storedemo::StorageNodeStatusCode::kOk,
                    deleted_identity,
                    "client-node-t053",
                    checksum,
                    checksum.size_bytes,
                    storedemo::ChunkState::kDeleted,
                    false,
                    true,
                    false,
                    false,
                    "already deleted");
                return grpc::Status::OK;
            };

            storedemo::StorageNodeClient client{
                std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
            auto request = MakeDeleteRequest(deleted_identity, "delete-deleted-t053");
            request.expected_checksum = checksum;
            const auto response = client.DeleteChunk(request);

            EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk);
            EXPECT_FALSE(response.deleted);
            EXPECT_TRUE(response.already_missing);
            EXPECT_TRUE(response.already_deleted);
            EXPECT_FALSE(response.retryable);
            EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kDeleted);
        }
    }

    TEST_F(StorageNodeClientTest, DeleteChunkMapsGrpcFailureStatuses)
    {
        struct Case
        {
            grpc::StatusCode grpc_code;
            storedemo::StorageNodeStatusCode expected_status;
            bool expected_retryable;
        };

        const std::vector<Case> cases{
            {grpc::StatusCode::DEADLINE_EXCEEDED,
             storedemo::StorageNodeStatusCode::kTimeout,
             true},
            {grpc::StatusCode::CANCELLED,
             storedemo::StorageNodeStatusCode::kCancelled,
             false},
            {grpc::StatusCode::UNAVAILABLE,
             storedemo::StorageNodeStatusCode::kNodeUnavailable,
             true},
            {grpc::StatusCode::INVALID_ARGUMENT,
             storedemo::StorageNodeStatusCode::kInvalidArgument,
             false},
            {grpc::StatusCode::INTERNAL,
             storedemo::StorageNodeStatusCode::kIoError,
             true}};

        const auto identity = MakeStoreIdentityOrThrow("obj-t053-grpc", 1, 0, 0);
        for (const auto &test_case : cases)
        {
            auto *stub_ptr = new FakeStorageNodeStub();
            stub_ptr->delete_handler =
                [test_case](grpc::ClientContext *,
                            const storage::DeleteChunkRequest &,
                            storage::DeleteChunkResponse *)
            {
                return grpc::Status(test_case.grpc_code, "delete grpc failure");
            };

            storedemo::StorageNodeClient client{
                std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
            const auto response =
                client.DeleteChunk(MakeDeleteRequest(identity, "delete-grpc-t053"));

            EXPECT_EQ(response.status, test_case.expected_status);
            EXPECT_EQ(response.error_detail, "delete grpc failure");
            EXPECT_EQ(response.retryable, test_case.expected_retryable);
        }
    }

    TEST_F(StorageNodeClientTest, DeleteChunkChecksumMismatchDoesNotRemoveLiveChunk)
    {
        const auto fixture = LoadDeleteFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.zip");

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_client_delete_checksum");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 53));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t053-checksum", 1, 0, 0);
        const auto checksum = ComputeStoreChecksumOrThrow(fixture.payload);
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-delete-checksum-t053",
                          .identity = identity,
                          .expected_size =
                              static_cast<std::uint64_t>(fixture.payload.size()),
                          .expected_checksum = checksum,
                          .payload = fixture.payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        storedemo::StorageNodeClient client{server.channel()};
        auto request = MakeDeleteRequest(identity, "delete-checksum-t053");
        request.expected_checksum =
            ComputeStoreChecksumOrThrow("different-delete-payload");
        const auto response = client.DeleteChunk(
            request,
            {.context = {.timeout_ms = 1500, .best_effort_cancel = true}});

        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_FALSE(response.deleted);
        EXPECT_FALSE(response.already_missing);
        EXPECT_FALSE(response.already_deleted);
        EXPECT_FALSE(response.retryable);
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kLive);

        const auto read_response = store->ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-delete-checksum-t053"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, fixture.payload);
    }

    TEST_F(StorageNodeClientTest,
           DeleteChunkUsesZipFixtureThroughRealServiceAndKeepsMetadataVisibility)
    {
        const auto fixture = LoadDeleteFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.zip");

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t053-delete",
                            "create-bucket-t053-delete"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t053-delete", 1, 0, 0);
        const auto checksum = ComputeStoreChecksumOrThrow(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t053-delete",
                                                        "objects/test_file.zip",
                                                        identity.object_id,
                                                        "create-object-t053-delete",
                                                        fixture.payload.size(),
                                                        "etag-t053-delete"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> chunks{
            raftdemo::ChunkRef{.chunk_id = identity.chunk_id,
                               .offset = identity.offset,
                               .size = static_cast<std::uint64_t>(fixture.payload.size()),
                               .replica_nodes = {storedemo::test::MakeStorageNodeIdFixture(54)},
                               .checksum = checksum.value}};
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCommitObjectCommandWithChunks("bucket-t053-delete",
                                                          "objects/test_file.zip",
                                                          identity.object_id,
                                                          "commit-object-t053-delete",
                                                          fixture.payload.size(),
                                                          "etag-t053-delete",
                                                          std::move(chunks)))
                        .Ok);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_client_delete_live");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 54));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-delete-live-t053",
                          .identity = identity,
                          .expected_size =
                              static_cast<std::uint64_t>(fixture.payload.size()),
                          .expected_checksum = checksum,
                          .payload = fixture.payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        storedemo::StorageNodeClient client{server.channel()};
        auto request = MakeDeleteRequest(identity, "delete-live-t053");
        request.expected_checksum = checksum;
        const auto response = client.DeleteChunk(
            request,
            {.context = {.timeout_ms = 1500, .best_effort_cancel = true}});

        ASSERT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk)
            << response.error_detail;
        EXPECT_TRUE(response.deleted);
        EXPECT_FALSE(response.already_missing);
        EXPECT_FALSE(response.already_deleted);
        EXPECT_FALSE(response.retryable);
        EXPECT_EQ(response.metadata.node_id, store->config().node_id);
        EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kDeleted);

        const auto read_after_delete =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-delete-live-t053"));
        EXPECT_NE(read_after_delete.status, storedemo::StorageNodeStatusCode::kOk);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t053-delete", .object_key = "objects/test_file.zip"});
        ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(head.record.has_value());
        EXPECT_TRUE(head.record->IsCommitted());

        const auto listed = machine.ListObjects(
            {.bucket = "bucket-t053-delete", .prefix = "objects/"});
        ASSERT_EQ(listed.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_EQ(listed.records.size(), 1U);
        EXPECT_EQ(listed.records.front().object_key, "objects/test_file.zip");
    }

    TEST_F(StorageNodeClientTest, BatchDeleteChunksMapsIndependentResultsAndAggregateFacts)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto success_identity = MakeStoreIdentityOrThrow("obj-t053-batch-success", 1, 0, 0);
        const auto missing_identity = MakeStoreIdentityOrThrow("obj-t053-batch-missing", 1, 0, 0);
        const auto deleted_identity = MakeStoreIdentityOrThrow("obj-t053-batch-deleted", 1, 0, 0);
        const auto retry_identity = MakeStoreIdentityOrThrow("obj-t053-batch-retry", 1, 0, 0);
        const auto nonretry_identity = MakeStoreIdentityOrThrow("obj-t053-batch-nonretry", 1, 0, 0);
        const auto checksum =
            ComputeStoreChecksumOrThrow(storedemo::test::MakeChunkPayload(128, "t053-batch"));

        stub_ptr->batch_delete_handler =
            [success_identity,
             missing_identity,
             deleted_identity,
             retry_identity,
             nonretry_identity,
             checksum](grpc::ClientContext *,
                       const storage::BatchDeleteChunksRequest &,
                       storage::BatchDeleteChunksResponse *response)
        {
            response->mutable_summary()->set_code(storage::STORAGE_NODE_STATUS_CODE_OK);
            response->mutable_summary()->set_message(
                "BatchDeleteChunks completed with partial failures");
            response->mutable_summary()->set_request_id("batch-delete-t053");
            response->mutable_summary()->set_node_id("client-node-t053");
            response->mutable_summary()->set_retry_after_ms(25);
            response->set_success_count(1);
            response->set_idempotent_count(2);
            response->set_retryable_failure_count(1);
            response->set_non_retryable_failure_count(1);
            response->set_partial_failure(true);
            *response->add_results() = MakeProtoBatchDeleteResult(
                storedemo::StorageNodeStatusCode::kOk,
                success_identity,
                "client-node-t053",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kDeleted,
                true,
                false,
                false,
                false);
            *response->add_results() = MakeProtoBatchDeleteResult(
                storedemo::StorageNodeStatusCode::kOk,
                missing_identity,
                "client-node-t053",
                {},
                0,
                storedemo::ChunkState::kMissing,
                false,
                true,
                false,
                false);
            *response->add_results() = MakeProtoBatchDeleteResult(
                storedemo::StorageNodeStatusCode::kOk,
                deleted_identity,
                "client-node-t053",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kDeleted,
                false,
                true,
                false,
                false);
            *response->add_results() = MakeProtoBatchDeleteResult(
                storedemo::StorageNodeStatusCode::kTimeout,
                retry_identity,
                "client-node-t053",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kLive,
                false,
                false,
                false,
                true,
                "retry later",
                25);
            *response->add_results() = MakeProtoBatchDeleteResult(
                storedemo::StorageNodeStatusCode::kInvalidArgument,
                nonretry_identity,
                "client-node-t053",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kLive,
                false,
                false,
                false,
                false,
                "non-retryable invalid");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};

        storedemo::StorageNodeClientBatchDeleteChunksRequest request;
        request.request_id = "batch-delete-t053";
        request.chunks = {MakeBatchDeleteChunkRequest(success_identity),
                          MakeBatchDeleteChunkRequest(missing_identity),
                          MakeBatchDeleteChunkRequest(deleted_identity),
                          MakeBatchDeleteChunkRequest(retry_identity),
                          MakeBatchDeleteChunkRequest(nonretry_identity)};
        for (auto &item : request.chunks)
        {
            item.expected_checksum = checksum;
        }
        request.chunks[1].expected_checksum = {};

        const auto response = client.BatchDeleteChunks(
            request,
            {.context = {.timeout_ms = 2400, .best_effort_cancel = true}});

        ASSERT_EQ(stub_ptr->batch_delete_calls, 1U);
        EXPECT_EQ(stub_ptr->last_batch_delete_request.request_id(), "batch-delete-t053");
        EXPECT_EQ(stub_ptr->last_batch_delete_request.timeout_ms(), 2400U);
        EXPECT_TRUE(stub_ptr->last_batch_delete_request.best_effort_cancel());
        ASSERT_EQ(stub_ptr->last_batch_delete_request.chunks_size(), 5);
        EXPECT_EQ(stub_ptr->last_batch_delete_request.chunks(0).chunk_id(),
                  success_identity.chunk_id);
        EXPECT_EQ(stub_ptr->last_batch_delete_request.chunks(0).object_id(),
                  success_identity.object_id);
        EXPECT_EQ(stub_ptr->last_batch_delete_request.chunks(3).expected_checksum().value(),
                  checksum.value);

        const auto deadline_delta = stub_ptr->batch_delete_observed_deadline -
                                    stub_ptr->batch_delete_call_observed_at;
        EXPECT_GT(deadline_delta, 0ms);
        EXPECT_LE(deadline_delta, 2700ms);

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(response.error_detail,
                  "BatchDeleteChunks completed with partial failures");
        EXPECT_EQ(response.retry_after_ms, 25U);
        EXPECT_EQ(response.success_count, 1U);
        EXPECT_EQ(response.idempotent_count, 2U);
        EXPECT_EQ(response.retryable_failure_count, 1U);
        EXPECT_EQ(response.non_retryable_failure_count, 1U);
        EXPECT_TRUE(response.partial_failure);
        ASSERT_EQ(response.results.size(), 5U);

        EXPECT_EQ(response.results[0].status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(response.results[0].deleted);
        EXPECT_FALSE(response.results[0].already_missing);
        EXPECT_FALSE(response.results[0].retryable);

        EXPECT_EQ(response.results[1].status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(response.results[1].already_missing);
        EXPECT_FALSE(response.results[1].already_deleted);
        EXPECT_EQ(response.results[1].metadata.state, storedemo::ChunkState::kMissing);

        EXPECT_EQ(response.results[2].status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(response.results[2].already_missing);
        EXPECT_TRUE(response.results[2].already_deleted);
        EXPECT_EQ(response.results[2].metadata.state, storedemo::ChunkState::kDeleted);

        EXPECT_EQ(response.results[3].status, storedemo::StorageNodeStatusCode::kTimeout);
        EXPECT_TRUE(response.results[3].retryable);
        EXPECT_EQ(response.results[3].retry_after_ms, 25U);

        EXPECT_EQ(response.results[4].status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);
        EXPECT_FALSE(response.results[4].retryable);
    }
} // namespace
