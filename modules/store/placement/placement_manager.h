#pragma once

#include <span>

#include "store/placement/replica_policy.h"

namespace storedemo
{
    class PlacementManager
    {
    public:
        PlacementManager() = default;

        [[nodiscard]] PlacementDecisionResult SelectPlacement(
            const PlacementRequest &request,
            std::span<const StorageNodePlacementCandidate> candidates) const;

    private:
        ReplicaPolicySelector selector_;
    };
}
