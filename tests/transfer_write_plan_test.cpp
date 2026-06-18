#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "metadata.grpc.pb.h"
#include "store/transfer/metadata_transfer_client.h"
#include "store/transfer/object_transfer.h"
#include "store/transfer/storage_transfer_client.h"
#include "support/store_test_utils.h"
#include "view/view_client.h"
#include "view.grpc.pb.h"

namespace storedemo
{
    std::vector<StorageTransferTarget> ResolveSelectedChunkTargetsForTesting(
        const TransferChunkPlan &chunk_plan,
        const std::unordered_map<StorageNodeId, StorageTransferTarget> &storage_targets,
        std::string *error_detail);
}

namespace
{
    storedemo::ChunkChecksum ComputeChecksumOrThrow(const std::string_view payload)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute checksum: " + error_detail);
        }
        return checksum;
    }

    storedemo::TransferObjectChecksumFacts MakeObjectChecksumFacts(
        const std::string_view payload)
    {
        const auto checksum = ComputeChecksumOrThrow(payload);
        storedemo::TransferObjectChecksumFacts facts;
        facts.size = static_cast<std::uint64_t>(payload.size());
        facts.checksum = checksum;
        facts.etag = checksum.value;
        return facts;
    }

    std::filesystem::path WritePayloadFile(const std::filesystem::path &root,
                                           const std::string &payload)
    {
        const auto path = root / "upload.bin";
        std::ofstream output(path, std::ios::binary);
        output.write(payload.data(),
                     static_cast<std::streamsize>(payload.size()));
        output.close();
        if (!output)
        {
            throw std::runtime_error("failed to write upload payload file");
        }
        return path;
    }

    class FakeMetadataService final : public raft::MetadataService::Service
    {
    public:
        grpc::Status CreateObject(grpc::ServerContext *,
                                  const raft::CreateObjectRequest *request,
                                  raft::CreateObjectResponse *response) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++create_calls_;
            last_create_request_ = *request;

            response->mutable_summary()->set_code(raft::METADATA_STATUS_CODE_OK);
            response->mutable_summary()->set_message("ok");
            response->mutable_summary()->set_request_id(request->request_id());
            response->mutable_summary()->set_bucket(request->bucket());
            response->mutable_summary()->set_object_key(request->object_key());
            response->mutable_summary()->set_object_id(request->object_id());
            response->mutable_summary()->set_state(raft::METADATA_OBJECT_STATE_PENDING);
            response->mutable_summary()->set_term(7);
            response->mutable_summary()->set_log_index(11);
            response->mutable_summary()->mutable_leader_hint()->set_leader_id(1);
            response->mutable_summary()->mutable_leader_hint()->set_leader_address(
                leader_address_);

            auto *object = response->mutable_object();
            object->set_bucket(request->bucket());
            object->set_object_key(request->object_key());
            object->set_object_id(request->object_id());
            object->set_version(next_version_);
            object->set_size(request->size());
            object->set_etag(request->etag());
            object->set_state(raft::METADATA_OBJECT_STATE_PENDING);
            object->set_create_time(created_at_unix_ms_);
            return grpc::Status::OK;
        }

        grpc::Status CommitObject(grpc::ServerContext *,
                                  const raft::CommitObjectRequest *request,
                                  raft::CommitObjectResponse *response) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++commit_calls_;
            last_commit_request_ = *request;

            response->mutable_summary()->set_code(raft::METADATA_STATUS_CODE_OK);
            response->mutable_summary()->set_message("ok");
            response->mutable_summary()->set_request_id(request->request_id());
            response->mutable_summary()->set_bucket(request->bucket());
            response->mutable_summary()->set_object_key(request->object_key());
            response->mutable_summary()->set_object_id(request->object_id());
            response->mutable_summary()->set_state(
                raft::METADATA_OBJECT_STATE_COMMITTED);
            response->mutable_summary()->set_term(7);
            response->mutable_summary()->set_log_index(12);
            response->mutable_summary()->mutable_leader_hint()->set_leader_id(1);
            response->mutable_summary()->mutable_leader_hint()->set_leader_address(
                leader_address_);

            auto *object = response->mutable_object();
            object->set_bucket(request->bucket());
            object->set_object_key(request->object_key());
            object->set_object_id(request->object_id());
            object->set_version(request->version());
            object->set_size(request->size());
            object->set_etag(request->etag());
            object->set_state(raft::METADATA_OBJECT_STATE_COMMITTED);
            object->set_create_time(created_at_unix_ms_);
            object->set_commit_time(commit_at_unix_ms_);
            for (const auto &chunk : request->chunks())
            {
                object->add_chunks()->CopyFrom(chunk);
            }
            return grpc::Status::OK;
        }

        void SetLeaderAddress(std::string leader_address)
        {
            std::lock_guard<std::mutex> lock(mu_);
            leader_address_ = std::move(leader_address);
        }

        [[nodiscard]] std::size_t create_calls() const
        {
            std::lock_guard<std::mutex> lock(mu_);
            return create_calls_;
        }

        [[nodiscard]] std::size_t commit_calls() const
        {
            std::lock_guard<std::mutex> lock(mu_);
            return commit_calls_;
        }

        [[nodiscard]] std::optional<raft::CreateObjectRequest> last_create_request() const
        {
            std::lock_guard<std::mutex> lock(mu_);
            return last_create_request_;
        }

        [[nodiscard]] std::optional<raft::CommitObjectRequest> last_commit_request() const
        {
            std::lock_guard<std::mutex> lock(mu_);
            return last_commit_request_;
        }

        [[nodiscard]] std::uint64_t created_at_unix_ms() const
        {
            return created_at_unix_ms_;
        }

    private:
        mutable std::mutex mu_;
        std::string leader_address_;
        std::uint64_t next_version_{7};
        std::uint64_t created_at_unix_ms_{1714000000000ULL};
        std::uint64_t commit_at_unix_ms_{1714000001234ULL};
        std::size_t create_calls_{0};
        std::size_t commit_calls_{0};
        std::optional<raft::CreateObjectRequest> last_create_request_;
        std::optional<raft::CommitObjectRequest> last_commit_request_;
    };

    class ScopedFakeMetadataServer
    {
    public:
        ScopedFakeMetadataServer()
        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("127.0.0.1:0",
                                     grpc::InsecureServerCredentials(),
                                     &selected_port_);
            builder.RegisterService(&service_);
            server_ = builder.BuildAndStart();
            if (server_ == nullptr || selected_port_ <= 0)
            {
                throw std::runtime_error("failed to start fake metadata server");
            }
            address_ = "127.0.0.1:" + std::to_string(selected_port_);
            service_.SetLeaderAddress(address_);
        }

        ~ScopedFakeMetadataServer()
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

        [[nodiscard]] FakeMetadataService &service()
        {
            return service_;
        }

    private:
        int selected_port_{0};
        std::string address_;
        FakeMetadataService service_;
        std::unique_ptr<grpc::Server> server_;
    };

    class FakeViewNodeService final : public view::ViewNodeService::Service
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
            response->mutable_summary()->set_node_id("view-test-1");
            response->set_observed_at_unix_ms(observed_at_unix_ms_);
            response->set_membership_epoch(5);
            response->mutable_leader_hint()->set_node_id("meta-test-1");
            response->mutable_leader_hint()->set_endpoint(metadata_endpoint_);
            response->mutable_leader_hint()->set_observed_at_unix_ms(
                observed_at_unix_ms_);

            auto *snapshot = response->add_metadata_nodes();
            snapshot->set_cluster_id(request->cluster_id());
            snapshot->set_node_id("meta-test-1");
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
            response->mutable_summary()->set_node_id("view-test-1");
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
        std::uint64_t observed_at_unix_ms_{1714001000000ULL};
        std::vector<view::ViewNodeSnapshot> storage_nodes_;
    };

    class ScopedFakeViewServer
    {
    public:
        ScopedFakeViewServer()
        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("127.0.0.1:0",
                                     grpc::InsecureServerCredentials(),
                                     &selected_port_);
            builder.RegisterService(&service_);
            server_ = builder.BuildAndStart();
            if (server_ == nullptr || selected_port_ <= 0)
            {
                throw std::runtime_error("failed to start fake view server");
            }
            address_ = "127.0.0.1:" + std::to_string(selected_port_);
        }

        ~ScopedFakeViewServer()
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

        [[nodiscard]] FakeViewNodeService &service()
        {
            return service_;
        }

    private:
        int selected_port_{0};
        std::string address_;
        FakeViewNodeService service_;
        std::unique_ptr<grpc::Server> server_;
    };

    class RecordingStorageTransferClient final : public storedemo::StorageTransferClient
    {
    public:
        storedemo::StorageTransferWriteResult WriteChunk(
            const storedemo::StorageTransferWriteRequest &request) override
        {
            std::size_t chunk_attempt = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                writes.push_back(request);
                chunk_attempt =
                    ++write_attempts_by_chunk_[request.identity.chunk_index];
            }

            if (write_behavior)
            {
                auto forced = write_behavior(request, chunk_attempt);
                if (forced.has_value())
                {
                    if (forced->target.node_id.empty())
                    {
                        forced->target = request.target;
                    }
                    return *forced;
                }
            }

            storedemo::StorageTransferWriteResult result;
            result.status = storedemo::StorageNodeStatusCode::kOk;
            result.target = request.target;
            result.durable = true;
            result.metadata.identity = request.identity;
            result.metadata.node_id = request.target.node_id;
            result.metadata.size = static_cast<std::uint64_t>(request.payload.size());
            result.metadata.checksum = request.expected_checksum;
            result.metadata.state = storedemo::ChunkState::kLive;
            return result;
        }

        storedemo::StorageTransferReadResult ReadChunk(
            const storedemo::StorageTransferReadRequest &request) override
        {
            storedemo::StorageTransferReadResult result;
            result.status = storedemo::StorageNodeStatusCode::kUnsupported;
            result.target = request.target;
            result.error_detail = "read path not implemented in recording client";
            return result;
        }

        std::function<std::optional<storedemo::StorageTransferWriteResult>(
            const storedemo::StorageTransferWriteRequest &request,
            std::size_t chunk_attempt)>
            write_behavior;
        std::vector<storedemo::StorageTransferWriteRequest> writes;

    private:
        std::mutex mu_;
        std::unordered_map<std::uint32_t, std::size_t> write_attempts_by_chunk_;
    };

    view::ViewNodeSnapshot MakeStorageSnapshot(const std::string &cluster_id,
                                               const std::size_t index,
                                               const std::uint64_t available_capacity_bytes,
                                               const std::uint32_t queued_ops,
                                               const std::string &zone)
    {
        view::ViewNodeSnapshot snapshot;
        snapshot.set_cluster_id(cluster_id);
        snapshot.set_node_id(storedemo::test::MakeStorageNodeIdFixture(index));
        snapshot.set_node_type(view::VIEW_NODE_TYPE_STORAGE);
        snapshot.set_endpoint("127.0.0.1:" + std::to_string(7400 + index));
        snapshot.set_data_plane_endpoint("127.0.0.1:" + std::to_string(8400 + index));
        snapshot.set_registered_at_unix_ms(1714001000000ULL);
        snapshot.set_last_seen_unix_ms(1714001000000ULL);
        snapshot.set_last_sequence(static_cast<std::uint64_t>(index));
        snapshot.set_liveness(view::VIEW_NODE_LIVENESS_STATE_LIVE);
        snapshot.mutable_failure_domain()->set_zone(zone);
        snapshot.mutable_failure_domain()->set_rack("rack-" + std::to_string(index % 3));
        snapshot.mutable_health()->set_health(view::VIEW_NODE_HEALTH_HEALTHY);
        snapshot.mutable_health()->set_disk_pressure(view::VIEW_NODE_DISK_PRESSURE_LOW);
        snapshot.mutable_capacity()->set_total_capacity_bytes(
            available_capacity_bytes + 8192ULL);
        snapshot.mutable_capacity()->set_used_capacity_bytes(8192ULL);
        snapshot.mutable_capacity()->set_available_capacity_bytes(
            available_capacity_bytes);
        snapshot.mutable_load()->set_queued_ops(queued_ops);
        snapshot.mutable_load()->set_active_reads(queued_ops);
        snapshot.mutable_load()->set_active_writes(queued_ops / 2U);
        snapshot.mutable_load()->set_write_admission_overloaded(false);
        return snapshot;
    }

    std::vector<std::string> SortedReplicaSet(
        const std::vector<storedemo::StorageNodeId> &node_ids)
    {
        std::vector<std::string> ordered(node_ids.begin(), node_ids.end());
        std::sort(ordered.begin(), ordered.end());
        return ordered;
    }

    std::vector<view::ViewNodeSnapshot> MakeBalancedStorageSnapshots(
        const std::string &cluster_id,
        const std::size_t node_count = 9)
    {
        static const std::vector<std::string> zones = {
            "zone-a",
            "zone-b",
            "zone-c"};

        std::vector<view::ViewNodeSnapshot> snapshots;
        snapshots.reserve(node_count);
        for (std::size_t index = 0; index < node_count; ++index)
        {
            const auto node_number = (index * 2) % node_count + 1;
            snapshots.push_back(MakeStorageSnapshot(cluster_id,
                                                    node_number,
                                                    128ULL * 1024ULL * 1024ULL,
                                                    0,
                                                    zones[index % zones.size()]));
        }
        return snapshots;
    }

    storedemo::StorageTransferTarget MakeResolvedTarget(const std::size_t index)
    {
        storedemo::StorageTransferTarget target;
        target.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        target.endpoint = "127.0.0.1:" + std::to_string(8400 + index);
        return target;
    }

    std::vector<std::string> CollectWriteTargetsForChunk(
        const RecordingStorageTransferClient &storage_client,
        const std::uint32_t chunk_index)
    {
        std::vector<std::string> targets;
        for (const auto &write : storage_client.writes)
        {
            if (write.identity.chunk_index == chunk_index)
            {
                targets.push_back(write.target.node_id);
            }
        }
        return targets;
    }
}

