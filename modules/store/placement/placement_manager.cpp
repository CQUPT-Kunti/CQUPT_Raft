#include "store/placement/placement_manager.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "store/node/storage_node_registry.h"

namespace storedemo
{
    namespace
    {
        bool HasValidCapacityFacts(
            const StorageNodeRegistryCapacityFacts &capacity)
        {
            if (capacity.total_capacity_bytes == 0)
            {
                return false;
            }

            if (capacity.used_capacity_bytes > capacity.total_capacity_bytes)
            {
                return false;
            }

            if (capacity.available_capacity_bytes > capacity.total_capacity_bytes)
            {
                return false;
            }

            return capacity.used_capacity_bytes + capacity.available_capacity_bytes <=
                   capacity.total_capacity_bytes;
        }

        std::optional<std::string> EvaluateRegistrySnapshotEligibility(
            const StorageNodeRegistryNodeSnapshot &snapshot)
        {
            if (snapshot.node_id.empty())
            {
                return "registry snapshot node_id must not be empty";
            }

            if (snapshot.liveness != StorageNodeRegistryLiveness::kLive)
            {
                return std::string("node registry facts are not live: ") +
                       (snapshot.liveness == StorageNodeRegistryLiveness::kStale
                            ? "Stale"
                            : "Dead");
            }

            if (!HasValidCapacityFacts(snapshot.facts.capacity))
            {
                return "node registry capacity facts are incomplete or invalid";
            }

            return std::nullopt;
        }

        StorageNodePlacementCandidate BuildPlacementCandidateFromSnapshot(
            const StorageNodeRegistryNodeSnapshot &snapshot)
        {
            StorageNodePlacementCandidate candidate;
            candidate.node_id = snapshot.node_id;
            candidate.endpoint = snapshot.endpoint;
            candidate.health = snapshot.facts.health.health;
            candidate.disk_pressure = snapshot.facts.health.disk_pressure;
            candidate.total_capacity_bytes = snapshot.facts.capacity.total_capacity_bytes;
            candidate.used_capacity_bytes = snapshot.facts.capacity.used_capacity_bytes;
            candidate.available_capacity_bytes =
                snapshot.facts.capacity.available_capacity_bytes;
            candidate.load = snapshot.facts.load.load;
            candidate.write_admission_overloaded =
                snapshot.facts.load.write_admission_overloaded;
            candidate.zone = snapshot.facts.failure_domain.zone;
            candidate.rack = snapshot.facts.failure_domain.rack;
            return candidate;
        }

        PlacementDecisionResult SelectPlacementFromRegistrySnapshot(
            const ReplicaPolicySelector &selector,
            const PlacementRequest &request,
            const StorageNodeRegistrySnapshotResult &registry_snapshot)
        {
            PlacementDecisionResult result;
            if (!registry_snapshot.ok())
            {
                result.status = registry_snapshot.status;
                result.error_detail = registry_snapshot.error_detail;
                result.decision.decision_epoch = request.decision_epoch;
                result.decision.required_replica_count = request.policy.replica_count;
                result.decision.minimum_successful_writes =
                    request.policy.minimum_successful_writes;
                result.decision.reasons.push_back(
                    "placement_manager registry snapshot was not available");
                return result;
            }

            std::vector<StorageNodePlacementCandidate> candidates;
            candidates.reserve(registry_snapshot.nodes.size());

            std::vector<PlacementNodeExclusion> registry_exclusions;
            registry_exclusions.reserve(registry_snapshot.nodes.size());

            for (const auto &snapshot : registry_snapshot.nodes)
            {
                const auto rejection_reason =
                    EvaluateRegistrySnapshotEligibility(snapshot);
                if (rejection_reason.has_value())
                {
                    registry_exclusions.push_back(
                        PlacementNodeExclusion{.node_id = snapshot.node_id,
                                               .reason = *rejection_reason});
                    continue;
                }

                candidates.push_back(BuildPlacementCandidateFromSnapshot(snapshot));
            }

            result = selector.SelectReplicas(request, candidates);
            result.decision.excluded_nodes.insert(result.decision.excluded_nodes.begin(),
                                                  registry_exclusions.begin(),
                                                  registry_exclusions.end());
            result.decision.reasons.insert(
                result.decision.reasons.begin(),
                "placement_manager evaluated " +
                    std::to_string(registry_snapshot.nodes.size()) +
                    " registry snapshot nodes");
            result.decision.reasons.insert(
                result.decision.reasons.begin() + 1,
                "placement_manager registry snapshot kept " +
                    std::to_string(candidates.size()) +
                    " live candidates after liveness/facts filtering");

            return result;
        }
    }

    PlacementDecisionResult PlacementManager::SelectPlacement(
        const PlacementRequest &request,
        const std::span<const StorageNodePlacementCandidate> candidates) const
    {
        auto result = selector_.SelectReplicas(request, candidates);

        result.decision.reasons.insert(
            result.decision.reasons.begin(),
            "placement_manager evaluated " + std::to_string(candidates.size()) +
                " static candidates");
        result.decision.reasons.insert(
            result.decision.reasons.begin() + 1,
            "placement_manager policy requires replica_count=" +
                std::to_string(request.policy.replica_count) +
                ", minimum_successful_writes=" +
                std::to_string(request.policy.minimum_successful_writes));

        if (!request.excluded_nodes.empty())
        {
            result.decision.reasons.insert(
                result.decision.reasons.begin() + 2,
                "placement_manager caller excluded " +
                    std::to_string(request.excluded_nodes.size()) + " nodes");
        }

        return result;
    }

    PlacementDecisionResult PlacementManager::SelectPlacement(
        const PlacementRequest &request,
        const StorageNodeRegistry &registry,
        const std::uint64_t now_unix_ms) const
    {
        auto registry_snapshot = registry.Snapshot(now_unix_ms);
        auto result =
            SelectPlacementFromRegistrySnapshot(selector_, request, registry_snapshot);
        if (result.decision.reasons.size() >= 2)
        {
            result.decision.reasons.insert(
                result.decision.reasons.begin() + 2,
                "placement_manager consumed production registry snapshot at now_unix_ms=" +
                    std::to_string(now_unix_ms));
        }
        else
        {
            result.decision.reasons.push_back(
                "placement_manager consumed production registry snapshot at now_unix_ms=" +
                std::to_string(now_unix_ms));
        }
        return result;
    }

    PlacementDecisionResult PlacementManager::SelectPlacement(
        const PlacementRequest &request,
        const StorageNodeRegistrySnapshotResult &registry_snapshot) const
    {
        return SelectPlacementFromRegistrySnapshot(selector_,
                                                   request,
                                                   registry_snapshot);
    }
}
