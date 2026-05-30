#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "store/placement/placement_manager.h"
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
        candidate.endpoint = "127.0.0.1:" + std::to_string(6000 + index);
        candidate.health = health;
        candidate.disk_pressure = disk_pressure;
        candidate.total_capacity_bytes = available_capacity_bytes + 4096;
        candidate.used_capacity_bytes = 4096;
        candidate.available_capacity_bytes = available_capacity_bytes;
        candidate.load.queued_ops = queued_ops;
        return candidate;
    }

    storedemo::PlacementRequest MakeRequest(const std::size_t replica_count,
                                            const std::size_t minimum_successful_writes,
                                            const std::uint64_t chunk_size_bytes)
    {
        storedemo::PlacementRequest request;
        request.identity.object_id = "obj-t034";
        request.identity.version = 7;
        request.identity.chunk_index = 3;
        request.chunk_size_bytes = chunk_size_bytes;
        request.policy.replica_count = replica_count;
        request.policy.minimum_successful_writes = minimum_successful_writes;
        request.policy.avoid_same_node = true;
        request.decision_epoch = 34;
        return request;
    }

    bool ContainsReason(const std::vector<std::string> &reasons,
                        const std::string &needle)
    {
        return std::any_of(reasons.begin(),
                           reasons.end(),
                           [&needle](const std::string &reason)
                           { return reason.find(needle) != std::string::npos; });
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

    TEST(StorePlacementManagerTest, SelectsReplicaCountNodesAndAddsDecisionSummary)
    {
        storedemo::PlacementManager manager;
        auto request = MakeRequest(2, 1, 512);

        std::vector<storedemo::StorageNodePlacementCandidate> candidates;
        candidates.push_back(MakeCandidate(1, 4096, 3));
        candidates.push_back(MakeCandidate(2, 8192, 1));
        candidates.push_back(MakeCandidate(3, 6144, 2));

        const auto result = manager.SelectPlacement(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 2U);
        EXPECT_EQ(result.decision.replica_nodes[0].node_id,
                  storedemo::test::MakeStorageNodeIdFixture(2));
        EXPECT_EQ(result.decision.replica_nodes[1].node_id,
                  storedemo::test::MakeStorageNodeIdFixture(3));
        EXPECT_TRUE(ContainsReason(result.decision.reasons,
                                   "placement_manager evaluated 3 static candidates"));
        EXPECT_TRUE(ContainsReason(result.decision.reasons,
                                   "replica_count=2, minimum_successful_writes=1"));
    }

    TEST(StorePlacementManagerTest, ExplicitExcludedNodesAreSkippedAndRecorded)
    {
        storedemo::PlacementManager manager;
        auto request = MakeRequest(2, 1, 256);

        auto excluded_best = MakeCandidate(1, 16384);
        auto selected_second = MakeCandidate(2, 8192);
        auto selected_third = MakeCandidate(3, 4096);
        request.excluded_nodes.push_back(excluded_best.node_id);

        std::vector<storedemo::StorageNodePlacementCandidate> candidates = {
            excluded_best, selected_second, selected_third};

        const auto result = manager.SelectPlacement(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 2U);
        EXPECT_EQ(result.decision.replica_nodes[0].node_id, selected_second.node_id);
        EXPECT_EQ(result.decision.replica_nodes[1].node_id, selected_third.node_id);

        const auto *excluded =
            FindExclusion(result.decision.excluded_nodes, excluded_best.node_id);
        ASSERT_NE(excluded, nullptr);
        EXPECT_EQ(excluded->reason, "node is explicitly excluded");
        EXPECT_TRUE(ContainsReason(result.decision.reasons,
                                   "placement_manager caller excluded 1 nodes"));
    }

    TEST(StorePlacementManagerTest, EligibilityFailuresRemainObservableThroughManager)
    {
        storedemo::PlacementManager manager;
        auto request = MakeRequest(1, 1, 1024);

        auto overloaded = MakeCandidate(1, 4096);
        overloaded.write_admission_overloaded = true;
        auto insufficient = MakeCandidate(2, 512);
        auto unhealthy = MakeCandidate(3,
                                       8192,
                                       0,
                                       storedemo::StorageNodeHealth::kDraining);
        auto selected = MakeCandidate(4, 2048);

        std::vector<storedemo::StorageNodePlacementCandidate> candidates = {
            overloaded, insufficient, unhealthy, selected};

        const auto result = manager.SelectPlacement(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.decision.replica_nodes.size(), 1U);
        EXPECT_EQ(result.decision.replica_nodes.front().node_id, selected.node_id);

        const auto *overloaded_exclusion =
            FindExclusion(result.decision.excluded_nodes, overloaded.node_id);
        ASSERT_NE(overloaded_exclusion, nullptr);
        EXPECT_EQ(overloaded_exclusion->reason,
                  "node write admission is overloaded");

        const auto *insufficient_exclusion =
            FindExclusion(result.decision.excluded_nodes, insufficient.node_id);
        ASSERT_NE(insufficient_exclusion, nullptr);
        EXPECT_EQ(insufficient_exclusion->reason,
                  "node capacity is insufficient for requested chunk");

        const auto *unhealthy_exclusion =
            FindExclusion(result.decision.excluded_nodes, unhealthy.node_id);
        ASSERT_NE(unhealthy_exclusion, nullptr);
        EXPECT_EQ(unhealthy_exclusion->reason,
                  "node health is not writable: Draining");
    }

    TEST(StorePlacementManagerTest, InsufficientEligibleNodesReturnNodeUnavailable)
    {
        storedemo::PlacementManager manager;
        auto request = MakeRequest(3, 2, 1024);

        auto selected = MakeCandidate(1, 4096);
        auto excluded = MakeCandidate(2, 8192);
        request.excluded_nodes.push_back(excluded.node_id);
        auto full_disk = MakeCandidate(3,
                                       4096,
                                       0,
                                       storedemo::StorageNodeHealth::kHealthy,
                                       storedemo::StorageNodeDiskPressure::kFull);

        std::vector<storedemo::StorageNodePlacementCandidate> candidates = {
            selected, excluded, full_disk};

        const auto result = manager.SelectPlacement(request, candidates);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_TRUE(ContainsReason(result.decision.reasons,
                                   "selection failed because eligible nodes were insufficient"));
    }
}
