#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "store/common/store_types.h"
#include "store/placement/replica_policy.h"
#include "support/store_test_utils.h"

namespace
{
    enum class TestRegistryNodeLiveness : std::uint8_t
    {
        kLive = 0,
        kStale = 1,
    };

    struct TestRegistryNodeRecord
    {
        storedemo::StorageNodePlacementCandidate placement;
        std::uint64_t chunk_count{0};
        std::uint64_t io_error_count{0};
        std::uint64_t last_sequence{0};
        std::uint64_t last_seen_unix_ms{0};
    };

    struct TestRegisterNodeRequest
    {
        storedemo::StorageNodeId node_id;
        std::string endpoint;
        std::uint64_t observed_at_unix_ms{0};
        std::uint64_t chunk_count{0};
        std::uint64_t io_error_count{0};
        storedemo::StorageNodePlacementCandidate placement;
    };

    struct TestHeartbeatRequest
    {
        storedemo::StorageNodeId node_id;
        std::string endpoint;
        std::uint64_t sequence{0};
        std::uint64_t observed_at_unix_ms{0};
        std::uint64_t chunk_count{0};
        std::uint64_t io_error_count{0};
        storedemo::StorageNodePlacementCandidate placement;
    };

    struct TestRegisterNodeResult
    {
        storedemo::StorageNodeStatusCode status{storedemo::StorageNodeStatusCode::kOk};
        bool created{false};
        bool idempotent{false};
    };

    struct TestHeartbeatResult
    {
        storedemo::StorageNodeStatusCode status{storedemo::StorageNodeStatusCode::kOk};
        bool applied{false};
        bool idempotent{false};
        bool stale_ignored{false};
    };

    struct TestRegistryNodeSnapshot
    {
        TestRegistryNodeRecord record;
        TestRegistryNodeLiveness liveness{TestRegistryNodeLiveness::kStale};
    };

    bool IsValidNodeId(const std::string &node_id)
    {
        if (node_id.empty())
        {
            return false;
        }

        return std::all_of(node_id.begin(),
                           node_id.end(),
                           [](const unsigned char ch)
                           {
                               return std::isalnum(ch) != 0 || ch == '-' || ch == '_';
                           });
    }

    bool IsValidEndpoint(const std::string &endpoint)
    {
        const auto separator = endpoint.rfind(':');
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 >= endpoint.size())
        {
            return false;
        }

        const auto port = endpoint.substr(separator + 1);
        if (!std::all_of(port.begin(),
                         port.end(),
                         [](const unsigned char ch)
                         { return std::isdigit(ch) != 0; }))
        {
            return false;
        }

