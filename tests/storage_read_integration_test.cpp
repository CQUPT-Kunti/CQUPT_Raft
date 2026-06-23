#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "metadata.grpc.pb.h"
#include "raft/common/metadata_result.h"
#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/node/storage_node_registry.h"
#include "store/io/durable_file.h"
#include "store/transfer/metadata_transfer_client.h"
#include "store/transfer/object_transfer.h"
#include "store/transfer/storage_transfer_client.h"
#include "support/metadata_test_utils.h"
#include "support/storage_read_test_utils.h"
#include "support/store_test_utils.h"
#include "support/storage_upload_test_utils.h"
#include "view/view_client.h"
#include "view.grpc.pb.h"

namespace
{
    using storedemo::test::CountingReplicaReader;
    using storedemo::test::ReadObjectByManifest;
    using storedemo::test::ReadObjectByManifestRequest;

    storedemo::ChunkIdentity MakeStoreIdentityOrThrow(const std::string_view object_id,
                                                      const std::uint64_t version,
                                                      const std::uint32_t chunk_index,
                                                      const std::uint64_t offset)
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
            .expected_checksum = storedemo::test::ComputeStoreChecksumOrThrow(payload),
                .payload = payload};
    }

    std::filesystem::path ResolveFinalPathOrThrow(const std::filesystem::path &data_root,
                                                  const storedemo::ChunkId &chunk_id)
    {
        storedemo::ChunkPathLayout layout;
        std::string error_detail;
        const auto layout_status =
            storedemo::BuildChunkPathLayout(chunk_id, "probe", &layout, &error_detail);
        if (layout_status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to build final path layout: " +
                                     error_detail);
        }

        std::filesystem::path final_path;
        const auto resolve_status =
            storedemo::ResolveDurablePathUnderRoot(data_root,
                                                   layout.final_relative_path,
                                                   &final_path,
                                                   &error_detail);
        if (resolve_status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to resolve final path: " + error_detail);
        }

        return final_path;
    }

    raftdemo::ChunkRef MakeChunkRefFromMetadata(const storedemo::ChunkMetadata &metadata)
    {
        return raftdemo::ChunkRef{
            .chunk_id = metadata.identity.chunk_id,
            .offset = metadata.identity.offset,
            .size = metadata.size,
            .replica_nodes = {metadata.node_id},
            .checksum = metadata.checksum.value};
    }

    raftdemo::ChunkRef MakeChunkRef(const storedemo::ChunkIdentity &identity,
                                    const std::string &payload,
                                    std::vector<storedemo::StorageNodeId> replica_nodes)
    {
        const auto checksum = storedemo::test::ComputeStoreChecksumOrThrow(payload);
        return raftdemo::ChunkRef{
            .chunk_id = identity.chunk_id,
            .offset = identity.offset,
            .size = static_cast<std::uint64_t>(payload.size()),
            .replica_nodes = std::move(replica_nodes),
            .checksum = checksum.value};
    }

    storedemo::StorageNodeRegistryFacts MakeRegistryFactsForRead(
        const storedemo::StorageNodeHealth health =
            storedemo::StorageNodeHealth::kHealthy,
        const storedemo::StorageNodeDiskPressure disk_pressure =
            storedemo::StorageNodeDiskPressure::kLow,
        const std::uint32_t active_reads = 0,
        const bool read_overloaded = false)
    {
        storedemo::StorageNodeRegistryFacts facts;
        facts.capacity.total_capacity_bytes = 64 * 1024;
        facts.capacity.used_capacity_bytes = 8 * 1024;
        facts.capacity.available_capacity_bytes = 56 * 1024;
        facts.capacity.chunk_count = 1;
        facts.health.health = health;
        facts.health.disk_pressure = disk_pressure;
        facts.load.load.active_reads = active_reads;
        facts.load.load.active_writes = active_reads / 2;
        facts.load.load.queued_ops = active_reads / 3;
        facts.load.read_admission_overloaded = read_overloaded;
        return facts;
    }

    class FakeManifestMetadataService final : public raft::MetadataService::Service
    {
    public:
        void SetLeaderAddress(std::string leader_address)
        {
            std::lock_guard<std::mutex> lock(mu_);
            leader_address_ = std::move(leader_address);
        }

        void SetCommittedObject(const raft::ObjectRecord &object)
        {
            std::lock_guard<std::mutex> lock(mu_);
            object_ = object;
        }

        grpc::Status HeadObject(grpc::ServerContext *,
                                const raft::HeadObjectRequest *request,
                                raft::HeadObjectResponse *response) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            response->mutable_summary()->set_request_id("head-object");
            response->mutable_summary()->set_bucket(request->bucket());
            response->mutable_summary()->set_object_key(request->object_key());
            response->mutable_summary()->set_object_id(request->object_id());
            response->mutable_summary()->set_term(7);
            response->mutable_summary()->set_log_index(12);
            response->mutable_summary()->mutable_leader_hint()->set_leader_id(1);
            response->mutable_summary()->mutable_leader_hint()->set_leader_address(
                leader_address_);

            if (!object_.has_value() ||
                object_->bucket() != request->bucket() ||
                object_->object_key() != request->object_key())
            {
                response->mutable_summary()->set_code(
                    raft::METADATA_STATUS_CODE_NOT_FOUND);
                response->mutable_summary()->set_message("not found");
                response->set_found(false);
                return grpc::Status::OK;
            }

            response->mutable_summary()->set_code(raft::METADATA_STATUS_CODE_OK);
            response->mutable_summary()->set_message("ok");
            response->mutable_summary()->set_state(raft::METADATA_OBJECT_STATE_COMMITTED);
            response->set_found(true);
            response->mutable_object()->CopyFrom(*object_);
            return grpc::Status::OK;
        }

    private:
        mutable std::mutex mu_;
        std::string leader_address_;
        std::optional<raft::ObjectRecord> object_;
    };

    class ScopedManifestMetadataServer
    {
    public:
        ScopedManifestMetadataServer()
        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("127.0.0.1:0",
                                     grpc::InsecureServerCredentials(),
                                     &selected_port_);
            builder.RegisterService(&service_);
            server_ = builder.BuildAndStart();
            if (server_ == nullptr || selected_port_ <= 0)
            {
                throw std::runtime_error("failed to start manifest metadata server");
            }
            address_ = "127.0.0.1:" + std::to_string(selected_port_);
            service_.SetLeaderAddress(address_);
        }

        ~ScopedManifestMetadataServer()
        {
            if (server_ != nullptr)
            {
                server_->Shutdown();
            }
        }

        [[nodiscard]] const std::string &address() const
        {
            return address_;
        }

        [[nodiscard]] FakeManifestMetadataService &service()
        {
            return service_;
        }

    private:
        int selected_port_{0};
        std::string address_;
        FakeManifestMetadataService service_;
        std::unique_ptr<grpc::Server> server_;
    };

    class FakeManifestViewNodeService final : public view::ViewNodeService::Service
    {
    public:
        void SetMetadataEndpoint(std::string metadata_endpoint)
        {
            std::lock_guard<std::mutex> lock(mu_);
            metadata_endpoint_ = std::move(metadata_endpoint);
        }

        void SetStorageNodes(std::vector<view::ViewNodeSnapshot> storage_nodes,
                             const std::uint64_t observed_at_unix_ms)
        {
            std::lock_guard<std::mutex> lock(mu_);
            storage_nodes_ = std::move(storage_nodes);
            observed_at_unix_ms_ = observed_at_unix_ms;
        }

        grpc::Status DiscoverMetadata(grpc::ServerContext *,
                                      const view::DiscoverMetadataRequest *request,
                                      view::DiscoverMetadataResponse *response) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            response->mutable_summary()->set_code(view::VIEW_NODE_STATUS_CODE_OK);
            response->mutable_summary()->set_message("ok");
            response->mutable_summary()->set_request_id(request->request_id());
            response->mutable_summary()->set_cluster_id(request->cluster_id());
            response->mutable_summary()->set_node_id("view-read-test");
            response->set_observed_at_unix_ms(observed_at_unix_ms_);
            response->set_membership_epoch(5);
            response->mutable_leader_hint()->set_node_id("meta-read-test");
            response->mutable_leader_hint()->set_endpoint(metadata_endpoint_);
            response->mutable_leader_hint()->set_observed_at_unix_ms(
                observed_at_unix_ms_);

            auto *snapshot = response->add_metadata_nodes();
            snapshot->set_cluster_id(request->cluster_id());
            snapshot->set_node_id("meta-read-test");
            snapshot->set_node_type(view::VIEW_NODE_TYPE_METADATA);
            snapshot->set_endpoint(metadata_endpoint_);
            snapshot->set_control_plane_endpoint(metadata_endpoint_);
            snapshot->set_registered_at_unix_ms(observed_at_unix_ms_);
            snapshot->set_last_seen_unix_ms(observed_at_unix_ms_);
            snapshot->set_last_sequence(1);
            snapshot->set_liveness(view::VIEW_NODE_LIVENESS_STATE_LIVE);
            snapshot->mutable_health()->set_health(view::VIEW_NODE_HEALTH_HEALTHY);
            snapshot->mutable_health()->set_disk_pressure(
                view::VIEW_NODE_DISK_PRESSURE_LOW);
            return grpc::Status::OK;
        }

        grpc::Status DiscoverStorage(grpc::ServerContext *,
                                     const view::DiscoverStorageRequest *request,
                                     view::DiscoverStorageResponse *response) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            response->mutable_summary()->set_code(view::VIEW_NODE_STATUS_CODE_OK);
            response->mutable_summary()->set_message("ok");
            response->mutable_summary()->set_request_id(request->request_id());
            response->mutable_summary()->set_cluster_id(request->cluster_id());
            response->mutable_summary()->set_node_id("view-read-test");
            response->set_observed_at_unix_ms(observed_at_unix_ms_);
            for (const auto &storage_node : storage_nodes_)
            {
                response->add_storage_nodes()->CopyFrom(storage_node);
            }
            return grpc::Status::OK;
        }

    private:
        mutable std::mutex mu_;
        std::string metadata_endpoint_;
        std::uint64_t observed_at_unix_ms_{1714002000000ULL};
        std::vector<view::ViewNodeSnapshot> storage_nodes_;
    };

    class ScopedManifestViewServer
    {
    public:
        ScopedManifestViewServer()
        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("127.0.0.1:0",
                                     grpc::InsecureServerCredentials(),
                                     &selected_port_);
            builder.RegisterService(&service_);
            server_ = builder.BuildAndStart();
            if (server_ == nullptr || selected_port_ <= 0)
            {
                throw std::runtime_error("failed to start manifest view server");
            }
            address_ = "127.0.0.1:" + std::to_string(selected_port_);
        }

        ~ScopedManifestViewServer()
        {
            if (server_ != nullptr)
            {
                server_->Shutdown();
            }
        }

        [[nodiscard]] const std::string &address() const
        {
            return address_;
        }

        [[nodiscard]] FakeManifestViewNodeService &service()
        {
            return service_;
        }

    private:
        int selected_port_{0};
        std::string address_;
        FakeManifestViewNodeService service_;
        std::unique_ptr<grpc::Server> server_;
    };

    class RecordingReadStorageTransferClient final : public storedemo::StorageTransferClient
    {
    public:
        storedemo::StorageTransferWriteResult WriteChunk(
            const storedemo::StorageTransferWriteRequest &) override
        {
            ++write_calls;
            storedemo::StorageTransferWriteResult result;
            result.status = storedemo::StorageNodeStatusCode::kUnsupported;
            result.error_detail = "write not implemented";
            return result;
        }

        storedemo::StorageTransferReadResult ReadChunk(
            const storedemo::StorageTransferReadRequest &request) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            reads.push_back(request);
            if (read_behavior)
            {
                auto result = read_behavior(request);
                if (result.target.node_id.empty())
                {
                    result.target = request.target;
                }
                return result;
            }

            storedemo::StorageTransferReadResult result;
            result.status = storedemo::StorageNodeStatusCode::kNotFound;
            result.error_detail = "no read behavior configured";
            result.target = request.target;
            return result;
        }

        std::function<storedemo::StorageTransferReadResult(
            const storedemo::StorageTransferReadRequest &request)>
            read_behavior;
        std::vector<storedemo::StorageTransferReadRequest> reads;
        std::size_t write_calls{0};

    private:
        std::mutex mu_;
    };

    view::ViewNodeSnapshot MakeReadStorageSnapshot(const std::string &cluster_id,
                                                   const std::string &node_id,
                                                   const std::string &endpoint,
                                                   const std::uint64_t available_capacity_bytes)
    {
        view::ViewNodeSnapshot snapshot;
        snapshot.set_cluster_id(cluster_id);
        snapshot.set_node_id(node_id);
        snapshot.set_node_type(view::VIEW_NODE_TYPE_STORAGE);
        snapshot.set_endpoint(endpoint);
        snapshot.set_data_plane_endpoint(endpoint);
        snapshot.set_registered_at_unix_ms(1714002000000ULL);
        snapshot.set_last_seen_unix_ms(1714002000000ULL);
        snapshot.set_last_sequence(1);
        snapshot.set_liveness(view::VIEW_NODE_LIVENESS_STATE_LIVE);
        snapshot.mutable_health()->set_health(view::VIEW_NODE_HEALTH_HEALTHY);
        snapshot.mutable_health()->set_disk_pressure(view::VIEW_NODE_DISK_PRESSURE_LOW);
        snapshot.mutable_capacity()->set_total_capacity_bytes(
            available_capacity_bytes + 8192ULL);
        snapshot.mutable_capacity()->set_used_capacity_bytes(8192ULL);
        snapshot.mutable_capacity()->set_available_capacity_bytes(
            available_capacity_bytes);
        return snapshot;
    }

    raft::ObjectRecord MakeCommittedObjectRecord(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::uint64_t version,
        const std::string &payload,
        const std::vector<raftdemo::ChunkRef> &chunks)
    {
        const auto checksum = storedemo::test::ComputeStoreChecksumOrThrow(payload);
        raft::ObjectRecord object;
        object.set_bucket(bucket);
        object.set_object_key(object_key);
        object.set_object_id(object_id);
        object.set_version(version);
        object.set_size(static_cast<std::uint64_t>(payload.size()));
        object.set_etag(checksum.value);
        object.set_state(raft::METADATA_OBJECT_STATE_COMMITTED);
        object.set_create_time(1714002000000ULL);
        object.set_commit_time(1714002001000ULL);
        for (const auto &chunk : chunks)
        {
            auto *proto_chunk = object.add_chunks();
            proto_chunk->set_chunk_id(chunk.chunk_id);
            proto_chunk->set_offset(chunk.offset);
            proto_chunk->set_size(chunk.size);
            for (const auto &node_id : chunk.replica_nodes)
            {
                proto_chunk->add_replica_nodes(node_id);
            }
            proto_chunk->set_checksum(chunk.checksum);
        }
        return object;
    }

    std::string ReadBinaryFileOrThrow(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open binary file: " + path.string());
        }
        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
    }

    bool ResultHasDiagnosticContaining(
        const storedemo::DownloadObjectResult &result,
        const std::string_view needle,
        const std::string_view node_id = {})
    {
        return std::any_of(
            result.diagnostics.begin(),
            result.diagnostics.end(),
            [needle, node_id](const storedemo::ObjectTransferDiagnostic &diagnostic)
            {
                if (!node_id.empty() && diagnostic.node_id != node_id)
                {
                    return false;
                }
                return diagnostic.message.find(needle) != std::string::npos;
            });
    }

    std::filesystem::path MakeExpectedDownloadTempPath(
        const std::filesystem::path &destination_path,
        const std::string_view request_id)
    {
        auto temp_path = destination_path;
        temp_path += ".";
        temp_path += request_id;
        temp_path += ".part";
        return temp_path;
    }

    class StorageReadIntegrationTest : public ::testing::Test
    {
    protected:
        static std::vector<std::string> SplitPayloadIntoChunks(const std::string &payload)
        {
            const std::size_t total = payload.size();
            const std::size_t first = total / 3;
            const std::size_t second = total / 3;
            const std::size_t third = total - first - second;

            return {
                payload.substr(0, first),
                payload.substr(first, second),
                payload.substr(first + second, third)};
        }
    };

    TEST_F(StorageReadIntegrationTest, CommittedObjectReadsManifestChunksInOffsetOrder)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T040 committed manifest read integration currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t040_committed");
        storedemo::LocalDiskChunkStore store(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("store"), 40));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t040-read",
                            "create-bucket-t040-read"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");
        ASSERT_FALSE(fixture.payload.empty());

        const std::string object_id = "obj-t040-read";
        const std::string object_key = "objects/test_file.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);
        ASSERT_EQ(payload_parts.size(), 3U);
        ASSERT_FALSE(payload_parts.at(0).empty());
        ASSERT_FALSE(payload_parts.at(1).empty());
        ASSERT_FALSE(payload_parts.at(2).empty());

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t040-read",
                            object_key,
                            object_id,
                            version,
                            "create-object-t040-read",
                            fixture.payload.size(),
                            "etag-t040-read"))
                        .Ok);

        std::vector<storedemo::ChunkIdentity> identities;
        identities.reserve(payload_parts.size());
        std::vector<raftdemo::ChunkRef> ordered_manifest;
        ordered_manifest.reserve(payload_parts.size());
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            const auto identity = MakeStoreIdentityOrThrow(object_id,
                                                           version,
                                                           static_cast<std::uint32_t>(chunk_index),
                                                           next_offset);
            identities.push_back(identity);
            next_offset += payload_parts.at(chunk_index).size();

            const auto write = store.WriteChunk(
                MakeWriteRequest(identity,
                                 payload_parts.at(chunk_index),
                                 "write-t040-read-" + std::to_string(chunk_index)));
            ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
                << write.error_detail;
            ASSERT_TRUE(write.durable);
            ordered_manifest.push_back(MakeChunkRefFromMetadata(write.metadata));
        }

        std::vector<raftdemo::ChunkRef> shuffled_manifest{
            ordered_manifest.at(2),
            ordered_manifest.at(0),
            ordered_manifest.at(1)};
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t040-read",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t040-read",
                            fixture.payload.size(),
                            "etag-t040-read",
                            std::move(shuffled_manifest)))
                        .Ok);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t040-read", .object_key = object_key});
        ASSERT_EQ(head.result.code, raftdemo::MetadataStatusCode::kOk);
        ASSERT_TRUE(head.record.has_value());
        EXPECT_TRUE(head.record->IsCommitted());

        const auto manifest = machine.FindChunkRefs("bucket-t040-read", object_key);
        ASSERT_TRUE(manifest.has_value());
        ASSERT_EQ(manifest->size(), payload_parts.size());

        for (std::size_t chunk_index = 0; chunk_index < manifest->size(); ++chunk_index)
        {
            const auto &chunk_ref = manifest->at(chunk_index);
            EXPECT_EQ(chunk_ref.replica_nodes.size(), 1U);
            EXPECT_EQ(chunk_ref.replica_nodes.front(), store.config().node_id);

            storedemo::ReadChunkRequest read_request;
            read_request.request_id =
                "verify-manifest-t040-" + std::to_string(chunk_index);
            read_request.chunk_id = chunk_ref.chunk_id;
            read_request.expected_checksum.algorithm =
                storedemo::ChunkChecksumAlgorithm::kSha256;
            read_request.expected_checksum.value = chunk_ref.checksum;
            read_request.expected_checksum.size_bytes = chunk_ref.size;
            read_request.verify_checksum = true;

            const auto read = store.ReadChunk(read_request);
            ASSERT_EQ(read.status, storedemo::StorageNodeStatusCode::kOk)
                << read.error_detail;
            EXPECT_EQ(read.metadata.size, chunk_ref.size);
            EXPECT_EQ(read.metadata.checksum.value, chunk_ref.checksum);
        }

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                EXPECT_EQ(node_id, store.config().node_id);
                return store.ReadChunk(request);
            });
        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t040-read",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.read_calls(), payload_parts.size());

        const std::vector<std::string> expected_read_order{
            identities.at(0).chunk_id,
            identities.at(1).chunk_id,
            identities.at(2).chunk_id};
        EXPECT_EQ(reader.read_chunk_ids(), expected_read_order);
