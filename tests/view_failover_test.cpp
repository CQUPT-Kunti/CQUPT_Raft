#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "view/view_client.h"
#include "view/view_registry.h"
#include "view/view_service_impl.h"

namespace
{
    using viewdemo::ClusterId;
    using viewdemo::DiscoverMetadataRequest;
    using viewdemo::DiscoverStorageRequest;
    using viewdemo::MetadataLeaderHint;
    using viewdemo::MetadataMembershipObservedState;
    using viewdemo::MetadataNodeObservation;
    using viewdemo::MetadataRaftObservedRole;
    using viewdemo::NodeRegistration;
    using viewdemo::RegisterNodeRequest;
    using viewdemo::ViewNodeClient;
    using viewdemo::ViewNodeClientConfig;
    using viewdemo::ViewNodeDiskPressure;
    using viewdemo::ViewNodeHealth;
    using viewdemo::ViewNodeLivenessState;
    using viewdemo::ViewNodeRegistry;
    using viewdemo::ViewNodeServiceImpl;
    using viewdemo::ViewNodeServiceImplConfig;
    using viewdemo::ViewNodeSnapshot;
    using viewdemo::ViewNodeType;
    using viewdemo::ViewRegistryConfig;
    using viewdemo::ViewRegistryDiagnostic;
    using viewdemo::ViewRegistryIssueCode;
    using viewdemo::ViewRegistryStatusCode;

    constexpr std::uint64_t kNow = 200;
    const ClusterId kClusterId = "cluster_009_view_failover";

    NodeRegistration MakeRegistration(const ViewNodeType node_type,
                                      std::string node_id,
                                      const std::uint16_t port,
                                      const std::uint64_t observed_at_unix_ms)
    {
        NodeRegistration registration;
        registration.cluster_id = kClusterId;
        registration.node_id = std::move(node_id);
        registration.node_type = node_type;
        registration.endpoint = "127.0.0.1:" + std::to_string(port);
        registration.control_plane_endpoint =
            "127.0.0.1:" +
            std::to_string(static_cast<std::uint32_t>(port) + 1000);
        registration.data_plane_endpoint =
            "127.0.0.1:" +
            std::to_string(static_cast<std::uint32_t>(port) + 2000);
        registration.data_dir_fingerprint =
            "fingerprint-" + registration.node_id;
        registration.observed_at_unix_ms = observed_at_unix_ms;
        registration.failure_domain.zone = "zone-a";
        registration.failure_domain.rack = "rack-1";
        registration.health.health = ViewNodeHealth::kHealthy;
        registration.health.disk_pressure = ViewNodeDiskPressure::kLow;
        registration.capacity.total_capacity_bytes =
            node_type == ViewNodeType::kStorage ? 1'024 : 0;
        registration.capacity.used_capacity_bytes =
            node_type == ViewNodeType::kStorage ? 256 : 0;
        registration.capacity.available_capacity_bytes =
            node_type == ViewNodeType::kStorage ? 768 : 0;
        registration.capacity.chunk_count =
            node_type == ViewNodeType::kStorage ? 4 : 0;
        registration.load.active_reads = 1;
        registration.load.active_writes = 2;
        registration.load.queued_ops = 3;
        return registration;
    }

    MetadataNodeObservation MakeMetadataObservation(
        const MetadataRaftObservedRole role,
        const MetadataMembershipObservedState membership_state,
        const std::uint64_t membership_epoch,
        const std::uint64_t observed_term,
        std::optional<MetadataLeaderHint> leader_hint = std::nullopt)
    {
        MetadataNodeObservation observation;
        observation.raft_role = role;
        observation.membership_state = membership_state;
        observation.leader_hint = std::move(leader_hint);
        observation.observed_term = observed_term;
        observation.commit_index = observed_term * 10;
        observation.membership_epoch = membership_epoch;
        return observation;
    }

    RegisterNodeRequest MakeRegisterRequest(NodeRegistration registration,
                                            const std::string &request_id)
    {
        RegisterNodeRequest request;
        request.request_id = request_id;
        request.registration = std::move(registration);
        return request;
    }

