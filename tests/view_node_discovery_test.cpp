#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

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
    using viewdemo::RegisterNodeRequest;
    using viewdemo::ViewNodeDiskPressure;
    using viewdemo::ViewNodeHealth;
    using viewdemo::ViewNodeClient;
    using viewdemo::ViewNodeClientConfig;
    using viewdemo::ViewRegistryDiagnostic;
    using viewdemo::ViewNodeLivenessState;
    using viewdemo::ViewNodeRegistry;
    using viewdemo::ViewNodeServiceImpl;
    using viewdemo::ViewNodeServiceImplConfig;
    using viewdemo::ViewNodeType;
    using viewdemo::ViewRegistryConfig;
    using viewdemo::ViewRegistryIssueCode;
    using viewdemo::ViewRegistryStatusCode;

    constexpr std::uint64_t kNow = 200;
    const ClusterId kClusterId = "cluster_008";

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
            "127.0.0.1:" + std::to_string(static_cast<std::uint32_t>(port) + 1000);
        registration.data_plane_endpoint =
            "127.0.0.1:" + std::to_string(static_cast<std::uint32_t>(port) + 2000);
        registration.data_dir_fingerprint = "fingerprint-" + registration.node_id;
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
                    "failed to start ViewNode discovery integration service");
            }

            endpoint_ = "127.0.0.1:" + std::to_string(selected_port_);
            auto channel = grpc::CreateChannel(endpoint_,
                                               grpc::InsecureChannelCredentials());
            if (!channel->WaitForConnected(std::chrono::system_clock::now() +
                                           std::chrono::seconds(5)))
            {
                throw std::runtime_error(
                    "ViewNode discovery integration channel did not connect");
            }

            client_ = std::make_unique<ViewNodeClient>(
                std::move(channel),
                endpoint_,
                ViewNodeClientConfig{
                    .register_timeout = std::chrono::seconds(5),
                    .heartbeat_timeout = std::chrono::seconds(5),
                    .discovery_timeout = std::chrono::seconds(5),
                    .cluster_view_timeout = std::chrono::seconds(5),
                    .wait_for_ready = true,
                });
        }

        ~RunningViewNodeDiscoveryService()
        {
            if (server_ != nullptr)
            {
                server_->Shutdown();
                server_->Wait();
            }
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

    void RegisterNodeOrAssert(ViewNodeRegistry *registry,
                              const RegisterNodeRequest &request)
    {
        const auto result = registry->RegisterNode(request);
        ASSERT_EQ(result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(result.created);
        ASSERT_TRUE(result.snapshot.has_value());
    }

    [[nodiscard]] const ViewRegistryDiagnostic *FindDiagnosticByNodeIdAndMessage(
        const std::vector<ViewRegistryDiagnostic> &diagnostics,
        const std::string &node_id,
        const std::string &message_fragment)
    {
        for (const auto &diagnostic : diagnostics)
        {
            if (diagnostic.node_id == node_id &&
                diagnostic.message.find(message_fragment) != std::string::npos)
            {
                return &diagnostic;
            }
        }
        return nullptr;
    }

    void ExpectObservedStateFacts(const viewdemo::ViewNodeSnapshot &snapshot,
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

    TEST(ViewNodeDiscoveryTest, RegisterStoresNodeFactsAndLookupOrClusterViewSorted)
    {
        ViewNodeRegistry registry;

        auto view = MakeRegistration(ViewNodeType::kView, "view-1", 9001, 100);
        auto metadata =
            MakeRegistration(ViewNodeType::kMetadata, "meta-1", 9002, 101);
        metadata.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kFollower,
            MetadataMembershipObservedState::kRegistered,
            3,
            10);
        auto storage =
            MakeRegistration(ViewNodeType::kStorage, "store-1", 9003, 102);
        storage.failure_domain.zone = "zone-b";
        storage.failure_domain.rack = "rack-9";
        storage.capacity.total_capacity_bytes = 8'192;
        storage.capacity.used_capacity_bytes = 2'048;
        storage.capacity.available_capacity_bytes = 6'144;
        storage.capacity.chunk_count = 64;

        RegisterNodeOrAssert(&registry, MakeRegisterRequest(view, "register-view"));
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(metadata, "register-meta"));
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(storage, "register-store"));

        ASSERT_EQ(registry.size(), 3U);

        const auto lookup = registry.LookupNode(kClusterId, "store-1", 120);
        ASSERT_EQ(lookup.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(lookup.snapshot.has_value());
        EXPECT_EQ(lookup.snapshot->node_type, ViewNodeType::kStorage);
        EXPECT_EQ(lookup.snapshot->liveness, ViewNodeLivenessState::kLive);
        EXPECT_EQ(lookup.snapshot->failure_domain.zone, "zone-b");
        EXPECT_EQ(lookup.snapshot->capacity.available_capacity_bytes, 6'144U);
        ExpectObservedStateFacts(*lookup.snapshot, "", 0U, 102U);

        GetClusterViewRequest request;
        request.request_id = "cluster-view";
        request.cluster_id = kClusterId;
        request.include_dead_nodes = true;
        request.include_warnings = true;

        const auto cluster_view = registry.GetClusterView(request, 120);
        ASSERT_EQ(cluster_view.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(cluster_view.snapshot.view_nodes.size(), 1U);
        ASSERT_EQ(cluster_view.snapshot.metadata_nodes.size(), 1U);
        ASSERT_EQ(cluster_view.snapshot.storage_nodes.size(), 1U);
        EXPECT_EQ(cluster_view.snapshot.view_nodes[0].node_id, "view-1");
        EXPECT_EQ(cluster_view.snapshot.metadata_nodes[0].node_id, "meta-1");
        EXPECT_EQ(cluster_view.snapshot.storage_nodes[0].node_id, "store-1");
        ExpectObservedStateFacts(cluster_view.snapshot.view_nodes[0], "", 0U, 100U);
        ExpectObservedStateFacts(cluster_view.snapshot.metadata_nodes[0], "", 0U, 101U);
        ExpectObservedStateFacts(cluster_view.snapshot.storage_nodes[0], "", 0U, 102U);
        EXPECT_FALSE(cluster_view.snapshot.leader_hint.has_value());
    }

    TEST(ViewNodeDiscoveryTest,
         DuplicateRegisterIsIdempotentAndConflictsOnEndpointOrFingerprintMismatch)
    {
        ViewNodeRegistry registry;

        auto original =
            MakeRegistration(ViewNodeType::kStorage, "store-1", 9101, 100);
        original.capacity.total_capacity_bytes = 4'096;
        original.capacity.used_capacity_bytes = 1'024;
        original.capacity.available_capacity_bytes = 3'072;

        const auto first =
            registry.RegisterNode(MakeRegisterRequest(original, "register-first"));
        ASSERT_EQ(first.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(first.created);

        auto duplicate = original;
        duplicate.observed_at_unix_ms = 150;
        duplicate.capacity.total_capacity_bytes = 16'384;
        duplicate.capacity.available_capacity_bytes = 15'360;
        const auto second =
            registry.RegisterNode(MakeRegisterRequest(duplicate, "register-dup"));
        ASSERT_EQ(second.summary.status,
                  ViewRegistryStatusCode::kIdempotentReplay);
        ASSERT_TRUE(second.idempotent);
        ASSERT_TRUE(second.snapshot.has_value());
        EXPECT_EQ(second.snapshot->last_seen_unix_ms, 100U);
        EXPECT_EQ(second.snapshot->capacity.total_capacity_bytes, 4'096U);

        auto fingerprint_conflict = original;
        fingerprint_conflict.data_dir_fingerprint = "fingerprint-changed";
        const auto fingerprint_result = registry.RegisterNode(
            MakeRegisterRequest(fingerprint_conflict, "register-fingerprint-conflict"));
        ASSERT_EQ(fingerprint_result.summary.status,
                  ViewRegistryStatusCode::kConflict);
        ASSERT_FALSE(fingerprint_result.diagnostics.empty());
        EXPECT_EQ(fingerprint_result.diagnostics[0].code,
                  ViewRegistryIssueCode::kDataDirFingerprintConflict);

        auto endpoint_conflict =
            MakeRegistration(ViewNodeType::kStorage, "store-2", 9102, 160);
        endpoint_conflict.endpoint = original.endpoint;
        const auto endpoint_result = registry.RegisterNode(
            MakeRegisterRequest(endpoint_conflict, "register-endpoint-conflict"));
        ASSERT_EQ(endpoint_result.summary.status,
                  ViewRegistryStatusCode::kConflict);
        ASSERT_FALSE(endpoint_result.diagnostics.empty());
        EXPECT_EQ(endpoint_result.diagnostics[0].code,
                  ViewRegistryIssueCode::kEndpointConflict);

        EXPECT_EQ(registry.size(), 1U);
    }

    TEST(ViewNodeDiscoveryTest,
         HeartbeatAppliesNewObservationAndRejectsStaleOrDuplicateSequence)
    {
        ViewNodeRegistry registry;

        auto registration =
            MakeRegistration(ViewNodeType::kMetadata, "meta-1", 9201, 100);
        registration.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kFollower,
            MetadataMembershipObservedState::kJoining,
            2,
            4);
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(registration, "register-meta-1"));

        auto applied = MakeHeartbeatRequest(ViewNodeType::kMetadata,
                                            "meta-1",
                                            9201,
                                            7,
                                            160);
        applied.observation.health.health = ViewNodeHealth::kDegraded;
        applied.observation.load.active_reads = 8;
        applied.observation.load.active_writes = 5;
        applied.observation.load.queued_ops = 13;
        applied.observation.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kVoter,
            9,
            12,
            MetadataLeaderHint{.node_id = "meta-1",
                               .raft_id = std::optional<std::int32_t>{11},
                               .endpoint = applied.observation.endpoint,
                               .observed_term = 12,
                               .observed_at_unix_ms = 160});
        const auto applied_result = registry.HeartbeatNode(applied);
        ASSERT_EQ(applied_result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(applied_result.applied);
        EXPECT_EQ(applied_result.accepted_sequence, 7U);

        auto stale_sequence = MakeHeartbeatRequest(ViewNodeType::kMetadata,
                                                   "meta-1",
                                                   9201,
                                                   6,
                                                   170);
        stale_sequence.observation.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kFollower,
            MetadataMembershipObservedState::kVoter,
            10,
            13);
        const auto stale_sequence_result = registry.HeartbeatNode(stale_sequence);
        ASSERT_EQ(stale_sequence_result.summary.status,
                  ViewRegistryStatusCode::kStaleIgnored);
        EXPECT_TRUE(stale_sequence_result.stale_ignored);
        EXPECT_EQ(stale_sequence_result.accepted_sequence, 7U);

        auto stale_observed_at = MakeHeartbeatRequest(ViewNodeType::kMetadata,
                                                      "meta-1",
                                                      9201,
                                                      8,
                                                      150);
        stale_observed_at.observation.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kVoter,
            11,
            14);
        const auto stale_observed_result =
            registry.HeartbeatNode(stale_observed_at);
        ASSERT_EQ(stale_observed_result.summary.status,
                  ViewRegistryStatusCode::kStaleIgnored);
        EXPECT_TRUE(stale_observed_result.stale_ignored);
        EXPECT_EQ(stale_observed_result.accepted_sequence, 7U);

        auto duplicate = applied;
        duplicate.observation.observed_at_unix_ms = 180;
        duplicate.observation.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kVoter,
            99,
            21);
        const auto duplicate_result = registry.HeartbeatNode(duplicate);
        ASSERT_EQ(duplicate_result.summary.status,
                  ViewRegistryStatusCode::kIdempotentReplay);
        EXPECT_TRUE(duplicate_result.idempotent);
        EXPECT_FALSE(duplicate_result.applied);

        const auto lookup = registry.LookupNode(kClusterId, "meta-1", 181);
        ASSERT_EQ(lookup.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(lookup.snapshot.has_value());
        ExpectObservedStateFacts(*lookup.snapshot, "", 7U, 160U);
        EXPECT_EQ(lookup.snapshot->last_sequence, 7U);
        EXPECT_EQ(lookup.snapshot->last_seen_unix_ms, 160U);
        EXPECT_EQ(lookup.snapshot->health.health, ViewNodeHealth::kDegraded);
        ASSERT_TRUE(lookup.snapshot->metadata.has_value());
        EXPECT_EQ(lookup.snapshot->metadata->raft_role,
                  MetadataRaftObservedRole::kLeader);
        ASSERT_TRUE(lookup.snapshot->metadata->leader_hint.has_value());
        EXPECT_EQ(lookup.snapshot->metadata->leader_hint->node_id, "meta-1");
        EXPECT_EQ(lookup.snapshot->metadata->leader_hint->observed_term, 12U);
    }

    TEST(ViewNodeDiscoveryTest, LivenessTransitionsAcrossLiveStaleSuspectAndDead)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);
        ViewNodeRegistry registry(config);

        RegisterNodeOrAssert(
            &registry,
            MakeRegisterRequest(
                MakeRegistration(ViewNodeType::kView, "view-1", 9301, 100),
                "register-view"));

        auto lookup = registry.LookupNode(kClusterId, "view-1", 100);
        ASSERT_TRUE(lookup.snapshot.has_value());
        EXPECT_EQ(lookup.snapshot->liveness, ViewNodeLivenessState::kLive);

        lookup = registry.LookupNode(kClusterId, "view-1", 131);
        ASSERT_TRUE(lookup.snapshot.has_value());
        EXPECT_EQ(lookup.snapshot->liveness, ViewNodeLivenessState::kStale);

        lookup = registry.LookupNode(kClusterId, "view-1", 161);
        ASSERT_TRUE(lookup.snapshot.has_value());
        EXPECT_EQ(lookup.snapshot->liveness, ViewNodeLivenessState::kSuspect);

        lookup = registry.LookupNode(kClusterId, "view-1", 191);
        ASSERT_TRUE(lookup.snapshot.has_value());
        EXPECT_EQ(lookup.snapshot->liveness, ViewNodeLivenessState::kDead);
    }

    TEST(ViewNodeDiscoveryTest,
         DiscoverMetadataReturnsLiveCandidatesAndNewestLeaderHint)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);
        ViewNodeRegistry registry(config);

        auto leader =
            MakeRegistration(ViewNodeType::kMetadata, "meta-1", 9401, 180);
        leader.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kRegistered,
            3,
            10,
            MetadataLeaderHint{.node_id = "meta-1",
                               .raft_id = std::optional<std::int32_t>{1},
                               .endpoint = leader.endpoint,
                               .observed_term = 10,
                               .observed_at_unix_ms = 180});

        auto follower =
            MakeRegistration(ViewNodeType::kMetadata, "meta-2", 9402, 175);
        follower.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kFollower,
            MetadataMembershipObservedState::kVoter,
            7,
            12,
            MetadataLeaderHint{.node_id = "meta-1",
                               .raft_id = std::optional<std::int32_t>{1},
                               .endpoint = leader.endpoint,
                               .observed_term = 12,
                               .observed_at_unix_ms = 185});

        auto stale =
            MakeRegistration(ViewNodeType::kMetadata, "meta-3", 9403, 100);
        stale.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLearner,
            MetadataMembershipObservedState::kLearner,
            11,
            8);

        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(leader, "register-meta-1"));
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(follower, "register-meta-2"));
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(stale, "register-meta-3"));

        DiscoverMetadataRequest request;
        request.request_id = "discover-metadata";
        request.cluster_id = kClusterId;
        request.prefer_leader = true;
        request.live_only = true;

        const auto result = registry.DiscoverMetadata(request, kNow);
        ASSERT_EQ(result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(result.metadata_nodes.size(), 2U);
        EXPECT_EQ(result.metadata_nodes[0].node_id, "meta-1");
        EXPECT_EQ(result.metadata_nodes[1].node_id, "meta-2");
        EXPECT_EQ(result.metadata_nodes[1].metadata->membership_state,
                  MetadataMembershipObservedState::kVoter);
        EXPECT_EQ(result.membership_epoch, 7U);
        ASSERT_TRUE(result.leader_hint.has_value());
        EXPECT_EQ(result.leader_hint->node_id, "meta-1");
        EXPECT_EQ(result.leader_hint->observed_term, 12U);
        EXPECT_EQ(result.leader_hint->observed_at_unix_ms, 185U);
        ASSERT_FALSE(result.diagnostics.empty());
        EXPECT_EQ(result.diagnostics[0].code,
                  ViewRegistryIssueCode::kLivenessExcluded);
    }

    TEST(ViewNodeDiscoveryTest,
         DiscoverStorageFiltersByCapacityZoneRackWritableAndLiveness)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);
        ViewNodeRegistry registry(config);

        auto writable =
            MakeRegistration(ViewNodeType::kStorage, "store-1", 9501, 180);
        writable.failure_domain.zone = "zone-a";
        writable.failure_domain.rack = "rack-1";
        writable.capacity.total_capacity_bytes = 4'096;
        writable.capacity.used_capacity_bytes = 1'024;
        writable.capacity.available_capacity_bytes = 3'072;

        auto low_capacity =
            MakeRegistration(ViewNodeType::kStorage, "store-2", 9502, 181);
        low_capacity.failure_domain.zone = "zone-a";
        low_capacity.failure_domain.rack = "rack-1";
        low_capacity.capacity.total_capacity_bytes = 2'048;
        low_capacity.capacity.used_capacity_bytes = 1'920;
        low_capacity.capacity.available_capacity_bytes = 128;

        auto non_writable =
            MakeRegistration(ViewNodeType::kStorage, "store-3", 9503, 182);
        non_writable.failure_domain.zone = "zone-a";
        non_writable.failure_domain.rack = "rack-1";
        non_writable.capacity.total_capacity_bytes = 8'192;
        non_writable.capacity.used_capacity_bytes = 2'048;
        non_writable.capacity.available_capacity_bytes = 6'144;
        non_writable.health.health = ViewNodeHealth::kReadOnly;

        auto dead =
            MakeRegistration(ViewNodeType::kStorage, "store-4", 9504, 90);
        dead.failure_domain.zone = "zone-a";
        dead.failure_domain.rack = "rack-1";
        dead.capacity.total_capacity_bytes = 16'384;
        dead.capacity.used_capacity_bytes = 4'096;
        dead.capacity.available_capacity_bytes = 12'288;

        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(writable, "register-store-1"));
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(low_capacity, "register-store-2"));
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(non_writable, "register-store-3"));
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(dead, "register-store-4"));

        DiscoverStorageRequest request;
        request.request_id = "discover-storage";
        request.cluster_id = kClusterId;
        request.live_only = true;
        request.minimum_available_capacity_bytes = 1'024;
        request.zone = "zone-a";
        request.rack = "rack-1";
        request.require_writable = true;

        const auto result = registry.DiscoverStorage(request, kNow);
        ASSERT_EQ(result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(result.storage_nodes.size(), 1U);
        EXPECT_EQ(result.storage_nodes[0].node_id, "store-1");
        EXPECT_EQ(result.storage_nodes[0].failure_domain.zone, "zone-a");
        EXPECT_EQ(result.storage_nodes[0].failure_domain.rack, "rack-1");

        bool saw_capacity = false;
        bool saw_health = false;
        bool saw_liveness = false;
        for (const auto &diagnostic : result.diagnostics)
        {
            if (diagnostic.code == ViewRegistryIssueCode::kCapacityInsufficient)
            {
                saw_capacity = true;
            }
            if (diagnostic.code == ViewRegistryIssueCode::kHealthExcluded)
            {
                saw_health = true;
            }
            if (diagnostic.code == ViewRegistryIssueCode::kLivenessExcluded)
            {
                saw_liveness = true;
            }
        }
        EXPECT_TRUE(saw_capacity);
        EXPECT_TRUE(saw_health);
        EXPECT_TRUE(saw_liveness);
    }

    TEST(ViewNodeDiscoveryTest, ClusterViewCanExcludeDeadNodesAndEmitWarnings)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);
        ViewNodeRegistry registry(config);

        auto view = MakeRegistration(ViewNodeType::kView, "view-1", 9601, 190);
        auto metadata =
            MakeRegistration(ViewNodeType::kMetadata, "meta-1", 9602, 150);
        metadata.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kFollower,
            MetadataMembershipObservedState::kDown,
            5,
            9,
            MetadataLeaderHint{.node_id = "meta-1",
                               .raft_id = std::optional<std::int32_t>{2},
                               .endpoint = metadata.endpoint,
                               .observed_term = 9,
                               .observed_at_unix_ms = 150});
        auto storage =
            MakeRegistration(ViewNodeType::kStorage, "store-1", 9603, 100);
        storage.capacity.total_capacity_bytes = 2'048;
        storage.capacity.used_capacity_bytes = 512;
        storage.capacity.available_capacity_bytes = 1'536;

        RegisterNodeOrAssert(&registry, MakeRegisterRequest(view, "register-view"));
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(metadata, "register-meta"));
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(storage, "register-storage"));

        GetClusterViewRequest request;
        request.request_id = "cluster-view-filtered";
        request.cluster_id = kClusterId;
        request.include_dead_nodes = false;
        request.include_warnings = true;

        const auto result = registry.GetClusterView(request, kNow);
        ASSERT_EQ(result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(result.snapshot.view_nodes.size(), 1U);
        ASSERT_EQ(result.snapshot.metadata_nodes.size(), 1U);
        EXPECT_TRUE(result.snapshot.storage_nodes.empty());
        ASSERT_TRUE(result.snapshot.leader_hint.has_value());
        EXPECT_EQ(result.snapshot.leader_hint->node_id, "meta-1");

        ASSERT_EQ(result.snapshot.diagnostics.size(), 2U);
        EXPECT_EQ(result.snapshot.diagnostics[0].code,
                  ViewRegistryIssueCode::kLivenessExcluded);
        EXPECT_EQ(result.snapshot.diagnostics[1].code,
                  ViewRegistryIssueCode::kLivenessExcluded);
    }

    TEST(ViewNodeDiscoveryTest, ViewNodeSelfRefreshKeepsSelfLiveBeyondDeadTtl)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);
        RunningViewNodeDiscoveryService service(config, 100);

        auto self =
            MakeRegistration(ViewNodeType::kView, "view-self-1", 9700, 100);

        const auto register_result = service.client().RegisterNode(
            MakeRegisterRequest(self, "integration-register-view-self-1"));
        ASSERT_TRUE(register_result.transport_ok());
        ASSERT_EQ(register_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(register_result.result.created);

        GetClusterViewRequest cluster_request;
        cluster_request.request_id = "integration-cluster-view-self-initial";
        cluster_request.cluster_id = kClusterId;
        cluster_request.include_dead_nodes = false;
        cluster_request.include_warnings = true;

        auto cluster_result = service.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        ASSERT_EQ(cluster_result.result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(cluster_result.result.snapshot.view_nodes.size(), 1U);
        EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].node_id,
                  "view-self-1");
        EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].liveness,
                  ViewNodeLivenessState::kLive);
        EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].last_seen_unix_ms,
                  100U);
        EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].last_sequence, 0U);

        service.set_now_unix_ms(191);
        auto self_refresh = MakeSelfRefreshRequest(
            "view-self-1",
            9700,
            "view-self-1:boot:191000000:42:1",
            1,
            191);
        // app 层当前通过 request_id 间接携带 incarnation；这里清空显式字段，
        // 直接覆盖 T023 的 registry 解析路径。
        self_refresh.incarnation_id.clear();
        const auto self_refresh_result = service.RefreshSelfNode(self_refresh);
        ASSERT_EQ(self_refresh_result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(self_refresh_result.applied);
        EXPECT_EQ(self_refresh_result.accepted_sequence, 1U);
        ASSERT_TRUE(self_refresh_result.snapshot.has_value());
        EXPECT_EQ(self_refresh_result.snapshot->incarnation_id,
                  "view-self-1:boot:191000000:42:1");

        cluster_request.request_id =
            "integration-cluster-view-self-beyond-dead-ttl";
        cluster_result = service.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        ASSERT_EQ(cluster_result.result.summary.status, ViewRegistryStatusCode::kOk);

        // 健康运行中的 ViewNode 依靠 registry self refresh 持续维持 LIVE。
        // 这里不启动 app 层 loop，只直接触发一次 registry self refresh，
        // 验证它遵守正常 update 语义而不是依赖 TTL 豁免。
        ASSERT_EQ(cluster_result.result.snapshot.view_nodes.size(), 1U);
        EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].node_id,
                  "view-self-1");
        EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].liveness,
                  ViewNodeLivenessState::kLive);
        EXPECT_GT(cluster_result.result.snapshot.view_nodes[0].last_seen_unix_ms,
                  100U);
        EXPECT_GT(cluster_result.result.snapshot.view_nodes[0].last_sequence, 0U);
        const auto *self_refresh_diagnostic =
            FindDiagnosticByNodeIdAndMessage(
                cluster_result.result.snapshot.diagnostics,
                "view-self-1",
                "self_refresh_state source=self_refresh");
        ASSERT_NE(self_refresh_diagnostic, nullptr);
        EXPECT_NE(
            self_refresh_diagnostic->message.find("liveness=live"),
            std::string::npos);
        EXPECT_NE(
            self_refresh_diagnostic->message.find(
                "incarnation=view-self-1:boot:191000000:42:1"),
            std::string::npos);
    }

    TEST(ViewNodeDiscoveryTest,
         SelfRefreshPayloadIncludesIncarnationSequenceObservedTimeHealthAndEndpoint)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);
        ViewNodeRegistry registry(config);

        auto self = MakeRegistration(ViewNodeType::kView,
                                     "view-self-payload-1",
                                     9706,
                                     100);
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(
                                 self,
                                 "register-view-self-payload-1"));

        const std::string old_incarnation =
            "view-self-payload-1:boot:110000000:10:1";
        const std::string new_incarnation =
            "view-self-payload-1:boot:111000000:11:1";

        auto old_refresh = MakeSelfRefreshRequest("view-self-payload-1",
                                                  9706,
                                                  old_incarnation,
                                                  10,
                                                  110);
        old_refresh.observation.health.health = ViewNodeHealth::kDegraded;
        old_refresh.observation.health.disk_pressure =
            ViewNodeDiskPressure::kMedium;
        const auto old_refresh_result = registry.RefreshSelfNode(old_refresh);
        ASSERT_EQ(old_refresh_result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(old_refresh_result.applied);
        ASSERT_TRUE(old_refresh_result.snapshot.has_value());
        ExpectObservedStateFacts(*old_refresh_result.snapshot,
                                 old_incarnation,
                                 10U,
                                 110U);
        EXPECT_EQ(old_refresh_result.snapshot->incarnation_id, old_incarnation);
        EXPECT_EQ(old_refresh_result.snapshot->last_sequence, 10U);
        EXPECT_EQ(old_refresh_result.snapshot->last_seen_unix_ms, 110U);
        EXPECT_EQ(old_refresh_result.snapshot->health.health,
                  ViewNodeHealth::kDegraded);
        EXPECT_EQ(old_refresh_result.snapshot->liveness,
                  ViewNodeLivenessState::kLive);

        auto new_refresh = MakeSelfRefreshRequest("view-self-payload-1",
                                                  9706,
                                                  new_incarnation,
                                                  1,
                                                  111);
        new_refresh.observation.health.health = ViewNodeHealth::kDegraded;
        new_refresh.observation.health.disk_pressure =
            ViewNodeDiskPressure::kMedium;
        const auto new_refresh_result = registry.RefreshSelfNode(new_refresh);
        ASSERT_EQ(new_refresh_result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(new_refresh_result.applied);
        ASSERT_TRUE(new_refresh_result.snapshot.has_value());
        ExpectObservedStateFacts(*new_refresh_result.snapshot,
                                 new_incarnation,
                                 1U,
                                 111U);
        EXPECT_EQ(new_refresh_result.snapshot->incarnation_id, new_incarnation);
        EXPECT_EQ(new_refresh_result.snapshot->last_sequence, 1U);
        EXPECT_EQ(new_refresh_result.snapshot->last_seen_unix_ms, 111U);
        EXPECT_EQ(new_refresh_result.snapshot->endpoint, self.endpoint);
        EXPECT_EQ(new_refresh_result.snapshot->health.health,
                  ViewNodeHealth::kDegraded);

        auto same_incarnation_higher_sequence =
            MakeSelfRefreshRequest("view-self-payload-1",
                                   9706,
                                   new_incarnation,
                                   2,
                                   112);
        same_incarnation_higher_sequence.observation.health.health =
            ViewNodeHealth::kHealthy;
        same_incarnation_higher_sequence.observation.health.disk_pressure =
            ViewNodeDiskPressure::kLow;
        const auto higher_sequence_result =
            registry.RefreshSelfNode(same_incarnation_higher_sequence);
        ASSERT_EQ(higher_sequence_result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(higher_sequence_result.applied);

        auto stale_same_incarnation = MakeSelfRefreshRequest("view-self-payload-1",
                                                             9706,
                                                             new_incarnation,
                                                             1,
                                                             113);
        stale_same_incarnation.observation.health.health =
            ViewNodeHealth::kUnavailable;
        const auto stale_same_incarnation_result =
            registry.RefreshSelfNode(stale_same_incarnation);
        ASSERT_EQ(stale_same_incarnation_result.summary.status,
                  ViewRegistryStatusCode::kStaleIgnored);
        ASSERT_TRUE(stale_same_incarnation_result.stale_ignored);

        auto old_incarnation_late = MakeSelfRefreshRequest("view-self-payload-1",
                                                           9706,
                                                           old_incarnation,
                                                           99,
                                                           114);
        old_incarnation_late.observation.health.health =
            ViewNodeHealth::kReadOnly;
        const auto old_incarnation_late_result =
            registry.RefreshSelfNode(old_incarnation_late);
        ASSERT_EQ(old_incarnation_late_result.summary.status,
                  ViewRegistryStatusCode::kStaleIgnored);
        ASSERT_TRUE(old_incarnation_late_result.stale_ignored);

        const auto lookup = registry.LookupNode(kClusterId,
                                                "view-self-payload-1",
                                                114);
        ASSERT_EQ(lookup.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(lookup.snapshot.has_value());
        ExpectObservedStateFacts(*lookup.snapshot, new_incarnation, 2U, 112U);
        EXPECT_EQ(lookup.snapshot->node_id, "view-self-payload-1");
        EXPECT_EQ(lookup.snapshot->endpoint, self.endpoint);
        EXPECT_EQ(lookup.snapshot->incarnation_id, new_incarnation);
        EXPECT_EQ(lookup.snapshot->last_sequence, 2U);
        EXPECT_EQ(lookup.snapshot->last_seen_unix_ms, 112U);
        EXPECT_EQ(lookup.snapshot->health.health, ViewNodeHealth::kHealthy);
        EXPECT_EQ(lookup.snapshot->liveness, ViewNodeLivenessState::kLive);
    }

    TEST(ViewNodeDiscoveryTest, HigherSequenceWinsWithinSameIncarnation)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(1000);
        config.suspect_timeout = std::chrono::milliseconds(2000);
        config.dead_timeout = std::chrono::milliseconds(3000);
        ViewNodeRegistry registry(config);

        auto self = MakeRegistration(ViewNodeType::kView,
                                     "view-sequence-order-1",
                                     9709,
                                     100);
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(
                                 self,
                                 "register-view-sequence-order-1"));

        const std::string incarnation_id =
            "view-sequence-order-1:boot:110000000:21:1";

        auto sequence_10 = MakeSelfRefreshRequest("view-sequence-order-1",
                                                  9709,
                                                  incarnation_id,
                                                  10,
                                                  110);
        sequence_10.observation.health.health = ViewNodeHealth::kDegraded;
        sequence_10.observation.health.disk_pressure =
            ViewNodeDiskPressure::kMedium;
        const auto sequence_10_result = registry.RefreshSelfNode(sequence_10);
        ASSERT_EQ(sequence_10_result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(sequence_10_result.applied);

        auto sequence_11 = MakeSelfRefreshRequest("view-sequence-order-1",
                                                  9709,
                                                  incarnation_id,
                                                  11,
                                                  111);
        sequence_11.observation.health.health = ViewNodeHealth::kHealthy;
        sequence_11.observation.health.disk_pressure =
            ViewNodeDiskPressure::kLow;
        const auto sequence_11_result = registry.RefreshSelfNode(sequence_11);
        ASSERT_EQ(sequence_11_result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(sequence_11_result.applied);
        ASSERT_TRUE(sequence_11_result.snapshot.has_value());
        ExpectObservedStateFacts(*sequence_11_result.snapshot,
                                 incarnation_id,
                                 11U,
                                 111U);
        EXPECT_EQ(sequence_11_result.snapshot->incarnation_id, incarnation_id);
        EXPECT_EQ(sequence_11_result.snapshot->last_sequence, 11U);
        EXPECT_EQ(sequence_11_result.snapshot->last_seen_unix_ms, 111U);
        EXPECT_EQ(sequence_11_result.snapshot->health.health,
                  ViewNodeHealth::kHealthy);

        auto late_lower_sequence = MakeSelfRefreshRequest("view-sequence-order-1",
                                                          9709,
                                                          incarnation_id,
                                                          10,
                                                          250);
        late_lower_sequence.observation.health.health =
            ViewNodeHealth::kUnavailable;
        late_lower_sequence.observation.health.disk_pressure =
            ViewNodeDiskPressure::kFull;
        const auto late_lower_sequence_result =
            registry.RefreshSelfNode(late_lower_sequence);
        ASSERT_EQ(late_lower_sequence_result.summary.status,
                  ViewRegistryStatusCode::kStaleIgnored);
        ASSERT_TRUE(late_lower_sequence_result.stale_ignored);
        EXPECT_FALSE(late_lower_sequence_result.applied);
        EXPECT_EQ(late_lower_sequence_result.accepted_sequence, 11U);

        const auto lookup = registry.LookupNode(kClusterId,
                                                "view-sequence-order-1",
                                                250);
        ASSERT_EQ(lookup.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(lookup.snapshot.has_value());
        ExpectObservedStateFacts(*lookup.snapshot, incarnation_id, 11U, 111U);
        EXPECT_EQ(lookup.snapshot->incarnation_id, incarnation_id);
        EXPECT_EQ(lookup.snapshot->last_sequence, 11U);
        EXPECT_EQ(lookup.snapshot->last_seen_unix_ms, 111U);
        EXPECT_EQ(lookup.snapshot->health.health, ViewNodeHealth::kHealthy);
        EXPECT_EQ(lookup.snapshot->health.disk_pressure,
                  ViewNodeDiskPressure::kLow);
        EXPECT_EQ(lookup.snapshot->endpoint, self.endpoint);
        EXPECT_EQ(lookup.snapshot->liveness, ViewNodeLivenessState::kLive);
    }

    TEST(ViewNodeDiscoveryTest, HigherIncarnationWinsForViewNodeObservedState)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);
        ViewNodeRegistry registry(config);

        auto self = MakeRegistration(ViewNodeType::kView,
                                     "view-incarnation-order-1",
                                     9708,
                                     100);
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(
                                 self,
                                 "register-view-incarnation-order-1"));

        const std::string old_incarnation =
            "view-incarnation-order-1:boot:110000000:10:1";
        const std::string new_incarnation =
            "view-incarnation-order-1:boot:111000000:11:1";

        auto old_refresh = MakeSelfRefreshRequest("view-incarnation-order-1",
                                                  9708,
                                                  old_incarnation,
                                                  5,
                                                  105);
        old_refresh.observation.health.health = ViewNodeHealth::kDegraded;
        old_refresh.observation.health.disk_pressure =
            ViewNodeDiskPressure::kMedium;
        const auto old_refresh_result = registry.RefreshSelfNode(old_refresh);
        ASSERT_EQ(old_refresh_result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(old_refresh_result.applied);
        ASSERT_TRUE(old_refresh_result.snapshot.has_value());
        EXPECT_EQ(old_refresh_result.snapshot->incarnation_id, old_incarnation);
        EXPECT_EQ(old_refresh_result.snapshot->last_sequence, 5U);
        EXPECT_EQ(old_refresh_result.snapshot->last_seen_unix_ms, 105U);

        auto new_refresh = MakeSelfRefreshRequest("view-incarnation-order-1",
                                                  9708,
                                                  new_incarnation,
                                                  1,
                                                  111);
        new_refresh.observation.health.health = ViewNodeHealth::kHealthy;
        new_refresh.observation.health.disk_pressure =
            ViewNodeDiskPressure::kLow;
        const auto new_refresh_result = registry.RefreshSelfNode(new_refresh);
        ASSERT_EQ(new_refresh_result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(new_refresh_result.applied);
        ASSERT_TRUE(new_refresh_result.snapshot.has_value());
        ExpectObservedStateFacts(*new_refresh_result.snapshot,
                                 new_incarnation,
                                 1U,
                                 111U);
        EXPECT_EQ(new_refresh_result.snapshot->incarnation_id, new_incarnation);
        EXPECT_EQ(new_refresh_result.snapshot->last_sequence, 1U);
        EXPECT_EQ(new_refresh_result.snapshot->last_seen_unix_ms, 111U);
        EXPECT_EQ(new_refresh_result.snapshot->health.health,
                  ViewNodeHealth::kHealthy);
        EXPECT_EQ(new_refresh_result.snapshot->liveness,
                  ViewNodeLivenessState::kLive);

        auto old_incarnation_late = MakeSelfRefreshRequest(
            "view-incarnation-order-1",
            9708,
            old_incarnation,
            99,
            120);
        old_incarnation_late.observation.health.health =
            ViewNodeHealth::kUnavailable;
        old_incarnation_late.observation.health.disk_pressure =
            ViewNodeDiskPressure::kFull;
        const auto old_incarnation_late_result =
            registry.RefreshSelfNode(old_incarnation_late);
        ASSERT_EQ(old_incarnation_late_result.summary.status,
                  ViewRegistryStatusCode::kStaleIgnored);
        ASSERT_TRUE(old_incarnation_late_result.stale_ignored);
        ASSERT_TRUE(old_incarnation_late_result.snapshot.has_value());
        ExpectObservedStateFacts(*old_incarnation_late_result.snapshot,
                                 new_incarnation,
                                 1U,
                                 111U);
        EXPECT_EQ(old_incarnation_late_result.snapshot->incarnation_id,
                  new_incarnation);
        EXPECT_EQ(old_incarnation_late_result.snapshot->last_sequence, 1U);
        EXPECT_EQ(old_incarnation_late_result.snapshot->last_seen_unix_ms, 111U);
        EXPECT_EQ(old_incarnation_late_result.snapshot->health.health,
                  ViewNodeHealth::kHealthy);
        EXPECT_EQ(old_incarnation_late_result.snapshot->liveness,
                  ViewNodeLivenessState::kLive);

        const auto lookup =
            registry.LookupNode(kClusterId, "view-incarnation-order-1", 121);
        ASSERT_EQ(lookup.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(lookup.snapshot.has_value());
        ExpectObservedStateFacts(*lookup.snapshot, new_incarnation, 1U, 111U);
        EXPECT_EQ(lookup.snapshot->node_id, "view-incarnation-order-1");
        EXPECT_EQ(lookup.snapshot->endpoint, self.endpoint);
        EXPECT_EQ(lookup.snapshot->incarnation_id, new_incarnation);
        EXPECT_EQ(lookup.snapshot->last_sequence, 1U);
        EXPECT_EQ(lookup.snapshot->last_seen_unix_ms, 111U);
        EXPECT_EQ(lookup.snapshot->health.health, ViewNodeHealth::kHealthy);
        EXPECT_EQ(lookup.snapshot->liveness, ViewNodeLivenessState::kLive);

        GetClusterViewRequest cluster_request;
        cluster_request.request_id = "cluster-view-higher-incarnation-wins";
        cluster_request.cluster_id = kClusterId;
        cluster_request.include_dead_nodes = true;
        cluster_request.include_warnings = true;
        const auto cluster_view = registry.GetClusterView(cluster_request, 121);
        ASSERT_EQ(cluster_view.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(cluster_view.snapshot.view_nodes.size(), 1U);
        ExpectObservedStateFacts(cluster_view.snapshot.view_nodes[0],
                                 new_incarnation,
                                 1U,
                                 111U);
        EXPECT_EQ(cluster_view.snapshot.view_nodes[0].node_id,
                  "view-incarnation-order-1");
        EXPECT_EQ(cluster_view.snapshot.view_nodes[0].incarnation_id,
                  new_incarnation);
        EXPECT_EQ(cluster_view.snapshot.view_nodes[0].last_sequence, 1U);
        EXPECT_EQ(cluster_view.snapshot.view_nodes[0].last_seen_unix_ms, 111U);
        EXPECT_EQ(cluster_view.snapshot.view_nodes[0].health.health,
                  ViewNodeHealth::kHealthy);
        EXPECT_EQ(cluster_view.snapshot.view_nodes[0].liveness,
                  ViewNodeLivenessState::kLive);
    }

    TEST(ViewNodeDiscoveryTest, ObservedTimeOnlyCannotOverrideHigherSequence)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(1000);
        config.suspect_timeout = std::chrono::milliseconds(2000);
        config.dead_timeout = std::chrono::milliseconds(3000);
        ViewNodeRegistry registry(config);

        auto self = MakeRegistration(ViewNodeType::kView,
                                     "view-observed-time-order-1",
                                     9710,
                                     100);
        RegisterNodeOrAssert(&registry,
                             MakeRegisterRequest(
                                 self,
                                 "register-view-observed-time-order-1"));

        const std::string incarnation_id =
            "view-observed-time-order-1:boot:210000000:31:1";

        auto live_sequence_11 = MakeSelfRefreshRequest(
            "view-observed-time-order-1",
            9710,
            incarnation_id,
            11,
            210);
        live_sequence_11.observation.health.health = ViewNodeHealth::kHealthy;
        live_sequence_11.observation.health.disk_pressure =
            ViewNodeDiskPressure::kLow;
        const auto live_result = registry.RefreshSelfNode(live_sequence_11);
        ASSERT_EQ(live_result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(live_result.applied);
        ASSERT_TRUE(live_result.snapshot.has_value());
        ExpectObservedStateFacts(*live_result.snapshot, incarnation_id, 11U, 210U);
        EXPECT_EQ(live_result.snapshot->last_sequence, 11U);
        EXPECT_EQ(live_result.snapshot->last_seen_unix_ms, 210U);
        EXPECT_EQ(live_result.snapshot->health.health,
                  ViewNodeHealth::kHealthy);
        EXPECT_EQ(live_result.snapshot->liveness, ViewNodeLivenessState::kLive);

        auto stale_dead_late = MakeSelfRefreshRequest(
            "view-observed-time-order-1",
            9710,
            incarnation_id,
            10,
            999);
        stale_dead_late.observation.health.health =
            ViewNodeHealth::kUnavailable;
        stale_dead_late.observation.health.disk_pressure =
            ViewNodeDiskPressure::kFull;
        const auto stale_result = registry.RefreshSelfNode(stale_dead_late);
        ASSERT_EQ(stale_result.summary.status,
                  ViewRegistryStatusCode::kStaleIgnored);
        ASSERT_TRUE(stale_result.stale_ignored);
        EXPECT_FALSE(stale_result.applied);
        EXPECT_EQ(stale_result.accepted_sequence, 11U);
        ASSERT_TRUE(stale_result.snapshot.has_value());
        ExpectObservedStateFacts(*stale_result.snapshot,
                                 incarnation_id,
                                 11U,
                                 210U);
        EXPECT_EQ(stale_result.snapshot->incarnation_id, incarnation_id);
        EXPECT_EQ(stale_result.snapshot->last_sequence, 11U);
        EXPECT_EQ(stale_result.snapshot->last_seen_unix_ms, 210U);
        EXPECT_EQ(stale_result.snapshot->health.health,
                  ViewNodeHealth::kHealthy);
        EXPECT_EQ(stale_result.snapshot->health.disk_pressure,
                  ViewNodeDiskPressure::kLow);
        EXPECT_EQ(stale_result.snapshot->liveness, ViewNodeLivenessState::kLive);

        const auto lookup = registry.LookupNode(kClusterId,
                                                "view-observed-time-order-1",
                                                999);
        ASSERT_EQ(lookup.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(lookup.snapshot.has_value());
        ExpectObservedStateFacts(*lookup.snapshot, incarnation_id, 11U, 210U);
        EXPECT_EQ(lookup.snapshot->incarnation_id, incarnation_id);
        EXPECT_EQ(lookup.snapshot->last_sequence, 11U);
        EXPECT_EQ(lookup.snapshot->last_seen_unix_ms, 210U);
        EXPECT_EQ(lookup.snapshot->health.health, ViewNodeHealth::kHealthy);
        EXPECT_EQ(lookup.snapshot->health.disk_pressure,
                  ViewNodeDiskPressure::kLow);
        EXPECT_EQ(lookup.snapshot->liveness, ViewNodeLivenessState::kLive);
    }

    TEST(ViewNodeDiscoveryTest,
         IntegrationClusterViewExposesSelfRefreshSequenceLivenessDiagnostics)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);
        RunningViewNodeDiscoveryService service(config, 100);

        auto self =
            MakeRegistration(ViewNodeType::kView, "view-self-diag-1", 9707, 100);

        const auto register_result = service.client().RegisterNode(
            MakeRegisterRequest(self, "integration-register-view-self-diag-1"));
        ASSERT_TRUE(register_result.transport_ok());
        ASSERT_EQ(register_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);

        GetClusterViewRequest cluster_request;
        cluster_request.cluster_id = kClusterId;
        cluster_request.include_dead_nodes = true;
        cluster_request.include_warnings = true;

        cluster_request.request_id = "integration-cluster-view-self-diag-initial";
        auto cluster_result = service.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        ASSERT_EQ(cluster_result.result.summary.status, ViewRegistryStatusCode::kOk);
        const auto *initial_diagnostic =
            FindDiagnosticByNodeIdAndMessage(
                cluster_result.result.snapshot.diagnostics,
                "view-self-diag-1",
                "self_refresh_state source=registration_only");
        ASSERT_NE(initial_diagnostic, nullptr);
        EXPECT_NE(initial_diagnostic->message.find("sequence=0"),
                  std::string::npos);
        EXPECT_NE(initial_diagnostic->message.find("liveness=live"),
                  std::string::npos);
        EXPECT_NE(initial_diagnostic->message.find("incarnation=<none>"),
                  std::string::npos);

        service.set_now_unix_ms(125);
        const std::string incarnation_id =
            "view-self-diag-1:boot:125000000:77:1";
        auto self_refresh = MakeSelfRefreshRequest("view-self-diag-1",
                                                   9707,
                                                   incarnation_id,
                                                   5,
                                                   125);
        const auto self_refresh_result = service.RefreshSelfNode(self_refresh);
        ASSERT_EQ(self_refresh_result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(self_refresh_result.applied);

        cluster_request.request_id = "integration-cluster-view-self-diag-live";
        cluster_result = service.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        ASSERT_EQ(cluster_result.result.snapshot.view_nodes.size(), 1U);
        EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].last_sequence, 5U);
        EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].liveness,
                  ViewNodeLivenessState::kLive);
        const auto *live_diagnostic =
            FindDiagnosticByNodeIdAndMessage(
                cluster_result.result.snapshot.diagnostics,
                "view-self-diag-1",
                "self_refresh_state source=self_refresh");
        ASSERT_NE(live_diagnostic, nullptr);
        EXPECT_NE(live_diagnostic->message.find(
                      std::string("endpoint=") + self.endpoint),
                  std::string::npos);
        EXPECT_NE(live_diagnostic->message.find(
                      std::string("incarnation=") + incarnation_id),
                  std::string::npos);
        EXPECT_NE(live_diagnostic->message.find("sequence=5"),
                  std::string::npos);
        EXPECT_NE(live_diagnostic->message.find("last_seen_unix_ms=125"),
                  std::string::npos);
        EXPECT_NE(live_diagnostic->message.find("health=healthy"),
                  std::string::npos);
        EXPECT_NE(live_diagnostic->message.find("liveness=live"),
                  std::string::npos);

        service.set_now_unix_ms(161);
        cluster_request.request_id = "integration-cluster-view-self-diag-stale";
        cluster_result = service.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        ASSERT_EQ(cluster_result.result.snapshot.view_nodes.size(), 1U);
        EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].liveness,
                  ViewNodeLivenessState::kStale);
        const auto *stale_diagnostic =
            FindDiagnosticByNodeIdAndMessage(
                cluster_result.result.snapshot.diagnostics,
                "view-self-diag-1",
                "self_refresh_state source=self_refresh");
        ASSERT_NE(stale_diagnostic, nullptr);
        EXPECT_NE(stale_diagnostic->message.find("sequence=5"),
                  std::string::npos);
        EXPECT_NE(stale_diagnostic->message.find("liveness=stale"),
                  std::string::npos);
    }

    TEST(ViewNodeDiscoveryTest,
         ViewNodeSelfRefreshDisabledAllowsTtlTransitions)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);
        RunningViewNodeDiscoveryService service(config, 100);

        auto self =
            MakeRegistration(ViewNodeType::kView, "view-self-disabled-1", 9705, 100);

        const auto register_result = service.client().RegisterNode(
            MakeRegisterRequest(self, "integration-register-view-self-disabled-1"));
        ASSERT_TRUE(register_result.transport_ok());
        ASSERT_EQ(register_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(register_result.result.created);

        GetClusterViewRequest cluster_request;
        cluster_request.cluster_id = kClusterId;
        cluster_request.include_dead_nodes = true;
        cluster_request.include_warnings = true;

        const auto expect_self_liveness =
            [&](const std::uint64_t now_unix_ms,
                const char *request_id,
                const ViewNodeLivenessState expected_liveness) {
                SCOPED_TRACE(request_id);
                service.set_now_unix_ms(now_unix_ms);
                cluster_request.request_id = request_id;
                const auto cluster_result =
                    service.client().GetClusterView(cluster_request);
                ASSERT_TRUE(cluster_result.transport_ok());
                ASSERT_EQ(cluster_result.result.summary.status,
                          ViewRegistryStatusCode::kOk);
                ASSERT_EQ(cluster_result.result.snapshot.view_nodes.size(), 1U);
                EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].node_id,
                          "view-self-disabled-1");
                EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].liveness,
                          expected_liveness);
                EXPECT_EQ(
                    cluster_result.result.snapshot.view_nodes[0].last_seen_unix_ms,
                    100U);
                EXPECT_EQ(cluster_result.result.snapshot.view_nodes[0].last_sequence,
                          0U);
            };

        // 不执行任何 self refresh / heartbeat；只用可控时钟推进，验证 TTL
        // 状态机仍会正常降级。
        expect_self_liveness(100,
                             "integration-cluster-view-self-disabled-live",
                             ViewNodeLivenessState::kLive);
        expect_self_liveness(131,
                             "integration-cluster-view-self-disabled-stale",
                             ViewNodeLivenessState::kStale);
        expect_self_liveness(161,
                             "integration-cluster-view-self-disabled-suspect",
                             ViewNodeLivenessState::kSuspect);
        expect_self_liveness(191,
                             "integration-cluster-view-self-disabled-dead",
                             ViewNodeLivenessState::kDead);

        cluster_request.request_id =
            "integration-cluster-view-self-disabled-dead-filtered";
        cluster_request.include_dead_nodes = false;
        const auto filtered_dead_result =
            service.client().GetClusterView(cluster_request);
        ASSERT_TRUE(filtered_dead_result.transport_ok());
        ASSERT_EQ(filtered_dead_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        EXPECT_TRUE(filtered_dead_result.result.snapshot.view_nodes.empty());
        ASSERT_EQ(filtered_dead_result.result.snapshot.diagnostics.size(), 1U);
        EXPECT_EQ(filtered_dead_result.result.snapshot.diagnostics[0].code,
                  ViewRegistryIssueCode::kLivenessExcluded);
        EXPECT_EQ(filtered_dead_result.result.snapshot.diagnostics[0].node_id,
                  "view-self-disabled-1");
    }

    TEST(ViewNodeDiscoveryTest,
         IntegrationMetadataDiscoveryReturnsEndpointAndObservedState)
    {
        RunningViewNodeDiscoveryService service;

        auto metadata =
            MakeRegistration(ViewNodeType::kMetadata, "meta-1", 9701, 190);
        metadata.metadata = MakeMetadataObservation(
            MetadataRaftObservedRole::kLeader,
            MetadataMembershipObservedState::kVoter,
            8,
            14,
            MetadataLeaderHint{.node_id = "meta-1",
                               .raft_id = std::optional<std::int32_t>{1},
                               .endpoint = metadata.endpoint,
                               .observed_term = 14,
                               .observed_at_unix_ms = 191});

        const auto register_result = service.client().RegisterNode(
            MakeRegisterRequest(metadata, "integration-register-meta"));
        ASSERT_TRUE(register_result.transport_ok());
        ASSERT_EQ(register_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(register_result.result.created);
        ASSERT_TRUE(register_result.result.snapshot.has_value());

        DiscoverMetadataRequest request;
        request.request_id = "integration-discover-metadata";
        request.cluster_id = kClusterId;
        request.prefer_leader = true;
        request.live_only = true;

        const auto discover_result = service.client().DiscoverMetadata(request);
        ASSERT_TRUE(discover_result.transport_ok());
        ASSERT_EQ(discover_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(discover_result.result.metadata_nodes.size(), 1U);
        EXPECT_EQ(discover_result.result.metadata_nodes[0].node_id, "meta-1");
        EXPECT_EQ(discover_result.result.metadata_nodes[0].endpoint,
                  metadata.endpoint);
        ASSERT_TRUE(discover_result.result.metadata_nodes[0].metadata.has_value());
        EXPECT_EQ(discover_result.result.metadata_nodes[0].metadata->raft_role,
                  MetadataRaftObservedRole::kLeader);
        EXPECT_EQ(
            discover_result.result.metadata_nodes[0].metadata->membership_state,
            MetadataMembershipObservedState::kVoter);
        EXPECT_EQ(discover_result.result.membership_epoch, 8U);
        ASSERT_TRUE(discover_result.result.leader_hint.has_value());
        EXPECT_EQ(discover_result.result.leader_hint->node_id, "meta-1");
        EXPECT_EQ(discover_result.result.leader_hint->endpoint,
                  metadata.endpoint);
        EXPECT_EQ(discover_result.result.leader_hint->observed_term, 14U);
    }

    TEST(ViewNodeDiscoveryTest,
         IntegrationStorageDiscoveryReturnsEndpointAndObservedState)
    {
        RunningViewNodeDiscoveryService service;

        auto storage =
            MakeRegistration(ViewNodeType::kStorage, "store-1", 9702, 192);
        storage.failure_domain.zone = "zone-c";
        storage.failure_domain.rack = "rack-9";
        storage.capacity.total_capacity_bytes = 16'384;
        storage.capacity.used_capacity_bytes = 4'096;
        storage.capacity.available_capacity_bytes = 12'288;
        storage.health.health = ViewNodeHealth::kHealthy;
        storage.load.active_reads = 7;
        storage.load.active_writes = 5;
        storage.load.queued_ops = 11;

        const auto register_result = service.client().RegisterNode(
            MakeRegisterRequest(storage, "integration-register-store"));
        ASSERT_TRUE(register_result.transport_ok());
        ASSERT_EQ(register_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(register_result.result.created);
        ASSERT_TRUE(register_result.result.snapshot.has_value());

        DiscoverStorageRequest request;
        request.request_id = "integration-discover-storage";
        request.cluster_id = kClusterId;
        request.live_only = true;
        request.minimum_available_capacity_bytes = 8'192;
        request.zone = "zone-c";
        request.rack = "rack-9";
        request.require_writable = true;

        const auto discover_result = service.client().DiscoverStorage(request);
        ASSERT_TRUE(discover_result.transport_ok());
        ASSERT_EQ(discover_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);
        ASSERT_EQ(discover_result.result.storage_nodes.size(), 1U);
        EXPECT_EQ(discover_result.result.storage_nodes[0].node_id, "store-1");
        EXPECT_EQ(discover_result.result.storage_nodes[0].endpoint,
                  storage.endpoint);
        EXPECT_EQ(discover_result.result.storage_nodes[0].failure_domain.zone,
                  "zone-c");
        EXPECT_EQ(discover_result.result.storage_nodes[0].failure_domain.rack,
                  "rack-9");
        EXPECT_EQ(
            discover_result.result.storage_nodes[0].capacity.available_capacity_bytes,
            12'288U);
        EXPECT_EQ(discover_result.result.storage_nodes[0].health.health,
                  ViewNodeHealth::kHealthy);
        EXPECT_EQ(discover_result.result.storage_nodes[0].liveness,
                  ViewNodeLivenessState::kLive);
    }

    TEST(ViewNodeDiscoveryTest,
         IntegrationHeartbeatRefreshesStateAndRejectsStaleUpdates)
    {
        RunningViewNodeDiscoveryService service;

        auto storage =
            MakeRegistration(ViewNodeType::kStorage, "store-2", 9703, 180);
        storage.capacity.total_capacity_bytes = 8'192;
        storage.capacity.used_capacity_bytes = 2'048;
        storage.capacity.available_capacity_bytes = 6'144;

        const auto register_result = service.client().RegisterNode(
            MakeRegisterRequest(storage, "integration-register-store-2"));
        ASSERT_TRUE(register_result.transport_ok());
        ASSERT_EQ(register_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);

        auto fresh = MakeHeartbeatRequest(ViewNodeType::kStorage,
                                          "store-2",
                                          9703,
                                          7,
                                          220);
        fresh.observation.health.health = ViewNodeHealth::kDegraded;
        fresh.observation.capacity.total_capacity_bytes = 16'384;
        fresh.observation.capacity.used_capacity_bytes = 4'096;
        fresh.observation.capacity.available_capacity_bytes = 12'288;
        fresh.observation.load.active_reads = 9;
        fresh.observation.load.active_writes = 6;
        fresh.observation.load.queued_ops = 15;

        const auto fresh_result = service.client().HeartbeatNode(fresh);
        ASSERT_TRUE(fresh_result.transport_ok());
        ASSERT_EQ(fresh_result.result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(fresh_result.result.applied);
        EXPECT_EQ(fresh_result.result.accepted_sequence, 7U);

        auto stale_sequence = MakeHeartbeatRequest(ViewNodeType::kStorage,
                                                   "store-2",
                                                   9703,
                                                   6,
                                                   230);
        stale_sequence.observation.health.health = ViewNodeHealth::kHealthy;
        stale_sequence.observation.capacity.available_capacity_bytes = 256;
        stale_sequence.observation.load.queued_ops = 1;
        const auto stale_sequence_result =
            service.client().HeartbeatNode(stale_sequence);
        ASSERT_TRUE(stale_sequence_result.transport_ok());
        ASSERT_EQ(stale_sequence_result.result.summary.status,
                  ViewRegistryStatusCode::kStaleIgnored);
        ASSERT_TRUE(stale_sequence_result.result.stale_ignored);
        EXPECT_EQ(stale_sequence_result.result.accepted_sequence, 7U);

        auto stale_observed_at = MakeHeartbeatRequest(ViewNodeType::kStorage,
                                                      "store-2",
                                                      9703,
                                                      8,
                                                      210);
        stale_observed_at.observation.health.health = ViewNodeHealth::kReadOnly;
        stale_observed_at.observation.capacity.available_capacity_bytes = 128;
        stale_observed_at.observation.load.queued_ops = 0;
        const auto stale_observed_result =
            service.client().HeartbeatNode(stale_observed_at);
        ASSERT_TRUE(stale_observed_result.transport_ok());
        ASSERT_EQ(stale_observed_result.result.summary.status,
                  ViewRegistryStatusCode::kStaleIgnored);
        ASSERT_TRUE(stale_observed_result.result.stale_ignored);
        EXPECT_EQ(stale_observed_result.result.accepted_sequence, 7U);

        DiscoverStorageRequest discover_request;
        discover_request.request_id = "integration-discover-store-2";
        discover_request.cluster_id = kClusterId;
        discover_request.live_only = true;
        discover_request.minimum_available_capacity_bytes = 8'192;
        discover_request.require_writable = false;

        const auto discover_result =
            service.client().DiscoverStorage(discover_request);
        ASSERT_TRUE(discover_result.transport_ok());
        ASSERT_EQ(discover_result.result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(discover_result.result.storage_nodes.size(), 1U);
        EXPECT_EQ(discover_result.result.storage_nodes[0].node_id, "store-2");
        EXPECT_EQ(discover_result.result.storage_nodes[0].health.health,
                  ViewNodeHealth::kDegraded);
        EXPECT_EQ(
            discover_result.result.storage_nodes[0].capacity.available_capacity_bytes,
            12'288U);
        EXPECT_EQ(discover_result.result.storage_nodes[0].load.active_reads, 9U);
        EXPECT_EQ(discover_result.result.storage_nodes[0].load.active_writes, 6U);
        EXPECT_EQ(discover_result.result.storage_nodes[0].load.queued_ops, 15U);
        EXPECT_EQ(discover_result.result.storage_nodes[0].last_sequence, 7U);
        EXPECT_EQ(discover_result.result.storage_nodes[0].last_seen_unix_ms, 220U);
    }

    TEST(ViewNodeDiscoveryTest,
         IntegrationLivenessTransitionsAppearInDiscoveryAndClusterView)
    {
        ViewRegistryConfig config;
        config.stale_timeout = std::chrono::milliseconds(30);
        config.suspect_timeout = std::chrono::milliseconds(60);
        config.dead_timeout = std::chrono::milliseconds(90);
        RunningViewNodeDiscoveryService service(config, 100);

        auto storage =
            MakeRegistration(ViewNodeType::kStorage, "store-3", 9704, 100);
        storage.capacity.total_capacity_bytes = 4'096;
        storage.capacity.used_capacity_bytes = 1'024;
        storage.capacity.available_capacity_bytes = 3'072;

        const auto register_result = service.client().RegisterNode(
            MakeRegisterRequest(storage, "integration-register-store-3"));
        ASSERT_TRUE(register_result.transport_ok());
        ASSERT_EQ(register_result.result.summary.status,
                  ViewRegistryStatusCode::kOk);

        DiscoverStorageRequest discover_request;
        discover_request.request_id = "integration-discover-store-3";
        discover_request.cluster_id = kClusterId;
        discover_request.live_only = true;

        auto discover_result = service.client().DiscoverStorage(discover_request);
        ASSERT_TRUE(discover_result.transport_ok());
        ASSERT_EQ(discover_result.result.storage_nodes.size(), 1U);
        EXPECT_EQ(discover_result.result.storage_nodes[0].liveness,
                  ViewNodeLivenessState::kLive);

        service.set_now_unix_ms(131);
        GetClusterViewRequest cluster_request;
        cluster_request.request_id = "integration-cluster-store-3-stale";
        cluster_request.cluster_id = kClusterId;
        cluster_request.include_dead_nodes = true;
        cluster_request.include_warnings = true;

        auto cluster_result = service.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        ASSERT_EQ(cluster_result.result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_EQ(cluster_result.result.snapshot.storage_nodes.size(), 1U);
        EXPECT_EQ(cluster_result.result.snapshot.storage_nodes[0].liveness,
                  ViewNodeLivenessState::kStale);

        discover_result = service.client().DiscoverStorage(discover_request);
        ASSERT_TRUE(discover_result.transport_ok());
        EXPECT_TRUE(discover_result.result.storage_nodes.empty());
        ASSERT_FALSE(discover_result.result.diagnostics.empty());
        EXPECT_EQ(discover_result.result.diagnostics[0].code,
                  ViewRegistryIssueCode::kLivenessExcluded);

        service.set_now_unix_ms(161);
        cluster_request.request_id = "integration-cluster-store-3-suspect";
        cluster_result = service.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        ASSERT_EQ(cluster_result.result.snapshot.storage_nodes.size(), 1U);
        EXPECT_EQ(cluster_result.result.snapshot.storage_nodes[0].liveness,
                  ViewNodeLivenessState::kSuspect);

        service.set_now_unix_ms(191);
        cluster_request.request_id = "integration-cluster-store-3-dead";
        cluster_result = service.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        ASSERT_EQ(cluster_result.result.snapshot.storage_nodes.size(), 1U);
        EXPECT_EQ(cluster_result.result.snapshot.storage_nodes[0].liveness,
                  ViewNodeLivenessState::kDead);

        cluster_request.include_dead_nodes = false;
        cluster_request.request_id = "integration-cluster-store-3-dead-hidden";
        cluster_result = service.client().GetClusterView(cluster_request);
        ASSERT_TRUE(cluster_result.transport_ok());
        EXPECT_TRUE(cluster_result.result.snapshot.storage_nodes.empty());
        ASSERT_FALSE(cluster_result.result.snapshot.diagnostics.empty());
        EXPECT_EQ(cluster_result.result.snapshot.diagnostics[0].code,
                  ViewRegistryIssueCode::kLivenessExcluded);
    }
} // namespace