TEST(MetadataTransferClientTest,
     CreateWritePlanReturnsBaseObjectAndPolicyFactsWithoutProtoChanges)
{
    ScopedFakeMetadataServer metadata_server;
    auto client = storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());

    const std::string payload = storedemo::test::MakeChunkPayload(257, "plan-base");
    const auto checksum_facts = MakeObjectChecksumFacts(payload);

    const auto call = client->CreateWritePlan(
        {.request_id = "create-plan-base",
         .bucket = "bucket-plan",
         .object_key = "objects/base.bin",
         .object_id = "obj-plan-base",
         .expected_object_checksum = checksum_facts,
         .chunk_size = 64,
         .desired_replica_count = 3,
         .minimum_successful_writes = 2,
         .client_time_unix_ms = 1714000000100ULL});

    ASSERT_TRUE(call.transport_ok()) << call.rpc.grpc_error_message;
    ASSERT_TRUE(call.result.ok()) << call.result.summary.message;
    ASSERT_TRUE(call.result.write_plan.has_value());
    ASSERT_TRUE(call.result.created_pending);
    EXPECT_EQ(call.result.write_plan->request_id, "create-plan-base");
    EXPECT_EQ(call.result.write_plan->bucket, "bucket-plan");
    EXPECT_EQ(call.result.write_plan->object_key, "objects/base.bin");
    EXPECT_EQ(call.result.write_plan->object_id, "obj-plan-base");
    EXPECT_EQ(call.result.write_plan->version, 7U);
    EXPECT_EQ(call.result.write_plan->chunk_size_bytes, 64U);
    EXPECT_EQ(call.result.write_plan->replica_count, 3U);
    EXPECT_EQ(call.result.write_plan->minimum_successful_writes, 2U);
    EXPECT_EQ(call.result.write_plan->total_chunks, 0U);
    EXPECT_EQ(call.result.write_plan->placement_epoch, 0U);
    EXPECT_TRUE(call.result.write_plan->chunks.empty());
    EXPECT_EQ(call.result.write_plan->created_at_unix_ms,
              metadata_server.service().created_at_unix_ms());

    const auto create_request = metadata_server.service().last_create_request();
    ASSERT_TRUE(create_request.has_value());
    EXPECT_EQ(create_request->size(), checksum_facts.size);
    EXPECT_EQ(create_request->etag(), checksum_facts.etag);
}

