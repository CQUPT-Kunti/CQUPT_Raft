#include "store/placement/placement_manager.h"

#include <string>

namespace storedemo
{
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
}
