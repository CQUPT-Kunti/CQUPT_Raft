#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "store/placement/replica_policy.h"
#include "support/store_test_utils.h"

namespace
{
    storedemo::StorageNodePlacementCandidate MakeCandidate(
        const std::size_t index,
        const std::uint64_t available_capacity_bytes,
        const std::uint32_t queued_ops = 0,
        const storedemo::StorageNodeHealth health =
            storedemo::StorageNodeHealth::kHealthy,
        const storedemo::StorageNodeDiskPressure disk_pressure =
            storedemo::StorageNodeDiskPressure::kLow)
    {
        storedemo::StorageNodePlacementCandidate candidate;
        candidate.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        candidate.endpoint = "127.0.0.1:" + std::to_string(5000 + index);
        candidate.health = health;
        candidate.disk_pressure = disk_pressure;
        candidate.total_capacity_bytes = available_capacity_bytes + 1024;
        candidate.used_capacity_bytes = 1024;
        candidate.available_capacity_bytes = available_capacity_bytes;
        candidate.load.queued_ops = queued_ops;
        return candidate;
    }

    storedemo::PlacementRequest MakeRequest(const std::size_t replica_count,
                                            const std::size_t minimum_successful_writes,
                                            const std::uint64_t chunk_size_bytes)
    {
        storedemo::PlacementRequest request;
        request.identity.object_id = "obj-t033";
        request.identity.version = 1;
        request.identity.chunk_index = 0;
        request.chunk_size_bytes = chunk_size_bytes;
        request.policy.replica_count = replica_count;
        request.policy.minimum_successful_writes = minimum_successful_writes;
        request.policy.avoid_same_node = true;
        request.decision_epoch = 33;
        return request;
    }

    storedemo::ReadReplicaCandidate MakeReadCandidate(
        const std::size_t index,
        const std::uint32_t active_reads = 0,
        const storedemo::StorageNodeHealth health =
            storedemo::StorageNodeHealth::kHealthy)
    {
        storedemo::ReadReplicaCandidate candidate;
        candidate.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        candidate.health = health;
        candidate.load.active_reads = active_reads;
        candidate.has_observed_facts = true;
        return candidate;
    }

    const storedemo::PlacementNodeExclusion *FindExclusion(
        const std::vector<storedemo::PlacementNodeExclusion> &excluded_nodes,
        const std::string &node_id)
    {
        const auto it = std::find_if(
            excluded_nodes.begin(),
            excluded_nodes.end(),
            [&node_id](const storedemo::PlacementNodeExclusion &exclusion)
            { return exclusion.node_id == node_id; });
        if (it == excluded_nodes.end())
        {
            return nullptr;
        }
        return &(*it);
    }

    TEST(StorePlacementPolicyTest, ReplicaCountOneSelectsHealthyNode)
    {
        storedemo::ReplicaPolicySelector selector;
        auto request = MakeRequest(1, 1, 256);

        std::vector<storedemo::StorageNodePlacementCandidate> candidates;
        candidates.push_back(MakeCandidate(1, 2048, 3));
        candidates.push_back(MakeCandidate(2, 8192, 2));
        candidates.push_back(MakeCandidate(3, 4096, 1));

        const auto result = selector.SelectReplicas(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 1U);
        EXPECT_EQ(result.decision.replica_nodes.front().node_id,
                  storedemo::test::MakeStorageNodeIdFixture(2));
        EXPECT_EQ(result.decision.required_replica_count, 1U);
        EXPECT_EQ(result.decision.minimum_successful_writes, 1U);
    }

    TEST(StorePlacementPolicyTest, ReplicaCountThreeSelectsDifferentNodes)
    {
        storedemo::ReplicaPolicySelector selector;
        auto request = MakeRequest(3, 2, 512);

        std::vector<storedemo::StorageNodePlacementCandidate> candidates;
        candidates.push_back(MakeCandidate(1, 4096));
        candidates.push_back(MakeCandidate(2, 8192));
        candidates.push_back(MakeCandidate(3, 2048));
        candidates.push_back(MakeCandidate(4, 16384));

        const auto result = selector.SelectReplicas(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 3U);

        std::unordered_set<std::string> unique_ids;
        for (const auto &candidate : result.decision.replica_nodes)
        {
            unique_ids.insert(candidate.node_id);
        }
        EXPECT_EQ(unique_ids.size(), result.decision.replica_nodes.size());
    }

    TEST(StorePlacementPolicyTest, EmptyCandidatesAndInvalidReplicaCountReturnInvalidArgument)
    {
        storedemo::ReplicaPolicySelector selector;

        auto invalid_policy_request = MakeRequest(0, 0, 512);
        const auto invalid_policy_result =
            selector.SelectReplicas(invalid_policy_request, {});
        EXPECT_EQ(invalid_policy_result.status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);

        auto empty_candidates_request = MakeRequest(1, 1, 512);
        const auto empty_candidates_result =
            selector.SelectReplicas(empty_candidates_request, {});
        EXPECT_EQ(empty_candidates_result.status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);
    }

