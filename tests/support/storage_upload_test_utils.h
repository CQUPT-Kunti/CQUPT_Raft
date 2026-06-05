#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/chunk_store.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/upload/upload_coordinator.h"
#include "support/metadata_test_utils.h"
#include "support/store_test_utils.h"

namespace raftdemo
{
    std::string SerializeMetadataCommand(const MetadataCommand &command);
}

namespace storedemo::test
{
    struct FixtureBinaryPayload
    {
        std::string payload;
        std::filesystem::path source_path;
        bool used_repo_fixture{false};
    };

    inline std::filesystem::path StoreUploadRepoRoot()
    {
        return std::filesystem::path(__FILE__)
            .parent_path()
            .parent_path()
            .parent_path()
            .lexically_normal();
    }

    inline FixtureBinaryPayload LoadUploadFixtureBinaryPayload()
    {
        const auto fixture_path =
            StoreUploadRepoRoot() / "tests" / "test_file" / "test_file.deb";
        if (!std::filesystem::exists(fixture_path))
        {
            throw std::runtime_error("missing upload fixture: " +
                                     fixture_path.string());
        }

        std::ifstream input(fixture_path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open upload fixture: " +
                                     fixture_path.string());
        }

        return FixtureBinaryPayload{
            .payload = std::string(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()),
            .source_path = fixture_path,
            .used_repo_fixture = true};
    }

    inline ChunkChecksum ComputeStoreChecksumOrThrow(std::string_view payload)
    {
        ChunkChecksum checksum;
        std::string error_detail;
        const auto status =
            ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute store checksum: " +
                                     error_detail);
        }
        return checksum;
    }

    inline LocalDiskChunkStoreConfig MakeUploadStoreConfig(
        const std::filesystem::path &root,
        const std::size_t node_index)
    {
        return LocalDiskChunkStoreConfig{
            .data_dir = root / ("node_" + std::to_string(node_index)),
            .node_id = MakeStorageNodeIdFixture(node_index)};
    }

    inline raftdemo::MetadataCommand MakeCreateObjectCommandWithSizeVersion(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::uint64_t version,
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
                                   version,
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

    inline raftdemo::MetadataCommand MakeCommitObjectCommandWithChunksVersion(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::uint64_t version,
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
            version,
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

    inline StorageNodeStatusCode MapMetadataStatusCode(
        const raftdemo::MetadataStatusCode code)
    {
        switch (code)
        {
        case raftdemo::MetadataStatusCode::kOk:
        case raftdemo::MetadataStatusCode::kIdempotentReplay:
            return StorageNodeStatusCode::kOk;
        case raftdemo::MetadataStatusCode::kInvalidArgument:
            return StorageNodeStatusCode::kInvalidArgument;
        case raftdemo::MetadataStatusCode::kNotFound:
            return StorageNodeStatusCode::kNotFound;
        case raftdemo::MetadataStatusCode::kIdempotencyConflict:
        case raftdemo::MetadataStatusCode::kStateConflict:
            return StorageNodeStatusCode::kConflict;
        case raftdemo::MetadataStatusCode::kTimeout:
            return StorageNodeStatusCode::kTimeout;
        case raftdemo::MetadataStatusCode::kNotLeader:
            return StorageNodeStatusCode::kNodeUnavailable;
        case raftdemo::MetadataStatusCode::kInternalError:
        default:
            return StorageNodeStatusCode::kIoError;
        }
    }

    class InMemoryUploadMetadataClient final : public UploadMetadataClient
    {
    public:
        explicit InMemoryUploadMetadataClient(raftdemo::MetadataStateMachine &machine)
            : machine_(machine)
        {
        }

        UploadMetadataResult CreateObject(
            const UploadMetadataCreateRequest &request) override
        {
            ++create_calls_;
            last_create_request_ = request;

            if (forced_create_failure_.has_value())
            {
                return *forced_create_failure_;
            }

            EnsureBucket(request.bucket);

            const auto apply = raftdemo::test::ApplyMetadataCommand(
                machine_,
                next_index_++,
                MakeCreateObjectCommandWithSizeVersion(request.bucket,
                                                       request.object_key,
                                                       request.object_id,
                                                       request.version,
                                                       request.request_id,
                                                       request.size,
                                                       request.etag,
                                                       request.client_time_unix_ms));
            return UploadMetadataResult{
                .status = apply.Ok ? StorageNodeStatusCode::kOk
                                   : StorageNodeStatusCode::kConflict,
                .error_detail = apply.message};
        }

        UploadMetadataResult CommitObject(
            const UploadMetadataCommitRequest &request) override
        {
            ++commit_calls_;
            last_commit_request_ = request;

            if (forced_commit_failure_.has_value())
            {
                return *forced_commit_failure_;
            }

            std::vector<raftdemo::ChunkRef> chunks;
            chunks.reserve(request.chunks.size());
            for (const auto &chunk : request.chunks)
            {
                chunks.push_back(raftdemo::ChunkRef{
                    .chunk_id = chunk.identity.chunk_id,
                    .offset = chunk.offset,
                    .size = chunk.size,
                    .replica_nodes = chunk.replica_nodes,
                    .checksum = chunk.checksum.value});
            }

            const auto apply = raftdemo::test::ApplyMetadataCommand(
                machine_,
                next_index_++,
                MakeCommitObjectCommandWithChunksVersion(request.bucket,
                                                         request.object_key,
                                                         request.object_id,
                                                         request.version,
                                                         request.request_id,
                                                         request.size,
                                                         request.etag,
                                                         std::move(chunks),
                                                         request.client_time_unix_ms));
            return UploadMetadataResult{
                .status = apply.Ok ? StorageNodeStatusCode::kOk
                                   : StorageNodeStatusCode::kConflict,
                .error_detail = apply.message};
        }

        void ForceCreateFailure(UploadMetadataResult result)
        {
            forced_create_failure_ = std::move(result);
        }

        void ForceCommitFailure(UploadMetadataResult result)
        {
            forced_commit_failure_ = std::move(result);
        }

        [[nodiscard]] std::size_t create_calls() const
        {
            return create_calls_;
        }

        [[nodiscard]] std::size_t commit_calls() const
        {
            return commit_calls_;
        }

        [[nodiscard]] const std::optional<UploadMetadataCreateRequest> &
        last_create_request() const
        {
            return last_create_request_;
        }

        [[nodiscard]] const std::optional<UploadMetadataCommitRequest> &
        last_commit_request() const
        {
            return last_commit_request_;
        }

    private:
        void EnsureBucket(const std::string &bucket)
        {
            if (created_buckets_.contains(bucket))
            {
                return;
            }

            const auto apply = raftdemo::test::ApplyMetadataCommand(
                machine_,
                next_index_++,
                raftdemo::test::MakeCreateBucketCommand(
                    bucket,
                    "create-bucket-" + bucket));
            if (!apply.Ok)
            {
                throw std::runtime_error("failed to create bucket in test metadata: " +
                                         apply.message);
            }
            created_buckets_.insert(bucket);
        }

        raftdemo::MetadataStateMachine &machine_;
        std::uint64_t next_index_{1};
        std::size_t create_calls_{0};
        std::size_t commit_calls_{0};
        std::unordered_set<std::string> created_buckets_;
        std::optional<UploadMetadataResult> forced_create_failure_;
        std::optional<UploadMetadataResult> forced_commit_failure_;
        std::optional<UploadMetadataCreateRequest> last_create_request_;
        std::optional<UploadMetadataCommitRequest> last_commit_request_;
    };

    class LocalStoreUploadChunkWriter final : public UploadChunkWriter
    {
    public:
        WriteChunkResponse WriteChunkToNode(
            const StorageNodePlacementCandidate &target,
            const WriteChunkRequest &request,
            const StorageTaskContext &context) override
        {
            (void)context;
            ++write_calls_;

            auto &history = write_history_[target.node_id];
            history.push_back(request);

            const auto forced = forced_responses_.find(target.node_id);
            if (forced != forced_responses_.end())
            {
                auto response = forced->second;
                response.metadata.identity = request.identity;
                response.metadata.node_id = target.node_id;
                return response;
            }

            const auto store_it = stores_.find(target.node_id);
            if (store_it == stores_.end() || store_it->second == nullptr)
            {
                WriteChunkResponse response;
                response.status = StorageNodeStatusCode::kNodeUnavailable;
                response.error_detail = "no chunk store registered for target node";
                response.metadata.identity = request.identity;
                response.metadata.node_id = target.node_id;
                return response;
            }

            auto response = store_it->second->WriteChunk(request);
            if (response.metadata.node_id.empty())
            {
                response.metadata.node_id = target.node_id;
            }
            return response;
        }

        void RegisterStore(const StorageNodeId &node_id,
                           std::shared_ptr<ChunkStore> store)
        {
            stores_[node_id] = std::move(store);
        }

        void ForceResponse(const StorageNodeId &node_id,
                           WriteChunkResponse response)
        {
            forced_responses_[node_id] = std::move(response);
        }

        [[nodiscard]] std::size_t write_calls() const
        {
            return write_calls_;
        }

        [[nodiscard]] const std::vector<WriteChunkRequest> &history_for(
            const StorageNodeId &node_id) const
        {
            static const std::vector<WriteChunkRequest> empty;
            const auto it = write_history_.find(node_id);
            return it == write_history_.end() ? empty : it->second;
        }

    private:
        std::size_t write_calls_{0};
        std::unordered_map<StorageNodeId, std::shared_ptr<ChunkStore>> stores_;
        std::unordered_map<StorageNodeId, WriteChunkResponse> forced_responses_;
        std::unordered_map<StorageNodeId, std::vector<WriteChunkRequest>> write_history_;
    };
} // namespace storedemo::test