TEST(MetadataTransferClientTest,
     CreateWritePlanRejectsInvalidReplicaCountsBeforeRpc)
{
    ScopedFakeMetadataServer metadata_server;
    auto client = storedemo::CreateGrpcMetadataTransferClient(metadata_server.address());

    const std::string payload = storedemo::test::MakeChunkPayload(128, "plan-invalid");
    const auto checksum_facts = MakeObjectChecksumFacts(payload);

    const auto zero_replica_call = client->CreateWritePlan(
        {.request_id = "create-plan-zero-replica",
         .bucket = "bucket-plan",
         .object_key = "objects/invalid.bin",
         .object_id = "obj-plan-invalid",
         .expected_object_checksum = checksum_facts,
         .chunk_size = 64,
         .desired_replica_count = 0,
         .minimum_successful_writes = 1,
         .client_time_unix_ms = 1714000000200ULL});
    EXPECT_EQ(zero_replica_call.result.summary.status,
              storedemo::MetadataTransferStatusCode::kInvalidArgument);

    const auto quorum_violation_call = client->CreateWritePlan(
        {.request_id = "create-plan-min-gt-replica",
         .bucket = "bucket-plan",
         .object_key = "objects/invalid.bin",
         .object_id = "obj-plan-invalid",
         .expected_object_checksum = checksum_facts,
         .chunk_size = 64,
         .desired_replica_count = 2,
         .minimum_successful_writes = 3,
         .client_time_unix_ms = 1714000000201ULL});
    EXPECT_EQ(quorum_violation_call.result.summary.status,
              storedemo::MetadataTransferStatusCode::kInvalidArgument);

    EXPECT_EQ(metadata_server.service().create_calls(), 0U);
}