    TEST(StorePlacementPolicyTest, InsufficientEligibleNodesReturnNodeUnavailable)
    {
        storedemo::ReplicaPolicySelector selector;
        auto request = MakeRequest(3, 2, 1024);

        std::vector<storedemo::StorageNodePlacementCandidate> candidates;
        candidates.push_back(MakeCandidate(1, 4096));
        candidates.push_back(MakeCandidate(2,
                                           4096,
                                           0,
                                           storedemo::StorageNodeHealth::kUnavailable));
        candidates.push_back(MakeCandidate(3,
                                           256,
                                           0,
                                           storedemo::StorageNodeHealth::kHealthy));

        const auto result = selector.SelectReplicas(request, candidates);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_FALSE(result.error_detail.empty());
        EXPECT_EQ(result.decision.replica_nodes.size(), 1U);
    }

    TEST(StorePlacementPolicyTest, ExcludesUnhealthyUnavailableOverloadedAndInsufficientCapacityNodes)
    {
        storedemo::ReplicaPolicySelector selector;
        auto request = MakeRequest(1, 1, 512);

        auto healthy = MakeCandidate(1, 4096);
        auto degraded = MakeCandidate(2,
                                      4096,
                                      0,
                                      storedemo::StorageNodeHealth::kDegraded);
        auto unavailable = MakeCandidate(3,
                                         4096,
                                         0,
                                         storedemo::StorageNodeHealth::kUnavailable);
        auto overloaded = MakeCandidate(4, 4096);
        overloaded.write_admission_overloaded = true;
        auto insufficient = MakeCandidate(5, 128);

        std::vector<storedemo::StorageNodePlacementCandidate> candidates = {
            degraded, unavailable, overloaded, insufficient, healthy};

        const auto result = selector.SelectReplicas(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 1U);
        EXPECT_EQ(result.decision.replica_nodes.front().node_id, healthy.node_id);
        ASSERT_EQ(result.decision.excluded_nodes.size(), 4U);
    }

    TEST(StorePlacementPolicyTest, CapacityAndLoadOrderingIsPredictable)
    {
        storedemo::ReplicaPolicySelector selector;
        auto request = MakeRequest(2, 1, 256);

        auto highest_capacity = MakeCandidate(1, 8192, 20);
        auto tie_capacity_lower_load = MakeCandidate(2, 4096, 1);
        auto tie_capacity_higher_load = MakeCandidate(3, 4096, 5);

        std::vector<storedemo::StorageNodePlacementCandidate> candidates = {
            tie_capacity_higher_load, highest_capacity, tie_capacity_lower_load};

        const auto result = selector.SelectReplicas(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 2U);
        EXPECT_EQ(result.decision.replica_nodes[0].node_id, highest_capacity.node_id);
        EXPECT_EQ(result.decision.replica_nodes[1].node_id,
                  tie_capacity_lower_load.node_id);
    }

    TEST(StorePlacementPolicyTest, DuplicateNodeIdsAreNotSelectedTwice)
    {
        storedemo::ReplicaPolicySelector selector;
        auto request = MakeRequest(2, 1, 256);

        auto best_duplicate = MakeCandidate(1, 8192);
        auto weaker_duplicate = MakeCandidate(9, 2048);
        weaker_duplicate.node_id = best_duplicate.node_id;
        auto unique_node = MakeCandidate(2, 4096);

        std::vector<storedemo::StorageNodePlacementCandidate> candidates = {
            weaker_duplicate, unique_node, best_duplicate};

        const auto result = selector.SelectReplicas(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 2U);
        EXPECT_NE(result.decision.replica_nodes[0].node_id,
                  result.decision.replica_nodes[1].node_id);
    }

    TEST(StorePlacementPolicyTest, PreferDistinctZonesWhenEnabled)
    {
        storedemo::ReplicaPolicySelector selector;
        auto request = MakeRequest(2, 1, 256);
        request.policy.prefer_distinct_zones = true;

        auto zone_a_best = MakeCandidate(1, 8192);
        zone_a_best.zone = "zone-a";
        auto zone_a_second = MakeCandidate(2, 6144);
        zone_a_second.zone = "zone-a";
        auto zone_b = MakeCandidate(3, 4096);
        zone_b.zone = "zone-b";

        std::vector<storedemo::StorageNodePlacementCandidate> candidates = {
            zone_a_best, zone_a_second, zone_b};

        const auto result = selector.SelectReplicas(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 2U);
        EXPECT_EQ(result.decision.replica_nodes[0].zone, "zone-a");
        EXPECT_EQ(result.decision.replica_nodes[1].zone, "zone-b");
    }

