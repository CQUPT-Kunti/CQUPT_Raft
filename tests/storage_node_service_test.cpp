#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "raft/common/metadata_command.h"
#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/index/chunk_index.h"
#include "store/node/storage_node_registry.h"
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

    std::size_t CountRegularFilesRecursively(const std::filesystem::path &root)
    {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec))
        {
            return 0;
        }

        std::size_t count = 0;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (entry.is_regular_file())
            {
                ++count;
            }
        }
        return count;
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

    storedemo::ReadChunkRequest MakeReadRequest(const storedemo::ChunkId &chunk_id,
                                                const std::string &request_id)
    {
        return storedemo::ReadChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id};
    }

    storage::ReadChunkRequest MakeProtoReadRequest(const storedemo::ChunkIdentity &identity,
                                                   const std::string &request_id)
    {
        storage::ReadChunkRequest request;
        request.set_request_id(request_id);
        request.set_chunk_id(identity.chunk_id);
        request.set_object_id(identity.object_id);
        request.set_version(identity.version);
        request.set_chunk_index(identity.chunk_index);
        request.set_timeout_ms(1500);
        request.set_best_effort_cancel(true);
        return request;
    }

    void FillProtoChecksum(const storedemo::ChunkChecksum &checksum,
                           storage::StorageChunkChecksum *proto_checksum);

    storage::ScrubChunkRequest MakeProtoScrubRequest(
        const storedemo::ChunkIdentity &identity,
        const std::string &request_id,
        const storedemo::ChunkChecksum *checksum = nullptr,
        const std::uint64_t expected_size = 0)
    {
        storage::ScrubChunkRequest request;
        request.set_request_id(request_id);
        request.set_chunk_id(identity.chunk_id);
        request.set_object_id(identity.object_id);
        request.set_version(identity.version);
        request.set_chunk_index(identity.chunk_index);
        request.set_expected_size(expected_size);
        if (checksum != nullptr)
        {
            FillProtoChecksum(*checksum, request.mutable_expected_checksum());
        }
        request.set_timeout_ms(1500);
        request.set_best_effort_cancel(true);
        request.set_verify_checksum(true);
        request.set_quarantine_on_corruption(true);
        return request;
    }

    storage::RepairChunkRequest MakeProtoRepairRequest(
        const storedemo::ChunkIdentity &identity,
        const std::string &payload,
        const std::string &request_id,
        const storedemo::StorageNodeId &source_node_id = "repair-source-node",
        const storedemo::ChunkState source_state = storedemo::ChunkState::kLive,
        const bool source_checksum_verified = true)
    {
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        storage::RepairChunkRequest request;
        request.set_request_id(request_id);
        request.set_chunk_id(identity.chunk_id);
        request.set_object_id(identity.object_id);
        request.set_version(identity.version);
        request.set_chunk_index(identity.chunk_index);
        request.set_offset(identity.offset);
        request.set_expected_size(static_cast<std::uint64_t>(payload.size()));
        FillProtoChecksum(checksum, request.mutable_expected_checksum());
        request.set_source_node_id(source_node_id);
        request.set_source_size(static_cast<std::uint64_t>(payload.size()));
        FillProtoChecksum(checksum, request.mutable_source_checksum());
        switch (source_state)
        {
        case storedemo::ChunkState::kStaging:
            request.set_source_state(storage::STORAGE_CHUNK_STATE_STAGING);
            break;
        case storedemo::ChunkState::kLive:
            request.set_source_state(storage::STORAGE_CHUNK_STATE_LIVE);
            break;
        case storedemo::ChunkState::kDeleting:
            request.set_source_state(storage::STORAGE_CHUNK_STATE_DELETING);
            break;
        case storedemo::ChunkState::kDeleted:
            request.set_source_state(storage::STORAGE_CHUNK_STATE_DELETED);
            break;
        case storedemo::ChunkState::kQuarantined:
            request.set_source_state(storage::STORAGE_CHUNK_STATE_QUARANTINED);
            break;
        case storedemo::ChunkState::kCorrupted:
            request.set_source_state(storage::STORAGE_CHUNK_STATE_CORRUPTED);
            break;
        case storedemo::ChunkState::kMissing:
        default:
            request.set_source_state(storage::STORAGE_CHUNK_STATE_MISSING);
            break;
        }
        request.set_source_checksum_verified(source_checksum_verified);
        request.set_payload(payload);
        request.set_timeout_ms(1500);
        request.set_best_effort_cancel(true);
        request.set_durability(storage::WRITE_CHUNK_DURABILITY_PUBLISH);
        return request;
    }

    std::shared_ptr<storedemo::ShardedChunkIndex> MakeSharedIndex()
    {
        return std::make_shared<storedemo::ShardedChunkIndex>();
    }

    storedemo::ChunkIndexEntry FindIndexEntryOrThrow(storedemo::ChunkIndex &index,
                                                     const storedemo::ChunkId &chunk_id)
    {
        const auto find_response = index.Find(chunk_id);
        if (!find_response.ok())
        {
            throw std::runtime_error("failed to find chunk index entry: " +
                                     find_response.error_detail);
        }
        return find_response.entry;
    }

    void UpdateIndexStateOrThrow(storedemo::ChunkIndex &index,
                                 const storedemo::ChunkId &chunk_id,
                                 const storedemo::ChunkState state)
    {
        auto entry = FindIndexEntryOrThrow(index, chunk_id);
        entry.state = state;
        ++entry.updated_at;

        const auto update_response = index.Update(entry);
        if (!update_response.ok())
        {
            throw std::runtime_error("failed to update chunk index entry: " +
                                     update_response.error_detail);
        }
    }

    void TamperChunkPayloadOrThrow(storedemo::LocalDiskChunkStore &store,
                                   const storedemo::ChunkId &chunk_id,
                                   const std::string &payload)
    {
        auto entry = FindIndexEntryOrThrow(*store.chunk_index(), chunk_id);
        const auto path = store.paths().data_root / entry.final_path;

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            throw std::runtime_error("failed to open chunk for tamper: " + path.string());
        }

        output.write(payload.data(),
                     static_cast<std::streamsize>(payload.size()));
        output.close();
        if (!output)
        {
            throw std::runtime_error("failed to tamper chunk payload: " +
                                     path.string());
        }
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

    storage::DeleteChunkRequest MakeProtoDeleteRequest(
        const storedemo::ChunkIdentity &identity,
        const std::string &request_id,
        const storedemo::ChunkChecksum *checksum = nullptr)
    {
        storage::DeleteChunkRequest request;
        request.set_request_id(request_id);
        request.set_chunk_id(identity.chunk_id);
        request.set_object_id(identity.object_id);
        request.set_version(identity.version);
        request.set_chunk_index(identity.chunk_index);
        request.set_reason("service delete test");
        request.set_metadata_boundary("metadata-first-boundary");
        request.set_timeout_ms(1500);
        request.set_best_effort_cancel(true);
        if (checksum != nullptr)
        {
            FillProtoChecksum(*checksum, request.mutable_expected_checksum());
        }
        return request;
    }

    storage::BatchDeleteChunkRequest MakeProtoBatchDeleteItem(
        const storedemo::ChunkIdentity &identity,
        const storedemo::ChunkChecksum *checksum = nullptr)
    {
        storage::BatchDeleteChunkRequest request;
        request.set_chunk_id(identity.chunk_id);
        request.set_object_id(identity.object_id);
        request.set_version(identity.version);
        request.set_chunk_index(identity.chunk_index);
        request.set_reason("service delete test");
        request.set_metadata_boundary("metadata-first-boundary");
        if (checksum != nullptr)
        {
            FillProtoChecksum(*checksum, request.mutable_expected_checksum());
        }
        return request;
    }

    storage::WriteChunkRequest MakeProtoWriteRequest(const storedemo::ChunkIdentity &identity,
                                                  const std::string &payload,
                                                  const std::string &request_id)
    {
        storage::WriteChunkRequest request;
        request.set_request_id(request_id);
        request.set_chunk_id(identity.chunk_id);
        request.set_object_id(identity.object_id);
        request.set_version(identity.version);
        request.set_chunk_index(identity.chunk_index);
        request.set_offset(identity.offset);
        request.set_expected_size(static_cast<std::uint64_t>(payload.size()));
        FillProtoChecksum(ComputeStoreChecksumOrThrow(payload),
                          request.mutable_expected_checksum());
        request.set_payload(payload);
        request.set_timeout_ms(1500);
        request.set_best_effort_cancel(true);
        request.set_durability(storage::WRITE_CHUNK_DURABILITY_PUBLISH);
        return request;
    }

    storage::StorageNodeFacts MakeProtoStorageNodeFacts(
        const std::size_t index,
        const std::uint64_t total_capacity_bytes = 8'192,
        const std::uint64_t used_capacity_bytes = 2'048,
        const storage::StorageNodeHealth health =
            storage::STORAGE_NODE_HEALTH_HEALTHY,
        const storage::StorageNodeDiskPressure disk_pressure =
            storage::STORAGE_NODE_DISK_PRESSURE_LOW)
    {
        storage::StorageNodeFacts facts;
        facts.mutable_capacity()->set_total_capacity_bytes(total_capacity_bytes);
        facts.mutable_capacity()->set_used_capacity_bytes(used_capacity_bytes);
        facts.mutable_capacity()->set_available_capacity_bytes(
            total_capacity_bytes >= used_capacity_bytes
                ? total_capacity_bytes - used_capacity_bytes
                : 0);
        facts.mutable_capacity()->set_chunk_count(10 + index);
        facts.mutable_health()->set_health(health);
        facts.mutable_health()->set_disk_pressure(disk_pressure);
        facts.mutable_health()->set_io_error_count(index);
        facts.mutable_load()->set_active_reads(static_cast<std::uint32_t>(index));
        facts.mutable_load()->set_active_writes(static_cast<std::uint32_t>(index + 1));
        facts.mutable_load()->set_queued_ops(static_cast<std::uint32_t>(index + 2));
        facts.mutable_load()->set_write_admission_overloaded(false);
        facts.mutable_load()->set_read_admission_overloaded(false);
        facts.mutable_failure_domain()->set_zone("zone-" + std::to_string(index % 2));
        facts.mutable_failure_domain()->set_rack("rack-" + std::to_string(index));
        return facts;
    }

    storage::RegisterStorageNodeRequest MakeProtoRegisterNodeRequest(
        const std::size_t index,
        const std::uint64_t observed_at_unix_ms)
    {
        storage::RegisterStorageNodeRequest request;
        request.set_request_id("register-" + std::to_string(index));
        request.set_node_id(storedemo::test::MakeStorageNodeIdFixture(index));
        request.set_endpoint("127.0.0.1:" + std::to_string(7000 + index));
        request.set_observed_at_unix_ms(observed_at_unix_ms);
        *request.mutable_facts() = MakeProtoStorageNodeFacts(index);
        return request;
    }

    storage::UpdateStorageNodeHeartbeatRequest MakeProtoHeartbeatRequest(
        const std::size_t index,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        storage::UpdateStorageNodeHeartbeatRequest request;
        request.set_request_id("heartbeat-" + std::to_string(index) + "-" +
                               std::to_string(sequence));
        request.mutable_heartbeat()->set_node_id(
            storedemo::test::MakeStorageNodeIdFixture(index));
        request.mutable_heartbeat()->set_endpoint("127.0.0.1:" +
                                                  std::to_string(7000 + index));
        request.mutable_heartbeat()->set_sequence(sequence);
        request.mutable_heartbeat()->set_observed_at_unix_ms(observed_at_unix_ms);
        *request.mutable_heartbeat()->mutable_facts() = MakeProtoStorageNodeFacts(index);
        return request;
    }

    storage::ReportHealthRequest MakeProtoHealthReportRequest(
        const std::size_t index,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        storage::ReportHealthRequest request;
        request.set_request_id("report-health-" + std::to_string(index) + "-" +
                               std::to_string(sequence));
        request.set_node_id(storedemo::test::MakeStorageNodeIdFixture(index));
        request.set_endpoint("127.0.0.1:" + std::to_string(7000 + index));
        request.set_sequence(sequence);
        request.set_observed_at_unix_ms(observed_at_unix_ms);
        *request.mutable_health() = MakeProtoStorageNodeFacts(index).health();
        return request;
    }

    storage::ReportCapacityRequest MakeProtoCapacityReportRequest(
        const std::size_t index,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        storage::ReportCapacityRequest request;
        request.set_request_id("report-capacity-" + std::to_string(index) + "-" +
                               std::to_string(sequence));
        request.set_node_id(storedemo::test::MakeStorageNodeIdFixture(index));
        request.set_endpoint("127.0.0.1:" + std::to_string(7000 + index));
        request.set_sequence(sequence);
        request.set_observed_at_unix_ms(observed_at_unix_ms);
        *request.mutable_capacity() = MakeProtoStorageNodeFacts(index).capacity();
        return request;
    }

    storage::ReportLoadRequest MakeProtoLoadReportRequest(
        const std::size_t index,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        storage::ReportLoadRequest request;
        request.set_request_id("report-load-" + std::to_string(index) + "-" +
                               std::to_string(sequence));
        request.set_node_id(storedemo::test::MakeStorageNodeIdFixture(index));
        request.set_endpoint("127.0.0.1:" + std::to_string(7000 + index));
        request.set_sequence(sequence);
        request.set_observed_at_unix_ms(observed_at_unix_ms);
        *request.mutable_load() = MakeProtoStorageNodeFacts(index).load();
        return request;
    }

    class RecordingChunkStore final : public storedemo::ChunkStore
    {
    public:
        storedemo::WriteChunkResponse WriteChunk(
            const storedemo::WriteChunkRequest &request) override
        {
            ++write_calls;
            last_write_request = request;
            if (write_handler)
            {
                return write_handler(request);
            }
            return default_write_response;
        }

        storedemo::ReadChunkResponse ReadChunk(
            const storedemo::ReadChunkRequest &request) override
        {
            ++read_calls;
            last_read_request = request;
            if (read_handler)
            {
                return read_handler(request);
            }
            return default_read_response;
        }

        storedemo::DeleteChunkResponse DeleteChunk(
            const storedemo::DeleteChunkRequest &request) override
        {
            ++delete_calls;
            last_delete_request = request;
            if (delete_handler)
            {
                return delete_handler(request);
            }
            return default_delete_response;
        }

        storedemo::StatChunkResponse StatChunk(
            const storedemo::StatChunkRequest &request) override
        {
            ++stat_calls;
            last_stat_request = request;
            if (stat_handler)
            {
                return stat_handler(request);
            }
            return default_stat_response;
        }

        storedemo::ListChunksResponse ListChunks(
            const storedemo::ListChunksRequest &) override
        {
            storedemo::ListChunksResponse response;
            response.status = storedemo::StorageNodeStatusCode::kUnsupported;
            response.error_detail = "not used in RecordingChunkStore";
            return response;
        }

        std::function<storedemo::WriteChunkResponse(const storedemo::WriteChunkRequest &)>
            write_handler;
        std::function<storedemo::ReadChunkResponse(const storedemo::ReadChunkRequest &)>
            read_handler;
        std::function<storedemo::DeleteChunkResponse(const storedemo::DeleteChunkRequest &)>
            delete_handler;
        std::function<storedemo::StatChunkResponse(const storedemo::StatChunkRequest &)>
            stat_handler;
        storedemo::WriteChunkRequest last_write_request;
        storedemo::ReadChunkRequest last_read_request;
        storedemo::DeleteChunkRequest last_delete_request;
        storedemo::StatChunkRequest last_stat_request;
        storedemo::WriteChunkResponse default_write_response;
        storedemo::ReadChunkResponse default_read_response;
        storedemo::DeleteChunkResponse default_delete_response;
        storedemo::StatChunkResponse default_stat_response;
        std::size_t write_calls{0};
        std::size_t read_calls{0};
        std::size_t delete_calls{0};
        std::size_t stat_calls{0};
    };

    class ForcedDeleteChunkStore final : public storedemo::ChunkStore
    {
    public:
        explicit ForcedDeleteChunkStore(std::shared_ptr<storedemo::ChunkStore> inner_store)
            : inner_store_(std::move(inner_store))
        {
            if (inner_store_ == nullptr)
            {
                throw std::invalid_argument("inner_store must not be null");
            }
        }

        void ForceDeleteResponse(const storedemo::ChunkId &chunk_id,
                                 storedemo::DeleteChunkResponse response)
        {
            forced_delete_responses_[chunk_id] = std::move(response);
        }

        storedemo::WriteChunkResponse WriteChunk(
            const storedemo::WriteChunkRequest &request) override
        {
            return inner_store_->WriteChunk(request);
        }

        storedemo::ReadChunkResponse ReadChunk(
            const storedemo::ReadChunkRequest &request) override
        {
            return inner_store_->ReadChunk(request);
        }

        storedemo::DeleteChunkResponse DeleteChunk(
            const storedemo::DeleteChunkRequest &request) override
        {
            ++delete_calls;
            last_delete_request = request;

            const auto forced = forced_delete_responses_.find(request.chunk_id);
            if (forced == forced_delete_responses_.end())
            {
                return inner_store_->DeleteChunk(request);
            }

            auto response = forced->second;
            if (response.metadata.identity.chunk_id.empty())
            {
                response.metadata.identity.chunk_id = request.chunk_id;
            }
            return response;
        }

        storedemo::StatChunkResponse StatChunk(
            const storedemo::StatChunkRequest &request) override
        {
            return inner_store_->StatChunk(request);
        }

        storedemo::ListChunksResponse ListChunks(
            const storedemo::ListChunksRequest &request) override
        {
            return inner_store_->ListChunks(request);
        }

        std::unordered_map<storedemo::ChunkId, storedemo::DeleteChunkResponse>
            forced_delete_responses_;
        storedemo::DeleteChunkRequest last_delete_request;
        std::size_t delete_calls{0};

    private:
        std::shared_ptr<storedemo::ChunkStore> inner_store_;
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

            stub_ = storage::StorageNodeService::NewStub(channel_);
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

        storage::WriteChunkResponse WriteChunk(const storage::WriteChunkRequest &request,
                                            grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::WriteChunkResponse response;
            grpc::Status status = stub_->WriteChunk(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

        storage::ReadChunkResponse ReadChunk(const storage::ReadChunkRequest &request,
                                          grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::ReadChunkResponse response;
            grpc::Status status = stub_->ReadChunk(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

        storage::DeleteChunkResponse DeleteChunk(const storage::DeleteChunkRequest &request,
                                                 grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::DeleteChunkResponse response;
            grpc::Status status = stub_->DeleteChunk(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

        storage::ScrubChunkResponse ScrubChunk(const storage::ScrubChunkRequest &request,
                                               grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::ScrubChunkResponse response;
            grpc::Status status = stub_->ScrubChunk(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

        storage::RepairChunkResponse RepairChunk(const storage::RepairChunkRequest &request,
                                                 grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::RepairChunkResponse response;
            grpc::Status status = stub_->RepairChunk(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

        storage::BatchDeleteChunksResponse BatchDeleteChunks(
            const storage::BatchDeleteChunksRequest &request,
            grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::BatchDeleteChunksResponse response;
            grpc::Status status =
                stub_->BatchDeleteChunks(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

        storage::RegisterStorageNodeResponse RegisterStorageNode(
            const storage::RegisterStorageNodeRequest &request,
            grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::RegisterStorageNodeResponse response;
            grpc::Status status = stub_->RegisterStorageNode(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

        storage::StorageNodeFactUpdateResponse UpdateStorageNodeHeartbeat(
            const storage::UpdateStorageNodeHeartbeatRequest &request,
            grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::StorageNodeFactUpdateResponse response;
            grpc::Status status =
                stub_->UpdateStorageNodeHeartbeat(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

        storage::StorageNodeFactUpdateResponse ReportHealth(
            const storage::ReportHealthRequest &request,
            grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::StorageNodeFactUpdateResponse response;
            grpc::Status status = stub_->ReportHealth(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

        storage::StorageNodeFactUpdateResponse ReportCapacity(
            const storage::ReportCapacityRequest &request,
            grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::StorageNodeFactUpdateResponse response;
            grpc::Status status = stub_->ReportCapacity(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

        storage::StorageNodeFactUpdateResponse ReportLoad(
            const storage::ReportLoadRequest &request,
            grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            storage::StorageNodeFactUpdateResponse response;
            grpc::Status status = stub_->ReportLoad(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

    private:
        std::shared_ptr<storedemo::StorageNodeService> service_;
        std::unique_ptr<grpc::Server> server_;
        std::shared_ptr<grpc::Channel> channel_;
        std::unique_ptr<storage::StorageNodeService::Stub> stub_;
        int selected_port_{0};
    };

    class StorageNodeServiceTest : public ::testing::Test
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

    TEST_F(StorageNodeServiceTest, ConstructingWithoutChunkStoreThrows)
    {
        EXPECT_THROW(storedemo::StorageNodeService(nullptr), std::invalid_argument);
    }

    TEST_F(StorageNodeServiceTest, RegisterStorageNodeMapsFieldsAndStoresRegistryFacts)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        auto registry = std::make_shared<storedemo::StorageNodeRegistry>();
        auto service = std::make_shared<storedemo::StorageNodeService>(
            recording_store,
            "service-node-t063",
            registry);
        RunningStorageNodeService server(service);

        auto request = MakeProtoRegisterNodeRequest(63, 100);
        request.set_request_id("register-service-t063");
        grpc::Status grpc_status;
        const auto response = server.RegisterStorageNode(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_EQ(response.summary().request_id(), "register-service-t063");
        EXPECT_EQ(response.summary().node_id(), request.node_id());
        EXPECT_TRUE(response.created());
        EXPECT_FALSE(response.idempotent());
        EXPECT_EQ(response.snapshot().node_id(), request.node_id());
        EXPECT_EQ(response.snapshot().endpoint(), request.endpoint());
        EXPECT_EQ(response.snapshot().last_sequence(), 0U);
        EXPECT_EQ(response.snapshot().last_seen_unix_ms(), 100U);
        EXPECT_EQ(response.snapshot().liveness(),
                  storage::STORAGE_NODE_LIVENESS_STATE_LIVE);
        EXPECT_EQ(response.snapshot().facts().capacity().total_capacity_bytes(), 8192U);
        EXPECT_EQ(response.snapshot().facts().capacity().chunk_count(), 73U);
        EXPECT_EQ(response.snapshot().facts().health().health(),
                  storage::STORAGE_NODE_HEALTH_HEALTHY);
        EXPECT_EQ(response.snapshot().facts().health().disk_pressure(),
                  storage::STORAGE_NODE_DISK_PRESSURE_LOW);
        EXPECT_EQ(response.snapshot().facts().load().active_writes(), 64U);

        const auto lookup = registry->LookupNode(request.node_id(), 100);
        ASSERT_EQ(lookup.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup.snapshot.endpoint, request.endpoint());
        EXPECT_EQ(lookup.snapshot.facts.capacity.total_capacity_bytes, 8192U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.chunk_count, 73U);
        EXPECT_EQ(recording_store->write_calls, 0U);
        EXPECT_EQ(recording_store->read_calls, 0U);
        EXPECT_EQ(recording_store->delete_calls, 0U);
    }

    TEST_F(StorageNodeServiceTest,
           RegisterStorageNodeKeepsIdempotentAndConflictSemantics)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        auto registry = std::make_shared<storedemo::StorageNodeRegistry>();
        auto service = std::make_shared<storedemo::StorageNodeService>(
            recording_store,
            "service-node-t063",
            registry);
        RunningStorageNodeService server(service);

        const auto original = MakeProtoRegisterNodeRequest(64, 100);
        grpc::Status grpc_status;
        ASSERT_EQ(server.RegisterStorageNode(original, &grpc_status).summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_OK);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();

        auto duplicate = original;
        duplicate.set_request_id("register-duplicate-t063");
        duplicate.mutable_facts()->mutable_capacity()->set_total_capacity_bytes(16'384);
        duplicate.mutable_facts()->mutable_capacity()->set_available_capacity_bytes(14'336);
        const auto duplicate_response = server.RegisterStorageNode(duplicate, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(duplicate_response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_FALSE(duplicate_response.created());
        EXPECT_TRUE(duplicate_response.idempotent());
        EXPECT_EQ(duplicate_response.snapshot().facts().capacity().total_capacity_bytes(),
                  original.facts().capacity().total_capacity_bytes());

        auto conflict = original;
        conflict.set_request_id("register-conflict-t063");
        conflict.set_endpoint("127.0.0.1:7999");
        const auto conflict_response = server.RegisterStorageNode(conflict, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(conflict_response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_CONFLICT);
        EXPECT_FALSE(conflict_response.created());
        EXPECT_FALSE(conflict_response.idempotent());
    }

    TEST_F(StorageNodeServiceTest,
           UpdateStorageNodeHeartbeatMatchesRegistryApplyStaleAndIdempotentSemantics)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        auto registry = std::make_shared<storedemo::StorageNodeRegistry>();
        auto service = std::make_shared<storedemo::StorageNodeService>(
            recording_store,
            "service-node-t063",
            registry);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        ASSERT_EQ(server.RegisterStorageNode(MakeProtoRegisterNodeRequest(65, 100),
                                             &grpc_status)
                      .summary()
                      .code(),
                  storage::STORAGE_NODE_STATUS_CODE_OK);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();

        auto heartbeat = MakeProtoHeartbeatRequest(65, 7, 160);
        heartbeat.set_request_id("heartbeat-apply-t063");
        auto *facts = heartbeat.mutable_heartbeat()->mutable_facts();
        facts->mutable_capacity()->set_total_capacity_bytes(32'768);
        facts->mutable_capacity()->set_used_capacity_bytes(12'288);
        facts->mutable_capacity()->set_available_capacity_bytes(20'480);
        facts->mutable_capacity()->set_chunk_count(77);
        facts->mutable_health()->set_health(storage::STORAGE_NODE_HEALTH_DEGRADED);
        facts->mutable_health()->set_disk_pressure(
            storage::STORAGE_NODE_DISK_PRESSURE_MEDIUM);
        facts->mutable_health()->set_io_error_count(9);
        facts->mutable_load()->set_active_reads(8);
        facts->mutable_load()->set_active_writes(3);
        facts->mutable_load()->set_queued_ops(11);
        facts->mutable_load()->set_write_admission_overloaded(true);

        const auto applied = server.UpdateStorageNodeHeartbeat(heartbeat, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(applied.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_EQ(applied.accepted_sequence(), 7U);
        EXPECT_TRUE(applied.applied());
        EXPECT_FALSE(applied.idempotent());
        EXPECT_FALSE(applied.stale_ignored());
        EXPECT_EQ(applied.snapshot().facts().capacity().total_capacity_bytes(), 32'768U);

        auto stale = MakeProtoHeartbeatRequest(65, 6, 150);
        stale.set_request_id("heartbeat-stale-t063");
        *stale.mutable_heartbeat()->mutable_facts() = MakeProtoStorageNodeFacts(
            65,
            4'096,
            1'024);
        const auto stale_response = server.UpdateStorageNodeHeartbeat(stale, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(stale_response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS);
        EXPECT_EQ(stale_response.accepted_sequence(), 7U);
        EXPECT_FALSE(stale_response.applied());
        EXPECT_FALSE(stale_response.idempotent());
        EXPECT_TRUE(stale_response.stale_ignored());

        auto duplicate = heartbeat;
        duplicate.set_request_id("heartbeat-idempotent-t063");
        duplicate.mutable_heartbeat()->set_observed_at_unix_ms(180);
        *duplicate.mutable_heartbeat()->mutable_facts() = MakeProtoStorageNodeFacts(
            65,
            65'536,
            4'096);
        const auto duplicate_response =
            server.UpdateStorageNodeHeartbeat(duplicate, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(duplicate_response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_EQ(duplicate_response.accepted_sequence(), 7U);
        EXPECT_FALSE(duplicate_response.applied());
        EXPECT_TRUE(duplicate_response.idempotent());
        EXPECT_FALSE(duplicate_response.stale_ignored());

        const auto lookup = registry->LookupNode(
            storedemo::test::MakeStorageNodeIdFixture(65), 181);
        ASSERT_EQ(lookup.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup.snapshot.last_sequence, 7U);
        EXPECT_EQ(lookup.snapshot.last_seen_unix_ms, 160U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.total_capacity_bytes, 32'768U);
        EXPECT_EQ(lookup.snapshot.facts.health.health,
                  storedemo::StorageNodeHealth::kDegraded);
        EXPECT_TRUE(lookup.snapshot.facts.load.write_admission_overloaded);
        EXPECT_EQ(recording_store->write_calls, 0U);
        EXPECT_EQ(recording_store->read_calls, 0U);
        EXPECT_EQ(recording_store->delete_calls, 0U);
    }

    TEST_F(StorageNodeServiceTest, ReportHealthCapacityAndLoadMergeIntoRegistryFacts)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        auto registry = std::make_shared<storedemo::StorageNodeRegistry>();
        auto service = std::make_shared<storedemo::StorageNodeService>(
            recording_store,
            "service-node-t063",
            registry);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        ASSERT_EQ(server.RegisterStorageNode(MakeProtoRegisterNodeRequest(66, 100),
                                             &grpc_status)
                      .summary()
                      .code(),
                  storage::STORAGE_NODE_STATUS_CODE_OK);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();

        auto health_request = MakeProtoHealthReportRequest(66, 2, 120);
        health_request.mutable_health()->set_health(storage::STORAGE_NODE_HEALTH_DEGRADED);
        health_request.mutable_health()->set_disk_pressure(
            storage::STORAGE_NODE_DISK_PRESSURE_HIGH);
        health_request.mutable_health()->set_io_error_count(6);
        const auto health_response = server.ReportHealth(health_request, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(health_response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(health_response.applied());

        auto capacity_request = MakeProtoCapacityReportRequest(66, 3, 130);
        capacity_request.mutable_capacity()->set_total_capacity_bytes(16'384);
        capacity_request.mutable_capacity()->set_used_capacity_bytes(4'096);
        capacity_request.mutable_capacity()->set_available_capacity_bytes(12'288);
        capacity_request.mutable_capacity()->set_chunk_count(99);
        const auto capacity_response =
            server.ReportCapacity(capacity_request, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(capacity_response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(capacity_response.applied());

        auto load_request = MakeProtoLoadReportRequest(66, 4, 140);
        load_request.mutable_load()->set_active_reads(8);
        load_request.mutable_load()->set_active_writes(3);
        load_request.mutable_load()->set_queued_ops(11);
        load_request.mutable_load()->set_write_admission_overloaded(true);
        load_request.mutable_load()->set_read_admission_overloaded(true);
        const auto load_response = server.ReportLoad(load_request, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(load_response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(load_response.applied());
        EXPECT_EQ(load_response.accepted_sequence(), 4U);
        EXPECT_EQ(load_response.snapshot().last_sequence(), 4U);

        const auto lookup = registry->LookupNode(
            storedemo::test::MakeStorageNodeIdFixture(66), 150);
        ASSERT_EQ(lookup.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup.snapshot.last_sequence, 4U);
        EXPECT_EQ(lookup.snapshot.last_seen_unix_ms, 140U);
        EXPECT_EQ(lookup.snapshot.facts.health.disk_pressure,
                  storedemo::StorageNodeDiskPressure::kHigh);
        EXPECT_EQ(lookup.snapshot.facts.capacity.total_capacity_bytes, 16'384U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.chunk_count, 99U);
        EXPECT_EQ(lookup.snapshot.facts.load.load.active_reads, 8U);
        EXPECT_TRUE(lookup.snapshot.facts.load.write_admission_overloaded);
        EXPECT_TRUE(lookup.snapshot.facts.load.read_admission_overloaded);
        EXPECT_EQ(lookup.snapshot.facts.failure_domain.zone, "zone-0");
        EXPECT_EQ(recording_store->write_calls, 0U);
        EXPECT_EQ(recording_store->read_calls, 0U);
        EXPECT_EQ(recording_store->delete_calls, 0U);
    }

    TEST_F(StorageNodeServiceTest, RegistryRpcInvalidRequestsReturnExplicitErrors)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        auto registry = std::make_shared<storedemo::StorageNodeRegistry>();
        auto service = std::make_shared<storedemo::StorageNodeService>(
            recording_store,
            "service-node-t063",
            registry);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;

        auto invalid_register = MakeProtoRegisterNodeRequest(67, 100);
        invalid_register.set_node_id("");
        const auto register_response = server.RegisterStorageNode(invalid_register,
                                                                  &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(register_response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT);
        EXPECT_FALSE(register_response.created());
        EXPECT_FALSE(register_response.idempotent());

        ASSERT_EQ(server.RegisterStorageNode(MakeProtoRegisterNodeRequest(67, 100),
                                             &grpc_status)
                      .summary()
                      .code(),
                  storage::STORAGE_NODE_STATUS_CODE_OK);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();

        auto invalid_heartbeat = MakeProtoHeartbeatRequest(67, 0, 120);
        const auto heartbeat_response =
            server.UpdateStorageNodeHeartbeat(invalid_heartbeat, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(heartbeat_response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT);
        EXPECT_FALSE(heartbeat_response.applied());

        auto invalid_capacity = MakeProtoCapacityReportRequest(67, 2, 121);
        invalid_capacity.mutable_capacity()->set_total_capacity_bytes(4'096);
        invalid_capacity.mutable_capacity()->set_used_capacity_bytes(3'000);
        invalid_capacity.mutable_capacity()->set_available_capacity_bytes(2'000);
        const auto capacity_response = server.ReportCapacity(invalid_capacity, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(capacity_response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT);

        auto conflict_load = MakeProtoLoadReportRequest(67, 2, 122);
        conflict_load.set_endpoint("127.0.0.1:7999");
        const auto load_response = server.ReportLoad(conflict_load, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(load_response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_CONFLICT);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkMapsFieldsAndResponseFacts)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        const auto identity = MakeStoreIdentityOrThrow("obj-t031-recording", 7, 3, 4096);
        const auto payload = storedemo::test::MakeChunkPayload(192, "t031-recording");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        recording_store->write_handler =
            [identity, checksum](const storedemo::WriteChunkRequest &request)
        {
            storedemo::WriteChunkResponse response;
            response.status = storedemo::StorageNodeStatusCode::kAlreadyExists;
            response.error_detail = "already durable";
            response.retry_after_ms = 27;
            response.metadata.identity = identity;
            response.metadata.node_id = "service-node-t031";
            response.metadata.size = checksum.size_bytes;
            response.metadata.checksum = checksum;
            response.metadata.state = storedemo::ChunkState::kLive;
            response.durable = true;
            response.already_exists = true;
            return response;
        };

        auto service = std::make_shared<storedemo::StorageNodeService>(recording_store,
                                                                       "service-node-t031");
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.WriteChunk(
            MakeProtoWriteRequest(identity, payload, "write-recording-t031"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        ASSERT_EQ(recording_store->write_calls, 1U);
        EXPECT_EQ(recording_store->last_write_request.request_id, "write-recording-t031");
        EXPECT_EQ(recording_store->last_write_request.identity.chunk_id, identity.chunk_id);
        EXPECT_EQ(recording_store->last_write_request.identity.object_id, identity.object_id);
        EXPECT_EQ(recording_store->last_write_request.identity.version, identity.version);
        EXPECT_EQ(recording_store->last_write_request.identity.chunk_index,
                  identity.chunk_index);
        EXPECT_EQ(recording_store->last_write_request.identity.offset, identity.offset);
        ASSERT_TRUE(recording_store->last_write_request.expected_size.has_value());
        EXPECT_EQ(*recording_store->last_write_request.expected_size, payload.size());
        EXPECT_EQ(recording_store->last_write_request.expected_checksum.algorithm,
                  checksum.algorithm);
        EXPECT_EQ(recording_store->last_write_request.expected_checksum.value,
                  checksum.value);
        EXPECT_EQ(recording_store->last_write_request.payload, payload);

        EXPECT_EQ(response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS);
        EXPECT_EQ(response.summary().message(), "already durable");
        EXPECT_EQ(response.summary().request_id(), "write-recording-t031");
        EXPECT_EQ(response.summary().node_id(), "service-node-t031");
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.summary().retry_after_ms(), 27U);
        EXPECT_TRUE(response.durable());
        EXPECT_TRUE(response.already_exists());
        EXPECT_EQ(response.size(), checksum.size_bytes);
        EXPECT_EQ(response.state(), storage::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_EQ(response.checksum().algorithm(),
                  storage::STORAGE_CHECKSUM_ALGORITHM_SHA256);
        EXPECT_EQ(response.checksum().value(), checksum.value);
        EXPECT_EQ(response.checksum().size_bytes(), checksum.size_bytes);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkDurableSuccessKeepsMetadataUncommitted)
    {
        const auto fixture = LoadFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t031-durable",
                            "create-bucket-t031-durable"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t031-durable", 1, 0, 0);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t031-durable",
                                                        "uploads/t031-durable",
                                                        identity.object_id,
                                                        "create-object-t031-durable",
                                                        fixture.payload.size(),
                                                        "etag-t031-durable"))
                        .Ok);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_durable");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 31));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.WriteChunk(
            MakeProtoWriteRequest(identity, fixture.payload, "write-durable-t031"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(response.durable());
        EXPECT_FALSE(response.already_exists());
        EXPECT_EQ(response.summary().node_id(), store->config().node_id);
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.state(), storage::STORAGE_CHUNK_STATE_LIVE);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t031-durable", .object_key = "uploads/t031-durable"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list = machine.ListObjects(
            {.bucket = "bucket-t031-durable", .prefix = "uploads/"});
        EXPECT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        const auto read_response =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-durable-t031"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, fixture.payload);
        EXPECT_EQ(CountRegularFilesRecursively(store->paths().live_root), 1U);
        EXPECT_EQ(CountRegularFilesRecursively(store->paths().staging_root), 0U);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkChecksumMismatchDoesNotWriteLiveChunk)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_checksum");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 32));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        const auto identity = MakeStoreIdentityOrThrow("obj-t031-checksum", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(128, "t031-checksum");
        auto request = MakeProtoWriteRequest(identity, payload, "write-checksum-t031");
        FillProtoChecksum(ComputeStoreChecksumOrThrow("different-checksum-payload"),
                          request.mutable_expected_checksum());

        grpc::Status grpc_status;
        const auto response = server.WriteChunk(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH);
        EXPECT_FALSE(response.durable());
        EXPECT_FALSE(response.already_exists());

        const auto read_response =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-checksum-t031"));
        EXPECT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(CountRegularFilesRecursively(store->paths().live_root), 0U);
        EXPECT_EQ(CountRegularFilesRecursively(store->paths().staging_root), 0U);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkSamePayloadRetryReturnsAlreadyExists)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_idempotent");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 33));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        const auto identity = MakeStoreIdentityOrThrow("obj-t031-idempotent", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(144, "t031-idempotent");
        const auto request = MakeProtoWriteRequest(identity, payload, "retry-safe-t031");

        grpc::Status grpc_status;
        const auto first = server.WriteChunk(request, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        ASSERT_EQ(first.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);

        const auto retry = server.WriteChunk(request, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_TRUE(retry.durable());
        EXPECT_TRUE(retry.already_exists());
        EXPECT_TRUE(retry.summary().code() == storage::STORAGE_NODE_STATUS_CODE_OK ||
                    retry.summary().code() ==
                        storage::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS);

        const auto read_response =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-idempotent-t031"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, payload);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkDifferentPayloadReturnsConflict)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_conflict");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 34));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        const auto identity = MakeStoreIdentityOrThrow("obj-t031-conflict", 1, 0, 0);
        const auto original_payload =
            storedemo::test::MakeChunkPayload(176, "t031-conflict-original");
        const auto conflicting_payload =
            storedemo::test::MakeChunkPayload(176, "t031-conflict-different");

        grpc::Status grpc_status;
        ASSERT_EQ(server.WriteChunk(
                      MakeProtoWriteRequest(identity,
                                            original_payload,
                                            "write-conflict-original-t031"),
                      &grpc_status)
                      .summary()
                      .code(),
                  storage::STORAGE_NODE_STATUS_CODE_OK);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();

        const auto conflict = server.WriteChunk(
            MakeProtoWriteRequest(identity,
                                  conflicting_payload,
                                  "write-conflict-different-t031"),
            &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(conflict.summary().code(), storage::STORAGE_NODE_STATUS_CODE_CONFLICT);
        EXPECT_FALSE(conflict.durable());

        const auto read_response =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-conflict-t031"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, original_payload);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkInvalidRequestReturnsInvalidArgument)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_invalid");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 35));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        storage::WriteChunkRequest request;
        request.set_object_id("obj-t031-invalid");
        request.set_version(1);
        request.set_chunk_index(0);
        request.set_payload("payload-without-request-id");
        request.set_expected_size(request.payload().size());

        grpc::Status grpc_status;
        const auto response = server.WriteChunk(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT);
        EXPECT_FALSE(response.durable());
    }

    TEST_F(StorageNodeServiceTest, WriteChunkOverloadedMapsStatusAndRetryAfter)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        recording_store->default_write_response.status =
            storedemo::StorageNodeStatusCode::kOverloaded;
        recording_store->default_write_response.error_detail = "executor queue is full";
        recording_store->default_write_response.retry_after_ms = 88;

        auto service = std::make_shared<storedemo::StorageNodeService>(recording_store,
                                                                       "service-node-t031");
        RunningStorageNodeService server(service);

        const auto identity = MakeStoreIdentityOrThrow("obj-t031-overloaded", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(96, "t031-overloaded");

        grpc::Status grpc_status;
        const auto response = server.WriteChunk(
            MakeProtoWriteRequest(identity, payload, "write-overloaded-t031"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OVERLOADED);
        EXPECT_EQ(response.summary().message(), "executor queue is full");
        EXPECT_EQ(response.summary().retry_after_ms(), 88U);
        EXPECT_FALSE(response.durable());
        EXPECT_FALSE(response.already_exists());
    }

    TEST_F(StorageNodeServiceTest, ReadChunkMapsDerivedFieldsAndResponseFacts)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        const auto identity = MakeStoreIdentityOrThrow("obj-t043-recording", 9, 2, 8192);
        const auto payload = storedemo::test::MakeChunkPayload(160, "t043-recording");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        recording_store->read_handler =
            [identity, payload, checksum](const storedemo::ReadChunkRequest &request)
                -> storedemo::ReadChunkResponse
        {
            EXPECT_EQ(request.request_id, "read-recording-t043");
            EXPECT_EQ(request.chunk_id, identity.chunk_id);
            EXPECT_TRUE(request.range.has_value());
            if (!request.range.has_value())
            {
                storedemo::ReadChunkResponse error_response;
                error_response.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
                error_response.error_detail = "expected range for recording read test";
                return error_response;
            }
            EXPECT_EQ(request.range->offset, 17U);
            EXPECT_EQ(request.range->length, 33U);
            EXPECT_EQ(request.expected_checksum.algorithm, checksum.algorithm);
            EXPECT_EQ(request.expected_checksum.value, checksum.value);
            EXPECT_TRUE(request.verify_checksum);

            storedemo::ReadChunkResponse response;
            response.status = storedemo::StorageNodeStatusCode::kOk;
            response.metadata.identity = identity;
            response.metadata.node_id = "service-node-t043";
            response.metadata.size = checksum.size_bytes;
            response.metadata.checksum = checksum;
            response.metadata.state = storedemo::ChunkState::kLive;
            response.actual_checksum = checksum;
            response.payload = payload;
            response.verified = true;
            return response;
        };

        auto service = std::make_shared<storedemo::StorageNodeService>(recording_store,
                                                                       "service-node-t043");
        RunningStorageNodeService server(service);

        storage::ReadChunkRequest request;
        request.set_request_id("read-recording-t043");
        request.set_object_id(identity.object_id);
        request.set_version(identity.version);
        request.set_chunk_index(identity.chunk_index);
        request.set_offset(17);
        request.set_length(33);
        request.set_timeout_ms(2500);
        request.set_best_effort_cancel(true);
        request.set_verify_checksum(true);
        FillProtoChecksum(checksum, request.mutable_expected_checksum());

        grpc::Status grpc_status;
        const auto response = server.ReadChunk(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        ASSERT_EQ(recording_store->read_calls, 1U);
        EXPECT_EQ(recording_store->last_read_request.request_id, "read-recording-t043");
        EXPECT_EQ(recording_store->last_read_request.chunk_id, identity.chunk_id);
        ASSERT_TRUE(recording_store->last_read_request.range.has_value());
        EXPECT_EQ(recording_store->last_read_request.range->offset, 17U);
        EXPECT_EQ(recording_store->last_read_request.range->length, 33U);
        EXPECT_EQ(recording_store->last_read_request.expected_checksum.value,
                  checksum.value);
        EXPECT_TRUE(recording_store->last_read_request.verify_checksum);

        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_EQ(response.summary().request_id(), "read-recording-t043");
        EXPECT_EQ(response.summary().node_id(), "service-node-t043");
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.payload(), payload);
        EXPECT_EQ(response.size(), checksum.size_bytes);
        EXPECT_EQ(response.checksum().algorithm(),
                  storage::STORAGE_CHECKSUM_ALGORITHM_SHA256);
        EXPECT_EQ(response.checksum().value(), checksum.value);
        EXPECT_EQ(response.checksum().size_bytes(), checksum.size_bytes);
        EXPECT_EQ(response.state(), storage::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_EQ(response.offset(), identity.offset);
        EXPECT_TRUE(response.complete());
        EXPECT_FALSE(response.full_read());
    }

    TEST_F(StorageNodeServiceTest, ReadChunkFullReadReturnsPayloadAndKeepsMetadataUncommitted)
    {
        const auto fixture = LoadFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t043-read",
                            "create-bucket-t043-read"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t043-read", 1, 0, 4096);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t043-read",
                                                        "objects/test_file.deb",
                                                        identity.object_id,
                                                        "create-object-t043-read",
                                                        fixture.payload.size(),
                                                        "etag-t043-read"))
                        .Ok);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_read_full");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 43));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-read-full-t043",
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

        auto request = MakeProtoReadRequest(identity, "read-full-t043");
        FillProtoChecksum(ComputeStoreChecksumOrThrow(fixture.payload),
                          request.mutable_expected_checksum());
        request.set_verify_checksum(true);

        grpc::Status grpc_status;
        const auto response = server.ReadChunk(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_EQ(response.summary().node_id(), store->config().node_id);
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.payload(), fixture.payload);
        EXPECT_EQ(response.size(), fixture.payload.size());
        EXPECT_EQ(response.checksum().algorithm(),
                  storage::STORAGE_CHECKSUM_ALGORITHM_SHA256);
        EXPECT_EQ(response.checksum().value(),
                  ComputeStoreChecksumOrThrow(fixture.payload).value);
        EXPECT_EQ(response.state(), storage::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_EQ(response.offset(), identity.offset);
        EXPECT_TRUE(response.complete());
        EXPECT_TRUE(response.full_read());

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t043-read", .object_key = "objects/test_file.deb"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());
    }

    TEST_F(StorageNodeServiceTest, ReadChunkChecksumMismatchReturnsExplicitError)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_read_checksum");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 44));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t043-checksum", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(128, "t043-checksum");
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-read-checksum-t043",
                          .identity = identity,
                          .expected_size = static_cast<std::uint64_t>(payload.size()),
                          .expected_checksum = ComputeStoreChecksumOrThrow(payload),
                          .payload = payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        auto request = MakeProtoReadRequest(identity, "read-checksum-t043");
        FillProtoChecksum(ComputeStoreChecksumOrThrow("different-payload"),
                          request.mutable_expected_checksum());
        request.set_verify_checksum(true);

        grpc::Status grpc_status;
        const auto response = server.ReadChunk(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH);
        EXPECT_FALSE(response.complete());
        EXPECT_FALSE(response.full_read());
        EXPECT_TRUE(response.payload().empty());
        EXPECT_EQ(response.state(), storage::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_EQ(response.checksum().value(),
                  ComputeStoreChecksumOrThrow(payload).value);
    }

    TEST_F(StorageNodeServiceTest, ReadChunkMissingChunkReturnsNotFound)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_read_missing");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 45));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        const auto identity = MakeStoreIdentityOrThrow("obj-t043-missing", 1, 0, 0);
        grpc::Status grpc_status;
        const auto response = server.ReadChunk(
            MakeProtoReadRequest(identity, "read-missing-t043"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_NOT_FOUND);
        EXPECT_TRUE(response.payload().empty());
        EXPECT_FALSE(response.complete());
        EXPECT_FALSE(response.full_read());
    }

    TEST_F(StorageNodeServiceTest, ReadChunkRejectsNonLiveState)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_read_non_live");
        const auto shared_index = MakeSharedIndex();
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            storedemo::LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.root(),
                .node_id = storedemo::test::MakeStorageNodeIdFixture(46),
                .chunk_index = shared_index});
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t043-non-live", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(96, "t043-non-live");
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-read-non-live-t043",
                          .identity = identity,
                          .expected_size = static_cast<std::uint64_t>(payload.size()),
                          .expected_checksum = ComputeStoreChecksumOrThrow(payload),
                          .payload = payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_NO_THROW(UpdateIndexStateOrThrow(*shared_index,
                                                identity.chunk_id,
                                                storedemo::ChunkState::kQuarantined));

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.ReadChunk(
            MakeProtoReadRequest(identity, "read-non-live-t043"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_CORRUPTED);
        EXPECT_EQ(response.state(), storage::STORAGE_CHUNK_STATE_QUARANTINED);
        EXPECT_TRUE(response.payload().empty());
        EXPECT_FALSE(response.complete());
        EXPECT_FALSE(response.full_read());
    }

    TEST_F(StorageNodeServiceTest, ReadChunkRangeRequestReturnsExplicitBoundary)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_read_range");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 47));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t043-range", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(120, "t043-range");
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-read-range-t043",
                          .identity = identity,
                          .expected_size = static_cast<std::uint64_t>(payload.size()),
                          .expected_checksum = ComputeStoreChecksumOrThrow(payload),
                          .payload = payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        auto request = MakeProtoReadRequest(identity, "read-range-t043");
        request.set_offset(4);
        request.set_length(8);

        grpc::Status grpc_status;
        const auto response = server.ReadChunk(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_TRUE(response.summary().code() ==
                        storage::STORAGE_NODE_STATUS_CODE_UNSUPPORTED ||
                    response.summary().code() ==
                        storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT);
        EXPECT_TRUE(response.payload().empty());
        EXPECT_FALSE(response.complete());
        EXPECT_FALSE(response.full_read());
    }

    TEST_F(StorageNodeServiceTest, ScrubChunkMapsHealthyLiveChunkFacts)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        const auto identity = MakeStoreIdentityOrThrow("obj-t082-scrub-healthy", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(64, "t082-scrub-healthy");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        std::size_t stat_sequence = 0;
        recording_store->stat_handler =
            [&](const storedemo::StatChunkRequest &request)
        {
            ++stat_sequence;
            EXPECT_EQ(request.request_id, "scrub-healthy-t082");
            EXPECT_EQ(request.chunk_id, identity.chunk_id);
            EXPECT_TRUE(request.include_quarantine);
            EXPECT_EQ(request.verify_checksum, stat_sequence == 2);

            storedemo::StatChunkResponse response;
            response.status = storedemo::StorageNodeStatusCode::kOk;
            response.metadata.identity = identity;
            response.metadata.node_id = "service-node-t082";
            response.metadata.size = checksum.size_bytes;
            response.metadata.checksum = checksum;
            response.metadata.state = storedemo::ChunkState::kLive;
            response.verified = request.verify_checksum;
            return response;
        };

        auto service = std::make_shared<storedemo::StorageNodeService>(recording_store,
                                                                       "service-node-t082");
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.ScrubChunk(
            MakeProtoScrubRequest(identity,
                                  "scrub-healthy-t082",
                                  &checksum,
                                  checksum.size_bytes),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        ASSERT_EQ(recording_store->stat_calls, 3U);
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_EQ(response.summary().request_id(), "scrub-healthy-t082");
        EXPECT_EQ(response.summary().node_id(), "service-node-t082");
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.result().fact().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.result().fact().expected_size(), checksum.size_bytes);
        EXPECT_EQ(response.result().fact().observed_size(), checksum.size_bytes);
        EXPECT_EQ(response.result().fact().expected_checksum().value(), checksum.value);
        EXPECT_EQ(response.result().fact().observed_checksum().value(), checksum.value);
        EXPECT_EQ(response.result().fact().state_before(),
                  storage::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_EQ(response.result().fact().state_after(),
                  storage::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_TRUE(response.result().fact().checksum_verified());
        EXPECT_FALSE(response.result().fact().known_corrupted());
        EXPECT_FALSE(response.result().fact().known_missing());
        EXPECT_FALSE(response.result().fact().quarantined());
        EXPECT_FALSE(response.result().repair_required());
        EXPECT_FALSE(response.result().retryable());
    }

    TEST_F(StorageNodeServiceTest, ScrubChunkMissingChunkReturnsNotFound)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_scrub_missing");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 82));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t082-scrub-missing", 1, 0, 0);
        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.ScrubChunk(
            MakeProtoScrubRequest(identity, "scrub-missing-t082"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_NOT_FOUND);
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.result().fact().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.result().fact().state_before(),
                  storage::STORAGE_CHUNK_STATE_MISSING);
        EXPECT_EQ(response.result().fact().state_after(),
                  storage::STORAGE_CHUNK_STATE_MISSING);
        EXPECT_TRUE(response.result().fact().known_missing());
        EXPECT_FALSE(response.result().fact().known_corrupted());
        EXPECT_TRUE(response.result().repair_required());
        EXPECT_FALSE(response.result().fact().checksum_verified());
    }

    TEST_F(StorageNodeServiceTest, ScrubChunkCorruptedChunkReturnsCorruptedAndQuarantines)
    {
        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_node_service_scrub_corrupted");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 83));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t082-scrub-corrupted", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(96, "t082-scrub-corrupted");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-scrub-corrupted-t082",
                          .identity = identity,
                          .expected_size = static_cast<std::uint64_t>(payload.size()),
                          .expected_checksum = checksum,
                          .payload = payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_NO_THROW(TamperChunkPayloadOrThrow(
            *store,
            identity.chunk_id,
            storedemo::test::MakeChunkPayload(payload.size(), "t082-tampered")));

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.ScrubChunk(
            MakeProtoScrubRequest(identity,
                                  "scrub-corrupted-t082",
                                  &checksum,
                                  checksum.size_bytes),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_CORRUPTED);
        EXPECT_EQ(response.result().fact().state_before(),
                  storage::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_EQ(response.result().fact().state_after(),
                  storage::STORAGE_CHUNK_STATE_QUARANTINED);
        EXPECT_TRUE(response.result().fact().known_corrupted());
        EXPECT_TRUE(response.result().fact().quarantined());
        EXPECT_FALSE(response.result().fact().known_missing());
        EXPECT_TRUE(response.result().repair_required());
        EXPECT_FALSE(response.result().fact().checksum_verified());
        EXPECT_TRUE(response.result().fact().observed_checksum().value().empty());

        const auto post_scrub = store->StatChunk(
            storedemo::StatChunkRequest{
                .request_id = "scrub-corrupted-post-t082",
                .chunk_id = identity.chunk_id});
        ASSERT_EQ(post_scrub.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(post_scrub.metadata.state, storedemo::ChunkState::kQuarantined);
    }

    TEST_F(StorageNodeServiceTest, ScrubChunkQuarantinedChunkReturnsCorruptedBoundary)
    {
        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_node_service_scrub_quarantined");
        auto shared_index = MakeSharedIndex();
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            storedemo::LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.root(),
                .node_id = storedemo::test::MakeStorageNodeIdFixture(84),
                .chunk_index = shared_index});
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t082-scrub-quarantined", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(72, "t082-scrub-quarantined");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-scrub-quarantined-t082",
                          .identity = identity,
                          .expected_size = static_cast<std::uint64_t>(payload.size()),
                          .expected_checksum = checksum,
                          .payload = payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_NO_THROW(UpdateIndexStateOrThrow(*shared_index,
                                               identity.chunk_id,
                                               storedemo::ChunkState::kQuarantined));

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.ScrubChunk(
            MakeProtoScrubRequest(identity,
                                  "scrub-quarantined-t082",
                                  &checksum,
                                  checksum.size_bytes),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_CORRUPTED);
        EXPECT_EQ(response.result().fact().state_before(),
                  storage::STORAGE_CHUNK_STATE_QUARANTINED);
        EXPECT_EQ(response.result().fact().state_after(),
                  storage::STORAGE_CHUNK_STATE_QUARANTINED);
        EXPECT_TRUE(response.result().fact().known_corrupted());
        EXPECT_TRUE(response.result().fact().quarantined());
        EXPECT_TRUE(response.result().repair_required());
        EXPECT_FALSE(response.result().fact().checksum_verified());
    }

    TEST_F(StorageNodeServiceTest, RepairChunkMapsDurableSuccessAndFacts)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_repair_success");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 85));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t085-repair-success", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(128, "t085-repair-success");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.RepairChunk(
            MakeProtoRepairRequest(identity, payload, "repair-success-t085"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.result().fact().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.result().fact().source_node_id(), "repair-source-node");
        EXPECT_EQ(response.result().fact().target_node_id(), store->config().node_id);
        EXPECT_EQ(response.result().fact().expected_size(), payload.size());
        EXPECT_EQ(response.result().fact().observed_size(), payload.size());
        EXPECT_EQ(response.result().fact().expected_checksum().value(), checksum.value);
        EXPECT_EQ(response.result().fact().observed_checksum().value(), checksum.value);
        EXPECT_EQ(response.result().fact().source_state(),
                  storage::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_EQ(response.result().fact().target_state(),
                  storage::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_TRUE(response.result().fact().source_checksum_verified());
        EXPECT_FALSE(response.result().fact().source_unavailable());
        EXPECT_TRUE(response.result().fact().target_durable());
        EXPECT_FALSE(response.result().fact().already_exists());
        EXPECT_TRUE(response.result().repaired());
        EXPECT_FALSE(response.result().retryable());

        const auto read_response = store->ReadChunk(
            MakeReadRequest(identity.chunk_id, "repair-success-read-t085"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(read_response.payload, payload);
    }

    TEST_F(StorageNodeServiceTest, RepairChunkAlreadyExistsSameChecksumIsIdempotentSuccess)
    {
        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_node_service_repair_already_exists");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 86));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity =
            MakeStoreIdentityOrThrow("obj-t085-repair-already-exists", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(96, "t085-repair-already-exists");
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "repair-prewrite-t085",
                          .identity = identity,
                          .expected_size = static_cast<std::uint64_t>(payload.size()),
                          .expected_checksum = ComputeStoreChecksumOrThrow(payload),
                          .payload = payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.RepairChunk(
            MakeProtoRepairRequest(identity, payload, "repair-already-exists-t085"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(response.result().fact().already_exists());
        EXPECT_TRUE(response.result().fact().target_durable());
        EXPECT_TRUE(response.result().repaired());
    }

    TEST_F(StorageNodeServiceTest, RepairChunkAlreadyExistsDifferentPayloadReturnsConflict)
    {
        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_node_service_repair_conflict");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 87));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity =
            MakeStoreIdentityOrThrow("obj-t085-repair-conflict", 1, 0, 0);
        const auto original_payload =
            storedemo::test::MakeChunkPayload(80, "t085-repair-original");
        const auto different_payload =
            storedemo::test::MakeChunkPayload(80, "t085-repair-different");
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "repair-conflict-prewrite-t085",
                          .identity = identity,
                          .expected_size =
                              static_cast<std::uint64_t>(original_payload.size()),
                          .expected_checksum =
                              ComputeStoreChecksumOrThrow(original_payload),
                          .payload = original_payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.RepairChunk(
            MakeProtoRepairRequest(identity,
                                   different_payload,
                                   "repair-conflict-t085"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_CONFLICT);
        EXPECT_FALSE(response.result().fact().target_durable());
        EXPECT_FALSE(response.result().repaired());

        const auto read_response = store->ReadChunk(
            MakeReadRequest(identity.chunk_id, "repair-conflict-read-t085"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(read_response.payload, original_payload);
    }

    TEST_F(StorageNodeServiceTest, RepairChunkPayloadChecksumMismatchDoesNotWriteTarget)
    {
        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_node_service_repair_checksum");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 88));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity =
            MakeStoreIdentityOrThrow("obj-t085-repair-checksum", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(72, "t085-repair-checksum");
        auto request = MakeProtoRepairRequest(identity, payload, "repair-checksum-t085");
        request.set_payload(
            storedemo::test::MakeChunkPayload(payload.size(), "t085-repair-tampered"));

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.RepairChunk(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH);
        EXPECT_FALSE(response.result().fact().target_durable());
        EXPECT_FALSE(response.result().repaired());

        EXPECT_EQ(store->StatChunk(
                      storedemo::StatChunkRequest{
                          .request_id = "repair-checksum-stat-t085",
                          .chunk_id = identity.chunk_id})
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
    }

    TEST_F(StorageNodeServiceTest, RepairChunkRejectsInvalidIdentityAndQuarantinedSource)
    {
        storedemo::test::ScopedStoreTestDir temp_dir(
            "storage_node_service_repair_invalid");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 89));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity =
            MakeStoreIdentityOrThrow("obj-t085-repair-invalid", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(64, "t085-repair-invalid");

        auto invalid_request =
            MakeProtoRepairRequest(identity, payload, "repair-invalid-t085");
        invalid_request.clear_chunk_id();
        invalid_request.clear_object_id();
        invalid_request.set_version(0);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto invalid_response = server.RepairChunk(invalid_request, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(invalid_response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT);
        EXPECT_FALSE(invalid_response.result().repaired());

        grpc::Status quarantined_grpc_status;
        const auto quarantined_response = server.RepairChunk(
            MakeProtoRepairRequest(identity,
                                   payload,
                                   "repair-quarantined-source-t085",
                                   "repair-source-node",
                                   storedemo::ChunkState::kQuarantined),
            &quarantined_grpc_status);
        ASSERT_TRUE(quarantined_grpc_status.ok())
            << quarantined_grpc_status.error_message();
        EXPECT_EQ(quarantined_response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_CORRUPTED);
        EXPECT_FALSE(quarantined_response.result().fact().target_durable());
        EXPECT_FALSE(quarantined_response.result().repaired());
    }

    TEST_F(StorageNodeServiceTest, DeleteChunkMapsFieldsAndRetryableResponseFacts)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        const auto identity = MakeStoreIdentityOrThrow("obj-t052-recording", 7, 1, 0);
        const auto checksum =
            ComputeStoreChecksumOrThrow("delete-recording-checksum-payload");

        recording_store->delete_handler =
            [identity, checksum](const storedemo::DeleteChunkRequest &request)
        {
            EXPECT_EQ(request.request_id, "delete-recording-t052");
            EXPECT_EQ(request.chunk_id, identity.chunk_id);
            EXPECT_EQ(request.reason, "service delete test");
            EXPECT_EQ(request.metadata_boundary, "metadata-first-boundary");
            EXPECT_EQ(request.expected_checksum.value, checksum.value);

            storedemo::DeleteChunkResponse response;
            response.status = storedemo::StorageNodeStatusCode::kTimeout;
            response.error_detail = "delete path timed out";
            response.retry_after_ms = 42;
            response.metadata.identity = identity;
            response.metadata.node_id = "service-node-t052";
            response.metadata.size = checksum.size_bytes;
            response.metadata.checksum = checksum;
            response.metadata.state = storedemo::ChunkState::kLive;
            return response;
        };

        auto service = std::make_shared<storedemo::StorageNodeService>(recording_store,
                                                                       "service-node-t052");
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.DeleteChunk(
            MakeProtoDeleteRequest(identity, "delete-recording-t052", &checksum),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        ASSERT_EQ(recording_store->delete_calls, 1U);
        EXPECT_EQ(recording_store->last_delete_request.request_id,
                  "delete-recording-t052");
        EXPECT_EQ(recording_store->last_delete_request.chunk_id, identity.chunk_id);
        EXPECT_EQ(recording_store->last_delete_request.reason, "service delete test");
        EXPECT_EQ(recording_store->last_delete_request.metadata_boundary,
                  "metadata-first-boundary");
        EXPECT_EQ(recording_store->last_delete_request.expected_checksum.value,
                  checksum.value);

        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_TIMEOUT);
        EXPECT_EQ(response.summary().message(), "delete path timed out");
        EXPECT_EQ(response.summary().request_id(), "delete-recording-t052");
        EXPECT_EQ(response.summary().node_id(), "service-node-t052");
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.summary().retry_after_ms(), 42U);
        EXPECT_EQ(response.chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.size(), checksum.size_bytes);
        EXPECT_EQ(response.checksum().value(), checksum.value);
        EXPECT_EQ(response.state(), storage::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_FALSE(response.deleted());
        EXPECT_FALSE(response.already_missing());
        EXPECT_FALSE(response.already_deleted());
        EXPECT_TRUE(response.retryable());
    }

    TEST_F(StorageNodeServiceTest, DeleteChunkDeletesLiveChunkAndKeepsMetadataCommittedVisible)
    {
        const auto fixture = LoadDeleteFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.zip");

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t052-delete-live",
                            "create-bucket-t052-delete-live"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t052-delete-live", 1, 0, 0);
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_delete_live");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 52));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-delete-live-t052",
                          .identity = identity,
                          .expected_size =
                              static_cast<std::uint64_t>(fixture.payload.size()),
                          .expected_checksum =
                              ComputeStoreChecksumOrThrow(fixture.payload),
                          .payload = fixture.payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t052-delete-live",
                                                        "objects/test_file.zip",
                                                        identity.object_id,
                                                        "create-object-t052-delete-live",
                                                        fixture.payload.size(),
                                                        "etag-t052-delete-live"))
                        .Ok);
        std::vector<raftdemo::ChunkRef> chunks{
            raftdemo::ChunkRef{.chunk_id = identity.chunk_id,
                               .offset = identity.offset,
                               .size = static_cast<std::uint64_t>(fixture.payload.size()),
                               .replica_nodes = {store->config().node_id},
                               .checksum =
                                   ComputeStoreChecksumOrThrow(fixture.payload).value}};
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCommitObjectCommandWithChunks("bucket-t052-delete-live",
                                                          "objects/test_file.zip",
                                                          identity.object_id,
                                                          "commit-object-t052-delete-live",
                                                          fixture.payload.size(),
                                                          "etag-t052-delete-live",
                                                          std::move(chunks)))
                        .Ok);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        const auto checksum = ComputeStoreChecksumOrThrow(fixture.payload);
        grpc::Status grpc_status;
        const auto response = server.DeleteChunk(
            MakeProtoDeleteRequest(identity, "delete-live-t052", &checksum),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_EQ(response.summary().node_id(), store->config().node_id);
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_TRUE(response.deleted());
        EXPECT_FALSE(response.already_missing());
        EXPECT_FALSE(response.already_deleted());
        EXPECT_FALSE(response.retryable());
        EXPECT_EQ(response.state(), storage::STORAGE_CHUNK_STATE_DELETED);

        const auto read_after_delete =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-delete-live-t052"));
        EXPECT_NE(read_after_delete.status, storedemo::StorageNodeStatusCode::kOk);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t052-delete-live", .object_key = "objects/test_file.zip"});
        ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(head.record.has_value());
        EXPECT_TRUE(head.record->IsCommitted());

        const auto listed = machine.ListObjects(
            {.bucket = "bucket-t052-delete-live", .prefix = "objects/"});
        ASSERT_EQ(listed.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_EQ(listed.records.size(), 1U);
        EXPECT_EQ(listed.records.front().object_key, "objects/test_file.zip");
    }

    TEST_F(StorageNodeServiceTest, DeleteChunkMissingAndDeletedRemainIdempotent)
    {
        const auto fixture = LoadDeleteFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.zip");

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_delete_idempotent");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 53));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        const auto missing_identity = MakeStoreIdentityOrThrow("obj-t052-missing", 1, 0, 0);
        grpc::Status grpc_status;
        const auto missing = server.DeleteChunk(
            MakeProtoDeleteRequest(missing_identity, "delete-missing-t052"),
            &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(missing.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(missing.already_missing());
        EXPECT_FALSE(missing.already_deleted());
        EXPECT_EQ(missing.state(), storage::STORAGE_CHUNK_STATE_MISSING);

        const auto deleted_identity = MakeStoreIdentityOrThrow("obj-t052-deleted", 1, 0, 0);
        const auto checksum = ComputeStoreChecksumOrThrow(fixture.payload);
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-delete-idempotent-t052",
                          .identity = deleted_identity,
                          .expected_size =
                              static_cast<std::uint64_t>(fixture.payload.size()),
                          .expected_checksum = checksum,
                          .payload = fixture.payload})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        const auto first_delete = server.DeleteChunk(
            MakeProtoDeleteRequest(deleted_identity,
                                   "delete-first-t052",
                                   &checksum),
            &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(first_delete.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(first_delete.deleted());
        EXPECT_FALSE(first_delete.already_missing());

        const auto second_delete = server.DeleteChunk(
            MakeProtoDeleteRequest(deleted_identity,
                                   "delete-second-t052",
                                   &checksum),
            &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(second_delete.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_FALSE(second_delete.deleted());
        EXPECT_TRUE(second_delete.already_missing());
        EXPECT_TRUE(second_delete.already_deleted());
        EXPECT_EQ(second_delete.state(), storage::STORAGE_CHUNK_STATE_DELETED);
    }

    TEST_F(StorageNodeServiceTest, DeleteChunkChecksumMismatchDoesNotRemoveLiveChunk)
    {
        const auto fixture = LoadDeleteFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.zip");

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_delete_checksum");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 54));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        const auto identity = MakeStoreIdentityOrThrow("obj-t052-checksum", 1, 0, 0);
        const auto checksum = ComputeStoreChecksumOrThrow(fixture.payload);
        ASSERT_EQ(store->WriteChunk(
                      storedemo::WriteChunkRequest{
                          .request_id = "write-delete-checksum-t052",
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

        const auto wrong_checksum = ComputeStoreChecksumOrThrow("different-delete-payload");
        grpc::Status grpc_status;
        const auto response = server.DeleteChunk(
            MakeProtoDeleteRequest(identity,
                                   "delete-checksum-t052",
                                   &wrong_checksum),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH);
        EXPECT_FALSE(response.deleted());
        EXPECT_FALSE(response.already_missing());
        EXPECT_FALSE(response.retryable());
        EXPECT_EQ(response.state(), storage::STORAGE_CHUNK_STATE_LIVE);

        const auto read_response = store->ReadChunk(
            MakeReadRequest(identity.chunk_id, "read-delete-checksum-t052"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, fixture.payload);
    }

    TEST_F(StorageNodeServiceTest, DeleteChunkIdentityMismatchReturnsExplicitErrorWithoutStoreCall)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        auto service = std::make_shared<storedemo::StorageNodeService>(recording_store,
                                                                       "service-node-t052");
        RunningStorageNodeService server(service);

        const auto actual_identity = MakeStoreIdentityOrThrow("obj-t052-identity-a", 1, 0, 0);
        const auto mismatched_identity =
            MakeStoreIdentityOrThrow("obj-t052-identity-b", 1, 0, 0);
        auto request = MakeProtoDeleteRequest(actual_identity, "delete-identity-mismatch-t052");
        request.set_object_id(mismatched_identity.object_id);
        request.set_version(mismatched_identity.version);
        request.set_chunk_index(mismatched_identity.chunk_index);

        grpc::Status grpc_status;
        const auto response = server.DeleteChunk(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT);
        EXPECT_EQ(recording_store->delete_calls, 0U);
        EXPECT_FALSE(response.deleted());
        EXPECT_FALSE(response.already_missing());
        EXPECT_FALSE(response.already_deleted());
        EXPECT_FALSE(response.retryable());
    }

    TEST_F(StorageNodeServiceTest, BatchDeleteChunksReturnsIndependentResultsAndAggregateFacts)
    {
        const auto fixture = LoadDeleteFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.zip");

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_delete_batch");
        auto inner_store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 55));
        ASSERT_EQ(inner_store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        auto store = std::make_shared<ForcedDeleteChunkStore>(inner_store);

        const auto success_identity = MakeStoreIdentityOrThrow("obj-t052-batch-success", 1, 0, 0);
        const auto deleted_identity = MakeStoreIdentityOrThrow("obj-t052-batch-deleted", 1, 0, 0);
        const auto retry_identity = MakeStoreIdentityOrThrow("obj-t052-batch-retry", 1, 0, 0);
        const auto nonretry_identity = MakeStoreIdentityOrThrow("obj-t052-batch-nonretry", 1, 0, 0);
        const auto missing_identity = MakeStoreIdentityOrThrow("obj-t052-batch-missing", 1, 0, 0);
        const auto checksum = ComputeStoreChecksumOrThrow(fixture.payload);

        for (const auto *identity : {&success_identity, &deleted_identity, &retry_identity, &nonretry_identity})
        {
            ASSERT_EQ(inner_store->WriteChunk(
                          storedemo::WriteChunkRequest{
                              .request_id = "write-" + identity->chunk_id,
                              .identity = *identity,
                              .expected_size =
                                  static_cast<std::uint64_t>(fixture.payload.size()),
                              .expected_checksum = checksum,
                              .payload = fixture.payload})
                          .status,
                      storedemo::StorageNodeStatusCode::kOk);
        }

        ASSERT_EQ(inner_store->DeleteChunk(
                      storedemo::DeleteChunkRequest{
                          .request_id = "prime-delete-t052",
                          .chunk_id = deleted_identity.chunk_id,
                          .reason = "prime delete",
                          .metadata_boundary = "prime"})
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        storedemo::DeleteChunkResponse retryable_response;
        retryable_response.status = storedemo::StorageNodeStatusCode::kTimeout;
        retryable_response.error_detail = "forced retryable timeout";
        retryable_response.retry_after_ms = 25;
        retryable_response.metadata.identity = retry_identity;
        retryable_response.metadata.node_id = inner_store->config().node_id;
        retryable_response.metadata.size = checksum.size_bytes;
        retryable_response.metadata.checksum = checksum;
        retryable_response.metadata.state = storedemo::ChunkState::kLive;
        store->ForceDeleteResponse(retry_identity.chunk_id, retryable_response);

        storedemo::DeleteChunkResponse nonretryable_response;
        nonretryable_response.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
        nonretryable_response.error_detail = "forced non-retryable invalid argument";
        nonretryable_response.metadata.identity = nonretry_identity;
        nonretryable_response.metadata.node_id = inner_store->config().node_id;
        nonretryable_response.metadata.size = checksum.size_bytes;
        nonretryable_response.metadata.checksum = checksum;
        nonretryable_response.metadata.state = storedemo::ChunkState::kLive;
        store->ForceDeleteResponse(nonretry_identity.chunk_id, nonretryable_response);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            inner_store->config().node_id);
        RunningStorageNodeService server(service);

        storage::BatchDeleteChunksRequest request;
        request.set_request_id("batch-delete-t052");
        request.set_timeout_ms(2500);
        request.set_best_effort_cancel(true);
        *request.add_chunks() =
            MakeProtoBatchDeleteItem(success_identity, &checksum);
        *request.add_chunks() =
            MakeProtoBatchDeleteItem(missing_identity);
        *request.add_chunks() =
            MakeProtoBatchDeleteItem(deleted_identity, &checksum);
        *request.add_chunks() =
            MakeProtoBatchDeleteItem(retry_identity, &checksum);
        *request.add_chunks() =
            MakeProtoBatchDeleteItem(nonretry_identity, &checksum);

        grpc::Status grpc_status;
        const auto response = server.BatchDeleteChunks(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        ASSERT_EQ(store->delete_calls, 5U);
        EXPECT_EQ(response.summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_EQ(response.summary().request_id(), "batch-delete-t052");
        EXPECT_EQ(response.summary().node_id(), inner_store->config().node_id);
        EXPECT_EQ(response.summary().message(),
                  "BatchDeleteChunks completed with partial failures");
        EXPECT_EQ(response.summary().retry_after_ms(), 25U);
        EXPECT_EQ(response.success_count(), 1U);
        EXPECT_EQ(response.idempotent_count(), 2U);
        EXPECT_EQ(response.retryable_failure_count(), 1U);
        EXPECT_EQ(response.non_retryable_failure_count(), 1U);
        EXPECT_TRUE(response.partial_failure());
        ASSERT_EQ(response.results_size(), 5);

        EXPECT_EQ(response.results(0).summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(response.results(0).deleted());
        EXPECT_FALSE(response.results(0).already_missing());
        EXPECT_FALSE(response.results(0).retryable());

        EXPECT_EQ(response.results(1).summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(response.results(1).already_missing());
        EXPECT_FALSE(response.results(1).already_deleted());
        EXPECT_EQ(response.results(1).state(), storage::STORAGE_CHUNK_STATE_MISSING);

        EXPECT_EQ(response.results(2).summary().code(), storage::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(response.results(2).already_missing());
        EXPECT_TRUE(response.results(2).already_deleted());
        EXPECT_EQ(response.results(2).state(), storage::STORAGE_CHUNK_STATE_DELETED);

        EXPECT_EQ(response.results(3).summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_TIMEOUT);
        EXPECT_TRUE(response.results(3).retryable());
        EXPECT_EQ(response.results(3).summary().retry_after_ms(), 25U);

        EXPECT_EQ(response.results(4).summary().code(),
                  storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT);
        EXPECT_FALSE(response.results(4).retryable());

        EXPECT_EQ(inner_store->ReadChunk(
                      MakeReadRequest(success_identity.chunk_id,
                                      "read-batch-success-after-delete-t052"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(inner_store->ReadChunk(
                      MakeReadRequest(retry_identity.chunk_id,
                                      "read-batch-retry-still-live-t052"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(inner_store->ReadChunk(
                      MakeReadRequest(nonretry_identity.chunk_id,
                                      "read-batch-nonretry-still-live-t052"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
    }
} // namespace