TEST(ObjectTransferWritePlanTest,
     UploadAssemblesPerChunkSelectedReplicaNodesIntoTransferWritePlan)
{
    ScopedFakeMetadataServer metadata_server;
    ScopedFakeViewServer view_server;
    view_server.service().SetMetadataEndpoint(metadata_server.address());
    view_server.service().SetStorageNodes(
        {
            MakeStorageSnapshot("cluster-plan", 9, 128ULL * 1024ULL * 1024ULL, 0, "zone-c"),
            MakeStorageSnapshot("cluster-plan", 1, 128ULL * 1024ULL * 1024ULL, 0, "zone-a"),
            MakeStorageSnapshot("cluster-plan", 5, 128ULL * 1024ULL * 1024ULL, 0, "zone-b"),
            MakeStorageSnapshot("cluster-plan", 3, 128ULL * 1024ULL * 1024ULL, 0, "zone-a"),
            MakeStorageSnapshot("cluster-plan", 7, 128ULL * 1024ULL * 1024ULL, 0, "zone-c"),
            MakeStorageSnapshot("cluster-plan", 2, 128ULL * 1024ULL * 1024ULL, 0, "zone-a"),
            MakeStorageSnapshot("cluster-plan", 8, 128ULL * 1024ULL * 1024ULL, 0, "zone-c"),
            MakeStorageSnapshot("cluster-plan", 4, 128ULL * 1024ULL * 1024ULL, 0, "zone-b"),
            MakeStorageSnapshot("cluster-plan", 6, 128ULL * 1024ULL * 1024ULL, 0, "zone-b"),
        },
        1714001000000ULL);

    auto metadata_client = storedemo::CreateGrpcMetadataTransferClient(
        metadata_server.address(),
        {.create_write_plan_timeout = std::chrono::milliseconds(2500)});
    auto storage_client =
        std::make_shared<RecordingStorageTransferClient>();
    auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
        grpc::CreateChannel(view_server.address(),
                            grpc::InsecureChannelCredentials()),
        view_server.address(),
        viewdemo::ViewNodeClientConfig{
            .discovery_timeout = std::chrono::milliseconds(1800)});

    storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

    std::string payload;
    for (std::size_t index = 0; index < 8; ++index)
    {
        payload += storedemo::test::MakeChunkPayload(32, "plan-chunk-" + std::to_string(index));
    }

    storedemo::test::ScopedStoreTestDir temp_dir("transfer_write_plan_upload");
    const auto source_path = WritePayloadFile(temp_dir.root(), payload);

    storedemo::UploadObjectRequest request;
    request.request_id = "upload-plan";
    request.cluster_id = "cluster-plan";
    request.bucket = "bucket-plan";
    request.object_key = "objects/plan.bin";
    request.object_id = "obj-plan-upload";
    request.source_path = source_path;
    request.chunk_size = 32;
    request.concurrency = 1;
    request.desired_replica_count = 3;
    request.minimum_successful_writes = 2;
    request.client_time_unix_ms = 1714001000100ULL;

    auto session = transfer.StartUploadSession(request);
    auto reader = storedemo::CreateFileTransferChunkReader();
    auto checksum_state = storedemo::CreateTransferChecksumState();
    const auto result = session->Execute(*reader, *checksum_state);

    ASSERT_TRUE(result.ok()) << result.error_detail;
    ASSERT_TRUE(result.write_plan.has_value());
    ASSERT_TRUE(result.committed);
    ASSERT_EQ(result.write_plan->chunk_size_bytes, 32U);
    ASSERT_EQ(result.write_plan->total_chunks, 8U);
    ASSERT_EQ(result.write_plan->replica_count, 3U);
    ASSERT_EQ(result.write_plan->minimum_successful_writes, 2U);
    ASSERT_EQ(result.write_plan->placement_epoch, 1714001000000ULL);
    EXPECT_GT(result.write_plan->expires_at_unix_ms,
              result.write_plan->placement_epoch);
    ASSERT_EQ(result.write_plan->chunks.size(), 8U);

    std::set<std::vector<std::string>> unique_replica_sets;
    std::set<std::string> covered_nodes;
    std::uint64_t expected_offset = 0;
    for (std::size_t index = 0; index < result.write_plan->chunks.size(); ++index)
    {
        const auto &chunk_plan = result.write_plan->chunks[index];
        EXPECT_EQ(chunk_plan.identity.chunk_index, index);
        EXPECT_EQ(chunk_plan.identity.offset, expected_offset);
        EXPECT_EQ(chunk_plan.offset, expected_offset);
        EXPECT_EQ(chunk_plan.expected_size, 32U);
        EXPECT_TRUE(chunk_plan.expected_checksum.IsSet());
        EXPECT_EQ(chunk_plan.required_replica_count, 3U);
        EXPECT_EQ(chunk_plan.minimum_successful_writes, 2U);
        ASSERT_EQ(chunk_plan.selected_replica_nodes.size(), 3U);
        EXPECT_TRUE(chunk_plan.candidate_nodes.empty());

        std::set<std::string> unique_chunk_nodes(chunk_plan.selected_replica_nodes.begin(),
                                                 chunk_plan.selected_replica_nodes.end());
        EXPECT_EQ(unique_chunk_nodes.size(), 3U);
        for (const auto &node_id : unique_chunk_nodes)
        {
            covered_nodes.insert(node_id);
        }
        unique_replica_sets.insert(SortedReplicaSet(chunk_plan.selected_replica_nodes));
        expected_offset += chunk_plan.expected_size;
    }

    EXPECT_GE(covered_nodes.size(), 6U);
    EXPECT_GE(unique_replica_sets.size(), 2U);

    const auto commit_request = metadata_server.service().last_commit_request();
    ASSERT_TRUE(commit_request.has_value());
    ASSERT_EQ(commit_request->chunks_size(),
              static_cast<int>(result.write_plan->chunks.size()));
    for (int index = 0; index < commit_request->chunks_size(); ++index)
    {
        const auto &planned_chunk = result.write_plan->chunks[static_cast<std::size_t>(index)];
        std::vector<std::string> committed_nodes(commit_request->chunks(index).replica_nodes().begin(),
                                                 commit_request->chunks(index).replica_nodes().end());
        EXPECT_EQ(committed_nodes, planned_chunk.selected_replica_nodes);
        EXPECT_EQ(SortedReplicaSet(
                      CollectWriteTargetsForChunk(*storage_client,
                                                  planned_chunk.identity.chunk_index)),
                  SortedReplicaSet(planned_chunk.selected_replica_nodes));
    }
}