    TEST(StorePlacementPolicyTest,
         HealthAwarePlacementSkipsReadonlyHighPressureAndReserveShortfallNodes)
    {
        storedemo::ReplicaPolicySelector selector;
        auto request = MakeRequest(2, 1, 512);
        request.policy.reserve_capacity_bytes = 256;

        auto readonly = MakeCandidate(1,
                                      8192,
                                      0,
                                      storedemo::StorageNodeHealth::kReadOnly);
        auto draining = MakeCandidate(2,
                                      8192,
                                      0,
                                      storedemo::StorageNodeHealth::kDraining);
        auto high_pressure = MakeCandidate(3,
                                           8192,
                                           0,
                                           storedemo::StorageNodeHealth::kHealthy,
                                           storedemo::StorageNodeDiskPressure::kHigh);
        auto full_pressure = MakeCandidate(4,
                                           8192,
                                           0,
                                           storedemo::StorageNodeHealth::kHealthy,
                                           storedemo::StorageNodeDiskPressure::kFull);
        auto insufficient = MakeCandidate(5, 700);
        auto healthy_lower_load = MakeCandidate(6, 4096);
        healthy_lower_load.load.active_reads = 1;
        healthy_lower_load.load.active_writes = 1;
        healthy_lower_load.load.queued_ops = 0;
        auto healthy_higher_load = MakeCandidate(7, 4096);
        healthy_higher_load.load.active_reads = 2;
        healthy_higher_load.load.active_writes = 2;
        healthy_higher_load.load.queued_ops = 1;

        const std::vector<storedemo::StorageNodePlacementCandidate> candidates = {
            readonly,
            draining,
            high_pressure,
            full_pressure,
            insufficient,
            healthy_higher_load,
            healthy_lower_load};

        const auto result = selector.SelectReplicas(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 2U);
        EXPECT_EQ(result.decision.replica_nodes[0].node_id,
                  healthy_lower_load.node_id);
        EXPECT_EQ(result.decision.replica_nodes[1].node_id,
                  healthy_higher_load.node_id);

        const auto *readonly_exclusion =
            FindExclusion(result.decision.excluded_nodes, readonly.node_id);
        ASSERT_NE(readonly_exclusion, nullptr);
        EXPECT_EQ(readonly_exclusion->reason,
                  "node health is not writable: ReadOnly");

        const auto *draining_exclusion =
            FindExclusion(result.decision.excluded_nodes, draining.node_id);
        ASSERT_NE(draining_exclusion, nullptr);
        EXPECT_EQ(draining_exclusion->reason,
                  "node health is not writable: Draining");

        const auto *high_pressure_exclusion =
            FindExclusion(result.decision.excluded_nodes, high_pressure.node_id);
        ASSERT_NE(high_pressure_exclusion, nullptr);
        EXPECT_EQ(high_pressure_exclusion->reason,
                  "node disk pressure is too high: High");

        const auto *full_pressure_exclusion =
            FindExclusion(result.decision.excluded_nodes, full_pressure.node_id);
        ASSERT_NE(full_pressure_exclusion, nullptr);
        EXPECT_EQ(full_pressure_exclusion->reason,
                  "node disk pressure is too high: Full");

        const auto *insufficient_exclusion =
            FindExclusion(result.decision.excluded_nodes, insufficient.node_id);
        ASSERT_NE(insufficient_exclusion, nullptr);
        EXPECT_EQ(insufficient_exclusion->reason,
                  "node capacity is insufficient for requested chunk");
    }

    TEST(StorePlacementPolicyTest,
         HealthAwarePlacementKeepsStableOrderingForHealthyLowLoadNodes)
    {
        storedemo::ReplicaPolicySelector selector;
        auto request = MakeRequest(3, 2, 256);

        auto medium_pressure_low_load = MakeCandidate(
            5,
            4096,
            0,
            storedemo::StorageNodeHealth::kHealthy,
            storedemo::StorageNodeDiskPressure::kMedium);
        medium_pressure_low_load.load.active_reads = 0;
        medium_pressure_low_load.load.active_writes = 1;
        medium_pressure_low_load.load.queued_ops = 0;

        auto low_load = MakeCandidate(2, 4096);
        low_load.load.active_reads = 1;
        low_load.load.active_writes = 1;
        low_load.load.queued_ops = 0;

        auto tie_node_id_first = MakeCandidate(1, 4096);
        tie_node_id_first.load.active_reads = 2;
        tie_node_id_first.load.active_writes = 1;
        tie_node_id_first.load.queued_ops = 0;

        auto tie_node_id_second = MakeCandidate(3, 4096);
        tie_node_id_second.load.active_reads = 2;
        tie_node_id_second.load.active_writes = 1;
        tie_node_id_second.load.queued_ops = 0;

        const std::vector<storedemo::StorageNodePlacementCandidate> candidates = {
            tie_node_id_second,
            low_load,
            medium_pressure_low_load,
            tie_node_id_first};

        const auto result = selector.SelectReplicas(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 3U);
        EXPECT_EQ(result.decision.replica_nodes[0].node_id,
                  medium_pressure_low_load.node_id);
        EXPECT_EQ(result.decision.replica_nodes[1].node_id, low_load.node_id);
        EXPECT_EQ(result.decision.replica_nodes[2].node_id,
                  tie_node_id_first.node_id);
    }

