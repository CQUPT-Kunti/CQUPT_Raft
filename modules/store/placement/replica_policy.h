#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "store/common/store_types.h"

namespace storedemo
{
    enum class StorageNodeHealth : std::uint8_t
    {
        kHealthy = 0,
        kDegraded = 1,
        kReadOnly = 2,
        kUnavailable = 3,
        kDraining = 4,
    };

    enum class StorageNodeDiskPressure : std::uint8_t
    {
        kLow = 0,
        kMedium = 1,
        kHigh = 2,
        kFull = 3,
    };

    const char *ToString(StorageNodeHealth health);
    const char *ToString(StorageNodeDiskPressure pressure);

    struct StorageNodeLoadSnapshot
    {
        std::uint32_t active_reads{0};
        std::uint32_t active_writes{0};
        std::uint32_t queued_ops{0};

        [[nodiscard]] std::uint64_t TotalInflight() const;
    };

    struct StorageNodePlacementCandidate
    {
        StorageNodeId node_id;
        std::string endpoint;
        StorageNodeHealth health{StorageNodeHealth::kHealthy};
        StorageNodeDiskPressure disk_pressure{StorageNodeDiskPressure::kLow};
        std::uint64_t total_capacity_bytes{0};
        std::uint64_t used_capacity_bytes{0};
        std::uint64_t available_capacity_bytes{0};
        StorageNodeLoadSnapshot load;
        bool write_admission_overloaded{false};
        std::string zone;
        std::string rack;

        [[nodiscard]] bool CanFit(std::uint64_t required_bytes,
                                  std::uint64_t reserve_bytes) const;
        [[nodiscard]] bool HasWritableHealth() const;
    };

    struct ReplicaPolicy
    {
        std::size_t replica_count{3};
        std::size_t minimum_successful_writes{2};
        bool avoid_same_node{true};
        bool prefer_distinct_zones{false};
        std::uint64_t reserve_capacity_bytes{0};
    };

    struct PlacementRequest
    {
        ChunkIdentity identity;
        std::uint64_t chunk_size_bytes{0};
        ReplicaPolicy policy;
        std::vector<StorageNodeId> excluded_nodes;
        std::uint64_t decision_epoch{0};
    };

    struct PlacementNodeExclusion
    {
        StorageNodeId node_id;
        std::string reason;
    };

    struct PlacementDecision
    {
        ChunkId chunk_id;
        std::vector<StorageNodePlacementCandidate> replica_nodes;
        std::size_t required_replica_count{0};
        std::size_t minimum_successful_writes{0};
        std::vector<PlacementNodeExclusion> excluded_nodes;
        std::uint64_t decision_epoch{0};
        std::vector<std::string> reasons;
    };

    struct PlacementDecisionResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        PlacementDecision decision;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    class ReplicaPolicySelector
    {
    public:
        PlacementDecisionResult SelectReplicas(
            const PlacementRequest &request,
            std::span<const StorageNodePlacementCandidate> candidates) const;
    };
}
