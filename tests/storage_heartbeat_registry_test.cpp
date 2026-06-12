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
    using storedemo::StorageNodeRegistryNodeSnapshot;
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

    const StorageNodeRegistryNodeSnapshot *FindNodeSnapshotById(
        const std::vector<StorageNodeRegistryNodeSnapshot> &nodes,
        const std::string &node_id)
    {
        for (const auto &node : nodes)
        {
            if (node.node_id == node_id)
            {
                return &node;
            }
        }
        return nullptr;
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
         RuntimeRegistrationAddsNewNodeToObservedRegistryViews)
    {
        StorageNodeRegistry registry;
        const auto initial = MakeRegisterRequest(1, 100);
        RegisterNodeOrAssert(&registry, initial);

        const auto before_join = registry.ListNodes(120);
        ASSERT_EQ(before_join.status, StorageNodeStatusCode::kOk);
        ASSERT_EQ(before_join.nodes.size(), 1U);
        ASSERT_NE(FindNodeSnapshotById(before_join.nodes, initial.node_id), nullptr);

        auto runtime_join = MakeRegisterRequest(7, 160);
        runtime_join.facts.capacity.total_capacity_bytes = 65'536;
        runtime_join.facts.capacity.used_capacity_bytes = 8'192;
        runtime_join.facts.capacity.available_capacity_bytes = 57'344;
        runtime_join.facts.capacity.chunk_count = 123;
        runtime_join.facts.failure_domain.zone = "zone-runtime";
        runtime_join.facts.failure_domain.rack = "rack-runtime";

        const auto join_result = registry.RegisterStorageNode(runtime_join);
        ASSERT_EQ(join_result.status, StorageNodeStatusCode::kOk);
        EXPECT_TRUE(join_result.created);
        EXPECT_FALSE(join_result.idempotent);
        EXPECT_EQ(join_result.snapshot.node_id, runtime_join.node_id);
        EXPECT_EQ(join_result.snapshot.endpoint, runtime_join.endpoint);
        EXPECT_EQ(join_result.snapshot.last_sequence, 0U);
        EXPECT_EQ(join_result.snapshot.last_seen_unix_ms,
                  runtime_join.observed_at_unix_ms);
        EXPECT_EQ(join_result.snapshot.liveness, StorageNodeRegistryLiveness::kLive);

        EXPECT_EQ(registry.size(), 2U);

        const auto lookup_runtime = registry.LookupNode(runtime_join.node_id, 170);
        ASSERT_EQ(lookup_runtime.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup_runtime.snapshot.endpoint, runtime_join.endpoint);
        EXPECT_EQ(lookup_runtime.snapshot.last_sequence, 0U);
        EXPECT_EQ(lookup_runtime.snapshot.last_seen_unix_ms,
                  runtime_join.observed_at_unix_ms);
        EXPECT_EQ(lookup_runtime.snapshot.liveness, StorageNodeRegistryLiveness::kLive);
        EXPECT_EQ(lookup_runtime.snapshot.facts.capacity.total_capacity_bytes,
                  runtime_join.facts.capacity.total_capacity_bytes);
        EXPECT_EQ(lookup_runtime.snapshot.facts.capacity.chunk_count,
                  runtime_join.facts.capacity.chunk_count);
        EXPECT_EQ(lookup_runtime.snapshot.facts.failure_domain.zone,
                  runtime_join.facts.failure_domain.zone);
        EXPECT_EQ(lookup_runtime.snapshot.facts.failure_domain.rack,
                  runtime_join.facts.failure_domain.rack);

        const auto after_join = registry.ListNodes(170);
        ASSERT_EQ(after_join.status, StorageNodeStatusCode::kOk);
        ASSERT_EQ(after_join.nodes.size(), 2U);
        const auto *initial_snapshot =
            FindNodeSnapshotById(after_join.nodes, initial.node_id);
        const auto *runtime_snapshot =
            FindNodeSnapshotById(after_join.nodes, runtime_join.node_id);
        ASSERT_NE(initial_snapshot, nullptr);
        ASSERT_NE(runtime_snapshot, nullptr);
        EXPECT_EQ(runtime_snapshot->endpoint, runtime_join.endpoint);
        EXPECT_EQ(runtime_snapshot->liveness, StorageNodeRegistryLiveness::kLive);
        EXPECT_EQ(initial_snapshot->endpoint, initial.endpoint);
        EXPECT_EQ(initial_snapshot->last_seen_unix_ms, initial.observed_at_unix_ms);
        EXPECT_EQ(initial_snapshot->last_sequence, 0U);

        const auto snapshot = registry.Snapshot(170);
        ASSERT_EQ(snapshot.status, StorageNodeStatusCode::kOk);
        ASSERT_EQ(snapshot.nodes.size(), 2U);
        const auto *runtime_from_snapshot =
            FindNodeSnapshotById(snapshot.nodes, runtime_join.node_id);
        ASSERT_NE(runtime_from_snapshot, nullptr);
        EXPECT_EQ(runtime_from_snapshot->endpoint, runtime_join.endpoint);
        EXPECT_EQ(runtime_from_snapshot->liveness,
                  StorageNodeRegistryLiveness::kLive);
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
         DuplicateNodeIdOrEndpointConflictDoesNotPolluteTrustedHealthyRecord)
    {
        StorageNodeRegistry registry;
        const auto registration = MakeRegisterRequest(1, 100);
        RegisterNodeOrAssert(&registry, registration);

        auto trusted_heartbeat = MakeHeartbeatRequest(1, 7, 160);
        trusted_heartbeat.facts = MakeFacts(1,
                                            32'768,
                                            12'288,
                                            StorageNodeHealth::kHealthy,
                                            StorageNodeDiskPressure::kLow);
        trusted_heartbeat.facts.health.io_error_count = 3;
        trusted_heartbeat.facts.load.load.active_reads = 8;
        trusted_heartbeat.facts.load.load.active_writes = 3;
        trusted_heartbeat.facts.load.load.queued_ops = 11;
        trusted_heartbeat.facts.capacity.chunk_count = 77;

        const auto trusted_result =
            registry.UpdateStorageNodeHeartbeat(trusted_heartbeat);
        ASSERT_EQ(trusted_result.status, StorageNodeStatusCode::kOk);
        ASSERT_TRUE(trusted_result.applied);
        ASSERT_EQ(trusted_result.accepted_sequence, 7U);

        auto duplicate_node_id = registration;
        duplicate_node_id.endpoint = "127.0.0.1:7999";
        duplicate_node_id.observed_at_unix_ms = 170;
        duplicate_node_id.facts = MakeFacts(1,
                                            4'096,
                                            1'024,
                                            StorageNodeHealth::kUnavailable,
                                            StorageNodeDiskPressure::kFull);
        const auto duplicate_node_id_result =
            registry.RegisterStorageNode(duplicate_node_id);
        EXPECT_EQ(duplicate_node_id_result.status, StorageNodeStatusCode::kConflict);
        EXPECT_EQ(duplicate_node_id_result.error_detail,
                  "node_id is already registered with a different endpoint");

        auto duplicate_endpoint = MakeRegisterRequest(2, 171);
        duplicate_endpoint.endpoint = registration.endpoint;
        duplicate_endpoint.facts = MakeFacts(2,
                                             4'096,
                                             1'024,
                                             StorageNodeHealth::kUnavailable,
                                             StorageNodeDiskPressure::kFull);
        const auto duplicate_endpoint_result =
            registry.RegisterStorageNode(duplicate_endpoint);
        EXPECT_EQ(duplicate_endpoint_result.status, StorageNodeStatusCode::kConflict);
        EXPECT_EQ(duplicate_endpoint_result.error_detail,
                  "endpoint is already registered to a different node_id");

        auto conflicting_heartbeat = MakeHeartbeatRequest(1, 8, 180);
        conflicting_heartbeat.endpoint = "127.0.0.1:7999";
        conflicting_heartbeat.facts = MakeFacts(1,
                                                4'096,
                                                1'024,
                                                StorageNodeHealth::kUnavailable,
                                                StorageNodeDiskPressure::kFull);
        conflicting_heartbeat.facts.health.io_error_count = 99;
        conflicting_heartbeat.facts.load.load.active_reads = 1;
        conflicting_heartbeat.facts.load.load.active_writes = 1;
        conflicting_heartbeat.facts.load.load.queued_ops = 1;
        const auto conflicting_heartbeat_result =
            registry.UpdateStorageNodeHeartbeat(conflicting_heartbeat);
        EXPECT_EQ(conflicting_heartbeat_result.status,
                  StorageNodeStatusCode::kConflict);
        EXPECT_EQ(conflicting_heartbeat_result.error_detail,
                  "node_id heartbeat endpoint does not match registration");
        EXPECT_FALSE(conflicting_heartbeat_result.applied);

        const auto lookup_after_conflicts = registry.LookupNode(registration.node_id,
                                                                181);
        ASSERT_EQ(lookup_after_conflicts.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup_after_conflicts.snapshot.node_id, registration.node_id);
        EXPECT_EQ(lookup_after_conflicts.snapshot.endpoint, registration.endpoint);
        EXPECT_EQ(lookup_after_conflicts.snapshot.last_sequence, 7U);
        EXPECT_EQ(lookup_after_conflicts.snapshot.last_seen_unix_ms, 160U);
        EXPECT_EQ(lookup_after_conflicts.snapshot.facts.capacity.total_capacity_bytes,
                  32'768U);
        EXPECT_EQ(lookup_after_conflicts.snapshot.facts.capacity.chunk_count, 77U);
        EXPECT_EQ(lookup_after_conflicts.snapshot.facts.health.health,
                  StorageNodeHealth::kHealthy);
        EXPECT_EQ(lookup_after_conflicts.snapshot.facts.health.disk_pressure,
                  StorageNodeDiskPressure::kLow);
        EXPECT_EQ(lookup_after_conflicts.snapshot.facts.health.io_error_count, 3U);
        EXPECT_EQ(lookup_after_conflicts.snapshot.facts.load.load.active_reads, 8U);
        EXPECT_EQ(lookup_after_conflicts.snapshot.facts.load.load.active_writes, 3U);
        EXPECT_EQ(lookup_after_conflicts.snapshot.facts.load.load.queued_ops, 11U);
        EXPECT_EQ(registry.size(), 1U);

        auto duplicate_restart = registration;
        duplicate_restart.observed_at_unix_ms = 182;
        duplicate_restart.facts = MakeFacts(1,
                                            65'536,
                                            8'192,
                                            StorageNodeHealth::kDegraded,
                                            StorageNodeDiskPressure::kMedium);
        const auto duplicate_restart_result =
            registry.RegisterStorageNode(duplicate_restart);
        ASSERT_EQ(duplicate_restart_result.status, StorageNodeStatusCode::kOk);
        EXPECT_TRUE(duplicate_restart_result.idempotent);
        EXPECT_FALSE(duplicate_restart_result.created);

        auto valid_higher_sequence = MakeHeartbeatRequest(1, 9, 190);
        valid_higher_sequence.facts = MakeFacts(1,
                                                65'536,
                                                8'192,
                                                StorageNodeHealth::kDegraded,
                                                StorageNodeDiskPressure::kMedium);
        valid_higher_sequence.facts.health.io_error_count = 4;
        valid_higher_sequence.facts.load.load.active_reads = 10;
        valid_higher_sequence.facts.load.load.active_writes = 4;
        valid_higher_sequence.facts.load.load.queued_ops = 12;
        valid_higher_sequence.facts.capacity.chunk_count = 88;
        const auto valid_higher_sequence_result =
            registry.UpdateStorageNodeHeartbeat(valid_higher_sequence);
        ASSERT_EQ(valid_higher_sequence_result.status, StorageNodeStatusCode::kOk);
        EXPECT_TRUE(valid_higher_sequence_result.applied);
        EXPECT_EQ(valid_higher_sequence_result.accepted_sequence, 9U);

        const auto lookup_after_valid_update =
            registry.LookupNode(registration.node_id, 191);
        ASSERT_EQ(lookup_after_valid_update.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup_after_valid_update.snapshot.endpoint, registration.endpoint);
        EXPECT_EQ(lookup_after_valid_update.snapshot.last_sequence, 9U);
        EXPECT_EQ(lookup_after_valid_update.snapshot.last_seen_unix_ms, 190U);
        EXPECT_EQ(
            lookup_after_valid_update.snapshot.facts.capacity.total_capacity_bytes,
            65'536U);
        EXPECT_EQ(lookup_after_valid_update.snapshot.facts.capacity.chunk_count,
                  88U);
        EXPECT_EQ(lookup_after_valid_update.snapshot.facts.health.health,
                  StorageNodeHealth::kDegraded);
        EXPECT_EQ(lookup_after_valid_update.snapshot.facts.health.disk_pressure,
                  StorageNodeDiskPressure::kMedium);
        EXPECT_EQ(lookup_after_valid_update.snapshot.facts.health.io_error_count,
                  4U);
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

    TEST(StorageHeartbeatRegistryTest,
         RestartSameNodeIdShouldAcceptNewIncarnationAndRejectOldProcessState)
    {
        StorageNodeRegistry registry;
        const auto registration = MakeRegisterRequest(1, 100);
        RegisterNodeOrAssert(&registry, registration);

        const std::string old_incarnation =
            "store-node-1:boot:100000000:71:1";
        const std::string new_incarnation =
            "store-node-1:boot:220000000:72:1";

        auto old_process_live = MakeHeartbeatRequest(1, 7, 160);
        old_process_live.incarnation_id = old_incarnation;
        old_process_live.facts = MakeFacts(1,
                                           32'768,
                                           12'288,
                                           StorageNodeHealth::kDegraded,
                                           StorageNodeDiskPressure::kMedium);
        old_process_live.facts.health.io_error_count = 9;
        old_process_live.facts.load.load.active_reads = 8;
        old_process_live.facts.load.load.active_writes = 3;
        old_process_live.facts.load.load.queued_ops = 11;
        const auto old_process_result =
            registry.UpdateStorageNodeHeartbeat(old_process_live);
        ASSERT_EQ(old_process_result.status, StorageNodeStatusCode::kOk);
        ASSERT_TRUE(old_process_result.applied);
        ASSERT_EQ(old_process_result.accepted_sequence, 7U);

        auto restarted_process = MakeHeartbeatRequest(1, 1, 220);
        restarted_process.incarnation_id = new_incarnation;
        restarted_process.facts = MakeFacts(1,
                                            65'536,
                                            4'096,
                                            StorageNodeHealth::kHealthy,
                                            StorageNodeDiskPressure::kLow);
        restarted_process.facts.health.io_error_count = 1;
        restarted_process.facts.load.load.active_reads = 2;
        restarted_process.facts.load.load.active_writes = 1;
        restarted_process.facts.load.load.queued_ops = 0;

        auto restarted_register = registration;
        restarted_register.incarnation_id = new_incarnation;
        restarted_register.observed_at_unix_ms = 220;
        restarted_register.facts = restarted_process.facts;
        const auto restarted_register_result =
            registry.RegisterStorageNode(restarted_register);
        ASSERT_EQ(restarted_register_result.status, StorageNodeStatusCode::kOk);
        EXPECT_FALSE(restarted_register_result.created);
        EXPECT_FALSE(restarted_register_result.idempotent);
        EXPECT_EQ(restarted_register_result.snapshot.incarnation_id,
                  new_incarnation);
        EXPECT_EQ(restarted_register_result.snapshot.last_sequence, 0U);
        EXPECT_EQ(restarted_register_result.snapshot.last_seen_unix_ms, 220U);
        EXPECT_EQ(
            restarted_register_result.snapshot.facts.capacity.total_capacity_bytes,
            65'536U);
        EXPECT_EQ(restarted_register_result.snapshot.facts.health.health,
                  StorageNodeHealth::kHealthy);
        EXPECT_EQ(restarted_register_result.snapshot.facts.health.disk_pressure,
                  StorageNodeDiskPressure::kLow);

        const auto restarted_result =
            registry.UpdateStorageNodeHeartbeat(restarted_process);
        ASSERT_EQ(restarted_result.status, StorageNodeStatusCode::kOk);
        ASSERT_TRUE(restarted_result.applied);
        EXPECT_EQ(restarted_result.accepted_sequence, 1U);
        EXPECT_EQ(restarted_result.snapshot.incarnation_id, new_incarnation);
        EXPECT_EQ(restarted_result.snapshot.last_sequence, 1U);
        EXPECT_EQ(restarted_result.snapshot.last_seen_unix_ms, 220U);
        EXPECT_EQ(restarted_result.snapshot.facts.capacity.total_capacity_bytes,
                  65'536U);
        EXPECT_EQ(restarted_result.snapshot.facts.health.health,
                  StorageNodeHealth::kHealthy);
        EXPECT_EQ(restarted_result.snapshot.facts.health.disk_pressure,
                  StorageNodeDiskPressure::kLow);

        auto old_process_late = MakeHeartbeatRequest(1, 8, 240);
        old_process_late.incarnation_id = old_incarnation;
        old_process_late.facts = MakeFacts(1,
                                           4'096,
                                           1'024,
                                           StorageNodeHealth::kUnavailable,
                                           StorageNodeDiskPressure::kFull);
        old_process_late.facts.health.io_error_count = 99;
        old_process_late.facts.load.load.active_reads = 1;
        old_process_late.facts.load.load.active_writes = 1;
        old_process_late.facts.load.load.queued_ops = 1;

        const auto old_process_late_result =
            registry.UpdateStorageNodeHeartbeat(old_process_late);
        EXPECT_EQ(old_process_late_result.status,
                  StorageNodeStatusCode::kAlreadyExists);
        EXPECT_TRUE(old_process_late_result.stale_ignored);

        const auto lookup = registry.LookupNode(registration.node_id, 250);
        ASSERT_EQ(lookup.status, StorageNodeStatusCode::kOk);
        EXPECT_EQ(lookup.snapshot.node_id, registration.node_id);
        EXPECT_EQ(lookup.snapshot.incarnation_id, new_incarnation);
        EXPECT_EQ(lookup.snapshot.last_sequence, 1U);
        EXPECT_EQ(lookup.snapshot.last_seen_unix_ms, 220U);
        EXPECT_EQ(lookup.snapshot.facts.capacity.total_capacity_bytes, 65'536U);
        EXPECT_EQ(lookup.snapshot.facts.health.health,
                  StorageNodeHealth::kHealthy);
        EXPECT_EQ(lookup.snapshot.facts.health.disk_pressure,
                  StorageNodeDiskPressure::kLow);
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
