#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"
#include "store/node/storage_node_registry.h"
#include "store/placement/replica_policy.h"
#include "support/store_test_utils.h"

namespace
{
    struct ScrubManifest
    {
        storedemo::ChunkIdentity identity;
        std::uint64_t expected_size{0};
        storedemo::ChunkChecksum expected_checksum;
        std::vector<storedemo::StorageNodeId> replica_nodes;
        std::size_t desired_replica_count{0};
    };

    struct ScrubReplicaFact
    {
        storedemo::StorageNodeId node_id;
        storedemo::StorageNodeStatusCode status{
            storedemo::StorageNodeStatusCode::kOk};
        storedemo::ChunkState local_state{storedemo::ChunkState::kMissing};
        bool checksum_verified{false};
        bool known_corrupted{false};
        bool known_missing{false};
    };

    struct ScrubRepairCandidate
    {
        storedemo::ChunkId chunk_id;
        std::uint64_t expected_size{0};
        storedemo::ChunkChecksum expected_checksum;
        std::vector<storedemo::StorageNodeId> bad_replicas;
        std::vector<storedemo::StorageNodeId> healthy_source_replicas;
        bool under_replicated{false};
        bool lost_or_unrecoverable{false};
    };

    struct ScrubRunResult
    {
        storedemo::StorageNodeStatusCode status{
            storedemo::StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::vector<ScrubReplicaFact> replica_facts;
        std::optional<ScrubRepairCandidate> repair_candidate;
    };

    struct ScrubObserver
    {
        std::function<void()> metadata_mutation_hook;
        std::function<void()> raft_call_hook;
        std::function<void(std::string_view)> payload_persist_hook;
    };

