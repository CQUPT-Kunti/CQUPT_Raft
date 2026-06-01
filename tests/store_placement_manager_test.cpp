#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
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

    struct TestRegistryPlacementFact
    {
        storedemo::StorageNodePlacementCandidate placement;
        std::uint64_t last_seen_unix_ms{0};
    };

    bool IsFactStale(const std::uint64_t last_seen_unix_ms,
                     const std::uint64_t now_unix_ms,
                     const std::uint64_t liveness_timeout_ms)
    {
        if (last_seen_unix_ms == 0 || liveness_timeout_ms == 0)
        {
            return true;
        }

        return now_unix_ms > last_seen_unix_ms + liveness_timeout_ms;
    }

    std::vector<storedemo::StorageNodePlacementCandidate>
    BuildPlacementCandidatesFromRegistryFacts(
        const std::vector<TestRegistryPlacementFact> &facts,
        const std::uint64_t now_unix_ms,
        const std::uint64_t liveness_timeout_ms)
    {
        std::vector<storedemo::StorageNodePlacementCandidate> candidates;
        candidates.reserve(facts.size());

        for (const auto &fact : facts)
        {
            auto candidate = fact.placement;
            if (IsFactStale(fact.last_seen_unix_ms,
                            now_unix_ms,
                            liveness_timeout_ms))
            {
                candidate.health = storedemo::StorageNodeHealth::kUnavailable;
            }
            candidates.push_back(std::move(candidate));
        }

        return candidates;
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

    TEST(StorePlacementManagerTest,
         RegistryFactsPreferFreshHealthyWritableNodesAndSkipStaleFacts)
    {
        storedemo::PlacementManager manager;
        auto request = MakeRequest(3, 2, 512);
        request.policy.reserve_capacity_bytes = 256;

        auto stale_best = MakeCandidate(1, 32768);
        stale_best.load.active_reads = 0;
        stale_best.load.active_writes = 0;
        stale_best.load.queued_ops = 0;

        auto readonly = MakeCandidate(2,
                                      16384,
                                      0,
                                      storedemo::StorageNodeHealth::kReadOnly);
        auto overloaded = MakeCandidate(3, 12288);
        overloaded.write_admission_overloaded = true;
        auto high_pressure = MakeCandidate(4,
                                           14336,
                                           0,
                                           storedemo::StorageNodeHealth::kHealthy,
                                           storedemo::StorageNodeDiskPressure::kHigh);
        auto duplicate_best = MakeCandidate(5, 16384);
        duplicate_best.load.active_reads = 0;
        duplicate_best.load.active_writes = 1;
        duplicate_best.load.queued_ops = 0;

        auto duplicate_weaker = MakeCandidate(9, 8192);
        duplicate_weaker.node_id = duplicate_best.node_id;
        duplicate_weaker.load.active_reads = 5;
        duplicate_weaker.load.active_writes = 5;
        duplicate_weaker.load.queued_ops = 5;

        auto healthy_low_load = MakeCandidate(6, 14336);
        healthy_low_load.load.active_reads = 0;
        healthy_low_load.load.active_writes = 1;
        healthy_low_load.load.queued_ops = 0;

        auto healthy_medium_pressure = MakeCandidate(
            7,
            14336,
            0,
            storedemo::StorageNodeHealth::kHealthy,
            storedemo::StorageNodeDiskPressure::kMedium);
        healthy_medium_pressure.load.active_reads = 0;
        healthy_medium_pressure.load.active_writes = 2;
        healthy_medium_pressure.load.queued_ops = 1;

        auto insufficient = MakeCandidate(8, 700);

        const std::vector<TestRegistryPlacementFact> registry_facts = {
            {.placement = stale_best, .last_seen_unix_ms = 100},
            {.placement = readonly, .last_seen_unix_ms = 225},
            {.placement = overloaded, .last_seen_unix_ms = 226},
            {.placement = high_pressure, .last_seen_unix_ms = 227},
            {.placement = duplicate_weaker, .last_seen_unix_ms = 228},
            {.placement = healthy_medium_pressure, .last_seen_unix_ms = 229},
            {.placement = healthy_low_load, .last_seen_unix_ms = 230},
            {.placement = duplicate_best, .last_seen_unix_ms = 231},
            {.placement = insufficient, .last_seen_unix_ms = 232}};

        const auto candidates =
            BuildPlacementCandidatesFromRegistryFacts(registry_facts, 240, 20);
        const auto result = manager.SelectPlacement(request, candidates);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_EQ(result.decision.replica_nodes.size(), 3U);
        EXPECT_EQ(result.decision.replica_nodes[0].node_id, duplicate_best.node_id);
        EXPECT_EQ(result.decision.replica_nodes[1].node_id, healthy_low_load.node_id);
        EXPECT_EQ(result.decision.replica_nodes[2].node_id,
                  healthy_medium_pressure.node_id);

        std::unordered_set<std::string> unique_ids;
        for (const auto &candidate : result.decision.replica_nodes)
        {
            unique_ids.insert(candidate.node_id);
        }
        EXPECT_EQ(unique_ids.size(), result.decision.replica_nodes.size());

        const auto *stale_exclusion =
            FindExclusion(result.decision.excluded_nodes, stale_best.node_id);
        ASSERT_NE(stale_exclusion, nullptr);
        EXPECT_EQ(stale_exclusion->reason,
                  "node health is not writable: Unavailable");

        const auto *readonly_exclusion =
            FindExclusion(result.decision.excluded_nodes, readonly.node_id);
        ASSERT_NE(readonly_exclusion, nullptr);
        EXPECT_EQ(readonly_exclusion->reason,
                  "node health is not writable: ReadOnly");

        const auto *overloaded_exclusion =
            FindExclusion(result.decision.excluded_nodes, overloaded.node_id);
        ASSERT_NE(overloaded_exclusion, nullptr);
        EXPECT_EQ(overloaded_exclusion->reason,
                  "node write admission is overloaded");

        const auto *high_pressure_exclusion =
            FindExclusion(result.decision.excluded_nodes, high_pressure.node_id);
        ASSERT_NE(high_pressure_exclusion, nullptr);
        EXPECT_EQ(high_pressure_exclusion->reason,
                  "node disk pressure is too high: High");

        EXPECT_TRUE(ContainsReason(result.decision.reasons,
                                   "placement_manager evaluated 9 static candidates"));
    }

    TEST(StorePlacementManagerTest,
         RegistryFactsAllIneligibleReturnNodeUnavailableWithObservableReasons)
    {
        storedemo::PlacementManager manager;
        auto request = MakeRequest(2, 1, 1024);
        request.policy.reserve_capacity_bytes = 64;

        auto stale = MakeCandidate(1, 32768);
        auto full = MakeCandidate(2,
                                  16384,
                                  0,
                                  storedemo::StorageNodeHealth::kHealthy,
                                  storedemo::StorageNodeDiskPressure::kFull);
        auto overloaded = MakeCandidate(3, 16384);
        overloaded.write_admission_overloaded = true;
        auto insufficient = MakeCandidate(4, 1024);

        const std::vector<TestRegistryPlacementFact> registry_facts = {
            {.placement = stale, .last_seen_unix_ms = 10},
            {.placement = full, .last_seen_unix_ms = 95},
            {.placement = overloaded, .last_seen_unix_ms = 96},
            {.placement = insufficient, .last_seen_unix_ms = 97}};

        const auto candidates =
            BuildPlacementCandidatesFromRegistryFacts(registry_facts, 100, 20);
        const auto result = manager.SelectPlacement(request, candidates);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_TRUE(result.decision.replica_nodes.empty());
        EXPECT_FALSE(result.error_detail.empty());
        EXPECT_TRUE(ContainsReason(result.decision.reasons,
                                   "selection failed because eligible nodes were insufficient"));

        const auto *stale_exclusion =
            FindExclusion(result.decision.excluded_nodes, stale.node_id);
        ASSERT_NE(stale_exclusion, nullptr);
        EXPECT_EQ(stale_exclusion->reason,
                  "node health is not writable: Unavailable");
    }
}