#endif
    }

    TEST_F(StorageReadIntegrationTest, PendingObjectDoesNotReadDataPlaneEvenIfChunkExists)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T040 pending-object read gate currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t040_pending");
        storedemo::LocalDiskChunkStore store(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("store"), 41));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t040-pending",
                            "create-bucket-t040-pending"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const auto identity =
            MakeStoreIdentityOrThrow("obj-t040-pending", 1, 0, 0);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t040-pending",
                            "objects/pending.deb",
                            identity.object_id,
                            identity.version,
                            "create-object-t040-pending",
                            fixture.payload.size(),
                            "etag-t040-pending"))
                        .Ok);

        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t040-pending"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t040-pending", .object_key = "objects/pending.deb"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list = machine.ListObjects(
            {.bucket = "bucket-t040-pending", .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                EXPECT_EQ(node_id, store.config().node_id);
                return store.ReadChunk(request);
            });
        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t040-pending",
                .object_key = "objects/pending.deb"});

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_TRUE(result.payload.empty());
        EXPECT_EQ(reader.read_calls(), 0U);
#endif
    }

    TEST_F(StorageReadIntegrationTest, DeletedObjectDoesNotReadDataPlaneAfterMetadataTombstone)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "T040 deleted-object read gate currently validated on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t040_deleted");
        storedemo::LocalDiskChunkStore store(
            storedemo::test::MakeUploadStoreConfig(temp_dir.Path("store"), 42));
        ASSERT_EQ(store.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t040-deleted",
                            "create-bucket-t040-deleted"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const auto identity =
            MakeStoreIdentityOrThrow("obj-t040-deleted", 1, 0, 0);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t040-deleted",
                            "objects/deleted.deb",
                            identity.object_id,
                            identity.version,
                            "create-object-t040-deleted",
                            fixture.payload.size(),
                            "etag-t040-deleted"))
                        .Ok);

        const auto write = store.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-t040-deleted"));
        ASSERT_EQ(write.status, storedemo::StorageNodeStatusCode::kOk)
            << write.error_detail;

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t040-deleted",
                            "objects/deleted.deb",
                            identity.object_id,
                            identity.version,
                            "commit-object-t040-deleted",
                            fixture.payload.size(),
                            "etag-t040-deleted",
                            {MakeChunkRefFromMetadata(write.metadata)}))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeDeleteObjectCommand(
                            "bucket-t040-deleted",
                            "objects/deleted.deb",
                            identity.object_id,
                            "delete-object-t040-deleted"))
                        .Ok);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t040-deleted", .object_key = "objects/deleted.deb"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list = machine.ListObjects(
            {.bucket = "bucket-t040-deleted", .prefix = "objects/"});
        ASSERT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        const auto stored_object =
            machine.FindObject("bucket-t040-deleted", "objects/deleted.deb");
        ASSERT_TRUE(stored_object.has_value());
        EXPECT_TRUE(stored_object->IsDeleted());

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                EXPECT_EQ(node_id, store.config().node_id);
                return store.ReadChunk(request);
            });
        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t040-deleted",
                .object_key = "objects/deleted.deb"});

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_TRUE(result.payload.empty());
        EXPECT_EQ(reader.read_calls(), 0U);
