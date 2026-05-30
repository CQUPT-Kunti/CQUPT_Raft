#include <gtest/gtest.h>

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
}