    class RunningViewNodeDiscoveryService
    {
    public:
        explicit RunningViewNodeDiscoveryService(
            ViewRegistryConfig registry_config = {},
            const std::uint64_t now_unix_ms = kNow)
            : now_unix_ms_(now_unix_ms),
              registry_(std::make_shared<ViewNodeRegistry>(registry_config)),
              service_(registry_,
                       ViewNodeServiceImplConfig{
                           .now_unix_ms = [this]() {
                               return now_unix_ms_;
                           }})
        {
            grpc::ServerBuilder builder;
            builder.AddListeningPort("127.0.0.1:0",
                                     grpc::InsecureServerCredentials(),
                                     &selected_port_);
            builder.RegisterService(&service_);
            server_ = builder.BuildAndStart();
            if (server_ == nullptr || selected_port_ <= 0)
            {
                throw std::runtime_error(
                    "failed to start ViewNode failover integration service");
            }

            endpoint_ = "127.0.0.1:" + std::to_string(selected_port_);
            auto channel = grpc::CreateChannel(endpoint_,
                                               grpc::InsecureChannelCredentials());
            if (!channel->WaitForConnected(std::chrono::system_clock::now() +
                                           std::chrono::seconds(5)))
            {
                throw std::runtime_error(
                    "ViewNode failover integration channel did not connect");
            }

            client_ = std::make_unique<ViewNodeClient>(
                std::move(channel),
                endpoint_,
                ViewNodeClientConfig{
                    .register_timeout = std::chrono::seconds(5),
                    .heartbeat_timeout = std::chrono::seconds(5),
                    .discovery_timeout = std::chrono::seconds(5),
                    .cluster_view_timeout = std::chrono::seconds(5),
                    .peer_sync_timeout = std::chrono::seconds(5),
                    .wait_for_ready = true,
                });
        }

        ~RunningViewNodeDiscoveryService()
        {
            Stop();
        }

        RunningViewNodeDiscoveryService(
            const RunningViewNodeDiscoveryService &) = delete;
        RunningViewNodeDiscoveryService &operator=(
            const RunningViewNodeDiscoveryService &) = delete;

        [[nodiscard]] ViewNodeClient &client() const
        {
            return *client_;
        }

        void set_now_unix_ms(const std::uint64_t now_unix_ms)
        {
            now_unix_ms_ = now_unix_ms;
        }

        void Stop()
        {
            if (server_ != nullptr)
            {
                server_->Shutdown();
                server_->Wait();
                server_.reset();
            }
        }

    private:
        std::uint64_t now_unix_ms_{kNow};
        std::shared_ptr<ViewNodeRegistry> registry_;
        ViewNodeServiceImpl service_;
        int selected_port_{0};
        std::string endpoint_;
        std::unique_ptr<grpc::Server> server_;
        std::unique_ptr<ViewNodeClient> client_;
    };

    const ViewNodeSnapshot *FindSnapshotByNodeId(
        const std::vector<ViewNodeSnapshot> &snapshots,
        const std::string &node_id)
    {
        for (const auto &snapshot : snapshots)
        {
            if (snapshot.node_id == node_id)
            {
                return &snapshot;
            }
        }
        return nullptr;
    }

    bool ContainsDiagnosticCode(
        const std::vector<ViewRegistryDiagnostic> &diagnostics,
        const ViewRegistryIssueCode code)
    {
        for (const auto &diagnostic : diagnostics)
        {
            if (diagnostic.code == code)
            {
                return true;
            }
        }
        return false;
    }

    void RegisterNodeOrAssert(RunningViewNodeDiscoveryService &service,
                              NodeRegistration registration,
                              const std::string &request_id)
    {
        const auto result = service.client().RegisterNode(
            MakeRegisterRequest(std::move(registration), request_id));
        ASSERT_TRUE(result.transport_ok());
        ASSERT_EQ(result.result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(result.result.created);
    }

    TEST(ViewFailoverTest,
         SurvivingViewNodeRemainsAvailableWhenFailoverLeavesPartialRegistry)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);

        RunningViewNodeDiscoveryService primary(config, 180);
        RunningViewNodeDiscoveryService survivor(config, 180);