TEST(ObjectTransferWritePlanTest,
     ResolveSelectedChunkTargetsRejectsEmptySelectedNodesEvenIfCandidateNodesExist)
{
    storedemo::TransferChunkPlan chunk_plan;
    chunk_plan.identity.chunk_id = "chunk-empty-selected";
    chunk_plan.identity.chunk_index = 3;
    chunk_plan.required_replica_count = 3;
    chunk_plan.minimum_successful_writes = 2;
    chunk_plan.candidate_nodes = {
        storedemo::test::MakeStorageNodeIdFixture(1),
        storedemo::test::MakeStorageNodeIdFixture(2),
        storedemo::test::MakeStorageNodeIdFixture(3)};

    const std::unordered_map<std::string, storedemo::StorageTransferTarget> storage_targets{
        {storedemo::test::MakeStorageNodeIdFixture(1), MakeResolvedTarget(1)},
        {storedemo::test::MakeStorageNodeIdFixture(2), MakeResolvedTarget(2)},
        {storedemo::test::MakeStorageNodeIdFixture(3), MakeResolvedTarget(3)},
        {storedemo::test::MakeStorageNodeIdFixture(9), MakeResolvedTarget(9)},
    };

    std::string error_detail;
    const auto targets = storedemo::ResolveSelectedChunkTargetsForTesting(
        chunk_plan,
        storage_targets,
        &error_detail);

    EXPECT_TRUE(targets.empty());
    EXPECT_NE(error_detail.find("selected_replica_nodes are empty"),
              std::string::npos);
    EXPECT_NE(error_detail.find(chunk_plan.identity.chunk_id), std::string::npos);
}

TEST(ObjectTransferWritePlanTest,
     ResolveSelectedChunkTargetsRejectsInsufficientOrDuplicateSelectedNodes)
{
    const auto node1 = storedemo::test::MakeStorageNodeIdFixture(1);
    const auto node2 = storedemo::test::MakeStorageNodeIdFixture(2);
    const auto node3 = storedemo::test::MakeStorageNodeIdFixture(3);
    const std::unordered_map<std::string, storedemo::StorageTransferTarget> storage_targets{
        {node1, MakeResolvedTarget(1)},
        {node2, MakeResolvedTarget(2)},
        {node3, MakeResolvedTarget(3)},
    };

    storedemo::TransferChunkPlan insufficient_plan;
    insufficient_plan.identity.chunk_id = "chunk-insufficient-selected";
    insufficient_plan.identity.chunk_index = 1;
    insufficient_plan.required_replica_count = 3;
    insufficient_plan.minimum_successful_writes = 2;
    insufficient_plan.selected_replica_nodes = {node1, node2};

    std::string insufficient_error;
    const auto insufficient_targets =
        storedemo::ResolveSelectedChunkTargetsForTesting(insufficient_plan,
                                                         storage_targets,
                                                         &insufficient_error);
    EXPECT_TRUE(insufficient_targets.empty());
    EXPECT_NE(insufficient_error.find("does not match required_replica_count"),
              std::string::npos);
    EXPECT_NE(insufficient_error.find(insufficient_plan.identity.chunk_id),
              std::string::npos);

    storedemo::TransferChunkPlan duplicate_plan;
    duplicate_plan.identity.chunk_id = "chunk-duplicate-selected";
    duplicate_plan.identity.chunk_index = 2;
    duplicate_plan.required_replica_count = 3;
    duplicate_plan.minimum_successful_writes = 2;
    duplicate_plan.selected_replica_nodes = {node1, node1, node2};

    std::string duplicate_error;
    const auto duplicate_targets =
        storedemo::ResolveSelectedChunkTargetsForTesting(duplicate_plan,
                                                         storage_targets,
                                                         &duplicate_error);
    EXPECT_TRUE(duplicate_targets.empty());
    EXPECT_NE(duplicate_error.find("duplicate node_id=" + node1),
              std::string::npos);
    EXPECT_NE(duplicate_error.find(duplicate_plan.identity.chunk_id),
              std::string::npos);
}

TEST(ObjectTransferWritePlanTest,
     ResolveSelectedChunkTargetsRejectsMissingDiscoveryNodeWithoutFallback)
{
    const auto node1 = storedemo::test::MakeStorageNodeIdFixture(1);
    const auto node2 = storedemo::test::MakeStorageNodeIdFixture(2);
    const auto node3 = storedemo::test::MakeStorageNodeIdFixture(3);
    const auto extra_node = storedemo::test::MakeStorageNodeIdFixture(9);

    storedemo::TransferChunkPlan chunk_plan;
    chunk_plan.identity.chunk_id = "chunk-missing-selected-node";
    chunk_plan.identity.chunk_index = 4;
    chunk_plan.required_replica_count = 3;
    chunk_plan.minimum_successful_writes = 2;
    chunk_plan.selected_replica_nodes = {node1, node2, node3};
    chunk_plan.candidate_nodes = {node1, node2, node3, extra_node};

    const std::unordered_map<std::string, storedemo::StorageTransferTarget> storage_targets{
        {node1, MakeResolvedTarget(1)},
        {node2, MakeResolvedTarget(2)},
        {extra_node, MakeResolvedTarget(9)},
    };

    std::string error_detail;
    const auto targets = storedemo::ResolveSelectedChunkTargetsForTesting(
        chunk_plan,
        storage_targets,
        &error_detail);

    EXPECT_TRUE(targets.empty());
    EXPECT_NE(error_detail.find("selected node_id=" + node3),
              std::string::npos);
    EXPECT_NE(error_detail.find("not discoverable via ViewNode storage endpoints"),
              std::string::npos);
}