        try
        {
            const auto parsed = std::stoul(port);
            return parsed > 0 && parsed <= 65535;
        }
        catch (const std::exception &)
        {
            return false;
        }
    }

    bool HasConsistentCapacity(const storedemo::StorageNodePlacementCandidate &candidate)
    {
        if (candidate.total_capacity_bytes == 0)
        {
            return false;
        }
        if (candidate.used_capacity_bytes > candidate.total_capacity_bytes)
        {
            return false;
        }
        if (candidate.available_capacity_bytes > candidate.total_capacity_bytes)
        {
            return false;
        }

        return candidate.used_capacity_bytes + candidate.available_capacity_bytes <=
               candidate.total_capacity_bytes;
    }

    TestRegistryNodeLiveness DetermineLiveness(const TestRegistryNodeRecord &record,
                                               const std::uint64_t now_unix_ms,
                                               const std::uint64_t timeout_ms)
    {
        if (timeout_ms == 0)
        {
            return TestRegistryNodeLiveness::kStale;
        }

        return now_unix_ms <= record.last_seen_unix_ms + timeout_ms
                   ? TestRegistryNodeLiveness::kLive
                   : TestRegistryNodeLiveness::kStale;
    }

    class TestStorageNodeHeartbeatRegistry
    {
    public:
        TestRegisterNodeResult RegisterNode(const TestRegisterNodeRequest &request)
        {
            TestRegisterNodeResult result;
            result.status = ValidateNodeRequest(request.node_id,
                                                request.endpoint,
                                                request.placement,
                                                request.observed_at_unix_ms);
            if (result.status != storedemo::StorageNodeStatusCode::kOk)
            {
                return result;
            }

            auto existing = records_.find(request.node_id);
            if (existing != records_.end())
            {
                if (existing->second.placement.endpoint != request.endpoint)
                {
                    result.status = storedemo::StorageNodeStatusCode::kConflict;
                    return result;
                }

                result.idempotent = true;
                return result;
            }

            TestRegistryNodeRecord record;
            record.placement = request.placement;
            record.placement.node_id = request.node_id;
            record.placement.endpoint = request.endpoint;
            record.chunk_count = request.chunk_count;
            record.io_error_count = request.io_error_count;
            record.last_seen_unix_ms = request.observed_at_unix_ms;
            records_.emplace(request.node_id, std::move(record));

            result.created = true;
            return result;
        }

        TestHeartbeatResult ApplyHeartbeat(const TestHeartbeatRequest &request)
        {
            TestHeartbeatResult result;
            result.status = ValidateNodeRequest(request.node_id,
                                                request.endpoint,
                                                request.placement,
                                                request.observed_at_unix_ms);
            if (result.status != storedemo::StorageNodeStatusCode::kOk)
            {
                return result;
            }
            if (request.sequence == 0)
            {
                result.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
                return result;
            }

            auto existing = records_.find(request.node_id);
            if (existing == records_.end())
            {
                result.status = storedemo::StorageNodeStatusCode::kNotFound;
                return result;
            }
            if (existing->second.placement.endpoint != request.endpoint)
            {
                result.status = storedemo::StorageNodeStatusCode::kConflict;
                return result;
            }
            if (request.sequence < existing->second.last_sequence)
            {
                result.status = storedemo::StorageNodeStatusCode::kAlreadyExists;
                result.stale_ignored = true;
                return result;
            }
            if (request.sequence == existing->second.last_sequence)
            {
                result.idempotent = true;
                return result;
            }

            existing->second.placement = request.placement;
            existing->second.placement.node_id = request.node_id;
            existing->second.placement.endpoint = request.endpoint;
            existing->second.chunk_count = request.chunk_count;
            existing->second.io_error_count = request.io_error_count;
            existing->second.last_sequence = request.sequence;
            existing->second.last_seen_unix_ms = request.observed_at_unix_ms;
            result.applied = true;
            return result;
        }

        std::optional<TestRegistryNodeSnapshot> LookupNode(
            const std::string &node_id,
            const std::uint64_t now_unix_ms,
            const std::uint64_t liveness_timeout_ms) const
        {
            const auto existing = records_.find(node_id);
            if (existing == records_.end())
            {
                return std::nullopt;
            }

            return TestRegistryNodeSnapshot{
                .record = existing->second,
                .liveness = DetermineLiveness(existing->second,
                                              now_unix_ms,
                                              liveness_timeout_ms)};
        }

        std::vector<TestRegistryNodeSnapshot> ListNodes(
            const std::uint64_t now_unix_ms,
            const std::uint64_t liveness_timeout_ms) const
        {
            std::vector<TestRegistryNodeSnapshot> snapshots;
            snapshots.reserve(records_.size());
            for (const auto &[node_id, record] : records_)
            {
                (void)node_id;
                snapshots.push_back(TestRegistryNodeSnapshot{
                    .record = record,
                    .liveness = DetermineLiveness(record,
                                                  now_unix_ms,
                                                  liveness_timeout_ms)});
            }
            return snapshots;
        }

        [[nodiscard]] std::size_t size() const
        {
            return records_.size();
        }

    private:
        storedemo::StorageNodeStatusCode ValidateNodeRequest(
            const std::string &node_id,
            const std::string &endpoint,
            const storedemo::StorageNodePlacementCandidate &placement,
            const std::uint64_t observed_at_unix_ms) const
        {
            if (!IsValidNodeId(node_id))
            {
                return storedemo::StorageNodeStatusCode::kInvalidArgument;
            }
            if (!IsValidEndpoint(endpoint))
            {
                return storedemo::StorageNodeStatusCode::kInvalidArgument;
            }
            if (!HasConsistentCapacity(placement))
            {
                return storedemo::StorageNodeStatusCode::kInvalidArgument;
            }
            if (observed_at_unix_ms == 0)
            {
                return storedemo::StorageNodeStatusCode::kInvalidArgument;
            }
            return storedemo::StorageNodeStatusCode::kOk;
        }

        std::map<std::string, TestRegistryNodeRecord> records_;
    };

    storedemo::StorageNodePlacementCandidate MakePlacementCandidate(
        const std::size_t index,
        const std::uint64_t total_capacity_bytes = 8192,
        const std::uint64_t used_capacity_bytes = 2048,
        const storedemo::StorageNodeHealth health =
            storedemo::StorageNodeHealth::kHealthy,
        const storedemo::StorageNodeDiskPressure disk_pressure =
            storedemo::StorageNodeDiskPressure::kLow)
    {
        storedemo::StorageNodePlacementCandidate candidate;
        candidate.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        candidate.endpoint = "127.0.0.1:" + std::to_string(7000 + index);
        candidate.health = health;
        candidate.disk_pressure = disk_pressure;
        candidate.total_capacity_bytes = total_capacity_bytes;
        candidate.used_capacity_bytes = used_capacity_bytes;
        candidate.available_capacity_bytes =
            total_capacity_bytes >= used_capacity_bytes
                ? total_capacity_bytes - used_capacity_bytes
                : 0;
        candidate.load.active_reads = static_cast<std::uint32_t>(index);
        candidate.load.active_writes = static_cast<std::uint32_t>(index + 1);
        candidate.load.queued_ops = static_cast<std::uint32_t>(index + 2);
        candidate.zone = "zone-" + std::to_string(index % 2);
        candidate.rack = "rack-" + std::to_string(index);
        return candidate;
    }

    TestRegisterNodeRequest MakeRegisterRequest(const std::size_t index,
                                                const std::uint64_t observed_at_unix_ms)
    {
        TestRegisterNodeRequest request;
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.placement = MakePlacementCandidate(index);
        request.endpoint = request.placement.endpoint;
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.chunk_count = 10 + index;
        request.io_error_count = index;
        return request;
    }

    TestHeartbeatRequest MakeHeartbeatRequest(const std::size_t index,
                                              const std::uint64_t sequence,
                                              const std::uint64_t observed_at_unix_ms)
    {
        TestHeartbeatRequest request;
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.placement = MakePlacementCandidate(index);
        request.endpoint = request.placement.endpoint;
        request.sequence = sequence;
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.chunk_count = 20 + index;
        request.io_error_count = 5 + index;
        return request;
    }

    TEST(StorageHeartbeatRegistryTest, RegisterStoresNodeFactsAndListOrderIsStable)
    {
        TestStorageNodeHeartbeatRegistry registry;
        auto node_b = MakeRegisterRequest(2, 100);
        auto node_a = MakeRegisterRequest(1, 105);

        ASSERT_EQ(registry.RegisterNode(node_b).status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(registry.RegisterNode(node_a).status,
                  storedemo::StorageNodeStatusCode::kOk);

        ASSERT_EQ(registry.size(), 2U);
        const auto snapshots = registry.ListNodes(120, 50);
        ASSERT_EQ(snapshots.size(), 2U);
        EXPECT_EQ(snapshots[0].record.placement.node_id, node_a.node_id);
        EXPECT_EQ(snapshots[1].record.placement.node_id, node_b.node_id);
        EXPECT_EQ(snapshots[0].record.placement.total_capacity_bytes,
                  node_a.placement.total_capacity_bytes);
        EXPECT_EQ(snapshots[0].record.chunk_count, node_a.chunk_count);
        EXPECT_EQ(snapshots[0].liveness, TestRegistryNodeLiveness::kLive);

        const auto lookup = registry.LookupNode(node_b.node_id, 120, 50);
        ASSERT_TRUE(lookup.has_value());
        EXPECT_EQ(lookup->record.placement.endpoint, node_b.endpoint);
        EXPECT_EQ(lookup->record.io_error_count, node_b.io_error_count);
        EXPECT_EQ(lookup->record.last_seen_unix_ms, node_b.observed_at_unix_ms);
    }

    TEST(StorageHeartbeatRegistryTest,
         DuplicateRegisterIsIdempotentAndEndpointChangeConflicts)
    {
        TestStorageNodeHeartbeatRegistry registry;
        const auto original = MakeRegisterRequest(1, 100);
        auto duplicate = original;
        duplicate.placement.total_capacity_bytes = 16384;
        duplicate.placement.available_capacity_bytes = 14336;

        const auto first = registry.RegisterNode(original);
        ASSERT_EQ(first.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(first.created);

        const auto second = registry.RegisterNode(duplicate);
        EXPECT_EQ(second.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(second.idempotent);
        EXPECT_EQ(registry.size(), 1U);

        const auto snapshot = registry.LookupNode(original.node_id, 110, 50);
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_EQ(snapshot->record.placement.total_capacity_bytes,
                  original.placement.total_capacity_bytes);

        auto changed_endpoint = original;
        changed_endpoint.endpoint = "127.0.0.1:7999";
        changed_endpoint.placement.endpoint = changed_endpoint.endpoint;
        const auto conflict = registry.RegisterNode(changed_endpoint);
        EXPECT_EQ(conflict.status, storedemo::StorageNodeStatusCode::kConflict);
        EXPECT_EQ(registry.size(), 1U);
    }

    TEST(StorageHeartbeatRegistryTest, HeartbeatUpdatesCapacityHealthLoadAndPressureFacts)
    {
        TestStorageNodeHeartbeatRegistry registry;
        ASSERT_EQ(registry.RegisterNode(MakeRegisterRequest(1, 100)).status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto heartbeat = MakeHeartbeatRequest(1, 7, 160);
        heartbeat.placement.health = storedemo::StorageNodeHealth::kDegraded;
        heartbeat.placement.disk_pressure = storedemo::StorageNodeDiskPressure::kMedium;
        heartbeat.placement.total_capacity_bytes = 32768;
        heartbeat.placement.used_capacity_bytes = 12288;
        heartbeat.placement.available_capacity_bytes = 20480;
        heartbeat.placement.load.active_reads = 8;
        heartbeat.placement.load.active_writes = 3;
        heartbeat.placement.load.queued_ops = 11;
        heartbeat.chunk_count = 77;
        heartbeat.io_error_count = 9;

        const auto result = registry.ApplyHeartbeat(heartbeat);
        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(result.applied);

        const auto snapshot = registry.LookupNode(heartbeat.node_id, 170, 20);
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_EQ(snapshot->record.placement.health,
                  storedemo::StorageNodeHealth::kDegraded);
        EXPECT_EQ(snapshot->record.placement.disk_pressure,
                  storedemo::StorageNodeDiskPressure::kMedium);
        EXPECT_EQ(snapshot->record.placement.total_capacity_bytes, 32768U);
        EXPECT_EQ(snapshot->record.placement.used_capacity_bytes, 12288U);
        EXPECT_EQ(snapshot->record.placement.available_capacity_bytes, 20480U);
        EXPECT_EQ(snapshot->record.placement.load.active_reads, 8U);
        EXPECT_EQ(snapshot->record.placement.load.active_writes, 3U);
        EXPECT_EQ(snapshot->record.placement.load.queued_ops, 11U);
        EXPECT_EQ(snapshot->record.chunk_count, 77U);
        EXPECT_EQ(snapshot->record.io_error_count, 9U);
        EXPECT_EQ(snapshot->record.last_sequence, 7U);
        EXPECT_EQ(snapshot->record.last_seen_unix_ms, 160U);
    }

    TEST(StorageHeartbeatRegistryTest,
         StaleHeartbeatDoesNotOverrideNewerFactsAndSameSequenceIsIdempotent)
    {
        TestStorageNodeHeartbeatRegistry registry;
        ASSERT_EQ(registry.RegisterNode(MakeRegisterRequest(1, 100)).status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto newer = MakeHeartbeatRequest(1, 8, 180);
        newer.placement.total_capacity_bytes = 65536;
        newer.placement.used_capacity_bytes = 4096;
        newer.placement.available_capacity_bytes = 61440;
        newer.chunk_count = 88;
        ASSERT_EQ(registry.ApplyHeartbeat(newer).status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto stale = MakeHeartbeatRequest(1, 7, 150);
        stale.placement.total_capacity_bytes = 4096;
        stale.placement.used_capacity_bytes = 1024;
        stale.placement.available_capacity_bytes = 3072;
        stale.chunk_count = 11;
        const auto stale_result = registry.ApplyHeartbeat(stale);
        EXPECT_EQ(stale_result.status, storedemo::StorageNodeStatusCode::kAlreadyExists);
        EXPECT_TRUE(stale_result.stale_ignored);

        auto duplicate = newer;
        duplicate.placement.total_capacity_bytes = 8192;
        duplicate.placement.available_capacity_bytes = 4096;
        const auto duplicate_result = registry.ApplyHeartbeat(duplicate);
        EXPECT_EQ(duplicate_result.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(duplicate_result.idempotent);

        const auto snapshot = registry.LookupNode(newer.node_id, 190, 20);
        ASSERT_TRUE(snapshot.has_value());
        EXPECT_EQ(snapshot->record.last_sequence, 8U);
        EXPECT_EQ(snapshot->record.last_seen_unix_ms, 180U);
        EXPECT_EQ(snapshot->record.placement.total_capacity_bytes, 65536U);
        EXPECT_EQ(snapshot->record.chunk_count, 88U);
    }

    TEST(StorageHeartbeatRegistryTest, LivenessTransitionsFromLiveToStaleAfterTimeout)
    {
        TestStorageNodeHeartbeatRegistry registry;
        ASSERT_EQ(registry.RegisterNode(MakeRegisterRequest(1, 100)).status,
                  storedemo::StorageNodeStatusCode::kOk);

        const auto live_snapshot = registry.LookupNode(
            storedemo::test::MakeStorageNodeIdFixture(1), 129, 30);
        ASSERT_TRUE(live_snapshot.has_value());
        EXPECT_EQ(live_snapshot->liveness, TestRegistryNodeLiveness::kLive);

        const auto stale_snapshot = registry.LookupNode(
            storedemo::test::MakeStorageNodeIdFixture(1), 131, 30);
        ASSERT_TRUE(stale_snapshot.has_value());
        EXPECT_EQ(stale_snapshot->liveness, TestRegistryNodeLiveness::kStale);

        auto heartbeat = MakeHeartbeatRequest(1, 1, 150);
        ASSERT_EQ(registry.ApplyHeartbeat(heartbeat).status,
                  storedemo::StorageNodeStatusCode::kOk);

        const auto refreshed_snapshot = registry.LookupNode(
            storedemo::test::MakeStorageNodeIdFixture(1), 170, 30);
        ASSERT_TRUE(refreshed_snapshot.has_value());
        EXPECT_EQ(refreshed_snapshot->liveness, TestRegistryNodeLiveness::kLive);
    }

    TEST(StorageHeartbeatRegistryTest, InvalidInputsAndUnknownNodesReturnExplicitErrors)
    {
        TestStorageNodeHeartbeatRegistry registry;

        auto invalid_node_id = MakeRegisterRequest(1, 100);
        invalid_node_id.node_id = "store node invalid";
        EXPECT_EQ(registry.RegisterNode(invalid_node_id).status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);

        auto invalid_endpoint = MakeRegisterRequest(1, 100);
        invalid_endpoint.endpoint = "invalid-endpoint";
        invalid_endpoint.placement.endpoint = invalid_endpoint.endpoint;
        EXPECT_EQ(registry.RegisterNode(invalid_endpoint).status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);

        auto invalid_capacity = MakeRegisterRequest(1, 100);
        invalid_capacity.placement.total_capacity_bytes = 4096;
        invalid_capacity.placement.used_capacity_bytes = 3072;
        invalid_capacity.placement.available_capacity_bytes = 2048;
        EXPECT_EQ(registry.RegisterNode(invalid_capacity).status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);

        ASSERT_EQ(registry.RegisterNode(MakeRegisterRequest(1, 100)).status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto missing_node = MakeHeartbeatRequest(2, 1, 120);
        EXPECT_EQ(registry.ApplyHeartbeat(missing_node).status,
                  storedemo::StorageNodeStatusCode::kNotFound);

        auto invalid_sequence = MakeHeartbeatRequest(1, 0, 120);
        EXPECT_EQ(registry.ApplyHeartbeat(invalid_sequence).status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);

        auto wrong_endpoint = MakeHeartbeatRequest(1, 1, 120);
        wrong_endpoint.endpoint = "127.0.0.1:7999";
        wrong_endpoint.placement.endpoint = wrong_endpoint.endpoint;
        EXPECT_EQ(registry.ApplyHeartbeat(wrong_endpoint).status,
                  storedemo::StorageNodeStatusCode::kConflict);
    }
} // namespace
