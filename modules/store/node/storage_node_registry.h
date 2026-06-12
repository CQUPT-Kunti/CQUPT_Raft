#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "store/common/store_types.h"
#include "store/placement/replica_policy.h"

namespace storedemo
{
    enum class StorageNodeRegistryLiveness : std::uint8_t
    {
        kLive = 0,
        kStale = 1,
        kDead = 2,
    };

    struct StorageNodeFailureDomain
    {
        std::string zone;
        std::string rack;
    };

    struct StorageNodeRegistryCapacityFacts
    {
        std::uint64_t total_capacity_bytes{0};
        std::uint64_t used_capacity_bytes{0};
        std::uint64_t available_capacity_bytes{0};
        std::uint64_t chunk_count{0};
    };

    struct StorageNodeRegistryHealthFacts
    {
        StorageNodeHealth health{StorageNodeHealth::kHealthy};
        StorageNodeDiskPressure disk_pressure{StorageNodeDiskPressure::kLow};
        std::uint64_t io_error_count{0};
    };

    struct StorageNodeRegistryLoadFacts
    {
        StorageNodeLoadSnapshot load;
        bool write_admission_overloaded{false};
        bool read_admission_overloaded{false};
    };

    struct StorageNodeRegistryFacts
    {
        StorageNodeRegistryCapacityFacts capacity;
        StorageNodeRegistryHealthFacts health;
        StorageNodeRegistryLoadFacts load;
        StorageNodeFailureDomain failure_domain;
    };

    struct StorageNodeRegistryConfig
    {
        std::uint64_t stale_timeout_ms{30'000};
        std::uint64_t dead_timeout_ms{90'000};
        bool enforce_unique_endpoints{true};
    };

    struct RegisterStorageNodeRequest
    {
        StorageNodeId node_id;
        std::string endpoint;
        std::string incarnation_id;
        std::uint64_t observed_at_unix_ms{0};
        StorageNodeRegistryFacts facts;
    };

    struct UpdateStorageNodeHeartbeatRequest
    {
        StorageNodeId node_id;
        std::string endpoint;
        std::string incarnation_id;
        std::uint64_t sequence{0};
        std::uint64_t observed_at_unix_ms{0};
        StorageNodeRegistryFacts facts;
    };

    struct ReportHealthRequest
    {
        StorageNodeId node_id;
        std::string endpoint;
        std::string incarnation_id;
        std::uint64_t sequence{0};
        std::uint64_t observed_at_unix_ms{0};
        StorageNodeRegistryHealthFacts health;
    };

    struct ReportCapacityRequest
    {
        StorageNodeId node_id;
        std::string endpoint;
        std::string incarnation_id;
        std::uint64_t sequence{0};
        std::uint64_t observed_at_unix_ms{0};
        StorageNodeRegistryCapacityFacts capacity;
    };

    struct ReportLoadRequest
    {
        StorageNodeId node_id;
        std::string endpoint;
        std::string incarnation_id;
        std::uint64_t sequence{0};
        std::uint64_t observed_at_unix_ms{0};
        StorageNodeRegistryLoadFacts load;
    };

    struct StorageNodeRegistryNodeSnapshot
    {
        StorageNodeId node_id;
        std::string endpoint;
        std::string incarnation_id;
        std::uint64_t last_sequence{0};
        std::uint64_t last_seen_unix_ms{0};
        StorageNodeRegistryLiveness liveness{StorageNodeRegistryLiveness::kDead};
        StorageNodeRegistryFacts facts;
    };

    struct RegisterStorageNodeResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        bool created{false};
        bool idempotent{false};
        StorageNodeRegistryNodeSnapshot snapshot;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    struct StorageNodeRegistryUpdateResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::uint64_t accepted_sequence{0};
        bool applied{false};
        bool idempotent{false};
        bool stale_ignored{false};
        StorageNodeRegistryNodeSnapshot snapshot;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    struct StorageNodeRegistryLookupResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        StorageNodeRegistryNodeSnapshot snapshot;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    struct StorageNodeRegistryListResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::vector<StorageNodeRegistryNodeSnapshot> nodes;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    struct StorageNodeRegistrySnapshotResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::uint64_t generated_at_unix_ms{0};
        std::vector<StorageNodeRegistryNodeSnapshot> nodes;

        [[nodiscard]] bool ok() const
        {
            return status == StorageNodeStatusCode::kOk;
        }
    };

    class StorageNodeRegistry
    {
    public:
        explicit StorageNodeRegistry(StorageNodeRegistryConfig config = {});

        RegisterStorageNodeResult RegisterStorageNode(
            const RegisterStorageNodeRequest &request);

        StorageNodeRegistryUpdateResult UpdateStorageNodeHeartbeat(
            const UpdateStorageNodeHeartbeatRequest &request);

        StorageNodeRegistryUpdateResult ReportHealth(
            const ReportHealthRequest &request);

        StorageNodeRegistryUpdateResult ReportCapacity(
            const ReportCapacityRequest &request);

        StorageNodeRegistryUpdateResult ReportLoad(
            const ReportLoadRequest &request);

        [[nodiscard]] StorageNodeRegistryLookupResult LookupNode(
            std::string_view node_id,
            std::uint64_t now_unix_ms) const;

        [[nodiscard]] StorageNodeRegistryListResult ListNodes(
            std::uint64_t now_unix_ms) const;

        [[nodiscard]] StorageNodeRegistrySnapshotResult Snapshot(
            std::uint64_t now_unix_ms) const;

        [[nodiscard]] std::size_t size() const;
        [[nodiscard]] const StorageNodeRegistryConfig &config() const;

    private:
        struct Record
        {
            std::string endpoint;
            std::string incarnation_id;
            StorageNodeRegistryFacts facts;
            std::uint64_t last_sequence{0};
            std::uint64_t last_seen_unix_ms{0};
        };

        StorageNodeRegistryConfig config_;
        mutable std::mutex mutex_;
        std::map<StorageNodeId, Record, std::less<>> records_;
    };
}
