#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "store/node/storage_node_registry.h"
#include "support/store_test_utils.h"

namespace
{
    using storedemo::RegisterStorageNodeRequest;
    using storedemo::ReportCapacityRequest;
    using storedemo::ReportHealthRequest;
    using storedemo::ReportLoadRequest;
    using storedemo::StorageNodeDiskPressure;
    using storedemo::StorageNodeHealth;
    using storedemo::StorageNodeLoadSnapshot;
    using storedemo::StorageNodeRegistry;
    using storedemo::StorageNodeRegistryCapacityFacts;
    using storedemo::StorageNodeRegistryConfig;
    using storedemo::StorageNodeRegistryFacts;
    using storedemo::StorageNodeRegistryHealthFacts;
    using storedemo::StorageNodeRegistryLiveness;
    using storedemo::StorageNodeRegistryLoadFacts;
    using storedemo::StorageNodeStatusCode;
    using storedemo::UpdateStorageNodeHeartbeatRequest;

    StorageNodeRegistryFacts MakeFacts(
        const std::size_t index,
        const std::uint64_t total_capacity_bytes = 8'192,
        const std::uint64_t used_capacity_bytes = 2'048,
        const StorageNodeHealth health = StorageNodeHealth::kHealthy,
        const StorageNodeDiskPressure disk_pressure = StorageNodeDiskPressure::kLow)
    {
        StorageNodeRegistryFacts facts;
        facts.capacity.total_capacity_bytes = total_capacity_bytes;
        facts.capacity.used_capacity_bytes = used_capacity_bytes;
        facts.capacity.available_capacity_bytes =
            total_capacity_bytes >= used_capacity_bytes
                ? total_capacity_bytes - used_capacity_bytes
                : 0;
        facts.capacity.chunk_count = 10 + index;
        facts.health.health = health;
        facts.health.disk_pressure = disk_pressure;
        facts.health.io_error_count = index;
        facts.load.load.active_reads = static_cast<std::uint32_t>(index);
        facts.load.load.active_writes = static_cast<std::uint32_t>(index + 1);
        facts.load.load.queued_ops = static_cast<std::uint32_t>(index + 2);
        facts.load.write_admission_overloaded = false;
        facts.load.read_admission_overloaded = false;
        facts.failure_domain.zone = "zone-" + std::to_string(index % 2);
        facts.failure_domain.rack = "rack-" + std::to_string(index);
        return facts;
    }

    RegisterStorageNodeRequest MakeRegisterRequest(const std::size_t index,
                                                   const std::uint64_t observed_at_unix_ms)
    {
        RegisterStorageNodeRequest request;
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.endpoint = "127.0.0.1:" + std::to_string(7000 + index);
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.facts = MakeFacts(index);
        return request;
    }

    UpdateStorageNodeHeartbeatRequest MakeHeartbeatRequest(
        const std::size_t index,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        UpdateStorageNodeHeartbeatRequest request;
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.endpoint = "127.0.0.1:" + std::to_string(7000 + index);
        request.sequence = sequence;
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.facts = MakeFacts(index);
        return request;
    }

    ReportHealthRequest MakeHealthReportRequest(const std::size_t index,
                                                const std::uint64_t sequence,
                                                const std::uint64_t observed_at_unix_ms)
    {
        ReportHealthRequest request;
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.endpoint = "127.0.0.1:" + std::to_string(7000 + index);
        request.sequence = sequence;
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.health = MakeFacts(index).health;
        return request;
    }

    ReportCapacityRequest MakeCapacityReportRequest(
        const std::size_t index,
        const std::uint64_t sequence,
        const std::uint64_t observed_at_unix_ms)
    {
        ReportCapacityRequest request;
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.endpoint = "127.0.0.1:" + std::to_string(7000 + index);
        request.sequence = sequence;
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.capacity = MakeFacts(index).capacity;
        return request;
    }

    ReportLoadRequest MakeLoadReportRequest(const std::size_t index,
                                            const std::uint64_t sequence,
                                            const std::uint64_t observed_at_unix_ms)
    {
        ReportLoadRequest request;
        request.node_id = storedemo::test::MakeStorageNodeIdFixture(index);
        request.endpoint = "127.0.0.1:" + std::to_string(7000 + index);
        request.sequence = sequence;
        request.observed_at_unix_ms = observed_at_unix_ms;
        request.load = MakeFacts(index).load;
        return request;
    }

    void RegisterNodeOrAssert(StorageNodeRegistry *registry,
                              const RegisterStorageNodeRequest &request)
    {
        const auto result = registry->RegisterStorageNode(request);
        ASSERT_EQ(result.status, StorageNodeStatusCode::kOk);
        ASSERT_TRUE(result.created);
    }

