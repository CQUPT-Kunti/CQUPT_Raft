#pragma once

#include <cstdint>
#include <span>

#include "store/placement/replica_policy.h"

namespace storedemo
{
    class StorageNodeRegistry;
    struct StorageNodeRegistrySnapshotResult;

    class PlacementManager
    {
    public:
        PlacementManager() = default;

        [[nodiscard]] PlacementDecisionResult SelectPlacement(
            const PlacementRequest &request,
            std::span<const StorageNodePlacementCandidate> candidates) const;

        [[nodiscard]] PlacementDecisionResult SelectPlacement(
            const PlacementRequest &request,
            const StorageNodeRegistry &registry,
            std::uint64_t now_unix_ms) const;

        [[nodiscard]] PlacementDecisionResult SelectPlacement(
            const PlacementRequest &request,
            const StorageNodeRegistrySnapshotResult &registry_snapshot) const;

    private:
        ReplicaPolicySelector selector_;
    };
}
