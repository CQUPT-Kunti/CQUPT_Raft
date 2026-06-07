#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "view/view_registry.h"

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
    using viewdemo::ViewNodeLivenessState;
    using viewdemo::ViewNodeRegistry;
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

    void RegisterNodeOrAssert(ViewNodeRegistry *registry,
                              const RegisterNodeRequest &request)
    {
        const auto result = registry->RegisterNode(request);
        ASSERT_EQ(result.summary.status, ViewRegistryStatusCode::kOk);
        ASSERT_TRUE(result.created);
        ASSERT_TRUE(result.snapshot.has_value());
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
} // namespace