#endif
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectReadsFirstReadableReplicaWithoutFallback)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t045-read-first",
                            "create-bucket-t045-read-first"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t045-first";
        const std::string object_key = "objects/first-readable.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t045-read-first",
                            object_key,
                            object_id,
                            version,
                            "create-object-t045-read-first",
                            fixture.payload.size(),
                            "etag-t045-read-first"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t045-read-first",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t045-read-first",
                            fixture.payload.size(),
                            "etag-t045-read-first",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                if (node_id != "replica-a")
                {
                    storedemo::ReadChunkResponse response;
                    response.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                    response.error_detail = "unexpected fallback";
                    return response;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t045-read-first",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.read_calls(), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-a"), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-b"), 0U);
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectFallsBackAfterUnavailableReplica)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t045-read-unavailable",
                            "create-bucket-t045-read-unavailable"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t045-unavailable";
        const std::string object_key = "objects/fallback-unavailable.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t045-read-unavailable",
                            object_key,
                            object_id,
                            version,
                            "create-object-t045-read-unavailable",
                            fixture.payload.size(),
                            "etag-t045-read-unavailable"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t045-read-unavailable",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t045-read-unavailable",
                            fixture.payload.size(),
                            "etag-t045-read-unavailable",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                if (node_id == "replica-a")
                {
                    storedemo::ReadChunkResponse response;
                    response.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                    response.error_detail = "replica-a unavailable";
                    return response;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t045-read-unavailable",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-b"), payload_parts.size());
        const std::vector<std::string> expected_order{
            "replica-a", "replica-b",
            "replica-a", "replica-b",
            "replica-a", "replica-b"};
        EXPECT_EQ(reader.read_node_ids(), expected_order);
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectFallsBackAfterNotFoundReplica)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t047-read-not-found",
                            "create-bucket-t047-read-not-found"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t047-not-found";
        const std::string object_key = "objects/fallback-not-found.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t047-read-not-found",
                            object_key,
                            object_id,
                            version,
                            "create-object-t047-read-not-found",
                            fixture.payload.size(),
                            "etag-t047-read-not-found"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t047-read-not-found",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t047-read-not-found",
                            fixture.payload.size(),
                            "etag-t047-read-not-found",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                if (node_id == "replica-a")
                {
                    storedemo::ReadChunkResponse response;
                    response.status = storedemo::StorageNodeStatusCode::kNotFound;
                    response.error_detail = "replica-a missing chunk";
                    response.metadata.identity.chunk_id = request.chunk_id;
                    response.metadata.node_id = node_id;
                    return response;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t047-read-not-found",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-b"), payload_parts.size());
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectFallsBackAfterTimeoutReplica)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t047-read-timeout",
                            "create-bucket-t047-read-timeout"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t047-timeout";
        const std::string object_key = "objects/fallback-timeout.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t047-read-timeout",
                            object_key,
                            object_id,
                            version,
                            "create-object-t047-read-timeout",
                            fixture.payload.size(),
                            "etag-t047-read-timeout"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t047-read-timeout",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t047-read-timeout",
                            fixture.payload.size(),
                            "etag-t047-read-timeout",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                if (node_id == "replica-a")
                {
                    storedemo::ReadChunkResponse response;
                    response.status = storedemo::StorageNodeStatusCode::kTimeout;
                    response.error_detail = "replica-a timeout";
                    response.metadata.identity.chunk_id = request.chunk_id;
                    response.metadata.node_id = node_id;
                    return response;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t047-read-timeout",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-b"), payload_parts.size());
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectFallsBackAfterChecksumMismatchReplica)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t045-read-checksum",
                            "create-bucket-t045-read-checksum"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t045-checksum";
        const std::string object_key = "objects/fallback-checksum.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t045-read-checksum",
                            object_key,
                            object_id,
                            version,
                            "create-object-t045-read-checksum",
                            fixture.payload.size(),
                            "etag-t045-read-checksum"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t045-read-checksum",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t045-read-checksum",
                            fixture.payload.size(),
                            "etag-t045-read-checksum",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                if (node_id == "replica-a")
                {
                    storedemo::ReadChunkResponse failure;
                    failure.status = storedemo::StorageNodeStatusCode::kChecksumMismatch;
                    failure.error_detail = "replica-a checksum mismatch";
                    failure.payload = "corrupted-payload-must-not-surface";
                    failure.metadata.identity.chunk_id = request.chunk_id;
                    failure.metadata.node_id = node_id;
                    failure.metadata.size = request.expected_checksum.size_bytes;
                    failure.metadata.checksum = request.expected_checksum;
                    failure.metadata.state = storedemo::ChunkState::kCorrupted;
                    return failure;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t045-read-checksum",
                .object_key = object_key});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), payload_parts.size());
        EXPECT_EQ(reader.calls_for_node("replica-b"), payload_parts.size());
        ASSERT_EQ(result.chunk_results.size(), payload_parts.size());
        for (const auto &chunk_result : result.chunk_results)
        {
            ASSERT_EQ(chunk_result.attempts.size(), 2U);
            EXPECT_EQ(chunk_result.attempts[0].node_id, "replica-a");
            EXPECT_EQ(chunk_result.attempts[0].status,
                      storedemo::StorageNodeStatusCode::kChecksumMismatch);
            EXPECT_EQ(chunk_result.selected_node_id, "replica-b");
        }
    }

    TEST_F(StorageReadIntegrationTest, AllReplicaFailuresReturnExplicitError)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t045-read-all-fail",
                            "create-bucket-t045-read-all-fail"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t045-all-fail";
        const std::string object_key = "objects/all-fail.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t045-read-all-fail",
                            object_key,
                            object_id,
                            version,
                            "create-object-t045-read-all-fail",
                            fixture.payload.size(),
                            "etag-t045-read-all-fail"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t045-read-all-fail",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t045-read-all-fail",
                            fixture.payload.size(),
                            "etag-t045-read-all-fail",
                            manifest))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                storedemo::ReadChunkResponse response;
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                if (node_id == "replica-a")
                {
                    response.status = storedemo::StorageNodeStatusCode::kTimeout;
                    response.error_detail = "replica-a timeout";
                    return response;
                }

                response.status = storedemo::StorageNodeStatusCode::kChecksumMismatch;
                response.error_detail = "replica-b checksum mismatch";
                response.metadata.state = storedemo::ChunkState::kCorrupted;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t045-read-all-fail",
                .object_key = object_key});

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_TRUE(result.payload.empty());
        EXPECT_NE(result.error_detail.find("all replicas failed after"),
                  std::string::npos);
        EXPECT_EQ(reader.calls_for_node("replica-a"), 1U);
        EXPECT_EQ(reader.calls_for_node("replica-b"), 1U);
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectSkipsKnownCorruptedReplicaFacts)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t047-read-skip-corrupted",
                            "create-bucket-t047-read-skip-corrupted"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t047-skip-corrupted";
        const std::string object_key = "objects/skip-corrupted.deb";
        const std::uint64_t version = 1;
        const auto payload_parts = SplitPayloadIntoChunks(fixture.payload);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t047-read-skip-corrupted",
                            object_key,
                            object_id,
                            version,
                            "create-object-t047-read-skip-corrupted",
                            fixture.payload.size(),
                            "etag-t047-read-skip-corrupted"))
                        .Ok);

        std::vector<raftdemo::ChunkRef> manifest;
        std::unordered_map<std::string, std::string> payload_by_chunk_id;
        std::uint64_t next_offset = 0;
        for (std::size_t chunk_index = 0; chunk_index < payload_parts.size(); ++chunk_index)
        {
            auto identity = MakeStoreIdentityOrThrow(object_id,
                                                     version,
                                                     static_cast<std::uint32_t>(chunk_index),
                                                     next_offset);
            manifest.push_back(MakeChunkRef(identity,
                                            payload_parts[chunk_index],
                                            {"replica-a", "replica-b"}));
            payload_by_chunk_id.emplace(identity.chunk_id, payload_parts[chunk_index]);
            next_offset += payload_parts[chunk_index].size();
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t047-read-skip-corrupted",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t047-read-skip-corrupted",
                            fixture.payload.size(),
                            "etag-t047-read-skip-corrupted",
                            manifest))
                        .Ok);

        storedemo::StorageNodeRegistry registry(
            storedemo::StorageNodeRegistryConfig{
                .stale_timeout_ms = 30,
                .dead_timeout_ms = 90});
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-a",
                         .endpoint = "127.0.0.1:7401",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead( storedemo::StorageNodeHealth::kHealthy,
                                                            storedemo::StorageNodeDiskPressure::kLow,
                                                            0)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-b",
                         .endpoint = "127.0.0.1:7402",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead( storedemo::StorageNodeHealth::kHealthy,
                                                            storedemo::StorageNodeDiskPressure::kLow,
                                                            1)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                if (node_id == "replica-a")
                {
                    ADD_FAILURE() << "known corrupted replica should be filtered before read";
                    storedemo::ReadChunkResponse response;
                    response.status = storedemo::StorageNodeStatusCode::kCorrupted;
                    response.error_detail = "unexpected read against replica-a";
                    return response;
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = payload_by_chunk_id.at(request.chunk_id);
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t047-read-skip-corrupted",
                .object_key = object_key,
                .candidate_resolver =
                    [](const raftdemo::ChunkRef &chunk_ref)
                    {
                        return std::vector<storedemo::ReadReplicaCandidate>{
                            storedemo::ReadReplicaCandidate{
                                .node_id = chunk_ref.replica_nodes.at(0),
                                .known_corrupted = true,
                                .has_observed_facts = true},
                            storedemo::ReadReplicaCandidate{
                                .node_id = chunk_ref.replica_nodes.at(1),
                                .has_observed_facts = true}};
                    },
                .registry_snapshot_resolver =
                    [&registry]()
                    { return registry.Snapshot(110); }});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), 0U);
        EXPECT_EQ(reader.calls_for_node("replica-b"), payload_parts.size());
    }

    TEST_F(StorageReadIntegrationTest, CommittedObjectFallsBackAfterLocalStoreQuarantinesCorruptedReplica)
    {
#if !defined(__linux__)
        GTEST_SKIP() << "real local store quarantine fallback is only verified on Linux";
#else
        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t072_quarantine_fallback");
        storedemo::LocalDiskChunkStore store_a(
            storedemo::LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("stores") / "replica_a",
                .node_id = "replica-a"});
        storedemo::LocalDiskChunkStore store_b(
            storedemo::LocalDiskChunkStoreConfig{
                .data_dir = temp_dir.Path("stores") / "replica_b",
                .node_id = "replica-b"});
        ASSERT_EQ(store_a.Initialize().status, storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store_b.Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t072-read-quarantine",
                            "create-bucket-t072-read-quarantine"))
                        .Ok);

        const std::string object_id = "obj-t072-read-quarantine";
        const std::string object_key = "objects/t072-quarantine.bin";
        const std::uint64_t version = 1;
        const std::string payload = storedemo::test::MakeChunkPayload(96, "t072-read");
        const auto identity = MakeStoreIdentityOrThrow(object_id, version, 0, 0);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t072-read-quarantine",
                            object_key,
                            object_id,
                            version,
                            "create-object-t072-read-quarantine",
                            payload.size(),
                            "etag-t072-read-quarantine"))
                        .Ok);

        ASSERT_EQ(store_a.WriteChunk(MakeWriteRequest(identity, payload, "write-replica-a")).status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(store_b.WriteChunk(MakeWriteRequest(identity, payload, "write-replica-b")).status,
                  storedemo::StorageNodeStatusCode::kOk);

        const auto tampered_path =
            ResolveFinalPathOrThrow(temp_dir.Path("stores") / "replica_a",
                                    identity.chunk_id);
        {
            std::ofstream output(tampered_path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(output.is_open());
            output << storedemo::test::MakeChunkPayload(payload.size(), "tampered-a");
        }

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t072-read-quarantine",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t072-read-quarantine",
                            payload.size(),
                            "etag-t072-read-quarantine",
                            {MakeChunkRef(identity, payload, {"replica-a", "replica-b"})}))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request)
            {
                if (node_id == "replica-a")
                {
                    return store_a.ReadChunk(request);
                }
                if (node_id == "replica-b")
                {
                    return store_b.ReadChunk(request);
                }

                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kNotFound;
                response.error_detail = "unknown replica";
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t072-read-quarantine",
                .object_key = object_key,
                .candidate_resolver =
                    [](const raftdemo::ChunkRef &chunk_ref)
                    {
                        return std::vector<storedemo::ReadReplicaCandidate>{
                            storedemo::ReadReplicaCandidate{
                                .node_id = chunk_ref.replica_nodes.at(0),
                                .has_observed_facts = true},
                            storedemo::ReadReplicaCandidate{
                                .node_id = chunk_ref.replica_nodes.at(1),
                                .has_observed_facts = true}};
                    }});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, payload);
        ASSERT_EQ(result.chunk_results.size(), 1U);
        ASSERT_EQ(result.chunk_results[0].attempts.size(), 2U);
        EXPECT_EQ(result.chunk_results[0].attempts[0].node_id, "replica-a");
        EXPECT_EQ(result.chunk_results[0].attempts[0].status,
                  storedemo::StorageNodeStatusCode::kCorrupted);
        EXPECT_EQ(result.chunk_results[0].selected_node_id, "replica-b");

        const auto replica_a_stat =
            store_a.StatChunk({.request_id = "stat-replica-a", .chunk_id = identity.chunk_id});
        ASSERT_EQ(replica_a_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(replica_a_stat.metadata.state, storedemo::ChunkState::kQuarantined);
#endif
    }

    TEST_F(StorageReadIntegrationTest,
           CommittedObjectPrefersFreshHealthyLowLoadReplicaFromRegistryFacts)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t066-read-priority",
                            "create-bucket-t066-read-priority"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t066-priority";
        const std::string object_key = "objects/priority.deb";
        const std::uint64_t version = 1;
        const auto identity = MakeStoreIdentityOrThrow(object_id, version, 0, 0);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t066-read-priority",
                            object_key,
                            object_id,
                            version,
                            "create-object-t066-read-priority",
                            fixture.payload.size(),
                            "etag-t066-read-priority"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t066-read-priority",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t066-read-priority",
                            fixture.payload.size(),
                            "etag-t066-read-priority",
                            {MakeChunkRef(identity,
                                          fixture.payload,
                                          {"replica-a", "replica-b", "replica-c"})}))
                        .Ok);

        storedemo::StorageNodeRegistry registry(
            storedemo::StorageNodeRegistryConfig{
                .stale_timeout_ms = 20,
                .dead_timeout_ms = 60});
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-a",
                         .endpoint = "127.0.0.1:7501",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kHealthy,
                             storedemo::StorageNodeDiskPressure::kLow,
                             8)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-b",
                         .endpoint = "127.0.0.1:7502",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kHealthy,
                             storedemo::StorageNodeDiskPressure::kLow,
                             1)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-c",
                         .endpoint = "127.0.0.1:7503",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kReadOnly,
                             storedemo::StorageNodeDiskPressure::kHigh,
                             0)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = fixture.payload;
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t066-read-priority",
                .object_key = object_key,
                .registry_snapshot_resolver =
                    [&registry]()
                    { return registry.Snapshot(110); }});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.read_calls(), 1U);
        ASSERT_EQ(reader.read_node_ids().size(), 1U);
        EXPECT_EQ(reader.read_node_ids().front(), "replica-b");
    }

    TEST_F(StorageReadIntegrationTest,
           RegistryFactsSkipStaleReplicaAndFallbackToNextHealthyReplicaAfterReadFailure)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t066-read-fallback",
                            "create-bucket-t066-read-fallback"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t066-fallback";
        const std::string object_key = "objects/fallback.deb";
        const std::uint64_t version = 1;
        const auto identity = MakeStoreIdentityOrThrow(object_id, version, 0, 0);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t066-read-fallback",
                            object_key,
                            object_id,
                            version,
                            "create-object-t066-read-fallback",
                            fixture.payload.size(),
                            "etag-t066-read-fallback"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t066-read-fallback",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t066-read-fallback",
                            fixture.payload.size(),
                            "etag-t066-read-fallback",
                            {MakeChunkRef(identity,
                                          fixture.payload,
                                          {"replica-a", "replica-b", "replica-c"})}))
                        .Ok);

        storedemo::StorageNodeRegistry registry(
            storedemo::StorageNodeRegistryConfig{
                .stale_timeout_ms = 20,
                .dead_timeout_ms = 60});
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-a",
                         .endpoint = "127.0.0.1:7601",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kHealthy,
                             storedemo::StorageNodeDiskPressure::kLow,
                             0)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-b",
                         .endpoint = "127.0.0.1:7602",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kHealthy,
                             storedemo::StorageNodeDiskPressure::kLow,
                             1)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.RegisterStorageNode(
                        {.node_id = "replica-c",
                         .endpoint = "127.0.0.1:7603",
                         .observed_at_unix_ms = 100,
                         .facts = MakeRegistryFactsForRead(
                             storedemo::StorageNodeHealth::kHealthy,
                             storedemo::StorageNodeDiskPressure::kLow,
                             2)})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.ReportLoad(
                        {.node_id = "replica-b",
                         .endpoint = "127.0.0.1:7602",
                         .sequence = 1,
                         .observed_at_unix_ms = 110,
                         .load = storedemo::StorageNodeRegistryLoadFacts{
                             .load = storedemo::StorageNodeLoadSnapshot{
                                 .active_reads = 0,
                                 .active_writes = 0,
                                 .queued_ops = 0},
                             .read_admission_overloaded = false}})
                .status,
            storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(
            registry.ReportLoad(
                        {.node_id = "replica-c",
                         .endpoint = "127.0.0.1:7603",
                         .sequence = 1,
                         .observed_at_unix_ms = 111,
                         .load = storedemo::StorageNodeRegistryLoadFacts{
                             .load = storedemo::StorageNodeLoadSnapshot{
                                 .active_reads = 2,
                                 .active_writes = 1,
                                 .queued_ops = 0},
                             .read_admission_overloaded = false}})
                .status,
            storedemo::StorageNodeStatusCode::kOk);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &node_id,
                const storedemo::ReadChunkRequest &request) -> storedemo::ReadChunkResponse
            {
                storedemo::ReadChunkResponse response;
                response.metadata.identity.chunk_id = request.chunk_id;
                response.metadata.node_id = node_id;
                response.metadata.size = request.expected_checksum.size_bytes;
                response.metadata.checksum = request.expected_checksum;
                response.metadata.state = storedemo::ChunkState::kLive;
                response.actual_checksum = request.expected_checksum;
                response.verified = true;
                if (node_id == "replica-b")
                {
                    response.status = storedemo::StorageNodeStatusCode::kTimeout;
                    response.error_detail = "replica-b timed out";
                    return response;
                }

                response.status = storedemo::StorageNodeStatusCode::kOk;
                response.payload = fixture.payload;
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t066-read-fallback",
                .object_key = object_key,
                .registry_snapshot_resolver =
                    [&registry]()
                    { return registry.Snapshot(125); }});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.payload, fixture.payload);
        EXPECT_EQ(reader.calls_for_node("replica-a"), 0U);
        EXPECT_EQ(reader.calls_for_node("replica-b"), 1U);
        EXPECT_EQ(reader.calls_for_node("replica-c"), 1U);
    }

    TEST_F(StorageReadIntegrationTest, EmptyReplicaNodesFailBeforeDataPlaneRead)
    {
        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t045-read-empty",
                            "create-bucket-t045-read-empty"))
                        .Ok);

        const auto fixture = storedemo::test::LoadUploadFixtureBinaryPayload();
        ASSERT_TRUE(fixture.used_repo_fixture);
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        const std::string object_id = "obj-t045-empty";
        const std::string object_key = "objects/empty-replicas.deb";
        const std::uint64_t version = 1;
        const auto identity = MakeStoreIdentityOrThrow(object_id, version, 0, 0);

        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCreateObjectCommandWithSizeVersion(
                            "bucket-t045-read-empty",
                            object_key,
                            object_id,
                            version,
                            "create-object-t045-read-empty",
                            fixture.payload.size(),
                            "etag-t045-read-empty"))
                        .Ok);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        storedemo::test::MakeCommitObjectCommandWithChunksVersion(
                            "bucket-t045-read-empty",
                            object_key,
                            object_id,
                            version,
                            "commit-object-t045-read-empty",
                            fixture.payload.size(),
                            "etag-t045-read-empty",
                            {MakeChunkRef(identity, fixture.payload, {})}))
                        .Ok);

        CountingReplicaReader reader(
            [&](const storedemo::StorageNodeId &,
                const storedemo::ReadChunkRequest &) -> storedemo::ReadChunkResponse
            {
                ADD_FAILURE() << "empty replica_nodes should fail before data-plane read";
                storedemo::ReadChunkResponse response;
                response.status = storedemo::StorageNodeStatusCode::kIoError;
                response.error_detail = "unexpected read";
                return response;
            });

        const auto result = ReadObjectByManifest(
            machine,
            reader,
            ReadObjectByManifestRequest{
                .bucket = "bucket-t045-read-empty",
                .object_key = object_key});

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kInvalidArgument);
        EXPECT_TRUE(result.payload.empty());
        EXPECT_EQ(reader.read_calls(), 0U);
    }

    TEST_F(StorageReadIntegrationTest,
           ProductionDownloadFallsBackWithinSameChunkManifestReplicaList)
    {
        ScopedManifestMetadataServer metadata_server;
        ScopedManifestViewServer view_server;
        view_server.service().SetMetadataEndpoint(metadata_server.address());

        const std::string cluster_id = "cluster-t007a-fallback";
        const std::string bucket = "bucket-t007a-fallback";
        const std::string object_key = "objects/fallback.bin";
        const std::string object_id = "obj-t007a-fallback";
        const std::uint64_t version = 7;
        const std::string payload =
            storedemo::test::MakeChunkPayload(24, "fb0") +
            storedemo::test::MakeChunkPayload(24, "fb1");

        const auto chunk0 = MakeStoreIdentityOrThrow(object_id, version, 0, 0);
        const auto chunk1 = MakeStoreIdentityOrThrow(object_id, version, 1, 24);
        const std::vector<raftdemo::ChunkRef> manifest{
            MakeChunkRef(chunk0,
                         payload.substr(0, 24),
                         {"replica-a", "replica-b"}),
            MakeChunkRef(chunk1,
                         payload.substr(24),
                         {"replica-a", "replica-b"})};
        metadata_server.service().SetCommittedObject(
            MakeCommittedObjectRecord(bucket, object_key, object_id, version, payload, manifest));

        view_server.service().SetStorageNodes(
            {
                MakeReadStorageSnapshot(cluster_id, "replica-a", "127.0.0.1:8501",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-b", "127.0.0.1:8502",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-x", "127.0.0.1:8599",
                                        128ULL * 1024ULL * 1024ULL),
            },
            1714002000000ULL);

        auto metadata_client =
            storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());
        auto storage_client = std::make_shared<RecordingReadStorageTransferClient>();
        storage_client->read_behavior =
            [&](const storedemo::StorageTransferReadRequest &request)
        {
            storedemo::StorageTransferReadResult result;
            result.target = request.target;
            result.metadata.identity = request.identity;
            result.metadata.node_id = request.target.node_id;
            result.metadata.size = request.expected_checksum.size_bytes;
            result.metadata.checksum = request.expected_checksum;
            result.metadata.state = storedemo::ChunkState::kLive;
            if (request.target.node_id == "replica-a")
            {
                result.status = storedemo::StorageNodeStatusCode::kNotFound;
                result.error_detail = "replica-a missing chunk";
                return result;
            }

            result.status = storedemo::StorageNodeStatusCode::kOk;
            result.payload = request.identity.chunk_index == 0
                                 ? payload.substr(0, 24)
                                 : payload.substr(24);
            result.actual_checksum = request.expected_checksum;
            result.verified = true;
            return result;
        };

        auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
            grpc::CreateChannel(view_server.address(),
                                grpc::InsecureChannelCredentials()),
            view_server.address());
        storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t007a_fallback");
        const auto destination_path = temp_dir.Path("download.bin");
        auto session = transfer.StartDownloadSession(
            storedemo::DownloadObjectRequest{
                .request_id = "download-t007a-fallback",
                .cluster_id = cluster_id,
                .bucket = bucket,
                .object_key = object_key,
                .object_id = object_id,
                .version = version,
                .destination_path = destination_path,
                .concurrency = 1});
        auto checksum_state = storedemo::CreateTransferChecksumState();
        const auto result = session->Execute(*checksum_state);

        ASSERT_TRUE(result.ok()) << result.error_detail;
        EXPECT_TRUE(result.checksum_verified);
        EXPECT_EQ(ReadBinaryFileOrThrow(destination_path), payload);
        ASSERT_EQ(storage_client->reads.size(), 4U);
        EXPECT_EQ(storage_client->reads[0].target.node_id, "replica-a");
        EXPECT_EQ(storage_client->reads[1].target.node_id, "replica-b");
        EXPECT_EQ(storage_client->reads[2].target.node_id, "replica-a");
        EXPECT_EQ(storage_client->reads[3].target.node_id, "replica-b");
    }

    TEST_F(StorageReadIntegrationTest,
           ProductionDownloadUsesPerChunkManifestReplicaSetsOnly)
    {
        ScopedManifestMetadataServer metadata_server;
        ScopedManifestViewServer view_server;
        view_server.service().SetMetadataEndpoint(metadata_server.address());

        const std::string cluster_id = "cluster-t007a-per-chunk";
        const std::string bucket = "bucket-t007a-per-chunk";
        const std::string object_key = "objects/per-chunk.bin";
        const std::string object_id = "obj-t007a-per-chunk";
        const std::uint64_t version = 8;
        const std::string payload =
            storedemo::test::MakeChunkPayload(20, "ca") +
            storedemo::test::MakeChunkPayload(20, "cb");

        const auto chunk0 = MakeStoreIdentityOrThrow(object_id, version, 0, 0);
        const auto chunk1 = MakeStoreIdentityOrThrow(object_id, version, 1, 20);
        const std::vector<raftdemo::ChunkRef> manifest{
            MakeChunkRef(chunk0,
                         payload.substr(0, 20),
                         {"replica-a", "replica-b"}),
            MakeChunkRef(chunk1,
                         payload.substr(20),
                         {"replica-c", "replica-d"})};
        metadata_server.service().SetCommittedObject(
            MakeCommittedObjectRecord(bucket, object_key, object_id, version, payload, manifest));

        view_server.service().SetStorageNodes(
            {
                MakeReadStorageSnapshot(cluster_id, "replica-a", "127.0.0.1:8601",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-b", "127.0.0.1:8602",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-c", "127.0.0.1:8603",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-d", "127.0.0.1:8604",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-z", "127.0.0.1:8699",
                                        128ULL * 1024ULL * 1024ULL),
            },
            1714002000000ULL);

        auto metadata_client =
            storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());
        auto storage_client = std::make_shared<RecordingReadStorageTransferClient>();
        storage_client->read_behavior =
            [&](const storedemo::StorageTransferReadRequest &request)
        {
            storedemo::StorageTransferReadResult result;
            result.status = storedemo::StorageNodeStatusCode::kOk;
            result.target = request.target;
            result.metadata.identity = request.identity;
            result.metadata.node_id = request.target.node_id;
            result.metadata.size = request.expected_checksum.size_bytes;
            result.metadata.checksum = request.expected_checksum;
            result.metadata.state = storedemo::ChunkState::kLive;
            result.payload = request.identity.chunk_index == 0
                                 ? payload.substr(0, 20)
                                 : payload.substr(20);
            result.actual_checksum = request.expected_checksum;
            result.verified = true;
            return result;
        };

        auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
            grpc::CreateChannel(view_server.address(),
                                grpc::InsecureChannelCredentials()),
            view_server.address());
        storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t007a_per_chunk");
        const auto destination_path = temp_dir.Path("download.bin");
        auto session = transfer.StartDownloadSession(
            storedemo::DownloadObjectRequest{
                .request_id = "download-t007a-per-chunk",
                .cluster_id = cluster_id,
                .bucket = bucket,
                .object_key = object_key,
                .object_id = object_id,
                .version = version,
                .destination_path = destination_path,
                .concurrency = 1});
        auto checksum_state = storedemo::CreateTransferChecksumState();
        const auto result = session->Execute(*checksum_state);

        ASSERT_TRUE(result.ok()) << result.error_detail;
        ASSERT_EQ(storage_client->reads.size(), 2U);
        EXPECT_EQ(storage_client->reads[0].identity.chunk_index, 0U);
        EXPECT_EQ(storage_client->reads[0].target.node_id, "replica-a");
        EXPECT_EQ(storage_client->reads[1].identity.chunk_index, 1U);
        EXPECT_EQ(storage_client->reads[1].target.node_id, "replica-c");
    }

    TEST_F(StorageReadIntegrationTest,
           ProductionDownloadDoesNotAttemptManifestExternalReadableNodes)
    {
        ScopedManifestMetadataServer metadata_server;
        ScopedManifestViewServer view_server;
        view_server.service().SetMetadataEndpoint(metadata_server.address());

        const std::string cluster_id = "cluster-t007a-no-external";
        const std::string bucket = "bucket-t007a-no-external";
        const std::string object_key = "objects/no-external.bin";
        const std::string object_id = "obj-t007a-no-external";
        const std::uint64_t version = 9;
        const std::string payload = storedemo::test::MakeChunkPayload(28, "ne");

        const auto chunk0 = MakeStoreIdentityOrThrow(object_id, version, 0, 0);
        const std::vector<raftdemo::ChunkRef> manifest{
            MakeChunkRef(chunk0, payload, {"replica-a", "replica-b"})};
        metadata_server.service().SetCommittedObject(
            MakeCommittedObjectRecord(bucket, object_key, object_id, version, payload, manifest));

        view_server.service().SetStorageNodes(
            {
                MakeReadStorageSnapshot(cluster_id, "replica-a", "127.0.0.1:8701",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-b", "127.0.0.1:8702",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-extra", "127.0.0.1:8799",
                                        128ULL * 1024ULL * 1024ULL),
            },
            1714002000000ULL);

        auto metadata_client =
            storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());
        auto storage_client = std::make_shared<RecordingReadStorageTransferClient>();
        storage_client->read_behavior =
            [&](const storedemo::StorageTransferReadRequest &request)
        {
            EXPECT_NE(request.target.node_id, "replica-extra");
            storedemo::StorageTransferReadResult result;
            result.target = request.target;
            result.metadata.identity = request.identity;
            result.metadata.node_id = request.target.node_id;
            result.metadata.size = request.expected_checksum.size_bytes;
            result.metadata.checksum = request.expected_checksum;
            result.metadata.state = storedemo::ChunkState::kLive;
            if (request.target.node_id == "replica-a")
            {
                result.status = storedemo::StorageNodeStatusCode::kTimeout;
                result.error_detail = "replica-a timed out";
                return result;
            }

            result.status = storedemo::StorageNodeStatusCode::kOk;
            result.payload = payload;
            result.actual_checksum = request.expected_checksum;
            result.verified = true;
            return result;
        };

        auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
            grpc::CreateChannel(view_server.address(),
                                grpc::InsecureChannelCredentials()),
            view_server.address());
        storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t007a_no_external");
        const auto destination_path = temp_dir.Path("download.bin");
        auto session = transfer.StartDownloadSession(
            storedemo::DownloadObjectRequest{
                .request_id = "download-t007a-no-external",
                .cluster_id = cluster_id,
                .bucket = bucket,
                .object_key = object_key,
                .object_id = object_id,
                .version = version,
                .destination_path = destination_path,
                .concurrency = 1});
        auto checksum_state = storedemo::CreateTransferChecksumState();
        const auto result = session->Execute(*checksum_state);

        ASSERT_TRUE(result.ok()) << result.error_detail;
        ASSERT_EQ(storage_client->reads.size(), 2U);
        for (const auto &read : storage_client->reads)
        {
            EXPECT_NE(read.target.node_id, "replica-extra");
        }
    }

    TEST_F(StorageReadIntegrationTest,
           ProductionDownloadAllowsNeutralFallbackWhenObservedFactsAreMissing)
    {
        ScopedManifestMetadataServer metadata_server;
        ScopedManifestViewServer view_server;
        view_server.service().SetMetadataEndpoint(metadata_server.address());

        const std::string cluster_id = "cluster-t007a-neutral";
        const std::string bucket = "bucket-t007a-neutral";
        const std::string object_key = "objects/neutral.bin";
        const std::string object_id = "obj-t007a-neutral";
        const std::uint64_t version = 10;
        const std::string payload = storedemo::test::MakeChunkPayload(30, "nf");

        const auto chunk0 = MakeStoreIdentityOrThrow(object_id, version, 0, 0);
        const std::vector<raftdemo::ChunkRef> manifest{
            MakeChunkRef(chunk0, payload, {"replica-a", "replica-b"})};
        metadata_server.service().SetCommittedObject(
            MakeCommittedObjectRecord(bucket, object_key, object_id, version, payload, manifest));

        view_server.service().SetStorageNodes(
            {
                MakeReadStorageSnapshot(cluster_id, "replica-a", "127.0.0.1:8801",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-b", "127.0.0.1:8802",
                                        128ULL * 1024ULL * 1024ULL),
            },
            1714002000000ULL);

        auto metadata_client =
            storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());
        auto storage_client = std::make_shared<RecordingReadStorageTransferClient>();
        storage_client->read_behavior =
            [&](const storedemo::StorageTransferReadRequest &request)
        {
            storedemo::StorageTransferReadResult result;
            result.target = request.target;
            result.metadata.identity = request.identity;
            result.metadata.node_id = request.target.node_id;
            result.metadata.size = request.expected_checksum.size_bytes;
            result.metadata.checksum = request.expected_checksum;
            result.metadata.state = storedemo::ChunkState::kLive;
            if (request.target.node_id == "replica-a")
            {
                result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                result.error_detail = "replica-a unavailable";
                return result;
            }

            result.status = storedemo::StorageNodeStatusCode::kOk;
            result.payload = payload;
            result.actual_checksum = request.expected_checksum;
            result.verified = true;
            return result;
        };

        auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
            grpc::CreateChannel(view_server.address(),
                                grpc::InsecureChannelCredentials()),
            view_server.address());
        storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t007a_neutral");
        const auto destination_path = temp_dir.Path("download.bin");
        auto session = transfer.StartDownloadSession(
            storedemo::DownloadObjectRequest{
                .request_id = "download-t007a-neutral",
                .cluster_id = cluster_id,
                .bucket = bucket,
                .object_key = object_key,
                .object_id = object_id,
                .version = version,
                .destination_path = destination_path,
                .concurrency = 1});
        auto checksum_state = storedemo::CreateTransferChecksumState();
        const auto result = session->Execute(*checksum_state);

        ASSERT_TRUE(result.ok()) << result.error_detail;
        ASSERT_EQ(storage_client->reads.size(), 2U);
        EXPECT_EQ(storage_client->reads[0].target.node_id, "replica-a");
        EXPECT_EQ(storage_client->reads[1].target.node_id, "replica-b");
    }

    TEST_F(StorageReadIntegrationTest,
           ProductionDownloadFallsBackAfterChunkChecksumMismatch)
    {
        ScopedManifestMetadataServer metadata_server;
        ScopedManifestViewServer view_server;
        view_server.service().SetMetadataEndpoint(metadata_server.address());

        const std::string cluster_id = "cluster-t007b-checksum-fallback";
        const std::string bucket = "bucket-t007b-checksum-fallback";
        const std::string object_key = "objects/checksum-fallback.bin";
        const std::string object_id = "obj-t007b-checksum-fallback";
        const std::uint64_t version = 11;
        const std::string payload = storedemo::test::MakeChunkPayload(32, "cs");
        const std::string corrupted_payload =
            storedemo::test::MakeChunkPayload(32, "bad");

        const auto chunk0 = MakeStoreIdentityOrThrow(object_id, version, 0, 0);
        const std::vector<raftdemo::ChunkRef> manifest{
            MakeChunkRef(chunk0, payload, {"replica-a", "replica-b"})};
        metadata_server.service().SetCommittedObject(
            MakeCommittedObjectRecord(bucket, object_key, object_id, version, payload, manifest));

        view_server.service().SetStorageNodes(
            {
                MakeReadStorageSnapshot(cluster_id, "replica-a", "127.0.0.1:8901",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-b", "127.0.0.1:8902",
                                        128ULL * 1024ULL * 1024ULL),
            },
            1714002000000ULL);

        auto metadata_client =
            storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());
        auto storage_client = std::make_shared<RecordingReadStorageTransferClient>();
        storage_client->read_behavior =
            [&](const storedemo::StorageTransferReadRequest &request)
        {
            storedemo::StorageTransferReadResult result;
            result.status = storedemo::StorageNodeStatusCode::kOk;
            result.target = request.target;
            result.metadata.identity = request.identity;
            result.metadata.node_id = request.target.node_id;
            result.metadata.size = request.expected_checksum.size_bytes;
            result.metadata.checksum = request.expected_checksum;
            result.metadata.state = storedemo::ChunkState::kLive;
            if (request.target.node_id == "replica-a")
            {
                result.payload = corrupted_payload;
                result.actual_checksum =
                    storedemo::test::ComputeStoreChecksumOrThrow(corrupted_payload);
                result.verified = false;
                return result;
            }

            result.payload = payload;
            result.actual_checksum = request.expected_checksum;
            result.verified = true;
            return result;
        };

        auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
            grpc::CreateChannel(view_server.address(),
                                grpc::InsecureChannelCredentials()),
            view_server.address());
        storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t007b_checksum");
        const auto destination_path = temp_dir.Path("download.bin");
        auto session = transfer.StartDownloadSession(
            storedemo::DownloadObjectRequest{
                .request_id = "download-t007b-checksum",
                .cluster_id = cluster_id,
                .bucket = bucket,
                .object_key = object_key,
                .object_id = object_id,
                .version = version,
                .destination_path = destination_path,
                .concurrency = 1});
        auto checksum_state = storedemo::CreateTransferChecksumState();
        const auto result = session->Execute(*checksum_state);

        ASSERT_TRUE(result.ok()) << result.error_detail;
        EXPECT_TRUE(result.checksum_verified);
        EXPECT_EQ(ReadBinaryFileOrThrow(destination_path), payload);
        ASSERT_EQ(storage_client->reads.size(), 2U);
        EXPECT_EQ(storage_client->reads[0].target.node_id, "replica-a");
        EXPECT_EQ(storage_client->reads[1].target.node_id, "replica-b");
        EXPECT_TRUE(ResultHasDiagnosticContaining(result, "checksum mismatch", "replica-a"));
        EXPECT_EQ(storage_client->write_calls, 0U);
    }

    TEST_F(StorageReadIntegrationTest,
           ProductionDownloadFallsBackAfterChunkSizeMismatch)
    {
        ScopedManifestMetadataServer metadata_server;
        ScopedManifestViewServer view_server;
        view_server.service().SetMetadataEndpoint(metadata_server.address());

        const std::string cluster_id = "cluster-t007b-size-fallback";
        const std::string bucket = "bucket-t007b-size-fallback";
        const std::string object_key = "objects/size-fallback.bin";
        const std::string object_id = "obj-t007b-size-fallback";
        const std::uint64_t version = 12;
        const std::string payload = storedemo::test::MakeChunkPayload(36, "sz");
        const std::string truncated_payload = payload.substr(0, payload.size() - 5U);

        const auto chunk0 = MakeStoreIdentityOrThrow(object_id, version, 0, 0);
        const std::vector<raftdemo::ChunkRef> manifest{
            MakeChunkRef(chunk0, payload, {"replica-a", "replica-b"})};
        metadata_server.service().SetCommittedObject(
            MakeCommittedObjectRecord(bucket, object_key, object_id, version, payload, manifest));

        view_server.service().SetStorageNodes(
            {
                MakeReadStorageSnapshot(cluster_id, "replica-a", "127.0.0.1:8911",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-b", "127.0.0.1:8912",
                                        128ULL * 1024ULL * 1024ULL),
            },
            1714002000000ULL);

        auto metadata_client =
            storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());
        auto storage_client = std::make_shared<RecordingReadStorageTransferClient>();
        storage_client->read_behavior =
            [&](const storedemo::StorageTransferReadRequest &request)
        {
            storedemo::StorageTransferReadResult result;
            result.status = storedemo::StorageNodeStatusCode::kOk;
            result.target = request.target;
            result.metadata.identity = request.identity;
            result.metadata.node_id = request.target.node_id;
            result.metadata.size = request.expected_checksum.size_bytes;
            result.metadata.checksum = request.expected_checksum;
            result.metadata.state = storedemo::ChunkState::kLive;
            if (request.target.node_id == "replica-a")
            {
                result.payload = truncated_payload;
                result.actual_checksum =
                    storedemo::test::ComputeStoreChecksumOrThrow(truncated_payload);
                result.verified = false;
                return result;
            }

            result.payload = payload;
            result.actual_checksum = request.expected_checksum;
            result.verified = true;
            return result;
        };

        auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
            grpc::CreateChannel(view_server.address(),
                                grpc::InsecureChannelCredentials()),
            view_server.address());
        storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t007b_size");
        const auto destination_path = temp_dir.Path("download.bin");
        auto session = transfer.StartDownloadSession(
            storedemo::DownloadObjectRequest{
                .request_id = "download-t007b-size",
                .cluster_id = cluster_id,
                .bucket = bucket,
                .object_key = object_key,
                .object_id = object_id,
                .version = version,
                .destination_path = destination_path,
                .concurrency = 1});
        auto checksum_state = storedemo::CreateTransferChecksumState();
        const auto result = session->Execute(*checksum_state);

        ASSERT_TRUE(result.ok()) << result.error_detail;
        EXPECT_EQ(ReadBinaryFileOrThrow(destination_path), payload);
        ASSERT_EQ(storage_client->reads.size(), 2U);
        EXPECT_TRUE(ResultHasDiagnosticContaining(result, "size mismatch", "replica-a"));
        EXPECT_EQ(storage_client->write_calls, 0U);
    }

    TEST_F(StorageReadIntegrationTest,
           ProductionDownloadAggregatesAllManifestReplicaFailuresAndCleansOutput)
    {
        ScopedManifestMetadataServer metadata_server;
        ScopedManifestViewServer view_server;
        view_server.service().SetMetadataEndpoint(metadata_server.address());

        const std::string cluster_id = "cluster-t007b-all-fail";
        const std::string bucket = "bucket-t007b-all-fail";
        const std::string object_key = "objects/all-fail.bin";
        const std::string object_id = "obj-t007b-all-fail";
        const std::uint64_t version = 13;
        const std::string payload = storedemo::test::MakeChunkPayload(40, "af");
        const std::string corrupted_payload =
            storedemo::test::MakeChunkPayload(40, "bad");

        const auto chunk0 = MakeStoreIdentityOrThrow(object_id, version, 0, 0);
        const std::vector<raftdemo::ChunkRef> manifest{
            MakeChunkRef(chunk0,
                         payload,
                         {"replica-a", "replica-b", "replica-c"})};
        metadata_server.service().SetCommittedObject(
            MakeCommittedObjectRecord(bucket, object_key, object_id, version, payload, manifest));

        view_server.service().SetStorageNodes(
            {
                MakeReadStorageSnapshot(cluster_id, "replica-a", "127.0.0.1:8921",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-b", "127.0.0.1:8922",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-c", "127.0.0.1:8923",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-extra", "127.0.0.1:8999",
                                        128ULL * 1024ULL * 1024ULL),
            },
            1714002000000ULL);

        auto metadata_client =
            storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());
        auto storage_client = std::make_shared<RecordingReadStorageTransferClient>();
        storage_client->read_behavior =
            [&](const storedemo::StorageTransferReadRequest &request)
        {
            EXPECT_NE(request.target.node_id, "replica-extra");

            storedemo::StorageTransferReadResult result;
            result.target = request.target;
            result.metadata.identity = request.identity;
            result.metadata.node_id = request.target.node_id;
            result.metadata.size = request.expected_checksum.size_bytes;
            result.metadata.checksum = request.expected_checksum;
            result.metadata.state = storedemo::ChunkState::kLive;
            if (request.target.node_id == "replica-a")
            {
                result.status = storedemo::StorageNodeStatusCode::kTimeout;
                result.error_detail = "replica-a timed out";
                result.retryable = true;
                return result;
            }
            if (request.target.node_id == "replica-b")
            {
                result.status = storedemo::StorageNodeStatusCode::kNotFound;
                result.error_detail = "replica-b missing chunk";
                return result;
            }

            result.status = storedemo::StorageNodeStatusCode::kOk;
            result.payload = corrupted_payload;
            result.actual_checksum =
                storedemo::test::ComputeStoreChecksumOrThrow(corrupted_payload);
            result.verified = false;
            return result;
        };

        auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
            grpc::CreateChannel(view_server.address(),
                                grpc::InsecureChannelCredentials()),
            view_server.address());
        storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t007b_all_fail");
        const auto destination_path = temp_dir.Path("download.bin");
        const auto temp_output_path = MakeExpectedDownloadTempPath(
            destination_path,
            "download-t007b-all-fail");
        auto session = transfer.StartDownloadSession(
            storedemo::DownloadObjectRequest{
                .request_id = "download-t007b-all-fail",
                .cluster_id = cluster_id,
                .bucket = bucket,
                .object_key = object_key,
                .object_id = object_id,
                .version = version,
                .destination_path = destination_path,
                .concurrency = 1});
        auto checksum_state = storedemo::CreateTransferChecksumState();
        const auto result = session->Execute(*checksum_state);

        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.status, storedemo::ObjectTransferStatusCode::kChecksumMismatch);
        EXPECT_NE(result.error_detail.find("chunk 0"), std::string::npos);
        EXPECT_NE(result.error_detail.find("replica-a"), std::string::npos);
        EXPECT_NE(result.error_detail.find("replica-b"), std::string::npos);
        EXPECT_NE(result.error_detail.find("replica-c"), std::string::npos);
        EXPECT_NE(result.error_detail.find("timeout"), std::string::npos);
        EXPECT_NE(result.error_detail.find("missing"), std::string::npos);
        EXPECT_NE(result.error_detail.find("checksum mismatch"), std::string::npos);
        EXPECT_TRUE(ResultHasDiagnosticContaining(result, "timeout", "replica-a"));
        EXPECT_TRUE(ResultHasDiagnosticContaining(result, "missing", "replica-b"));
        EXPECT_TRUE(ResultHasDiagnosticContaining(result, "checksum mismatch",
                                                  "replica-c"));
        EXPECT_EQ(storage_client->reads.size(), 3U);
        EXPECT_EQ(storage_client->write_calls, 0U);
        EXPECT_FALSE(std::filesystem::exists(destination_path));
        EXPECT_FALSE(std::filesystem::exists(temp_output_path));
    }

    TEST_F(StorageReadIntegrationTest,
           ProductionDownloadRejectsFinalObjectChecksumMismatchWithoutPublishingOutput)
    {
        ScopedManifestMetadataServer metadata_server;
        ScopedManifestViewServer view_server;
        view_server.service().SetMetadataEndpoint(metadata_server.address());

        const std::string cluster_id = "cluster-t007b-object-checksum";
        const std::string bucket = "bucket-t007b-object-checksum";
        const std::string object_key = "objects/object-checksum.bin";
        const std::string object_id = "obj-t007b-object-checksum";
        const std::uint64_t version = 14;
        const std::string payload =
            storedemo::test::MakeChunkPayload(18, "oc0") +
            storedemo::test::MakeChunkPayload(18, "oc1");

        const auto chunk0 = MakeStoreIdentityOrThrow(object_id, version, 0, 0);
        const auto chunk1 = MakeStoreIdentityOrThrow(object_id, version, 1, 18);
        const std::vector<raftdemo::ChunkRef> manifest{
            MakeChunkRef(chunk0, payload.substr(0, 18), {"replica-a"}),
            MakeChunkRef(chunk1, payload.substr(18), {"replica-b"})};
        auto object = MakeCommittedObjectRecord(
            bucket, object_key, object_id, version, payload, manifest);
        object.set_etag(std::string(64, '0'));
        metadata_server.service().SetCommittedObject(object);

        view_server.service().SetStorageNodes(
            {
                MakeReadStorageSnapshot(cluster_id, "replica-a", "127.0.0.1:8931",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-b", "127.0.0.1:8932",
                                        128ULL * 1024ULL * 1024ULL),
            },
            1714002000000ULL);

        auto metadata_client =
            storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());
        auto storage_client = std::make_shared<RecordingReadStorageTransferClient>();
        storage_client->read_behavior =
            [&](const storedemo::StorageTransferReadRequest &request)
        {
            storedemo::StorageTransferReadResult result;
            result.status = storedemo::StorageNodeStatusCode::kOk;
            result.target = request.target;
            result.metadata.identity = request.identity;
            result.metadata.node_id = request.target.node_id;
            result.metadata.size = request.expected_checksum.size_bytes;
            result.metadata.checksum = request.expected_checksum;
            result.metadata.state = storedemo::ChunkState::kLive;
            result.payload = request.identity.chunk_index == 0
                                 ? payload.substr(0, 18)
                                 : payload.substr(18);
            result.actual_checksum = request.expected_checksum;
            result.verified = true;
            return result;
        };

        auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
            grpc::CreateChannel(view_server.address(),
                                grpc::InsecureChannelCredentials()),
            view_server.address());
        storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t007b_object_checksum");
        const auto destination_path = temp_dir.Path("download.bin");
        const auto temp_output_path = MakeExpectedDownloadTempPath(
            destination_path,
            "download-t007b-object-checksum");
        auto session = transfer.StartDownloadSession(
            storedemo::DownloadObjectRequest{
                .request_id = "download-t007b-object-checksum",
                .cluster_id = cluster_id,
                .bucket = bucket,
                .object_key = object_key,
                .object_id = object_id,
                .version = version,
                .destination_path = destination_path,
                .concurrency = 1});
        auto checksum_state = storedemo::CreateTransferChecksumState();
        const auto result = session->Execute(*checksum_state);

        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.status, storedemo::ObjectTransferStatusCode::kChecksumMismatch);
        EXPECT_FALSE(std::filesystem::exists(destination_path));
        EXPECT_FALSE(std::filesystem::exists(temp_output_path));
        EXPECT_EQ(storage_client->write_calls, 0U);
    }

    TEST_F(StorageReadIntegrationTest,
           ProductionDownloadRetainsRepairReadyDiagnosticsWithoutRepairWrites)
    {
        ScopedManifestMetadataServer metadata_server;
        ScopedManifestViewServer view_server;
        view_server.service().SetMetadataEndpoint(metadata_server.address());

        const std::string cluster_id = "cluster-t007b-repair-ready";
        const std::string bucket = "bucket-t007b-repair-ready";
        const std::string object_key = "objects/repair-ready.bin";
        const std::string object_id = "obj-t007b-repair-ready";
        const std::uint64_t version = 15;
        const std::string payload = storedemo::test::MakeChunkPayload(34, "rr");

        const auto chunk0 = MakeStoreIdentityOrThrow(object_id, version, 0, 0);
        const std::vector<raftdemo::ChunkRef> manifest{
            MakeChunkRef(chunk0, payload, {"replica-a", "replica-b"})};
        metadata_server.service().SetCommittedObject(
            MakeCommittedObjectRecord(bucket, object_key, object_id, version, payload, manifest));

        view_server.service().SetStorageNodes(
            {
                MakeReadStorageSnapshot(cluster_id, "replica-a", "127.0.0.1:8941",
                                        128ULL * 1024ULL * 1024ULL),
                MakeReadStorageSnapshot(cluster_id, "replica-b", "127.0.0.1:8942",
                                        128ULL * 1024ULL * 1024ULL),
            },
            1714002000000ULL);

        auto metadata_client =
            storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());
        auto storage_client = std::make_shared<RecordingReadStorageTransferClient>();
        storage_client->read_behavior =
            [&](const storedemo::StorageTransferReadRequest &request)
        {
            storedemo::StorageTransferReadResult result;
            result.target = request.target;
            result.metadata.identity = request.identity;
            result.metadata.node_id = request.target.node_id;
            result.metadata.size = request.expected_checksum.size_bytes;
            result.metadata.checksum = request.expected_checksum;
            if (request.target.node_id == "replica-a")
            {
                result.status = storedemo::StorageNodeStatusCode::kNotFound;
                result.error_detail = "replica-a missing chunk";
                return result;
            }

            result.status = storedemo::StorageNodeStatusCode::kCorrupted;
            result.error_detail = "replica-b corrupted payload";
            result.metadata.state = storedemo::ChunkState::kCorrupted;
            return result;
        };

        auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
            grpc::CreateChannel(view_server.address(),
                                grpc::InsecureChannelCredentials()),
            view_server.address());
        storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_read_t007b_repair_ready");
        const auto destination_path = temp_dir.Path("download.bin");
        auto session = transfer.StartDownloadSession(
            storedemo::DownloadObjectRequest{
                .request_id = "download-t007b-repair-ready",
                .cluster_id = cluster_id,
                .bucket = bucket,
                .object_key = object_key,
                .object_id = object_id,
                .version = version,
                .destination_path = destination_path,
                .concurrency = 1});
        auto checksum_state = storedemo::CreateTransferChecksumState();
        const auto result = session->Execute(*checksum_state);

        EXPECT_FALSE(result.ok());
        EXPECT_TRUE(ResultHasDiagnosticContaining(result, "missing", "replica-a"));
        EXPECT_TRUE(ResultHasDiagnosticContaining(result, "corruption", "replica-b"));
        EXPECT_EQ(storage_client->write_calls, 0U);
        EXPECT_FALSE(std::filesystem::exists(destination_path));
    }
} // namespace