    TEST(StorePlacementPolicyTest, ReadReplicaSelectionPreservesManifestOrderWhenFactsAreNeutral)
    {
        storedemo::ReplicaPolicySelector selector;
        storedemo::ReadReplicaSelectionRequest request;
        request.chunk_id = "obj-t045-neutral~1~0";
        request.replica_nodes = {
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::test::MakeStorageNodeIdFixture(3)};

        const auto result =
            selector.SelectReadReplicas(request,
                                        std::span<const storedemo::ReadReplicaCandidate>{});

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.ordered_replicas.size(), 3U);
        EXPECT_EQ(result.decision.ordered_replicas[0].node_id, request.replica_nodes[0]);
        EXPECT_EQ(result.decision.ordered_replicas[1].node_id, request.replica_nodes[1]);
        EXPECT_EQ(result.decision.ordered_replicas[2].node_id, request.replica_nodes[2]);
        EXPECT_FALSE(result.decision.ordered_replicas[0].has_observed_facts);
    }

    TEST(StorePlacementPolicyTest,
         ReadReplicaSelectionSkipsCorruptedUnavailableStaleAndOverloadedReplicas)
    {
        storedemo::ReplicaPolicySelector selector;
        storedemo::ReadReplicaSelectionRequest request;
        request.chunk_id = "obj-t045-filter~1~0";
        request.replica_nodes = {
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::test::MakeStorageNodeIdFixture(3),
            storedemo::test::MakeStorageNodeIdFixture(4),
            storedemo::test::MakeStorageNodeIdFixture(5)};

        auto corrupted = MakeReadCandidate(1, 1);
        corrupted.known_corrupted = true;
        auto unavailable = MakeReadCandidate(2, 1, storedemo::StorageNodeHealth::kUnavailable);
        auto stale = MakeReadCandidate(3, 1);
        stale.stale = true;
        auto overloaded = MakeReadCandidate(4, 1);
        overloaded.read_admission_overloaded = true;
        auto healthy = MakeReadCandidate(5, 2);

        const std::vector<storedemo::ReadReplicaCandidate> candidates = {
            corrupted, unavailable, stale, overloaded, healthy};
        const auto result = selector.SelectReadReplicas(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.ordered_replicas.size(), 1U);
        EXPECT_EQ(result.decision.ordered_replicas.front().node_id, healthy.node_id);
        ASSERT_EQ(result.decision.excluded_nodes.size(), 4U);
    }

    TEST(StorePlacementPolicyTest,
         ReadReplicaSelectionPrefersObservedFactsBeforeUnknownFallbackReplicas)
    {
        storedemo::ReplicaPolicySelector selector;
        storedemo::ReadReplicaSelectionRequest request;
        request.chunk_id = "obj-t045-observed~1~0";
        request.replica_nodes = {
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::test::MakeStorageNodeIdFixture(3)};

        auto observed_second = MakeReadCandidate(2, 1);
        auto observed_third = MakeReadCandidate(3, 5);
        const std::vector<storedemo::ReadReplicaCandidate> candidates = {
            observed_third, observed_second};

        const auto result = selector.SelectReadReplicas(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.ordered_replicas.size(), 3U);
        EXPECT_EQ(result.decision.ordered_replicas[0].node_id, observed_second.node_id);
        EXPECT_EQ(result.decision.ordered_replicas[1].node_id, observed_third.node_id);
        EXPECT_EQ(result.decision.ordered_replicas[2].node_id, request.replica_nodes[0]);
        EXPECT_FALSE(result.decision.ordered_replicas[2].has_observed_facts);
    }

    TEST(StorePlacementPolicyTest,
         ReadReplicaSelectionReturnsInvalidArgumentWhenManifestReplicaNodesEmpty)
    {
        storedemo::ReplicaPolicySelector selector;
        storedemo::ReadReplicaSelectionRequest request;
        request.chunk_id = "obj-t045-empty~1~0";

        const auto result =
            selector.SelectReadReplicas(request,
                                        std::span<const storedemo::ReadReplicaCandidate>{});

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kInvalidArgument);
        EXPECT_TRUE(result.decision.ordered_replicas.empty());
    }
}
