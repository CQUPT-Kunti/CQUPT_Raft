#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "store/placement/replica_policy.h"

namespace storedemo
{
    class StorageNodeRegistry;
    struct StorageNodeRegistrySnapshotResult;

    enum class ViewNodeStorageLiveness : std::uint8_t
    {
        kUnknown = 0,
        kLive = 1,
        kStale = 2,
        kSuspect = 3,
        kDead = 4,
    };

    enum class ViewNodeBackedStorageNodeSnapshotIssueCode : std::uint16_t
    {
        kUnknown = 0,
        kSnapshotUnavailable = 1,
        kMissingNodeId = 2,
        kMissingEndpoint = 3,
        kObservationIncomplete = 4,
        kLivenessExcluded = 5,
        kCapacityInvalid = 6,
        kNonAuthorityBoundary = 7,
    };

    // 这是 ViewNode 观测到的 StorageNode 事实快照在 placement 模块内的最小边界。
    // candidate 承载健康、容量、负载、failure domain 等策略输入；liveness / freshness
    // 与 facts 完整性字段只用于过滤和诊断，不授予 ViewNode 对对象可见性或
    // Raft membership 的任何 authority。
    struct ViewNodeBackedStorageNodeSnapshot
    {
        StorageNodePlacementCandidate candidate;
        ViewNodeStorageLiveness liveness{ViewNodeStorageLiveness::kUnknown};
        std::uint64_t last_seen_unix_ms{0};
        std::uint64_t observed_at_unix_ms{0};
        std::uint64_t source_sequence{0};
        bool has_complete_facts{true};
        bool has_valid_capacity_facts{true};
    };

    struct ViewNodeBackedStorageNodeSnapshotDiagnostic
    {
        ViewNodeBackedStorageNodeSnapshotIssueCode code{
            ViewNodeBackedStorageNodeSnapshotIssueCode::kUnknown};
        StorageNodeId node_id;
        std::string message;
    };

    struct ViewNodeBackedStorageNodeSnapshotResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::uint64_t snapshot_epoch{0};
        std::uint64_t generated_at_unix_ms{0};
        std::vector<ViewNodeBackedStorageNodeSnapshot> nodes;
        std::vector<ViewNodeBackedStorageNodeSnapshotDiagnostic> diagnostics;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    // adapter 只负责把 ViewNode 的 discovery / observation 结果转换成 placement 可消费
    // 的 StorageNode 事实快照。它不能决定对象是否 COMMITTED 可见，也不能把
    // ViewNode 注册状态解释为 Raft voter membership。
    class ViewNodeBackedStorageNodeSnapshotAdapter
    {
    public:
        virtual ~ViewNodeBackedStorageNodeSnapshotAdapter() = default;

        [[nodiscard]] virtual ViewNodeBackedStorageNodeSnapshotResult
        SnapshotStorageNodes() const = 0;
    };

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

        [[nodiscard]] PlacementDecisionResult SelectPlacement(
            const PlacementRequest &request,
            const ViewNodeBackedStorageNodeSnapshotResult &view_snapshot) const;

        [[nodiscard]] PlacementDecisionResult SelectPlacement(
            const PlacementRequest &request,
            const ViewNodeBackedStorageNodeSnapshotAdapter &snapshot_adapter) const;

    private:
        ReplicaPolicySelector selector_;
    };
}