    TEST(StorageHeartbeatRegistryTest, RegisterStoresNodeFactsAndStableSortedViews)
    {
        StorageNodeRegistry registry;
        const auto node_b = MakeRegisterRequest(2, 100);
        const auto node_a = MakeRegisterRequest(1, 105);

        RegisterNodeOrAssert(&registry, node_b);
        RegisterNodeOrAssert(&registry, node_a);

        ASSERT_EQ(registry.size(), 2U);

        const auto list = registry.ListNodes(120);
        ASSERT_EQ(list.status, StorageNodeStatusCode::kOk);
        ASSERT_EQ(list.nodes.size(), 2U);
        EXPECT_EQ(list.nodes[0].node_id, node_a.node_id);
        EXPECT_EQ(list.nodes[1].node_id, node_b.node_id);
        EXPECT_EQ(list.nodes[0].facts.capacity.total_capacity_bytes,
                  node_a.facts.capacity.total_capacity_bytes);
        EXPECT_EQ(list.nodes[1].facts.capacity.chunk_count,
                  node_b.facts.capacity.chunk_count);
        EXPECT_EQ(list.nodes[0].liveness, StorageNodeRegistryLiveness::kLive);

        const auto snapshot = registry.Snapshot(121);
        ASSERT_EQ(snapshot.status, StorageNodeStatusCode::kOk);
        ASSERT_EQ(snapshot.generated_at_unix_ms, 121U);
        ASSERT_EQ(snapshot.nodes.size(), 2U);
        EXPECT_EQ(snapshot.nodes[0].node_id, node_a.node_id);
        EXPECT_EQ(snapshot.nodes[1].node_id, node_b.node_id);

        const auto lookup = registry.LookupNode(node_b.node_id, 120);
        ASSERT_EQ(lookup.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup.snapshot.endpoint, node_b.endpoint);
        EXPECT_EQ(lookup.snapshot.facts.health.io_error_count,
                  node_b.facts.health.io_error_count);
        EXPECT_EQ(lookup.snapshot.last_seen_unix_ms, node_b.observed_at_unix_ms);
        EXPECT_EQ(lookup.snapshot.last_sequence, 0U);
    }

    TEST(StorageHeartbeatRegistryTest,
         DuplicateRegisterIsIdempotentAndConflictsOnIdentityOrEndpointMismatch)
    {
        StorageNodeRegistry registry;
        const auto original = MakeRegisterRequest(1, 100);

        auto duplicate = original;
        duplicate.observed_at_unix_ms = 150;
        duplicate.facts.capacity.total_capacity_bytes = 16'384;
        duplicate.facts.capacity.available_capacity_bytes = 14'336;

        const auto first = registry.RegisterStorageNode(original);
        ASSERT_EQ(first.status, StorageNodeStatusCode::kOk);
        EXPECT_TRUE(first.created);

        const auto second = registry.RegisterStorageNode(duplicate);
        ASSERT_EQ(second.status, StorageNodeStatusCode::kOk);
        EXPECT_TRUE(second.idempotent);
        EXPECT_FALSE(second.created);
        EXPECT_EQ(second.snapshot.last_seen_unix_ms, original.observed_at_unix_ms);
        EXPECT_EQ(second.snapshot.facts.capacity.total_capacity_bytes,
                  original.facts.capacity.total_capacity_bytes);

        auto changed_endpoint = original;
        changed_endpoint.endpoint = "127.0.0.1:7999";
        const auto node_conflict = registry.RegisterStorageNode(changed_endpoint);
        EXPECT_EQ(node_conflict.status, StorageNodeStatusCode::kConflict);

        auto endpoint_conflict = MakeRegisterRequest(2, 160);
        endpoint_conflict.endpoint = original.endpoint;
        const auto endpoint_result = registry.RegisterStorageNode(endpoint_conflict);
        EXPECT_EQ(endpoint_result.status, StorageNodeStatusCode::kConflict);

        EXPECT_EQ(registry.size(), 1U);
    }

