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
        bool HasValidCapacityFacts(const std::uint64_t total_capacity_bytes,
                                   const std::uint64_t used_capacity_bytes,
                                   const std::uint64_t available_capacity_bytes)
        {
            if (total_capacity_bytes == 0)
            {
                return false;
            }

            if (used_capacity_bytes > total_capacity_bytes)
            {
                return false;
            }

            if (available_capacity_bytes > total_capacity_bytes)
            {
                return false;
            }

            return used_capacity_bytes + available_capacity_bytes <=
                   total_capacity_bytes;
        }

        bool HasValidCapacityFacts(
            const StorageNodeRegistryCapacityFacts &capacity)
        {
            return HasValidCapacityFacts(capacity.total_capacity_bytes,
                                         capacity.used_capacity_bytes,
                                         capacity.available_capacity_bytes);
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

        const char *ToString(const ViewNodeStorageLiveness liveness)
        {
            switch (liveness)
            {
            case ViewNodeStorageLiveness::kUnknown:
                return "Unknown";
            case ViewNodeStorageLiveness::kLive:
                return "Live";
            case ViewNodeStorageLiveness::kStale:
                return "Stale";
            case ViewNodeStorageLiveness::kSuspect:
                return "Suspect";
            case ViewNodeStorageLiveness::kDead:
                return "Dead";
            }

            return "Unknown";
        }

        std::optional<std::string> EvaluateViewSnapshotEligibility(
            const ViewNodeBackedStorageNodeSnapshot &snapshot)
        {
            if (snapshot.candidate.node_id.empty())
            {
                return "view-backed snapshot node_id must not be empty";
            }

            if (snapshot.candidate.endpoint.empty())
            {
                return "view-backed snapshot endpoint must not be empty";
            }

            if (!snapshot.has_complete_facts)
            {
                return "view-backed snapshot facts are incomplete";
            }

            // ViewNode 只提供观测事实；placement 只接受明确 live 的节点进入策略层。
            if (snapshot.liveness != ViewNodeStorageLiveness::kLive)
            {
                return std::string("view-backed snapshot is not live: ") +
                       ToString(snapshot.liveness);
            }

            // freshness 的时间戳必须自洽；否则宁可保守排除也不静默选点。
            if (snapshot.observed_at_unix_ms != 0 &&
                snapshot.last_seen_unix_ms != 0 &&
                snapshot.last_seen_unix_ms > snapshot.observed_at_unix_ms)
            {
                return "view-backed snapshot freshness facts are invalid";
            }

            if (!snapshot.has_valid_capacity_facts ||
                !HasValidCapacityFacts(
                    snapshot.candidate.total_capacity_bytes,
                    snapshot.candidate.used_capacity_bytes,
                    snapshot.candidate.available_capacity_bytes))
            {
                return "view-backed snapshot capacity facts are incomplete or invalid";
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

        PlacementDecisionResult SelectPlacementFromViewSnapshot(
            const ReplicaPolicySelector &selector,
            const PlacementRequest &request,
            const ViewNodeBackedStorageNodeSnapshotResult &view_snapshot)
        {
            PlacementDecisionResult result;
            if (!view_snapshot.ok())
            {
                result.status = view_snapshot.status;
                result.error_detail = view_snapshot.error_detail;
                result.decision.decision_epoch = request.decision_epoch;
                result.decision.required_replica_count = request.policy.replica_count;
                result.decision.minimum_successful_writes =
                    request.policy.minimum_successful_writes;
                result.decision.reasons.push_back(
                    "placement_manager view-backed snapshot was not available");
                return result;
            }

            std::vector<StorageNodePlacementCandidate> candidates;
            candidates.reserve(view_snapshot.nodes.size());

            std::vector<PlacementNodeExclusion> view_exclusions;
            view_exclusions.reserve(view_snapshot.nodes.size());

            for (const auto &snapshot : view_snapshot.nodes)
            {
                const auto rejection_reason =
                    EvaluateViewSnapshotEligibility(snapshot);
                if (rejection_reason.has_value())
                {
                    view_exclusions.push_back(
                        PlacementNodeExclusion{.node_id = snapshot.candidate.node_id,
                                               .reason = *rejection_reason});
                    continue;
                }

                candidates.push_back(snapshot.candidate);
            }

            result = selector.SelectReplicas(request, candidates);
            result.decision.excluded_nodes.insert(result.decision.excluded_nodes.begin(),
                                                  view_exclusions.begin(),
                                                  view_exclusions.end());
            result.decision.reasons.insert(
                result.decision.reasons.begin(),
                "placement_manager evaluated " +
                    std::to_string(view_snapshot.nodes.size()) +
                    " view-backed snapshot nodes");
            result.decision.reasons.insert(
                result.decision.reasons.begin() + 1,
                "placement_manager view-backed snapshot kept " +
                    std::to_string(candidates.size()) +
                    " live candidates after liveness/facts filtering");

            if (!view_snapshot.diagnostics.empty())
            {
                result.decision.reasons.insert(
                    result.decision.reasons.begin() + 2,
                    "placement_manager observed " +
                        std::to_string(view_snapshot.diagnostics.size()) +
                        " upstream view snapshot diagnostics");
            }

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

    PlacementDecisionResult PlacementManager::SelectPlacement(
        const PlacementRequest &request,
        const ViewNodeBackedStorageNodeSnapshotResult &view_snapshot) const
    {
        auto result =
            SelectPlacementFromViewSnapshot(selector_, request, view_snapshot);
        const auto reason =
            "placement_manager consumed view-backed snapshot epoch=" +
            std::to_string(view_snapshot.snapshot_epoch) +
            " generated_at_unix_ms=" +
            std::to_string(view_snapshot.generated_at_unix_ms);
        if (result.decision.reasons.size() >= 2)
        {
            result.decision.reasons.insert(result.decision.reasons.begin() + 2,
                                           reason);
        }
        else
        {
            result.decision.reasons.push_back(reason);
        }
        return result;
    }

    PlacementDecisionResult PlacementManager::SelectPlacement(
        const PlacementRequest &request,
        const ViewNodeBackedStorageNodeSnapshotAdapter &snapshot_adapter) const
    {
        // adapter 只提供 ViewNode 观测事实；最终 placement policy 仍由本模块执行。
        auto view_snapshot = snapshot_adapter.SnapshotStorageNodes();
        auto result = SelectPlacement(request, view_snapshot);
        const std::string reason =
            "placement_manager consumed view-backed snapshot adapter";
        if (result.decision.reasons.size() >= 3)
        {
            result.decision.reasons.insert(result.decision.reasons.begin() + 3,
                                           reason);
        }
        else
        {
            result.decision.reasons.push_back(reason);
        }
        return result;
    }
}