TEST(ObjectTransferWritePlanTest,
     UploadCommitsOnlyActualDurableSelectedReplicasIntoManifest)
{
    ScopedFakeMetadataServer metadata_server;
    ScopedFakeViewServer view_server;
    view_server.service().SetMetadataEndpoint(metadata_server.address());
    view_server.service().SetStorageNodes(
        MakeBalancedStorageSnapshots("cluster-plan-manifest"),
        1714001002000ULL);

    auto metadata_client = storedemo::CreateGrpcMetadataTransferClient(
        metadata_server.address(),
        {.create_write_plan_timeout = std::chrono::milliseconds(2500)});
    auto storage_client =
        std::make_shared<RecordingStorageTransferClient>();
    std::unordered_map<std::uint32_t, std::string> failed_node_by_chunk;
    storage_client->write_behavior =
        [&failed_node_by_chunk](
            const storedemo::StorageTransferWriteRequest &request,
            const std::size_t chunk_attempt)
        -> std::optional<storedemo::StorageTransferWriteResult>
    {
        if (chunk_attempt != 3U)
        {
            return std::nullopt;
        }

        failed_node_by_chunk[request.identity.chunk_index] = request.target.node_id;

        storedemo::StorageTransferWriteResult result;
        result.status = storedemo::StorageNodeStatusCode::kOverloaded;
        result.error_detail = "forced third selected replica failure";
        result.target = request.target;
        result.retryable = true;
        return result;
    };

    auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
        grpc::CreateChannel(view_server.address(),
                            grpc::InsecureChannelCredentials()),
        view_server.address(),
        viewdemo::ViewNodeClientConfig{
            .discovery_timeout = std::chrono::milliseconds(1800)});

    storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

    std::string payload;
    for (std::size_t index = 0; index < 3; ++index)
    {
        payload += storedemo::test::MakeChunkPayload(
            48,
            "manifest-chunk-" + std::to_string(index));
    }

    storedemo::test::ScopedStoreTestDir temp_dir("transfer_write_plan_manifest_success");
    const auto source_path = WritePayloadFile(temp_dir.root(), payload);

    storedemo::UploadObjectRequest request;
    request.request_id = "upload-plan-manifest";
    request.cluster_id = "cluster-plan-manifest";
    request.bucket = "bucket-plan";
    request.object_key = "objects/manifest.bin";
    request.object_id = "obj-plan-manifest";
    request.source_path = source_path;
    request.chunk_size = 48;
    request.concurrency = 1;
    request.desired_replica_count = 3;
    request.minimum_successful_writes = 2;
    request.client_time_unix_ms = 1714001002100ULL;

    auto session = transfer.StartUploadSession(request);
    auto reader = storedemo::CreateFileTransferChunkReader();
    auto checksum_state = storedemo::CreateTransferChecksumState();
    const auto result = session->Execute(*reader, *checksum_state);

    ASSERT_TRUE(result.ok()) << result.error_detail;
    ASSERT_TRUE(result.committed);
    ASSERT_TRUE(result.write_plan.has_value());
    ASSERT_EQ(result.committed_chunks.size(), result.write_plan->chunks.size());

    const auto commit_request = metadata_server.service().last_commit_request();
    ASSERT_TRUE(commit_request.has_value());
    ASSERT_EQ(commit_request->chunks_size(),
              static_cast<int>(result.write_plan->chunks.size()));

    for (std::size_t index = 0; index < result.write_plan->chunks.size(); ++index)
    {
        const auto &planned_chunk = result.write_plan->chunks[index];
        ASSERT_EQ(planned_chunk.selected_replica_nodes.size(), 3U);
        const auto failed_it =
            failed_node_by_chunk.find(planned_chunk.identity.chunk_index);
        ASSERT_NE(failed_it, failed_node_by_chunk.end());

        const auto actual_targets =
            CollectWriteTargetsForChunk(*storage_client,
                                        planned_chunk.identity.chunk_index);
        EXPECT_EQ(SortedReplicaSet(actual_targets),
                  SortedReplicaSet(planned_chunk.selected_replica_nodes));

        std::vector<std::string> committed_nodes(
            commit_request->chunks(static_cast<int>(index)).replica_nodes().begin(),
            commit_request->chunks(static_cast<int>(index)).replica_nodes().end());
        ASSERT_EQ(committed_nodes.size(), 2U);
        EXPECT_EQ(committed_nodes[0], planned_chunk.selected_replica_nodes[0]);
        EXPECT_EQ(committed_nodes[1], planned_chunk.selected_replica_nodes[1]);
        EXPECT_EQ(std::find(committed_nodes.begin(),
                            committed_nodes.end(),
                            failed_it->second),
                  committed_nodes.end());
        EXPECT_EQ(result.committed_chunks[index].replica_nodes, committed_nodes);
    }
}

