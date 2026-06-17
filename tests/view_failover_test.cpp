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
    using viewdemo::GetClusterViewRequest;
    using viewdemo::HeartbeatNodeRequest;
    using viewdemo::MetadataLeaderHint;
    using viewdemo::MetadataMembershipObservedState;
    using viewdemo::MetadataNodeObservation;
    using viewdemo::MetadataRaftObservedRole;
    using viewdemo::NodeRegistration;
    using viewdemo::PullPeerViewSnapshotRequest;
    using viewdemo::PushPeerViewSnapshotRequest;
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

        [[nodiscard]] const std::string &endpoint() const
        {
            return endpoint_;
        }

        void set_now_unix_ms(const std::uint64_t now_unix_ms)
        {
            now_unix_ms_ = now_unix_ms;
        }

        [[nodiscard]] viewdemo::HeartbeatNodeResult RefreshSelfNode(
            const HeartbeatNodeRequest &request) const
        {
            return registry_->RefreshSelfNode(request);
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

    HeartbeatNodeRequest MakeHeartbeatRequest(const ViewNodeType node_type,
                                              std::string node_id,
                                              const std::uint16_t port,
                                              const std::uint64_t sequence,
                                              const std::uint64_t observed_at_unix_ms)
    {
        HeartbeatNodeRequest request;
        request.request_id =
            "heartbeat-" + node_id + "-" + std::to_string(sequence);
        request.cluster_id = kClusterId;
        request.node_id = std::move(node_id);
        request.node_type = node_type;
        request.sequence = sequence;
        request.observation = MakeRegistration(node_type,
                                               request.node_id,
                                               port,
                                               observed_at_unix_ms);
        return request;
    }

    HeartbeatNodeRequest MakeSelfRefreshRequest(
        std::string node_id,
        const std::uint16_t port,
        std::string incarnation_id,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        HeartbeatNodeRequest request = MakeHeartbeatRequest(ViewNodeType::kView,
                                                            node_id,
                                                            port,
                                                            sequence,
                                                            observed_at_unix_ms);
        request.request_id = "view-node-self-refresh-" + request.node_id + "-" +
                             incarnation_id + "-" +
                             std::to_string(sequence);
        request.incarnation_id = std::move(incarnation_id);
        return request;
    }

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

    void ExpectObservedStateFacts(const ViewNodeSnapshot &snapshot,
                                  const std::string &incarnation_id,
                                  const std::uint64_t sequence,
                                  const std::uint64_t observed_at_unix_ms)
    {
        EXPECT_EQ(snapshot.observed_state.incarnation_id, incarnation_id);
        EXPECT_EQ(snapshot.observed_state.sequence, sequence);
        EXPECT_EQ(snapshot.observed_state.observed_at_unix_ms,
                  observed_at_unix_ms);
        EXPECT_EQ(snapshot.incarnation_id, snapshot.observed_state.incarnation_id);
        EXPECT_EQ(snapshot.last_sequence, snapshot.observed_state.sequence);
        EXPECT_EQ(snapshot.last_seen_unix_ms,
                  snapshot.observed_state.observed_at_unix_ms);
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

    void RefreshSelfNodeOrAssert(RunningViewNodeDiscoveryService &service,
                                 std::string node_id,
                                 const std::uint16_t port,
                                 std::string incarnation_id,
                                 const std::uint64_t sequence,
                                 const std::uint64_t observed_at_unix_ms)
    {
        const auto result = service.RefreshSelfNode(
            MakeSelfRefreshRequest(std::move(node_id),
                                   port,
                                   std::move(incarnation_id),
                                   sequence,
                                   observed_at_unix_ms));
        ASSERT_EQ(result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(result.applied);
    }

    void HeartbeatNodeOrAssert(RunningViewNodeDiscoveryService &service,
                               HeartbeatNodeRequest request)
    {
        const auto result = service.client().HeartbeatNode(std::move(request));
        ASSERT_TRUE(result.transport_ok());
        ASSERT_EQ(result.result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(result.result.applied);
    }

    void SyncPeerSnapshotOrAssert(RunningViewNodeDiscoveryService &source,
                                  RunningViewNodeDiscoveryService &peer,
                                  const std::string &request_prefix)
    {
        PullPeerViewSnapshotRequest pull_request;
        pull_request.request_id = request_prefix + "-pull";
        pull_request.cluster_id = kClusterId;
        pull_request.include_dead_nodes = true;
        pull_request.include_warnings = true;

        const auto pull_result = source.client().PullPeerViewSnapshot(pull_request);
        ASSERT_TRUE(pull_result.transport_ok());
        ASSERT_EQ(pull_result.result.summary.status, ViewRegistryStatusCode::kOk);

        PushPeerViewSnapshotRequest push_request;
        push_request.request_id = request_prefix + "-push";
        push_request.cluster_id = kClusterId;
        push_request.snapshot = pull_result.result.snapshot;

        const auto push_result = peer.client().PushPeerViewSnapshot(push_request);
        ASSERT_TRUE(push_result.transport_ok());
        ASSERT_EQ(push_result.result.summary.status, ViewRegistryStatusCode::kOk);
        EXPECT_EQ(push_result.result.received_node_count,
                  pull_result.result.snapshot.view_nodes.size() +
                      pull_result.result.snapshot.metadata_nodes.size() +
                      pull_result.result.snapshot.storage_nodes.size());
        EXPECT_TRUE(ContainsDiagnosticCode(push_result.result.diagnostics,
                                           ViewRegistryIssueCode::kNonAuthorityBoundary));
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

    TEST(ViewFailoverTest,
         MultiViewSelfRefreshAndPeerSyncPreserveAvailabilityAcrossFailover)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);

        RunningViewNodeDiscoveryService primary(config, 180);
        RunningViewNodeDiscoveryService survivor(config, 180);

        auto primary_view =
            MakeRegistration(ViewNodeType::kView, "view-multi-primary-1", 9751, 180);
        auto survivor_view_stale =
            MakeRegistration(ViewNodeType::kView, "view-multi-survivor-1", 9752, 100);
        auto survivor_view_live =
            MakeRegistration(ViewNodeType::kView, "view-multi-survivor-1", 9752, 180);
        auto metadata =
            MakeRegistration(ViewNodeType::kMetadata, "meta-multi-1", 9753, 180);
        metadata.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kVoter,
            12,
            22,
            MetadataLeaderHint{.node_id = "meta-multi-1",
                               .raft_id = std::optional<std::int32_t>{6},
                               .endpoint = metadata.endpoint,
                               .observed_term = 22,
                               .observed_at_unix_ms = 180});

        auto storage =
            MakeRegistration(ViewNodeType::kStorage, "store-multi-1", 9754, 181);
        storage.failure_domain.zone = "zone-multi";
        storage.failure_domain.rack = "rack-multi";
        storage.capacity.total_capacity_bytes = 32'768;
        storage.capacity.used_capacity_bytes = 8'192;
        storage.capacity.available_capacity_bytes = 24'576;

        RegisterNodeOrAssert(primary,
                             primary_view,
                             "multi-failover-register-primary-view-on-primary");
        RegisterNodeOrAssert(primary,
                             survivor_view_stale,
                             "multi-failover-register-stale-survivor-view-on-primary");
        RegisterNodeOrAssert(primary,
                             metadata,
                             "multi-failover-register-meta-on-primary");
        RegisterNodeOrAssert(primary,
                             storage,
                             "multi-failover-register-store-on-primary");

        RegisterNodeOrAssert(survivor,
                             survivor_view_live,
                             "multi-failover-register-survivor-view-on-survivor");
        RefreshSelfNodeOrAssert(
            primary,
            "view-multi-primary-1",
            9751,
            "view-multi-primary-1:boot:180000000:51:1",
            5,
            180);
        RefreshSelfNodeOrAssert(
            survivor,
            "view-multi-survivor-1",
            9752,
            "view-multi-survivor-1:boot:180000000:52:1",
            7,
            180);

        SyncPeerSnapshotOrAssert(primary,
                                 survivor,
                                 "multi-failover-primary-to-survivor");

        GetClusterViewRequest initial_cluster_request;
        initial_cluster_request.request_id = "multi-failover-initial-cluster";
        initial_cluster_request.cluster_id = kClusterId;
        initial_cluster_request.include_dead_nodes = true;
        initial_cluster_request.include_warnings = true;

        auto initial_cluster = survivor.client().GetClusterView(initial_cluster_request);
        ASSERT_TRUE(initial_cluster.transport_ok());
        ASSERT_EQ(initial_cluster.result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(initial_cluster.result.snapshot.view_nodes.size(), 2U);
        ASSERT_EQ(initial_cluster.result.snapshot.metadata_nodes.size(), 1U);
        ASSERT_EQ(initial_cluster.result.snapshot.storage_nodes.size(), 1U);

        const auto *initial_survivor_snapshot = FindSnapshotByNodeId(
            initial_cluster.result.snapshot.view_nodes,
            "view-multi-survivor-1");
        ASSERT_NE(initial_survivor_snapshot, nullptr);
        EXPECT_EQ(initial_survivor_snapshot->liveness, ViewNodeLivenessState::kLive);
        EXPECT_NE(initial_survivor_snapshot->health.health,
                  ViewNodeHealth::kUnavailable);
        ExpectObservedStateFacts(*initial_survivor_snapshot,
                                 "view-multi-survivor-1:boot:180000000:52:1",
                                 7U,
                                 180U);

        const auto *initial_primary_snapshot = FindSnapshotByNodeId(
            initial_cluster.result.snapshot.view_nodes,
            "view-multi-primary-1");
        ASSERT_NE(initial_primary_snapshot, nullptr);
        EXPECT_EQ(initial_primary_snapshot->liveness, ViewNodeLivenessState::kLive);
        ExpectObservedStateFacts(*initial_primary_snapshot,
                                 "view-multi-primary-1:boot:180000000:51:1",
                                 5U,
                                 180U);

        SyncPeerSnapshotOrAssert(survivor,
                                 primary,
                                 "multi-failover-survivor-to-primary");

        primary.Stop();
        survivor.set_now_unix_ms(271);

        RefreshSelfNodeOrAssert(
            survivor,
            "view-multi-survivor-1",
            9752,
            "view-multi-survivor-1:boot:180000000:52:1",
            8,
            271);

        auto metadata_heartbeat = MakeHeartbeatRequest(ViewNodeType::kMetadata,
                                                       "meta-multi-1",
                                                       9753,
                                                       9,
                                                       271);
        metadata_heartbeat.incarnation_id = "meta-multi-1:boot:271000000:61:1";
        metadata_heartbeat.observation.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kVoter,
            12,
            23,
            MetadataLeaderHint{.node_id = "meta-multi-1",
                               .raft_id = std::optional<std::int32_t>{6},
                               .endpoint = metadata.endpoint,
                               .observed_term = 23,
                               .observed_at_unix_ms = 271});
        HeartbeatNodeOrAssert(survivor, std::move(metadata_heartbeat));

        auto storage_heartbeat = MakeHeartbeatRequest(ViewNodeType::kStorage,
                                                      "store-multi-1",
                                                      9754,
                                                      10,
                                                      271);
        storage_heartbeat.incarnation_id = "store-multi-1:boot:271000000:62:1";
        storage_heartbeat.observation.health.health = ViewNodeHealth::kHealthy;
        storage_heartbeat.observation.health.disk_pressure =
            ViewNodeDiskPressure::kLow;
        storage_heartbeat.observation.failure_domain.zone = "zone-multi";
        storage_heartbeat.observation.failure_domain.rack = "rack-multi";
        storage_heartbeat.observation.capacity.total_capacity_bytes = 32'768;
        storage_heartbeat.observation.capacity.used_capacity_bytes = 10'240;
        storage_heartbeat.observation.capacity.available_capacity_bytes = 22'528;
        HeartbeatNodeOrAssert(survivor, std::move(storage_heartbeat));

        GetClusterViewRequest failover_cluster_request;
        failover_cluster_request.request_id = "multi-failover-cluster";
        failover_cluster_request.cluster_id = kClusterId;
        failover_cluster_request.include_dead_nodes = true;
        failover_cluster_request.include_warnings = true;

        const auto failover_cluster =
            survivor.client().GetClusterView(failover_cluster_request);
        ASSERT_TRUE(failover_cluster.transport_ok());
        ASSERT_EQ(failover_cluster.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(failover_cluster.result.snapshot.view_nodes.size(), 2U);
        ASSERT_EQ(failover_cluster.result.snapshot.metadata_nodes.size(), 1U);
        ASSERT_EQ(failover_cluster.result.snapshot.storage_nodes.size(), 1U);

        const auto *survivor_snapshot = FindSnapshotByNodeId(
            failover_cluster.result.snapshot.view_nodes,
            "view-multi-survivor-1");
        ASSERT_NE(survivor_snapshot, nullptr);
        EXPECT_EQ(survivor_snapshot->liveness, ViewNodeLivenessState::kLive);
        EXPECT_NE(survivor_snapshot->health.health, ViewNodeHealth::kUnavailable);
        ExpectObservedStateFacts(*survivor_snapshot,
                                 "view-multi-survivor-1:boot:180000000:52:1",
                                 8U,
                                 271U);

        const auto *dead_primary_snapshot = FindSnapshotByNodeId(
            failover_cluster.result.snapshot.view_nodes,
            "view-multi-primary-1");
        ASSERT_NE(dead_primary_snapshot, nullptr);
        EXPECT_EQ(dead_primary_snapshot->liveness, ViewNodeLivenessState::kDead);
        EXPECT_NE(dead_primary_snapshot->health.health,
                  ViewNodeHealth::kUnavailable);

        DiscoverMetadataRequest metadata_request;
        metadata_request.request_id = "multi-failover-discover-metadata";
        metadata_request.cluster_id = kClusterId;
        metadata_request.prefer_leader = true;
        metadata_request.live_only = true;

        const auto metadata_result =
            survivor.client().DiscoverMetadata(metadata_request);
        ASSERT_TRUE(metadata_result.transport_ok());
        ASSERT_EQ(metadata_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(metadata_result.result.metadata_nodes.size(), 1U);
        ASSERT_TRUE(metadata_result.result.metadata_nodes[0].metadata.has_value());
        EXPECT_EQ(
            metadata_result.result.metadata_nodes[0].metadata->membership_state,
            MetadataMembershipObservedState::kVoter);

        DiscoverStorageRequest storage_request;
        storage_request.request_id = "multi-failover-discover-storage";
        storage_request.cluster_id = kClusterId;
        storage_request.live_only = true;
        storage_request.require_writable = true;
        storage_request.zone = "zone-multi";
        storage_request.rack = "rack-multi";

        const auto storage_result = survivor.client().DiscoverStorage(storage_request);
        ASSERT_TRUE(storage_result.transport_ok());
        ASSERT_EQ(storage_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(storage_result.result.storage_nodes.size(), 1U);
        EXPECT_EQ(storage_result.result.storage_nodes[0].node_id, "store-multi-1");
        EXPECT_EQ(storage_result.result.storage_nodes[0].health.health,
                  ViewNodeHealth::kHealthy);
    }

    TEST(ViewFailoverTest,
         RecoveredViewNodePeerSyncReconvergesWithoutOverwritingLiveState)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);

        RunningViewNodeDiscoveryService primary(config, 180);
        RunningViewNodeDiscoveryService survivor(config, 180);

        auto primary_view =
            MakeRegistration(ViewNodeType::kView, "view-recover-primary-1", 9761, 180);
        auto survivor_view_stale =
            MakeRegistration(ViewNodeType::kView, "view-recover-survivor-1", 9762, 100);
        auto survivor_view_live =
            MakeRegistration(ViewNodeType::kView, "view-recover-survivor-1", 9762, 180);
        auto metadata =
            MakeRegistration(ViewNodeType::kMetadata, "meta-recover-1", 9763, 180);
        metadata.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kVoter,
            18,
            31,
            MetadataLeaderHint{.node_id = "meta-recover-1",
                               .raft_id = std::optional<std::int32_t>{7},
                               .endpoint = metadata.endpoint,
                               .observed_term = 31,
                               .observed_at_unix_ms = 180});

        auto storage =
            MakeRegistration(ViewNodeType::kStorage, "store-recover-1", 9764, 181);
        storage.failure_domain.zone = "zone-recover";
        storage.failure_domain.rack = "rack-recover";
        storage.capacity.total_capacity_bytes = 65'536;
        storage.capacity.used_capacity_bytes = 20'480;
        storage.capacity.available_capacity_bytes = 45'056;

        RegisterNodeOrAssert(primary,
                             primary_view,
                             "recover-register-primary-view-on-primary");
        RegisterNodeOrAssert(primary,
                             survivor_view_stale,
                             "recover-register-stale-survivor-view-on-primary");
        RegisterNodeOrAssert(primary,
                             metadata,
                             "recover-register-meta-on-primary");
        RegisterNodeOrAssert(primary,
                             storage,
                             "recover-register-store-on-primary");

        RegisterNodeOrAssert(survivor,
                             survivor_view_live,
                             "recover-register-survivor-view-on-survivor");
        RefreshSelfNodeOrAssert(
            primary,
            "view-recover-primary-1",
            9761,
            "view-recover-primary-1:boot:180000000:71:1",
            4,
            180);
        RefreshSelfNodeOrAssert(
            survivor,
            "view-recover-survivor-1",
            9762,
            "view-recover-survivor-1:boot:180000000:72:1",
            6,
            180);

        SyncPeerSnapshotOrAssert(primary,
                                 survivor,
                                 "recover-primary-to-survivor");

        primary.Stop();
        survivor.set_now_unix_ms(271);
        RefreshSelfNodeOrAssert(
            survivor,
            "view-recover-survivor-1",
            9762,
            "view-recover-survivor-1:boot:180000000:72:1",
            7,
            271);

        auto metadata_heartbeat = MakeHeartbeatRequest(ViewNodeType::kMetadata,
                                                       "meta-recover-1",
                                                       9763,
                                                       11,
                                                       271);
        metadata_heartbeat.incarnation_id =
            "meta-recover-1:boot:271000000:81:1";
        metadata_heartbeat.observation.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kVoter,
            19,
            32,
            MetadataLeaderHint{.node_id = "meta-recover-1",
                               .raft_id = std::optional<std::int32_t>{7},
                               .endpoint = metadata.endpoint,
                               .observed_term = 32,
                               .observed_at_unix_ms = 271});
        HeartbeatNodeOrAssert(survivor, std::move(metadata_heartbeat));

        auto storage_heartbeat = MakeHeartbeatRequest(ViewNodeType::kStorage,
                                                      "store-recover-1",
                                                      9764,
                                                      12,
                                                      271);
        storage_heartbeat.incarnation_id =
            "store-recover-1:boot:271000000:82:1";
        storage_heartbeat.observation.failure_domain.zone = "zone-recover";
        storage_heartbeat.observation.failure_domain.rack = "rack-recover";
        storage_heartbeat.observation.capacity.total_capacity_bytes = 65'536;
        storage_heartbeat.observation.capacity.used_capacity_bytes = 24'576;
        storage_heartbeat.observation.capacity.available_capacity_bytes = 40'960;
        storage_heartbeat.observation.load.active_reads = 7;
        storage_heartbeat.observation.load.active_writes = 5;
        storage_heartbeat.observation.load.queued_ops = 9;
        HeartbeatNodeOrAssert(survivor, std::move(storage_heartbeat));

        RunningViewNodeDiscoveryService recovered_primary(config, 280);
        auto recovered_primary_view =
            MakeRegistration(ViewNodeType::kView, "view-recover-primary-1", 9761, 280);
        RegisterNodeOrAssert(recovered_primary,
                             recovered_primary_view,
                             "recover-register-primary-view-on-recovered-primary");
        RefreshSelfNodeOrAssert(
            recovered_primary,
            "view-recover-primary-1",
            9761,
            "view-recover-primary-1:boot:280000000:91:1",
            1,
            280);

        SyncPeerSnapshotOrAssert(survivor,
                                 recovered_primary,
                                 "recover-survivor-to-recovered-primary");

        GetClusterViewRequest recovered_cluster_request;
        recovered_cluster_request.request_id = "recover-cluster-on-primary";
        recovered_cluster_request.cluster_id = kClusterId;
        recovered_cluster_request.include_dead_nodes = true;
        recovered_cluster_request.include_warnings = true;

        const auto recovered_cluster =
            recovered_primary.client().GetClusterView(recovered_cluster_request);
        ASSERT_TRUE(recovered_cluster.transport_ok());
        ASSERT_EQ(recovered_cluster.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(recovered_cluster.result.snapshot.view_nodes.size(), 2U);
        ASSERT_EQ(recovered_cluster.result.snapshot.metadata_nodes.size(), 1U);
        ASSERT_EQ(recovered_cluster.result.snapshot.storage_nodes.size(), 1U);

        const auto *recovered_primary_snapshot = FindSnapshotByNodeId(
            recovered_cluster.result.snapshot.view_nodes,
            "view-recover-primary-1");
        ASSERT_NE(recovered_primary_snapshot, nullptr);
        EXPECT_EQ(recovered_primary_snapshot->liveness, ViewNodeLivenessState::kLive);
        EXPECT_NE(recovered_primary_snapshot->health.health,
                  ViewNodeHealth::kUnavailable);
        ExpectObservedStateFacts(*recovered_primary_snapshot,
                                 "view-recover-primary-1:boot:280000000:91:1",
                                 1U,
                                 280U);

        const auto *recovered_survivor_snapshot = FindSnapshotByNodeId(
            recovered_cluster.result.snapshot.view_nodes,
            "view-recover-survivor-1");
        ASSERT_NE(recovered_survivor_snapshot, nullptr);
        EXPECT_EQ(recovered_survivor_snapshot->liveness, ViewNodeLivenessState::kLive);
        ExpectObservedStateFacts(*recovered_survivor_snapshot,
                                 "view-recover-survivor-1:boot:180000000:72:1",
                                 7U,
                                 271U);

        recovered_primary.set_now_unix_ms(285);
        RefreshSelfNodeOrAssert(
            recovered_primary,
            "view-recover-primary-1",
            9761,
            "view-recover-primary-1:boot:280000000:91:1",
            2,
            285);
        survivor.set_now_unix_ms(285);
        SyncPeerSnapshotOrAssert(recovered_primary,
                                 survivor,
                                 "recover-recovered-primary-to-survivor");

        GetClusterViewRequest survivor_cluster_request;
        survivor_cluster_request.request_id = "recover-cluster-on-survivor";
        survivor_cluster_request.cluster_id = kClusterId;
        survivor_cluster_request.include_dead_nodes = true;
        survivor_cluster_request.include_warnings = true;

        const auto survivor_cluster =
            survivor.client().GetClusterView(survivor_cluster_request);
        ASSERT_TRUE(survivor_cluster.transport_ok());
        ASSERT_EQ(survivor_cluster.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(survivor_cluster.result.snapshot.view_nodes.size(), 2U);
        ASSERT_EQ(survivor_cluster.result.snapshot.metadata_nodes.size(), 1U);
        ASSERT_EQ(survivor_cluster.result.snapshot.storage_nodes.size(), 1U);

        const auto *live_primary_snapshot = FindSnapshotByNodeId(
            survivor_cluster.result.snapshot.view_nodes,
            "view-recover-primary-1");
        ASSERT_NE(live_primary_snapshot, nullptr);
        EXPECT_EQ(live_primary_snapshot->liveness, ViewNodeLivenessState::kLive);
        EXPECT_NE(live_primary_snapshot->health.health,
                  ViewNodeHealth::kUnavailable);
        ExpectObservedStateFacts(*live_primary_snapshot,
                                 "view-recover-primary-1:boot:280000000:91:1",
                                 2U,
                                 285U);

        DiscoverMetadataRequest metadata_request;
        metadata_request.request_id = "recover-discover-metadata";
        metadata_request.cluster_id = kClusterId;
        metadata_request.prefer_leader = true;
        metadata_request.live_only = true;

        const auto metadata_result =
            survivor.client().DiscoverMetadata(metadata_request);
        ASSERT_TRUE(metadata_result.transport_ok());
        ASSERT_EQ(metadata_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(metadata_result.result.metadata_nodes.size(), 1U);
        ASSERT_TRUE(metadata_result.result.metadata_nodes[0].metadata.has_value());
        EXPECT_EQ(metadata_result.result.metadata_nodes[0].metadata->membership_epoch,
                  19U);

        DiscoverStorageRequest storage_request;
        storage_request.request_id = "recover-discover-storage";
        storage_request.cluster_id = kClusterId;
        storage_request.live_only = true;
        storage_request.require_writable = true;
        storage_request.zone = "zone-recover";
        storage_request.rack = "rack-recover";

        const auto storage_result = survivor.client().DiscoverStorage(storage_request);
        ASSERT_TRUE(storage_result.transport_ok());
        ASSERT_EQ(storage_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(storage_result.result.storage_nodes.size(), 1U);
        EXPECT_EQ(storage_result.result.storage_nodes[0].node_id,
                  "store-recover-1");
        EXPECT_EQ(storage_result.result.storage_nodes[0].capacity
                      .available_capacity_bytes,
                  40'960U);
    }
} // namespace