    TEST(StorageHeartbeatRegistryTest,
         HeartbeatAppliesFullFactsAndProtectsAgainstStaleOrDuplicateUpdates)
    {
        StorageNodeRegistry registry;
        RegisterNodeOrAssert(&registry, MakeRegisterRequest(1, 100));

        auto heartbeat = MakeHeartbeatRequest(1, 7, 160);
        heartbeat.facts = MakeFacts(1,
                                    32'768,
                                    12'288,
                                    StorageNodeHealth::kDegraded,
                                    StorageNodeDiskPressure::kMedium);
        heartbeat.facts.health.io_error_count = 9;
        heartbeat.facts.load.load.active_reads = 8;
        heartbeat.facts.load.load.active_writes = 3;
        heartbeat.facts.load.load.queued_ops = 11;
        heartbeat.facts.load.write_admission_overloaded = true;
        heartbeat.facts.capacity.chunk_count = 77;

        const auto applied = registry.UpdateStorageNodeHeartbeat(heartbeat);
        ASSERT_EQ(applied.status, StorageNodeStatusCode::kOk);
        EXPECT_TRUE(applied.applied);
        EXPECT_EQ(applied.accepted_sequence, 7U);

        auto stale_sequence = MakeHeartbeatRequest(1, 6, 170);
        stale_sequence.facts = MakeFacts(1, 4'096, 1'024);
        stale_sequence.facts.capacity.chunk_count = 11;
        const auto stale_sequence_result =
            registry.UpdateStorageNodeHeartbeat(stale_sequence);
        EXPECT_EQ(stale_sequence_result.status,
                  StorageNodeStatusCode::kAlreadyExists);
        EXPECT_TRUE(stale_sequence_result.stale_ignored);
        EXPECT_EQ(stale_sequence_result.accepted_sequence, 7U);

        auto stale_observed_at = MakeHeartbeatRequest(1, 8, 150);
        stale_observed_at.facts = MakeFacts(1, 65'536, 4'096);
        const auto stale_observed_result =
            registry.UpdateStorageNodeHeartbeat(stale_observed_at);
        EXPECT_EQ(stale_observed_result.status,
                  StorageNodeStatusCode::kAlreadyExists);
        EXPECT_TRUE(stale_observed_result.stale_ignored);
        EXPECT_EQ(stale_observed_result.accepted_sequence, 7U);

        auto duplicate = heartbeat;
        duplicate.observed_at_unix_ms = 180;
        duplicate.facts = MakeFacts(1, 8'192, 4'096);
        const auto duplicate_result = registry.UpdateStorageNodeHeartbeat(duplicate);
        EXPECT_EQ(duplicate_result.status, StorageNodeStatusCode::kOk);
        EXPECT_TRUE(duplicate_result.idempotent);
        EXPECT_FALSE(duplicate_result.applied);

        const auto lookup = registry.LookupNode(heartbeat.node_id, 181);
        ASSERT_EQ(lookup.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup.snapshot.last_sequence, 7U);
        EXPECT_EQ(lookup.snapshot.last_seen_unix_ms, 160U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.total_capacity_bytes, 32'768U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.chunk_count, 77U);
        EXPECT_EQ(lookup.snapshot.facts.health.health,
                  StorageNodeHealth::kDegraded);
        EXPECT_EQ(lookup.snapshot.facts.health.disk_pressure,
                  StorageNodeDiskPressure::kMedium);
        EXPECT_EQ(lookup.snapshot.facts.load.write_admission_overloaded, true);
    }

    TEST(StorageHeartbeatRegistryTest, PartialReportsMergeWithoutClearingOtherFacts)
    {
        StorageNodeRegistry registry;
        const auto original = MakeRegisterRequest(1, 100);
        RegisterNodeOrAssert(&registry, original);

        auto health_report = MakeHealthReportRequest(1, 2, 120);
        health_report.health.health = StorageNodeHealth::kDegraded;
        health_report.health.disk_pressure = StorageNodeDiskPressure::kHigh;
        health_report.health.io_error_count = 6;
        const auto health_result = registry.ReportHealth(health_report);
        ASSERT_EQ(health_result.status, StorageNodeStatusCode::kOk);
        EXPECT_TRUE(health_result.applied);

        auto capacity_report = MakeCapacityReportRequest(1, 3, 130);
        capacity_report.capacity.total_capacity_bytes = 16'384;
        capacity_report.capacity.used_capacity_bytes = 4'096;
        capacity_report.capacity.available_capacity_bytes = 12'288;
        capacity_report.capacity.chunk_count = 99;
        const auto capacity_result = registry.ReportCapacity(capacity_report);
        ASSERT_EQ(capacity_result.status, StorageNodeStatusCode::kOk);
        EXPECT_TRUE(capacity_result.applied);

        auto load_report = MakeLoadReportRequest(1, 4, 140);
        load_report.load.load.active_reads = 8;
        load_report.load.load.active_writes = 3;
        load_report.load.load.queued_ops = 11;
        load_report.load.write_admission_overloaded = true;
        load_report.load.read_admission_overloaded = true;
        const auto load_result = registry.ReportLoad(load_report);
        ASSERT_EQ(load_result.status, StorageNodeStatusCode::kOk);
        EXPECT_TRUE(load_result.applied);

        const auto lookup = registry.LookupNode(original.node_id, 150);
        ASSERT_EQ(lookup.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup.snapshot.last_sequence, 4U);
        EXPECT_EQ(lookup.snapshot.last_seen_unix_ms, 140U);
        EXPECT_EQ(lookup.snapshot.facts.health.health,
                  StorageNodeHealth::kDegraded);
        EXPECT_EQ(lookup.snapshot.facts.health.disk_pressure,
                  StorageNodeDiskPressure::kHigh);
        EXPECT_EQ(lookup.snapshot.facts.health.io_error_count, 6U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.total_capacity_bytes, 16'384U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.used_capacity_bytes, 4'096U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.available_capacity_bytes, 12'288U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.chunk_count, 99U);
        EXPECT_EQ(lookup.snapshot.facts.load.load.active_reads, 8U);
        EXPECT_EQ(lookup.snapshot.facts.load.load.active_writes, 3U);
        EXPECT_EQ(lookup.snapshot.facts.load.load.queued_ops, 11U);
        EXPECT_TRUE(lookup.snapshot.facts.load.write_admission_overloaded);
        EXPECT_TRUE(lookup.snapshot.facts.load.read_admission_overloaded);
        EXPECT_EQ(lookup.snapshot.facts.failure_domain.zone,
                  original.facts.failure_domain.zone);
        EXPECT_EQ(lookup.snapshot.facts.failure_domain.rack,
                  original.facts.failure_domain.rack);
    }

    TEST(StorageHeartbeatRegistryTest, LivenessTransitionsAcrossLiveStaleAndDead)
    {
        StorageNodeRegistry registry(StorageNodeRegistryConfig{
            .stale_timeout_ms = 30,
            .dead_timeout_ms = 90,
            .enforce_unique_endpoints = true});
        const auto request = MakeRegisterRequest(1, 100);
        RegisterNodeOrAssert(&registry, request);

        const auto live = registry.LookupNode(request.node_id, 129);
        ASSERT_EQ(live.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(live.snapshot.liveness, StorageNodeRegistryLiveness::kLive);

        const auto stale = registry.LookupNode(request.node_id, 150);
        ASSERT_EQ(stale.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(stale.snapshot.liveness, StorageNodeRegistryLiveness::kStale);

        const auto dead = registry.LookupNode(request.node_id, 191);
        ASSERT_EQ(dead.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(dead.snapshot.liveness, StorageNodeRegistryLiveness::kDead);
    }

    TEST(StorageHeartbeatRegistryTest, InvalidInputAndUnknownNodePathsReturnExplicitErrors)
    {
        StorageNodeRegistry registry;

        auto invalid_node = MakeRegisterRequest(1, 100);
        invalid_node.node_id.clear();
        EXPECT_EQ(registry.RegisterStorageNode(invalid_node).status,
                  StorageNodeStatusCode::kInvalidArgument);

        auto invalid_endpoint = MakeRegisterRequest(1, 100);
        invalid_endpoint.endpoint = "127.0.0.1";
        EXPECT_EQ(registry.RegisterStorageNode(invalid_endpoint).status,
                  StorageNodeStatusCode::kInvalidArgument);

        auto invalid_observed = MakeRegisterRequest(1, 0);
        EXPECT_EQ(registry.RegisterStorageNode(invalid_observed).status,
                  StorageNodeStatusCode::kInvalidArgument);

        auto invalid_capacity = MakeRegisterRequest(1, 100);
        invalid_capacity.facts.capacity.total_capacity_bytes = 0;
        invalid_capacity.facts.capacity.available_capacity_bytes = 0;
        EXPECT_EQ(registry.RegisterStorageNode(invalid_capacity).status,
                  StorageNodeStatusCode::kInvalidArgument);

        RegisterNodeOrAssert(&registry, MakeRegisterRequest(1, 100));

        auto unknown_heartbeat = MakeHeartbeatRequest(9, 1, 120);
        EXPECT_EQ(registry.UpdateStorageNodeHeartbeat(unknown_heartbeat).status,
                  StorageNodeStatusCode::kNotFound);

        auto invalid_health_report = MakeHealthReportRequest(1, 0, 125);
        EXPECT_EQ(registry.ReportHealth(invalid_health_report).status,
                  StorageNodeStatusCode::kInvalidArgument);

        auto invalid_capacity_report = MakeCapacityReportRequest(1, 2, 126);
        invalid_capacity_report.capacity.total_capacity_bytes = 4'096;
        invalid_capacity_report.capacity.used_capacity_bytes = 3'000;
        invalid_capacity_report.capacity.available_capacity_bytes = 2'000;
        EXPECT_EQ(registry.ReportCapacity(invalid_capacity_report).status,
                  StorageNodeStatusCode::kInvalidArgument);

        auto load_endpoint_conflict = MakeLoadReportRequest(1, 2, 127);
        load_endpoint_conflict.endpoint = "127.0.0.1:7999";
        EXPECT_EQ(registry.ReportLoad(load_endpoint_conflict).status,
                  StorageNodeStatusCode::kConflict);

        EXPECT_EQ(registry.LookupNode("bad node id", 130).status,
                  StorageNodeStatusCode::kInvalidArgument);
    }
}
