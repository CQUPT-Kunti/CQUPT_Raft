#include "store/placement/placement_manager.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "store/node/storage_node_registry.h"
#include "view/view_registry.h"

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

        StorageNodeStatusCode MapViewRegistryStatus(
            const viewdemo::ViewRegistryStatusCode status)
        {
            switch (status)
            {
            case viewdemo::ViewRegistryStatusCode::kOk:
            case viewdemo::ViewRegistryStatusCode::kIdempotentReplay:
            case viewdemo::ViewRegistryStatusCode::kStaleIgnored:
                return StorageNodeStatusCode::kOk;
            case viewdemo::ViewRegistryStatusCode::kInvalidArgument:
                return StorageNodeStatusCode::kInvalidArgument;
            case viewdemo::ViewRegistryStatusCode::kNotFound:
                return StorageNodeStatusCode::kNotFound;
            case viewdemo::ViewRegistryStatusCode::kConflict:
                return StorageNodeStatusCode::kConflict;
            case viewdemo::ViewRegistryStatusCode::kTimeout:
                return StorageNodeStatusCode::kTimeout;
            case viewdemo::ViewRegistryStatusCode::kOverloaded:
                return StorageNodeStatusCode::kOverloaded;
            case viewdemo::ViewRegistryStatusCode::kServiceUnavailable:
                return StorageNodeStatusCode::kNodeUnavailable;
            case viewdemo::ViewRegistryStatusCode::kUnsupported:
                return StorageNodeStatusCode::kUnsupported;
            case viewdemo::ViewRegistryStatusCode::kInternalError:
                return StorageNodeStatusCode::kIoError;
            }

            return StorageNodeStatusCode::kInvalidArgument;
        }

        ViewNodeBackedStorageNodeSnapshotIssueCode MapViewRegistryIssueCode(
            const viewdemo::ViewRegistryIssueCode code)
        {
            switch (code)
            {
            case viewdemo::ViewRegistryIssueCode::kMissingNodeId:
                return ViewNodeBackedStorageNodeSnapshotIssueCode::kMissingNodeId;
            case viewdemo::ViewRegistryIssueCode::kMissingEndpoint:
                return ViewNodeBackedStorageNodeSnapshotIssueCode::kMissingEndpoint;
            case viewdemo::ViewRegistryIssueCode::kLivenessExcluded:
                return ViewNodeBackedStorageNodeSnapshotIssueCode::kLivenessExcluded;
            case viewdemo::ViewRegistryIssueCode::kCapacityInsufficient:
                return ViewNodeBackedStorageNodeSnapshotIssueCode::kCapacityInvalid;
            case viewdemo::ViewRegistryIssueCode::kHealthExcluded:
            case viewdemo::ViewRegistryIssueCode::kStaleHeartbeat:
                return ViewNodeBackedStorageNodeSnapshotIssueCode::kObservationIncomplete;
            case viewdemo::ViewRegistryIssueCode::kNonAuthorityBoundary:
                return ViewNodeBackedStorageNodeSnapshotIssueCode::kNonAuthorityBoundary;
            case viewdemo::ViewRegistryIssueCode::kNodeUnavailable:
                return ViewNodeBackedStorageNodeSnapshotIssueCode::kSnapshotUnavailable;
            default:
                return ViewNodeBackedStorageNodeSnapshotIssueCode::kUnknown;
            }
        }

        ViewNodeStorageLiveness MapViewLiveness(
            const viewdemo::ViewNodeLivenessState liveness)
        {
            switch (liveness)
            {
            case viewdemo::ViewNodeLivenessState::kLive:
                return ViewNodeStorageLiveness::kLive;
            case viewdemo::ViewNodeLivenessState::kStale:
                return ViewNodeStorageLiveness::kStale;
            case viewdemo::ViewNodeLivenessState::kSuspect:
                return ViewNodeStorageLiveness::kSuspect;
            case viewdemo::ViewNodeLivenessState::kDead:
                return ViewNodeStorageLiveness::kDead;
            case viewdemo::ViewNodeLivenessState::kUnknown:
                break;
            }

            return ViewNodeStorageLiveness::kUnknown;
        }

        StorageNodeHealth MapViewHealth(
            const viewdemo::ViewNodeHealth health)
        {
            switch (health)
            {
            case viewdemo::ViewNodeHealth::kHealthy:
                return StorageNodeHealth::kHealthy;
            case viewdemo::ViewNodeHealth::kDegraded:
                return StorageNodeHealth::kDegraded;
            case viewdemo::ViewNodeHealth::kReadOnly:
                return StorageNodeHealth::kReadOnly;
            case viewdemo::ViewNodeHealth::kDraining:
                return StorageNodeHealth::kDraining;
            case viewdemo::ViewNodeHealth::kUnavailable:
            case viewdemo::ViewNodeHealth::kUnknown:
                break;
            }

            return StorageNodeHealth::kUnavailable;
        }

        StorageNodeDiskPressure MapViewDiskPressure(
            const viewdemo::ViewNodeDiskPressure pressure)
        {
            switch (pressure)
            {
            case viewdemo::ViewNodeDiskPressure::kLow:
                return StorageNodeDiskPressure::kLow;
            case viewdemo::ViewNodeDiskPressure::kMedium:
                return StorageNodeDiskPressure::kMedium;
            case viewdemo::ViewNodeDiskPressure::kHigh:
                return StorageNodeDiskPressure::kHigh;
            case viewdemo::ViewNodeDiskPressure::kFull:
                return StorageNodeDiskPressure::kFull;
            case viewdemo::ViewNodeDiskPressure::kUnknown:
                break;
            }

            return StorageNodeDiskPressure::kFull;
        }

        bool HasCompleteViewStorageFacts(const viewdemo::ViewNodeSnapshot &snapshot)
        {
            return snapshot.node_type == viewdemo::ViewNodeType::kStorage &&
                   !snapshot.node_id.empty() &&
                   !snapshot.endpoint.empty() &&
                   snapshot.health.health != viewdemo::ViewNodeHealth::kUnknown &&
                   snapshot.health.disk_pressure !=
                       viewdemo::ViewNodeDiskPressure::kUnknown;
        }

        bool HasValidCapacityFacts(const viewdemo::ViewNodeSnapshot &snapshot)
        {
            return HasValidCapacityFacts(snapshot.capacity.total_capacity_bytes,
                                         snapshot.capacity.used_capacity_bytes,
                                         snapshot.capacity.available_capacity_bytes);
        }

        ViewNodeBackedStorageNodeSnapshot BuildViewBackedStorageSnapshot(
            const viewdemo::ViewNodeSnapshot &snapshot)
        {
            ViewNodeBackedStorageNodeSnapshot converted;
            converted.candidate.node_id = snapshot.node_id;
            converted.candidate.endpoint = snapshot.endpoint;
            converted.candidate.health = MapViewHealth(snapshot.health.health);
            converted.candidate.disk_pressure =
                MapViewDiskPressure(snapshot.health.disk_pressure);
            converted.candidate.total_capacity_bytes =
                snapshot.capacity.total_capacity_bytes;
            converted.candidate.used_capacity_bytes =
                snapshot.capacity.used_capacity_bytes;
            converted.candidate.available_capacity_bytes =
                snapshot.capacity.available_capacity_bytes;
            converted.candidate.load.active_reads = snapshot.load.active_reads;
            converted.candidate.load.active_writes = snapshot.load.active_writes;
            converted.candidate.load.queued_ops = snapshot.load.queued_ops;
            converted.candidate.write_admission_overloaded =
                snapshot.load.write_admission_overloaded;
            converted.candidate.zone = snapshot.failure_domain.zone;
            converted.candidate.rack = snapshot.failure_domain.rack;
            converted.liveness = MapViewLiveness(snapshot.liveness);
            converted.last_seen_unix_ms = snapshot.last_seen_unix_ms;
            converted.observed_at_unix_ms =
                snapshot.observed_state.observed_at_unix_ms != 0
                    ? snapshot.observed_state.observed_at_unix_ms
                    : snapshot.last_seen_unix_ms;
            converted.source_sequence = snapshot.last_sequence;
            converted.has_complete_facts = HasCompleteViewStorageFacts(snapshot);
            converted.has_valid_capacity_facts = HasValidCapacityFacts(snapshot);
            return converted;
        }

        ViewNodeBackedStorageNodeSnapshotDiagnostic BuildViewBackedDiagnostic(
            const viewdemo::ViewRegistryDiagnostic &diagnostic)
        {
            return ViewNodeBackedStorageNodeSnapshotDiagnostic{
                .code = MapViewRegistryIssueCode(diagnostic.code),
                .node_id = diagnostic.node_id,
                .message = diagnostic.message,
            };
        }

        ViewNodeBackedStorageNodeSnapshotResult BuildViewBackedSnapshotResult(
            const viewdemo::DiscoverStorageResult &discovery_result)
        {
            ViewNodeBackedStorageNodeSnapshotResult result;
            result.status = MapViewRegistryStatus(discovery_result.summary.status);
            result.error_detail = discovery_result.summary.message;
            result.snapshot_epoch = discovery_result.observed_at_unix_ms;
            result.generated_at_unix_ms = discovery_result.observed_at_unix_ms;
            result.nodes.reserve(discovery_result.storage_nodes.size());
            for (const auto &snapshot : discovery_result.storage_nodes)
            {
                result.nodes.push_back(BuildViewBackedStorageSnapshot(snapshot));
            }
            result.diagnostics.reserve(discovery_result.diagnostics.size());
            for (const auto &diagnostic : discovery_result.diagnostics)
            {
                result.diagnostics.push_back(
                    BuildViewBackedDiagnostic(diagnostic));
            }
            return result;
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
        const viewdemo::DiscoverStorageResult &view_discovery_result) const
    {
        auto view_snapshot = BuildViewBackedSnapshotResult(view_discovery_result);
        auto result = SelectPlacement(request, view_snapshot);
        const std::string reason =
            "placement_manager consumed ViewNode DiscoverStorage observed_at_unix_ms=" +
            std::to_string(view_discovery_result.observed_at_unix_ms);
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

    PlacementDecisionResult PlacementManager::SelectPlacement(
        const PlacementRequest &request,
        const viewdemo::ViewNodeRegistry &registry,
        const viewdemo::DiscoverStorageRequest &discovery_request,
        const std::uint64_t now_unix_ms) const
    {
        const auto discovery_result =
            registry.DiscoverStorage(discovery_request, now_unix_ms);
        auto result = SelectPlacement(request, discovery_result);
        const std::string reason =
            "placement_manager consumed ViewNode registry merged observed storage state at now_unix_ms=" +
            std::to_string(now_unix_ms);
        if (result.decision.reasons.size() >= 4)
        {
            result.decision.reasons.insert(result.decision.reasons.begin() + 4,
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