TEST(ObjectTransferWritePlanTest,
     UploadFansOutSelectedReplicasWithBoundedOverlapAndStableManifestAggregation)
{
    ScopedFakeMetadataServer metadata_server;
    ScopedFakeViewServer view_server;
    view_server.service().SetMetadataEndpoint(metadata_server.address());
    view_server.service().SetStorageNodes(
        MakeBalancedStorageSnapshots("cluster-plan-fanout"),
        1714001002500ULL);

    auto metadata_client = storedemo::CreateGrpcMetadataTransferClient(
        metadata_server.address(),
        {.create_write_plan_timeout = std::chrono::milliseconds(2500)});
    auto storage_client =
        std::make_shared<RecordingStorageTransferClient>();

    std::mutex behavior_mu;
    std::condition_variable behavior_cv;
    std::size_t first_wave_started = 0;
    bool release_first_wave = false;
    std::size_t active_replica_tasks = 0;
    std::size_t max_active_replica_tasks = 0;
    bool payload_valid = true;
    std::vector<std::string> completion_order;

    storage_client->write_behavior =
        [&](const storedemo::StorageTransferWriteRequest &request,
            const std::size_t chunk_attempt)
        -> std::optional<storedemo::StorageTransferWriteResult>
    {
        {
            std::unique_lock<std::mutex> lock(behavior_mu);
            ++active_replica_tasks;
            max_active_replica_tasks = std::max(max_active_replica_tasks,
                                                active_replica_tasks);
            payload_valid = payload_valid &&
                            request.payload ==
                                storedemo::test::MakeChunkPayload(
                                    64,
                                    "parallel-fanout");
            if (first_wave_started < 2U)
            {
                ++first_wave_started;
                if (first_wave_started == 2U)
                {
                    release_first_wave = true;
                    behavior_cv.notify_all();
                }
                else
                {
                    behavior_cv.wait(lock,
                                     [&]()
                                     {
                                         return release_first_wave;
                                     });
                }
            }
        }

        if (chunk_attempt == 1U)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
        }
        else if (chunk_attempt == 2U)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }

        {
            std::lock_guard<std::mutex> lock(behavior_mu);
            completion_order.push_back(request.target.node_id);
            --active_replica_tasks;
            behavior_cv.notify_all();
        }

        if (chunk_attempt == 3U)
        {
            storedemo::StorageTransferWriteResult result;
            result.status = storedemo::StorageNodeStatusCode::kOverloaded;
            result.error_detail = "forced third selected replica failure";
            result.target = request.target;
            result.retryable = true;
            return result;
        }

        return std::nullopt;
    };

    auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
        grpc::CreateChannel(view_server.address(),
                            grpc::InsecureChannelCredentials()),
        view_server.address());

    storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

    const std::string payload =
        storedemo::test::MakeChunkPayload(64, "parallel-fanout");
    storedemo::test::ScopedStoreTestDir temp_dir("transfer_write_plan_parallel_fanout");
    const auto source_path = WritePayloadFile(temp_dir.root(), payload);

    storedemo::UploadObjectRequest request;
    request.request_id = "upload-plan-fanout";
    request.cluster_id = "cluster-plan-fanout";
    request.bucket = "bucket-plan";
    request.object_key = "objects/fanout.bin";
    request.object_id = "obj-plan-fanout";
    request.source_path = source_path;
    request.chunk_size = 64;
    request.concurrency = 1;
    request.desired_replica_count = 3;
    request.minimum_successful_writes = 2;
    request.client_time_unix_ms = 1714001002600ULL;

    auto session = transfer.StartUploadSession(request);
    auto reader = storedemo::CreateFileTransferChunkReader();
    auto checksum_state = storedemo::CreateTransferChecksumState();
    const auto result = session->Execute(*reader, *checksum_state);

    ASSERT_TRUE(result.ok()) << result.error_detail;
    ASSERT_TRUE(result.committed);
    ASSERT_TRUE(result.write_plan.has_value());
    ASSERT_EQ(result.write_plan->chunks.size(), 1U);
    ASSERT_EQ(storage_client->writes.size(), 3U);
    EXPECT_TRUE(payload_valid);
    EXPECT_EQ(max_active_replica_tasks, 2U);
    EXPECT_EQ(active_replica_tasks, 0U);

    const auto selected_nodes =
        result.write_plan->chunks.front().selected_replica_nodes;
    EXPECT_EQ(SortedReplicaSet(CollectWriteTargetsForChunk(*storage_client, 0U)),
              SortedReplicaSet(selected_nodes));
    ASSERT_EQ(completion_order.size(), 3U);
    EXPECT_NE(completion_order, selected_nodes);

    const auto commit_request = metadata_server.service().last_commit_request();
    ASSERT_TRUE(commit_request.has_value());
    ASSERT_EQ(commit_request->chunks_size(), 1);
    std::vector<std::string> committed_nodes(
        commit_request->chunks(0).replica_nodes().begin(),
        commit_request->chunks(0).replica_nodes().end());
    ASSERT_EQ(committed_nodes.size(), 2U);
    EXPECT_EQ(committed_nodes[0], selected_nodes[0]);
    EXPECT_EQ(committed_nodes[1], selected_nodes[1]);
    EXPECT_EQ(std::find(committed_nodes.begin(),
                        committed_nodes.end(),
                        selected_nodes[2]),
              committed_nodes.end());
}

TEST(ObjectTransferWritePlanTest,
     UploadDoesNotCommitWhenSelectedReplicasDoNotReachMinimumWritesAndDoesNotUseExtraNodes)
{
    ScopedFakeMetadataServer metadata_server;
    ScopedFakeViewServer view_server;
    view_server.service().SetMetadataEndpoint(metadata_server.address());
    view_server.service().SetStorageNodes(
        MakeBalancedStorageSnapshots("cluster-plan-failure"),
        1714001003000ULL);

    auto metadata_client = storedemo::CreateGrpcMetadataTransferClient(
        metadata_server.address());
    auto storage_client =
        std::make_shared<RecordingStorageTransferClient>();
    std::atomic<std::size_t> completed_replica_tasks{0};
    storage_client->write_behavior =
        [&completed_replica_tasks](
            const storedemo::StorageTransferWriteRequest &request,
            const std::size_t chunk_attempt)
        -> std::optional<storedemo::StorageTransferWriteResult>
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        completed_replica_tasks.fetch_add(1U);
        if (chunk_attempt != 3U)
        {
            return std::nullopt;
        }

        storedemo::StorageTransferWriteResult result;
        result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
        result.error_detail = "forced third selected replica failure";
        result.target = request.target;
        result.retryable = true;
        return result;
    };

    auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
        grpc::CreateChannel(view_server.address(),
                            grpc::InsecureChannelCredentials()),
        view_server.address());

    storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

    const std::string payload =
        storedemo::test::MakeChunkPayload(64, "plan-no-commit");
    storedemo::test::ScopedStoreTestDir temp_dir("transfer_write_plan_no_commit");
    const auto source_path = WritePayloadFile(temp_dir.root(), payload);

    storedemo::UploadObjectRequest request;
    request.request_id = "upload-plan-no-commit";
    request.cluster_id = "cluster-plan-failure";
    request.bucket = "bucket-plan";
    request.object_key = "objects/no-commit.bin";
    request.object_id = "obj-plan-no-commit";
    request.source_path = source_path;
    request.chunk_size = 64;
    request.concurrency = 1;
    request.desired_replica_count = 3;
    request.minimum_successful_writes = 3;
    request.client_time_unix_ms = 1714001003100ULL;

    auto session = transfer.StartUploadSession(request);
    auto reader = storedemo::CreateFileTransferChunkReader();
    auto checksum_state = storedemo::CreateTransferChecksumState();
    const auto result = session->Execute(*reader, *checksum_state);

    EXPECT_EQ(result.status, storedemo::ObjectTransferStatusCode::kStorageRejected);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(metadata_server.service().commit_calls(), 0U);
    EXPECT_FALSE(metadata_server.service().last_commit_request().has_value());
    ASSERT_TRUE(result.write_plan.has_value());
    ASSERT_EQ(result.write_plan->chunks.size(), 1U);
    EXPECT_EQ(
        SortedReplicaSet(CollectWriteTargetsForChunk(*storage_client, 0U)),
        SortedReplicaSet(result.write_plan->chunks.front().selected_replica_nodes));
    EXPECT_EQ(storage_client->writes.size(),
              result.write_plan->chunks.front().selected_replica_nodes.size());
    EXPECT_EQ(completed_replica_tasks.load(), storage_client->writes.size());
}