        auto primary_view =
            MakeRegistration(ViewNodeType::kView, "view-failover-primary-2", 9731, 100);
        auto survivor_view =
            MakeRegistration(ViewNodeType::kView, "view-failover-survivor-2", 9732, 180);
        auto metadata =
            MakeRegistration(ViewNodeType::kMetadata, "meta-failover-2", 9733, 180);
        metadata.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kVoter,
            10,
            16,
            MetadataLeaderHint{.node_id = "meta-failover-2",
                               .raft_id = std::optional<std::int32_t>{4},
                               .endpoint = metadata.endpoint,
                               .observed_term = 16,
                               .observed_at_unix_ms = 180});

        RegisterNodeOrAssert(primary,
                             primary_view,
                             "failover-partial-register-primary-view-on-primary");
        RegisterNodeOrAssert(primary,
                             survivor_view,
                             "failover-partial-register-survivor-view-on-primary");
        RegisterNodeOrAssert(primary,
                             metadata,
                             "failover-partial-register-meta-on-primary");

        RegisterNodeOrAssert(survivor,
                             primary_view,
                             "failover-partial-register-primary-view-on-survivor");
        RegisterNodeOrAssert(survivor,
                             survivor_view,
                             "failover-partial-register-survivor-view-on-survivor");
        RegisterNodeOrAssert(survivor,
                             metadata,
                             "failover-partial-register-meta-on-survivor");

        primary.Stop();
        survivor.set_now_unix_ms(191);

        viewdemo::GetClusterViewRequest cluster_request;
        cluster_request.request_id = "failover-partial-cluster-view";
        cluster_request.cluster_id = kClusterId;
        cluster_request.include_dead_nodes = true;
        cluster_request.include_warnings = true;

        const auto cluster_result = survivor.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        ASSERT_EQ(cluster_result.result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(cluster_result.result.snapshot.view_nodes.size(), 2U);
        ASSERT_EQ(cluster_result.result.snapshot.metadata_nodes.size(), 1U);
        EXPECT_TRUE(cluster_result.result.snapshot.storage_nodes.empty());
        EXPECT_TRUE(ContainsDiagnosticCode(cluster_result.result.snapshot.diagnostics,
                                           ViewRegistryIssueCode::kLivenessExcluded));

        const auto *survivor_snapshot = FindSnapshotByNodeId(
            cluster_result.result.snapshot.view_nodes,
            "view-failover-survivor-2");
        ASSERT_NE(survivor_snapshot, nullptr);
        EXPECT_EQ(survivor_snapshot->liveness, ViewNodeLivenessState::kLive);
        EXPECT_EQ(survivor_snapshot->health.health, ViewNodeHealth::kHealthy);

        const auto *primary_snapshot = FindSnapshotByNodeId(
            cluster_result.result.snapshot.view_nodes,
            "view-failover-primary-2");
        ASSERT_NE(primary_snapshot, nullptr);
        EXPECT_EQ(primary_snapshot->liveness, ViewNodeLivenessState::kDead);

        DiscoverMetadataRequest metadata_request;
        metadata_request.request_id = "failover-partial-discover-metadata";
        metadata_request.cluster_id = kClusterId;
        metadata_request.prefer_leader = true;
        metadata_request.live_only = true;

        const auto metadata_result =
            survivor.client().DiscoverMetadata(metadata_request);
        ASSERT_TRUE(metadata_result.transport_ok());
        ASSERT_EQ(metadata_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(metadata_result.result.metadata_nodes.size(), 1U);

        DiscoverStorageRequest storage_request;
        storage_request.request_id = "failover-partial-discover-storage";
        storage_request.cluster_id = kClusterId;
        storage_request.live_only = true;
        storage_request.require_writable = true;

        const auto storage_result = survivor.client().DiscoverStorage(storage_request);
        ASSERT_TRUE(storage_result.transport_ok());
        EXPECT_EQ(storage_result.result.summary.status,
                  ViewRegistryStatusCode::kNotFound);
        EXPECT_NE(storage_result.result.summary.status,
                  ViewRegistryStatusCode::kServiceUnavailable);
        EXPECT_TRUE(storage_result.result.storage_nodes.empty());
    }

    TEST(ViewFailoverTest,
         SurvivingViewNodeCanStayDegradedWithoutBecomingUnavailable)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);

        RunningViewNodeDiscoveryService primary(config, 180);
        RunningViewNodeDiscoveryService survivor(config, 180);

        auto primary_view =
            MakeRegistration(ViewNodeType::kView, "view-failover-primary-3", 9741, 100);
        auto survivor_view =
            MakeRegistration(ViewNodeType::kView, "view-failover-survivor-3", 9742, 180);
        survivor_view.health.health = ViewNodeHealth::kDegraded;

        auto metadata =
            MakeRegistration(ViewNodeType::kMetadata, "meta-failover-3", 9743, 180);
        metadata.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kVoter,
            11,
            17,
            MetadataLeaderHint{.node_id = "meta-failover-3",
                               .raft_id = std::optional<std::int32_t>{5},
                               .endpoint = metadata.endpoint,
                               .observed_term = 17,
                               .observed_at_unix_ms = 180});

        auto storage =
            MakeRegistration(ViewNodeType::kStorage, "store-failover-3", 9744, 181);
        storage.failure_domain.zone = "zone-f";
        storage.failure_domain.rack = "rack-3";
        storage.capacity.total_capacity_bytes = 16'384;
        storage.capacity.used_capacity_bytes = 8'192;
        storage.capacity.available_capacity_bytes = 8'192;

        RegisterNodeOrAssert(primary,
                             primary_view,
                             "failover-degraded-register-primary-view-on-primary");
        RegisterNodeOrAssert(primary,
                             survivor_view,
                             "failover-degraded-register-survivor-view-on-primary");
        RegisterNodeOrAssert(primary,
                             metadata,
                             "failover-degraded-register-meta-on-primary");
        RegisterNodeOrAssert(primary,
                             storage,
                             "failover-degraded-register-store-on-primary");

        RegisterNodeOrAssert(survivor,
                             primary_view,
                             "failover-degraded-register-primary-view-on-survivor");
        RegisterNodeOrAssert(survivor,
                             survivor_view,
                             "failover-degraded-register-survivor-view-on-survivor");
        RegisterNodeOrAssert(survivor,
                             metadata,
                             "failover-degraded-register-meta-on-survivor");
        RegisterNodeOrAssert(survivor,
                             storage,
                             "failover-degraded-register-store-on-survivor");

        primary.Stop();
        survivor.set_now_unix_ms(191);

        viewdemo::GetClusterViewRequest cluster_request;
        cluster_request.request_id = "failover-degraded-cluster-view";
        cluster_request.cluster_id = kClusterId;
        cluster_request.include_dead_nodes = true;
        cluster_request.include_warnings = true;

        const auto cluster_result = survivor.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        ASSERT_EQ(cluster_result.result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(cluster_result.result.snapshot.view_nodes.size(), 2U);
        ASSERT_EQ(cluster_result.result.snapshot.metadata_nodes.size(), 1U);
        ASSERT_EQ(cluster_result.result.snapshot.storage_nodes.size(), 1U);

        const auto *survivor_snapshot = FindSnapshotByNodeId(
            cluster_result.result.snapshot.view_nodes,
            "view-failover-survivor-3");
        ASSERT_NE(survivor_snapshot, nullptr);
        EXPECT_EQ(survivor_snapshot->liveness, ViewNodeLivenessState::kLive);
        EXPECT_EQ(survivor_snapshot->health.health, ViewNodeHealth::kDegraded);
        EXPECT_NE(survivor_snapshot->health.health, ViewNodeHealth::kUnavailable);

        DiscoverMetadataRequest metadata_request;
        metadata_request.request_id = "failover-degraded-discover-metadata";
        metadata_request.cluster_id = kClusterId;
        metadata_request.prefer_leader = true;
        metadata_request.live_only = true;

        const auto metadata_result =
            survivor.client().DiscoverMetadata(metadata_request);
        ASSERT_TRUE(metadata_result.transport_ok());
        ASSERT_EQ(metadata_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(metadata_result.result.metadata_nodes.size(), 1U);

        DiscoverStorageRequest storage_request;
        storage_request.request_id = "failover-degraded-discover-storage";
        storage_request.cluster_id = kClusterId;
        storage_request.live_only = true;
        storage_request.minimum_available_capacity_bytes = 8'192;
        storage_request.require_writable = true;
        storage_request.zone = "zone-f";
        storage_request.rack = "rack-3";

        const auto storage_result = survivor.client().DiscoverStorage(storage_request);
        ASSERT_TRUE(storage_result.transport_ok());
        ASSERT_EQ(storage_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(storage_result.result.storage_nodes.size(), 1U);
        EXPECT_EQ(storage_result.result.storage_nodes[0].node_id,
                  "store-failover-3");
    }
} // namespace