    storedemo::ChunkChecksum ComputeChecksumOrThrow(const std::string_view payload)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute checksum: " + error_detail);
        }
        return checksum;
    }

    storedemo::ChunkIdentity MakeIdentityOrThrow(const std::string_view object_id,
                                                 const std::uint64_t version,
                                                 const std::uint32_t chunk_index,
                                                 const std::uint64_t offset = 0)
    {
        storedemo::ChunkId chunk_id;
        std::string error_detail;
        const auto status = storedemo::MakeChunkId(object_id,
                                                   version,
                                                   chunk_index,
                                                   &chunk_id,
                                                   &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to build chunk id: " + error_detail);
        }

        storedemo::ChunkIdentity identity;
        identity.chunk_id = std::move(chunk_id);
        identity.object_id = std::string(object_id);
        identity.version = version;
        identity.chunk_index = chunk_index;
        identity.offset = offset;
        return identity;
    }

    storedemo::WriteChunkRequest MakeWriteRequest(const storedemo::ChunkIdentity &identity,
                                                  const std::string &payload,
                                                  const std::string &request_id)
    {
        return storedemo::WriteChunkRequest{
            .request_id = request_id,
            .identity = identity,
            .expected_size = static_cast<std::uint64_t>(payload.size()),
            .expected_checksum = ComputeChecksumOrThrow(payload),
            .payload = payload};
    }

    storedemo::StatChunkRequest MakeStatRequest(const storedemo::ChunkId &chunk_id,
                                                const std::string &request_id)
    {
        return storedemo::StatChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id};
    }

    storedemo::ReadChunkRequest MakeReadRequest(const storedemo::ChunkId &chunk_id,
                                                const std::string &request_id)
    {
        return storedemo::ReadChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id};
    }

    storedemo::StorageNodeRegistryFacts MakeRegistryFacts(
        const storedemo::StorageNodeHealth health =
            storedemo::StorageNodeHealth::kHealthy,
        const storedemo::StorageNodeDiskPressure disk_pressure =
            storedemo::StorageNodeDiskPressure::kLow,
        const std::uint32_t active_reads = 0)
    {
        storedemo::StorageNodeRegistryFacts facts;
        facts.capacity.total_capacity_bytes = 64 * 1024;
        facts.capacity.used_capacity_bytes = 8 * 1024;
        facts.capacity.available_capacity_bytes = 56 * 1024;
        facts.capacity.chunk_count = 1;
        facts.health.health = health;
        facts.health.disk_pressure = disk_pressure;
        facts.health.io_error_count = 0;
        facts.load.load.active_reads = active_reads;
        facts.load.load.active_writes = active_reads / 2;
        facts.load.load.queued_ops = active_reads / 3;
        facts.load.write_admission_overloaded = false;
        facts.load.read_admission_overloaded = false;
        return facts;
    }

    std::filesystem::path ResolveFinalPathOrThrow(const std::filesystem::path &data_root,
                                                  const storedemo::ChunkId &chunk_id)
    {
        storedemo::ChunkPathLayout layout;
        std::string error_detail;
        const auto layout_status =
            storedemo::BuildChunkPathLayout(chunk_id, "probe", &layout, &error_detail);
        if (layout_status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to build final path layout: " +
                                     error_detail);
        }

        std::filesystem::path final_path;
        const auto resolve_status =
            storedemo::ResolveDurablePathUnderRoot(data_root,
                                                   layout.final_relative_path,
                                                   &final_path,
                                                   &error_detail);
        if (resolve_status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to resolve final path: " + error_detail);
        }

        return final_path;
    }

    void WriteBinaryFileOrThrow(const std::filesystem::path &path,
                                const std::string_view payload)
    {
        std::error_code create_error;
        std::filesystem::create_directories(path.parent_path(), create_error);
        if (create_error)
        {
            throw std::runtime_error("failed to create parent directories for " +
                                     path.string() + ": " +
                                     create_error.message());
        }

        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open())
        {
            throw std::runtime_error("failed to open file for write: " +
                                     path.string());
        }

        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.close();
        if (!output)
        {
            throw std::runtime_error("failed to write payload to " + path.string());
        }
    }

    bool ChecksumEquals(const storedemo::ChunkChecksum &lhs,
                        const storedemo::ChunkChecksum &rhs)
    {
        return lhs.algorithm == rhs.algorithm && lhs.value == rhs.value &&
               lhs.size_bytes == rhs.size_bytes;
    }

    std::string CandidateSummary(const ScrubRepairCandidate &candidate)
    {
        std::string summary = candidate.chunk_id + "|" +
                              std::to_string(candidate.expected_size) + "|" +
                              candidate.expected_checksum.value + "|";
        for (const auto &node_id : candidate.bad_replicas)
        {
            summary += "bad:" + node_id + ";";
        }
        for (const auto &node_id : candidate.healthy_source_replicas)
        {
            summary += "src:" + node_id + ";";
        }
        summary += candidate.under_replicated ? "|under" : "|full";
        summary += candidate.lost_or_unrecoverable ? "|lost" : "|repairable";
        return summary;
    }

    std::string FactSummary(const std::vector<ScrubReplicaFact> &facts)
    {
        std::string summary;
        for (const auto &fact : facts)
        {
            summary += fact.node_id + ":" +
                       std::to_string(static_cast<int>(fact.status)) + ":" +
                       std::to_string(static_cast<int>(fact.local_state)) + ":" +
                       (fact.checksum_verified ? "v" : "n") + ":" +
                       (fact.known_corrupted ? "c" : "h") + ":" +
                       (fact.known_missing ? "m" : "p") + ";";
        }
        return summary;
    }

    void ExpectManifestEq(const ScrubManifest &actual, const ScrubManifest &expected)
    {
        EXPECT_EQ(actual.identity.chunk_id, expected.identity.chunk_id);
        EXPECT_EQ(actual.identity.object_id, expected.identity.object_id);
        EXPECT_EQ(actual.identity.version, expected.identity.version);
        EXPECT_EQ(actual.identity.chunk_index, expected.identity.chunk_index);
        EXPECT_EQ(actual.identity.offset, expected.identity.offset);
        EXPECT_EQ(actual.expected_size, expected.expected_size);
        EXPECT_TRUE(ChecksumEquals(actual.expected_checksum, expected.expected_checksum));
        EXPECT_EQ(actual.replica_nodes, expected.replica_nodes);
        EXPECT_EQ(actual.desired_replica_count, expected.desired_replica_count);
    }

    const ScrubReplicaFact *FindFact(const ScrubRunResult &result,
                                     const storedemo::StorageNodeId &node_id)
    {
        for (const auto &fact : result.replica_facts)
        {
            if (fact.node_id == node_id)
            {
                return &fact;
            }
        }
        return nullptr;
    }

    class TestOnlyScrubRunner
    {
    public:
        TestOnlyScrubRunner(
            std::map<storedemo::StorageNodeId, storedemo::LocalDiskChunkStore *> stores,
            const storedemo::StorageNodeRegistry *registry,
            ScrubObserver observer = {})
            : stores_(std::move(stores))
            , registry_(registry)
            , observer_(std::move(observer))
        {
        }

        ScrubRunResult Run(const ScrubManifest &manifest,
                           const std::uint64_t now_unix_ms) const
        {
            ScrubRunResult result;
            if (registry_ == nullptr)
            {
                result.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
                result.error_detail = "registry must not be null";
                return result;
            }

            if (manifest.replica_nodes.empty())
            {
                result.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
                result.error_detail = "scrub manifest replica_nodes must not be empty";
                return result;
            }

            const auto snapshot = registry_->Snapshot(now_unix_ms);
            if (!snapshot.ok())
            {
                result.status = snapshot.status;
                result.error_detail = snapshot.error_detail;
                return result;
            }

            std::map<storedemo::StorageNodeId, storedemo::StorageNodeRegistryNodeSnapshot>
                snapshot_by_node;
            for (const auto &node : snapshot.nodes)
            {
                snapshot_by_node.emplace(node.node_id, node);
            }

            std::map<storedemo::StorageNodeId, ScrubReplicaFact> fact_by_node;
            std::vector<storedemo::ReadReplicaCandidate> supplemental_candidates;
            supplemental_candidates.reserve(manifest.replica_nodes.size());

            for (std::size_t index = 0; index < manifest.replica_nodes.size(); ++index)
            {
                const auto &node_id = manifest.replica_nodes[index];
                ScrubReplicaFact fact;
                fact.node_id = node_id;

                const auto store_it = stores_.find(node_id);
                if (store_it == stores_.end() || store_it->second == nullptr)
                {
                    fact.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                    fact.local_state = storedemo::ChunkState::kMissing;
                    fact.known_missing = true;
                    fact_by_node.emplace(node_id, fact);
                    result.replica_facts.push_back(fact);
                    supplemental_candidates.push_back(storedemo::ReadReplicaCandidate{
                        .node_id = node_id,
                        .known_missing = true,
                        .has_observed_facts = true});
                    continue;
                }

                auto initial_stat =
                    store_it->second->StatChunk(MakeStatRequest(
                        manifest.identity.chunk_id,
                        "scrub-stat-" + std::to_string(index)));
                if (initial_stat.status == storedemo::StorageNodeStatusCode::kNotFound)
                {
                    fact.status = initial_stat.status;
                    fact.local_state = storedemo::ChunkState::kMissing;
                    fact.known_missing = true;
                }
                else if (!initial_stat.ok())
                {
                    fact.status = initial_stat.status;
                    fact.local_state = storedemo::ChunkState::kMissing;
                    fact.known_corrupted =
                        initial_stat.status == storedemo::StorageNodeStatusCode::kCorrupted;
                }
                else if (initial_stat.metadata.state != storedemo::ChunkState::kLive)
                {
                    fact.local_state = initial_stat.metadata.state;
                    fact.status =
                        initial_stat.metadata.state == storedemo::ChunkState::kQuarantined
                            ? storedemo::StorageNodeStatusCode::kCorrupted
                            : storedemo::StorageNodeStatusCode::kConflict;
                    fact.known_corrupted =
                        initial_stat.metadata.state == storedemo::ChunkState::kQuarantined ||
                        initial_stat.metadata.state == storedemo::ChunkState::kCorrupted;
                    fact.known_missing =
                        initial_stat.metadata.state == storedemo::ChunkState::kMissing ||
                        initial_stat.metadata.state == storedemo::ChunkState::kDeleted;
                }
                else
                {
                    auto verify_request =
                        MakeStatRequest(manifest.identity.chunk_id,
                                        "scrub-verify-" + std::to_string(index));
                    verify_request.verify_checksum = true;
                    const auto verify_result = store_it->second->StatChunk(verify_request);
                    if (verify_result.ok())
                    {
                        fact.local_state = verify_result.metadata.state;
                        fact.status = storedemo::StorageNodeStatusCode::kOk;
                        fact.checksum_verified =
                            verify_result.metadata.size == manifest.expected_size &&
                            ChecksumEquals(verify_result.metadata.checksum,
                                           manifest.expected_checksum);
                        if (!fact.checksum_verified)
                        {
                            fact.status =
                                storedemo::StorageNodeStatusCode::kChecksumMismatch;
                            fact.known_corrupted = true;
                        }
                    }
                    else
                    {
                        fact.status = verify_result.status;
                        const auto post_verify = store_it->second->StatChunk(
                            MakeStatRequest(manifest.identity.chunk_id,
                                            "scrub-post-verify-" +
                                                std::to_string(index)));
                        if (post_verify.ok())
                        {
                            fact.local_state = post_verify.metadata.state;
                        }
                        fact.known_corrupted =
                            verify_result.status ==
                                storedemo::StorageNodeStatusCode::kCorrupted ||
                            fact.local_state == storedemo::ChunkState::kQuarantined;
                        fact.known_missing =
                            verify_result.status ==
                            storedemo::StorageNodeStatusCode::kNotFound;
                    }
                }

                if (fact.local_state == storedemo::ChunkState::kMissing)
                {
                    fact.known_missing = true;
                }

                fact_by_node.emplace(node_id, fact);
                result.replica_facts.push_back(fact);

                if (fact.known_corrupted || fact.known_missing)
                {
                    supplemental_candidates.push_back(storedemo::ReadReplicaCandidate{
                        .node_id = node_id,
                        .known_corrupted = fact.known_corrupted,
                        .known_missing = fact.known_missing,
                        .has_observed_facts = true});
                }
            }

            storedemo::ReplicaPolicySelector selector;
            const auto selection = selector.SelectReadReplicas(
                storedemo::ReadReplicaSelectionRequest{
                    .chunk_id = manifest.identity.chunk_id,
                    .replica_nodes = manifest.replica_nodes},
                snapshot,
                supplemental_candidates);
            if (!selection.ok() &&
                selection.status != storedemo::StorageNodeStatusCode::kNodeUnavailable)
            {
                result.status = selection.status;
                result.error_detail = selection.error_detail;
                return result;
            }

            std::vector<storedemo::StorageNodeId> healthy_sources;
            std::vector<storedemo::StorageNodeId> bad_replicas;
            for (const auto &fact : result.replica_facts)
            {
                if (fact.known_corrupted || fact.known_missing)
                {
                    bad_replicas.push_back(fact.node_id);
                }
            }

            for (const auto &candidate : selection.decision.ordered_replicas)
            {
                const auto snapshot_it = snapshot_by_node.find(candidate.node_id);
                if (snapshot_it == snapshot_by_node.end())
                {
                    continue;
                }

                const auto fact_it = fact_by_node.find(candidate.node_id);
                if (fact_it == fact_by_node.end() ||
                    !fact_it->second.checksum_verified ||
                    fact_it->second.known_corrupted ||
                    fact_it->second.known_missing)
                {
                    continue;
                }

                const auto &node_snapshot = snapshot_it->second;
                if (node_snapshot.liveness !=
                    storedemo::StorageNodeRegistryLiveness::kLive)
                {
                    continue;
                }
                if (node_snapshot.facts.health.health !=
                    storedemo::StorageNodeHealth::kHealthy)
                {
                    continue;
                }
                if (node_snapshot.facts.health.disk_pressure ==
                        storedemo::StorageNodeDiskPressure::kHigh ||
                    node_snapshot.facts.health.disk_pressure ==
                        storedemo::StorageNodeDiskPressure::kFull)
                {
                    continue;
                }

                healthy_sources.push_back(candidate.node_id);
            }

            const bool under_replicated =
                healthy_sources.size() < manifest.desired_replica_count;
            if (!bad_replicas.empty() || under_replicated)
            {
                result.repair_candidate = ScrubRepairCandidate{
                    .chunk_id = manifest.identity.chunk_id,
                    .expected_size = manifest.expected_size,
                    .expected_checksum = manifest.expected_checksum,
                    .bad_replicas = std::move(bad_replicas),
                    .healthy_source_replicas = std::move(healthy_sources),
                    .under_replicated = under_replicated,
                    .lost_or_unrecoverable = false};
                result.repair_candidate->lost_or_unrecoverable =
                    result.repair_candidate->healthy_source_replicas.empty();
            }

            return result;
        }

    private:
        std::map<storedemo::StorageNodeId, storedemo::LocalDiskChunkStore *> stores_;
        const storedemo::StorageNodeRegistry *registry_;
        ScrubObserver observer_;
    };

    class StorageScrubRepairTest : public ::testing::Test
    {
    protected:
        StorageScrubRepairTest()
            : registry_(storedemo::StorageNodeRegistryConfig{
                  .stale_timeout_ms = 20,
                  .dead_timeout_ms = 80,
                  .enforce_unique_endpoints = true})
        {
        }

        storedemo::LocalDiskChunkStore &CreateStore(const std::size_t node_index)
        {
            const auto node_id = storedemo::test::MakeStorageNodeIdFixture(node_index);
            auto store = std::make_unique<storedemo::LocalDiskChunkStore>(
                storedemo::LocalDiskChunkStoreConfig{
                    .data_dir = temp_dir_.Path("store-" + std::to_string(node_index)),
                    .node_id = node_id,
                    .chunk_index = std::make_shared<storedemo::ShardedChunkIndex>()});
            const auto init_result = store->Initialize();
            EXPECT_EQ(init_result.status, storedemo::StorageNodeStatusCode::kOk)
                << init_result.error_detail;
            auto *raw_store = store.get();
            stores_.emplace(node_id, std::move(store));
            return *raw_store;
        }

        void RegisterNode(
            const std::size_t node_index,
            const std::uint64_t observed_at_unix_ms,
            const storedemo::StorageNodeHealth health =
                storedemo::StorageNodeHealth::kHealthy,
            const storedemo::StorageNodeDiskPressure disk_pressure =
                storedemo::StorageNodeDiskPressure::kLow,
            const std::uint32_t active_reads = 0)
        {
            storedemo::RegisterStorageNodeRequest request;
            request.node_id = storedemo::test::MakeStorageNodeIdFixture(node_index);
            request.endpoint = "127.0.0.1:" + std::to_string(7100 + node_index);
            request.observed_at_unix_ms = observed_at_unix_ms;
            request.facts = MakeRegistryFacts(health, disk_pressure, active_reads);
            const auto result = registry_.RegisterStorageNode(request);
            ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
                << result.error_detail;
        }

        void WriteReplica(storedemo::LocalDiskChunkStore &store,
                          const storedemo::ChunkIdentity &identity,
                          const std::string &payload,
                          const std::string &request_id)
        {
            const auto response =
                store.WriteChunk(MakeWriteRequest(identity, payload, request_id));
            ASSERT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk)
                << response.error_detail;
        }

        void TamperReplica(storedemo::LocalDiskChunkStore &store,
                           const storedemo::ChunkIdentity &identity,
                           const std::string &replacement_payload)
        {
            WriteBinaryFileOrThrow(
                ResolveFinalPathOrThrow(store.paths().data_root, identity.chunk_id),
                replacement_payload);
        }

        std::map<storedemo::StorageNodeId, storedemo::LocalDiskChunkStore *>
        RawStoreMap()
        {
            std::map<storedemo::StorageNodeId, storedemo::LocalDiskChunkStore *> map;
            for (auto &[node_id, store] : stores_)
            {
                map.emplace(node_id, store.get());
            }
            return map;
        }

        ScrubManifest MakeManifest(const storedemo::ChunkIdentity &identity,
                                   const std::string &payload,
                                   std::vector<storedemo::StorageNodeId> replica_nodes,
                                   const std::size_t desired_replica_count) const
        {
            return ScrubManifest{
                .identity = identity,
                .expected_size = static_cast<std::uint64_t>(payload.size()),
                .expected_checksum = ComputeChecksumOrThrow(payload),
                .replica_nodes = std::move(replica_nodes),
                .desired_replica_count = desired_replica_count};
        }

        storedemo::test::ScopedStoreTestDir temp_dir_{"storage_scrub_repair"};
        storedemo::StorageNodeRegistry registry_;
        std::map<storedemo::StorageNodeId,
                 std::unique_ptr<storedemo::LocalDiskChunkStore>>
            stores_;
    };

    TEST_F(StorageScrubRepairTest,
           HealthyReplicaChecksumValidationDoesNotEmitRepairCandidate)
    {
        auto &store = CreateStore(1);
        RegisterNode(1, 100);

        const auto identity = MakeIdentityOrThrow("scrub-healthy", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(32, "scrub-healthy");
        WriteReplica(store, identity, payload, "scrub-healthy-write");

        const auto manifest = MakeManifest(
            identity, payload, {storedemo::test::MakeStorageNodeIdFixture(1)}, 1);
        TestOnlyScrubRunner runner(RawStoreMap(), &registry_);
        const auto result = runner.Run(manifest, 110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_FALSE(result.repair_candidate.has_value());
        ASSERT_EQ(result.replica_facts.size(), 1U);
        EXPECT_TRUE(result.replica_facts.front().checksum_verified);
        EXPECT_FALSE(result.replica_facts.front().known_corrupted);
        EXPECT_EQ(result.replica_facts.front().local_state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageScrubRepairTest,
           CorruptedReplicaIsQuarantinedAndProducesRepairCandidate)
    {
        auto &healthy_store = CreateStore(1);
        auto &corrupted_store = CreateStore(2);
        RegisterNode(1, 100);
        RegisterNode(2, 100);

        const auto identity = MakeIdentityOrThrow("scrub-corrupted", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "scrub-corrupted");
        WriteReplica(healthy_store, identity, payload, "scrub-corrupted-write-1");
        WriteReplica(corrupted_store, identity, payload, "scrub-corrupted-write-2");
        TamperReplica(corrupted_store,
                      identity,
                      storedemo::test::MakeChunkPayload(payload.size(), "tampered"));

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2);
        TestOnlyScrubRunner runner(RawStoreMap(), &registry_);
        const auto result = runner.Run(manifest, 110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_TRUE(result.repair_candidate.has_value());
        EXPECT_EQ(result.repair_candidate->chunk_id, identity.chunk_id);
        EXPECT_EQ(result.repair_candidate->expected_size,
                  static_cast<std::uint64_t>(payload.size()));
        EXPECT_TRUE(ChecksumEquals(result.repair_candidate->expected_checksum,
                                   ComputeChecksumOrThrow(payload)));
        EXPECT_EQ(result.repair_candidate->bad_replicas,
                  std::vector<storedemo::StorageNodeId>{
                      storedemo::test::MakeStorageNodeIdFixture(2)});
        EXPECT_EQ(result.repair_candidate->healthy_source_replicas,
                  std::vector<storedemo::StorageNodeId>{
                      storedemo::test::MakeStorageNodeIdFixture(1)});
        EXPECT_TRUE(result.repair_candidate->under_replicated);
        EXPECT_FALSE(result.repair_candidate->lost_or_unrecoverable);

        const auto *corrupted_fact = FindFact(
            result, storedemo::test::MakeStorageNodeIdFixture(2));
        ASSERT_NE(corrupted_fact, nullptr);
        EXPECT_TRUE(corrupted_fact->known_corrupted);
        EXPECT_EQ(corrupted_fact->local_state, storedemo::ChunkState::kQuarantined);

        const auto post_scrub_stat = corrupted_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "scrub-corrupted-post-stat"));
        ASSERT_EQ(post_scrub_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(post_scrub_stat.metadata.state, storedemo::ChunkState::kQuarantined);
    }

    TEST_F(StorageScrubRepairTest,
           RepeatedScrubIsIdempotentAndDoesNotMutateManifestOrTouchRaft)
    {
        auto &healthy_store = CreateStore(1);
        auto &corrupted_store = CreateStore(2);
        RegisterNode(1, 100);
        RegisterNode(2, 100);

        const auto identity = MakeIdentityOrThrow("scrub-idempotent", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(40, "scrub-idempotent");
        WriteReplica(healthy_store, identity, payload, "scrub-idempotent-write-1");
        WriteReplica(corrupted_store, identity, payload, "scrub-idempotent-write-2");
        TamperReplica(corrupted_store,
                      identity,
                      storedemo::test::MakeChunkPayload(payload.size(), "tampered-idem"));

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2);
        const auto original_manifest = manifest;

        std::size_t metadata_mutation_calls = 0;
        std::size_t raft_calls = 0;
        std::size_t payload_persist_calls = 0;
        TestOnlyScrubRunner runner(
            RawStoreMap(),
            &registry_,
            ScrubObserver{
                .metadata_mutation_hook = [&metadata_mutation_calls]()
                { ++metadata_mutation_calls; },
                .raft_call_hook = [&raft_calls]()
                { ++raft_calls; },
                .payload_persist_hook = [&payload_persist_calls](std::string_view)
                { ++payload_persist_calls; }});

        const auto first = runner.Run(manifest, 110);
        const auto second = runner.Run(manifest, 120);

        ASSERT_EQ(first.status, storedemo::StorageNodeStatusCode::kOk)
            << first.error_detail;
        ASSERT_EQ(second.status, storedemo::StorageNodeStatusCode::kOk)
            << second.error_detail;
        ASSERT_TRUE(first.repair_candidate.has_value());
        ASSERT_TRUE(second.repair_candidate.has_value());
        EXPECT_EQ(CandidateSummary(*first.repair_candidate),
                  CandidateSummary(*second.repair_candidate));
        EXPECT_EQ(FactSummary(first.replica_facts), FactSummary(second.replica_facts));
        ExpectManifestEq(manifest, original_manifest);
        EXPECT_EQ(metadata_mutation_calls, 0U);
        EXPECT_EQ(raft_calls, 0U);
        EXPECT_EQ(payload_persist_calls, 0U);
    }

    TEST_F(StorageScrubRepairTest,
           AllCorruptedReplicasProduceLostCandidateWithoutHealthySource)
    {
        auto &first_store = CreateStore(1);
        auto &second_store = CreateStore(2);
        RegisterNode(1, 100);
        RegisterNode(2, 100);

        const auto identity = MakeIdentityOrThrow("scrub-lost", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(24, "scrub-lost");
        WriteReplica(first_store, identity, payload, "scrub-lost-write-1");
        WriteReplica(second_store, identity, payload, "scrub-lost-write-2");
        TamperReplica(first_store,
                      identity,
                      storedemo::test::MakeChunkPayload(payload.size(), "lost-a"));
        TamperReplica(second_store,
                      identity,
                      storedemo::test::MakeChunkPayload(payload.size(), "lost-b"));

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2);
        TestOnlyScrubRunner runner(RawStoreMap(), &registry_);
        const auto result = runner.Run(manifest, 110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_TRUE(result.repair_candidate.has_value());
        EXPECT_TRUE(result.repair_candidate->healthy_source_replicas.empty());
        EXPECT_TRUE(result.repair_candidate->lost_or_unrecoverable);
        EXPECT_EQ(result.repair_candidate->bad_replicas.size(), 2U);
    }

    TEST_F(StorageScrubRepairTest,
           UnderReplicatedChunkProducesRepairCandidateWithoutCreatingReplica)
    {
        auto &first_store = CreateStore(1);
        auto &second_store = CreateStore(2);
        auto &missing_target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("scrub-under-replicated", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(36, "scrub-under-replicated");
        WriteReplica(first_store, identity, payload, "scrub-under-write-1");
        WriteReplica(second_store, identity, payload, "scrub-under-write-2");

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2),
             storedemo::test::MakeStorageNodeIdFixture(3)},
            3);
        TestOnlyScrubRunner runner(RawStoreMap(), &registry_);
        const auto result = runner.Run(manifest, 110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_TRUE(result.repair_candidate.has_value());
        EXPECT_TRUE(result.repair_candidate->under_replicated);
        EXPECT_EQ(result.repair_candidate->healthy_source_replicas,
                  std::vector<storedemo::StorageNodeId>(
                      {storedemo::test::MakeStorageNodeIdFixture(1),
                       storedemo::test::MakeStorageNodeIdFixture(2)}));
        EXPECT_EQ(result.repair_candidate->bad_replicas,
                  std::vector<storedemo::StorageNodeId>{
                      storedemo::test::MakeStorageNodeIdFixture(3)});

        const auto missing_after_scrub = missing_target_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "scrub-under-missing-after"));
        EXPECT_EQ(missing_after_scrub.status, storedemo::StorageNodeStatusCode::kNotFound);
    }

    TEST_F(StorageScrubRepairTest,
           StaleUnavailableAndUnhealthyNodesAreNotChosenAsRepairSources)
    {
        auto &healthy_store = CreateStore(1);
        auto &stale_store = CreateStore(2);
        auto &unavailable_store = CreateStore(3);
        auto &degraded_store = CreateStore(4);
        auto &corrupted_store = CreateStore(5);

        RegisterNode(1, 100);
        RegisterNode(2, 60);
        RegisterNode(3, 100, storedemo::StorageNodeHealth::kUnavailable);
        RegisterNode(4, 100, storedemo::StorageNodeHealth::kDegraded);
        RegisterNode(5, 100);

        const auto identity = MakeIdentityOrThrow("scrub-source-filter", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(28, "scrub-source");
        WriteReplica(healthy_store, identity, payload, "scrub-source-write-1");
        WriteReplica(stale_store, identity, payload, "scrub-source-write-2");
        WriteReplica(unavailable_store, identity, payload, "scrub-source-write-3");
        WriteReplica(degraded_store, identity, payload, "scrub-source-write-4");
        WriteReplica(corrupted_store, identity, payload, "scrub-source-write-5");
        TamperReplica(corrupted_store,
                      identity,
                      storedemo::test::MakeChunkPayload(payload.size(), "tampered-src"));

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2),
             storedemo::test::MakeStorageNodeIdFixture(3),
             storedemo::test::MakeStorageNodeIdFixture(4),
             storedemo::test::MakeStorageNodeIdFixture(5)},
            5);
        TestOnlyScrubRunner runner(RawStoreMap(), &registry_);
        const auto result = runner.Run(manifest, 110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        ASSERT_TRUE(result.repair_candidate.has_value());
        EXPECT_EQ(result.repair_candidate->healthy_source_replicas,
                  std::vector<storedemo::StorageNodeId>{
                      storedemo::test::MakeStorageNodeIdFixture(1)});
        EXPECT_EQ(result.repair_candidate->bad_replicas,
                  std::vector<storedemo::StorageNodeId>{
                      storedemo::test::MakeStorageNodeIdFixture(5)});
    }
} // namespace