TEST(ObjectTransferWritePlanTest,
     UploadKeepsChunksSerialWhileReplicaFanoutRunsInParallel)
{
    ScopedFakeMetadataServer metadata_server;
    ScopedFakeViewServer view_server;
    view_server.service().SetMetadataEndpoint(metadata_server.address());
    view_server.service().SetStorageNodes(
        MakeBalancedStorageSnapshots("cluster-plan-serial"),
        1714001003500ULL);

    auto metadata_client = storedemo::CreateGrpcMetadataTransferClient(
        metadata_server.address());
    auto storage_client =
        std::make_shared<RecordingStorageTransferClient>();

    std::mutex behavior_mu;
    std::unordered_map<std::uint32_t, std::size_t> active_by_chunk;
    std::size_t max_simultaneous_chunks = 0;

    storage_client->write_behavior =
        [&](const storedemo::StorageTransferWriteRequest &request,
            const std::size_t)
        -> std::optional<storedemo::StorageTransferWriteResult>
    {
        {
            std::lock_guard<std::mutex> lock(behavior_mu);
            ++active_by_chunk[request.identity.chunk_index];
            max_simultaneous_chunks =
                std::max(max_simultaneous_chunks, active_by_chunk.size());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(40));

        {
            std::lock_guard<std::mutex> lock(behavior_mu);
            auto it = active_by_chunk.find(request.identity.chunk_index);
            if (it != active_by_chunk.end())
            {
                if (it->second > 1U)
                {
                    --it->second;
                }
                else
                {
                    active_by_chunk.erase(it);
                }
            }
        }
        return std::nullopt;
    };

    auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
        grpc::CreateChannel(view_server.address(),
                            grpc::InsecureChannelCredentials()),
        view_server.address());

    storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

    std::string payload;
    payload += storedemo::test::MakeChunkPayload(32, "serial-chunk-0");
    payload += storedemo::test::MakeChunkPayload(32, "serial-chunk-1");

    storedemo::test::ScopedStoreTestDir temp_dir("transfer_write_plan_serial_chunks");
    const auto source_path = WritePayloadFile(temp_dir.root(), payload);

    storedemo::UploadObjectRequest request;
    request.request_id = "upload-plan-serial";
    request.cluster_id = "cluster-plan-serial";
    request.bucket = "bucket-plan";
    request.object_key = "objects/serial.bin";
    request.object_id = "obj-plan-serial";
    request.source_path = source_path;
    request.chunk_size = 32;
    request.concurrency = 1;
    request.desired_replica_count = 3;
    request.minimum_successful_writes = 2;
    request.client_time_unix_ms = 1714001003600ULL;

    auto session = transfer.StartUploadSession(request);
    auto reader = storedemo::CreateFileTransferChunkReader();
    auto checksum_state = storedemo::CreateTransferChecksumState();
    const auto result = session->Execute(*reader, *checksum_state);

    ASSERT_TRUE(result.ok()) << result.error_detail;
    EXPECT_EQ(max_simultaneous_chunks, 1U);
    ASSERT_TRUE(result.write_plan.has_value());
    ASSERT_EQ(result.write_plan->chunks.size(), 2U);
    EXPECT_EQ(storage_client->writes.size(), 6U);
}

TEST(ObjectTransferWritePlanTest,
     UploadFailsPlanningWhenHealthyStorageNodesAreFewerThanReplicaCount)
{
    ScopedFakeMetadataServer metadata_server;
    ScopedFakeViewServer view_server;
    view_server.service().SetMetadataEndpoint(metadata_server.address());
    view_server.service().SetStorageNodes(
        {
            MakeStorageSnapshot("cluster-plan-fail", 1, 64ULL * 1024ULL * 1024ULL, 0, "zone-a"),
            MakeStorageSnapshot("cluster-plan-fail", 2, 64ULL * 1024ULL * 1024ULL, 0, "zone-b"),
        },
        1714002000000ULL);

    auto metadata_client = storedemo::CreateGrpcMetadataTransferClient(
        metadata_server.address());
    auto storage_client =
        std::make_shared<RecordingStorageTransferClient>();
    auto view_client = std::make_shared<viewdemo::ViewNodeClient>(
        grpc::CreateChannel(view_server.address(),
                            grpc::InsecureChannelCredentials()),
        view_server.address());

    storedemo::ObjectTransfer transfer(metadata_client, storage_client, view_client);

    const std::string payload = storedemo::test::MakeChunkPayload(96, "plan-fail");
    storedemo::test::ScopedStoreTestDir temp_dir("transfer_write_plan_fail");
    const auto source_path = WritePayloadFile(temp_dir.root(), payload);

    storedemo::UploadObjectRequest request;
    request.request_id = "upload-plan-fail";
    request.cluster_id = "cluster-plan-fail";
    request.bucket = "bucket-plan";
    request.object_key = "objects/plan-fail.bin";
    request.object_id = "obj-plan-fail";
    request.source_path = source_path;
    request.chunk_size = 32;
    request.concurrency = 1;
    request.desired_replica_count = 3;
    request.minimum_successful_writes = 2;
    request.client_time_unix_ms = 1714002000100ULL;

    auto session = transfer.StartUploadSession(request);
    auto reader = storedemo::CreateFileTransferChunkReader();
    auto checksum_state = storedemo::CreateTransferChecksumState();
    const auto result = session->Execute(*reader, *checksum_state);

    EXPECT_EQ(result.status, storedemo::ObjectTransferStatusCode::kStorageRejected);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(metadata_server.service().commit_calls(), 0U);
    EXPECT_TRUE(storage_client->writes.empty());
}
