#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
#include "store/maintenance/scrub_manager.h"
#include "store/node/storage_node_registry.h"
#include "store/placement/placement_manager.h"
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

    struct RepairReplicaFactsLedger
    {
        bool MarkDurable(const storedemo::ChunkId &chunk_id,
                         const storedemo::StorageNodeId &node_id)
        {
            return durable_replicas[chunk_id].insert(node_id).second;
        }

        [[nodiscard]] bool HasDurableReplica(
            const storedemo::ChunkId &chunk_id,
            const storedemo::StorageNodeId &node_id) const
        {
            const auto chunk_it = durable_replicas.find(chunk_id);
            if (chunk_it == durable_replicas.end())
            {
                return false;
            }
            return chunk_it->second.contains(node_id);
        }

        [[nodiscard]] std::size_t DurableReplicaCount(
            const storedemo::ChunkId &chunk_id) const
        {
            const auto chunk_it = durable_replicas.find(chunk_id);
            return chunk_it == durable_replicas.end() ? 0U : chunk_it->second.size();
        }

        std::map<storedemo::ChunkId, std::set<storedemo::StorageNodeId>> durable_replicas;
    };

    struct RepairRunResult
    {
        storedemo::StorageNodeStatusCode status{
            storedemo::StorageNodeStatusCode::kOk};
        std::string error_detail;
        storedemo::StorageNodeId source_node;
        storedemo::StorageNodeId target_node;
        bool target_durable{false};
        bool facts_updated{false};
        bool idempotent_success{false};
    };

    struct RepairObserver
    {
        std::function<void()> metadata_mutation_hook;
        std::function<void()> raft_call_hook;
        std::function<void(std::string_view)> payload_persist_hook;
    };

    struct RecordingWriterState
    {
        storedemo::DurableFileResult append_result;
        storedemo::DurableFileResult flush_result{
            .durable_boundary_reached = true};
        storedemo::DurableFileResult close_result;
        std::string appended_payload;
    };

    class RecordingDurableFileWriter : public storedemo::DurableFileWriter
    {
    public:
        RecordingDurableFileWriter(std::shared_ptr<RecordingWriterState> state,
                                   std::filesystem::path path)
            : state_(std::move(state))
            , path_(std::move(path))
        {
        }

        storedemo::DurableFileResult Append(
            const storedemo::DurableAppendRequest &request) override
        {
            const auto *chars =
                reinterpret_cast<const char *>(request.buffer.data());
            state_->appended_payload.assign(chars, chars + request.buffer.size());
            auto result = state_->append_result;
            if (result.ok())
            {
                result.bytes_transferred = request.buffer.size();
            }
            return result;
        }

        storedemo::DurableFileResult Flush(
            const storedemo::DurableFlushRequest &) override
        {
            return state_->flush_result;
        }

        storedemo::DurableFileResult Close(
            const storedemo::DurableCloseRequest &) override
        {
            return state_->close_result;
        }

        [[nodiscard]] const std::filesystem::path &path() const override
        {
            return path_;
        }

    private:
        std::shared_ptr<RecordingWriterState> state_;
        std::filesystem::path path_;
    };

    class RecordingDurableFile : public storedemo::DurableFile
    {
    public:
        explicit RecordingDurableFile(std::shared_ptr<RecordingWriterState> writer_state)
            : writer_state_(std::move(writer_state))
        {
            publish_result.durable_boundary_reached = true;
            sync_result.durable_boundary_reached = true;
        }

        storedemo::DurableFileResult publish_result;
        storedemo::DurableFileResult sync_result;

        storedemo::NormalizeDurablePathResponse NormalizePath(
            const storedemo::NormalizeDurablePathRequest &request) override
        {
            storedemo::NormalizeDurablePathResponse response;
            response.normalized_path = request.relative_path;
            return response;
        }

        storedemo::OpenStagingWriterResponse OpenStagingWriter(
            const storedemo::OpenStagingWriterRequest &request) override
        {
            storedemo::OpenStagingWriterResponse response;
            response.normalized_path = request.relative_path;
            response.writer = std::make_unique<RecordingDurableFileWriter>(
                writer_state_, request.relative_path);
            return response;
        }

        storedemo::DurableFileResult PublishStagedFile(
            const storedemo::PublishDurableFileRequest &) override
        {
            return publish_result;
        }

        storedemo::DurableFileResult SyncDirectory(
            const storedemo::SyncDurableDirectoryRequest &) override
        {
            return sync_result;
        }

    private:
        std::shared_ptr<RecordingWriterState> writer_state_;
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

    std::string CandidateSummary(const storedemo::ScrubRepairCandidate &candidate)
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

    ScrubRepairCandidate MakeRepairCandidate(
        const ScrubManifest &manifest,
        std::vector<storedemo::StorageNodeId> healthy_source_replicas,
        const bool under_replicated = true)
    {
        return ScrubRepairCandidate{
            .chunk_id = manifest.identity.chunk_id,
            .expected_size = manifest.expected_size,
            .expected_checksum = manifest.expected_checksum,
            .bad_replicas = {},
            .healthy_source_replicas = std::move(healthy_source_replicas),
            .under_replicated = under_replicated,
            .lost_or_unrecoverable = false};
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

    std::string FactSummary(const std::vector<storedemo::ScrubReplicaFact> &facts)
    {
        std::string summary;
        for (const auto &fact : facts)
        {
            summary += fact.node_id + ":" +
                       std::to_string(static_cast<int>(fact.status)) + ":" +
                       std::to_string(static_cast<int>(fact.state_before)) + ":" +
                       std::to_string(static_cast<int>(fact.state_after)) + ":" +
                       (fact.checksum_verified ? "v" : "n") + ":" +
                       (fact.known_corrupted ? "c" : "h") + ":" +
                       (fact.known_missing ? "m" : "p") + ":" +
                       (fact.quarantined ? "q" : "l") + ";";
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

    const storedemo::ScrubReplicaFact *FindFact(
        const storedemo::ScrubTaskResult &result,
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

    class TestOnlyRepairRunner
    {
    public:
        TestOnlyRepairRunner(
            std::map<storedemo::StorageNodeId, storedemo::LocalDiskChunkStore *> stores,
            const storedemo::StorageNodeRegistry *registry,
            RepairReplicaFactsLedger *facts_ledger,
            RepairObserver observer = {})
            : stores_(std::move(stores))
            , registry_(registry)
            , facts_ledger_(facts_ledger)
            , observer_(std::move(observer))
        {
        }

        RepairRunResult Run(const ScrubManifest &manifest,
                            const ScrubRepairCandidate &candidate,
                            const std::uint64_t now_unix_ms) const
        {
            RepairRunResult result;
            if (registry_ == nullptr || facts_ledger_ == nullptr)
            {
                result.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
                result.error_detail = "repair runner requires registry and facts ledger";
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

            std::string source_payload;
            for (const auto &node_id : candidate.healthy_source_replicas)
            {
                const auto snapshot_it = snapshot_by_node.find(node_id);
                if (snapshot_it == snapshot_by_node.end())
                {
                    continue;
                }

                const auto &node_snapshot = snapshot_it->second;
                if (node_snapshot.liveness !=
                        storedemo::StorageNodeRegistryLiveness::kLive ||
                    node_snapshot.facts.health.health !=
                        storedemo::StorageNodeHealth::kHealthy)
                {
                    continue;
                }

                const auto store_it = stores_.find(node_id);
                if (store_it == stores_.end() || store_it->second == nullptr)
                {
                    continue;
                }

                auto read_request = MakeReadRequest(
                    manifest.identity.chunk_id,
                    "repair-read-source-" + node_id);
                read_request.expected_checksum = candidate.expected_checksum;
                read_request.verify_checksum = true;
                const auto read_response = store_it->second->ReadChunk(read_request);
                if (!read_response.ok())
                {
                    continue;
                }
                if (read_response.metadata.size != candidate.expected_size ||
                    !ChecksumEquals(read_response.actual_checksum,
                                    candidate.expected_checksum))
                {
                    continue;
                }

                result.source_node = node_id;
                source_payload = read_response.payload;
                break;
            }

            if (result.source_node.empty())
            {
                result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                result.error_detail = "no healthy repair source is available";
                return result;
            }

            storedemo::PlacementManager placement_manager;
            storedemo::PlacementRequest placement_request;
            placement_request.identity = manifest.identity;
            placement_request.chunk_size_bytes = candidate.expected_size;
            placement_request.policy.replica_count = 1;
            placement_request.policy.minimum_successful_writes = 1;
            placement_request.excluded_nodes = manifest.replica_nodes;

            const auto placement = placement_manager.SelectPlacement(
                placement_request, *registry_, now_unix_ms);
            if (!placement.ok() || placement.decision.replica_nodes.empty())
            {
                result.status = placement.status;
                result.error_detail = placement.error_detail;
                return result;
            }

            result.target_node = placement.decision.replica_nodes.front().node_id;
            const auto store_it = stores_.find(result.target_node);
            if (store_it == stores_.end() || store_it->second == nullptr)
            {
                result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                result.error_detail = "selected repair target store is unavailable";
                return result;
            }

            const auto write_response = store_it->second->WriteChunk(
                MakeWriteRequest(manifest.identity,
                                 source_payload,
                                 "repair-write-target-" + result.target_node));
            if (!write_response.ok())
            {
                result.status = write_response.status;
                result.error_detail = write_response.error_detail;
                return result;
            }

            result.target_durable = true;
            result.idempotent_success = write_response.already_exists;
            result.facts_updated = facts_ledger_->MarkDurable(manifest.identity.chunk_id,
                                                              result.target_node);
            return result;
        }

    private:
        std::map<storedemo::StorageNodeId, storedemo::LocalDiskChunkStore *> stores_;
        const storedemo::StorageNodeRegistry *registry_;
        RepairReplicaFactsLedger *facts_ledger_;
        RepairObserver observer_;
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

        storedemo::LocalDiskChunkStore &CreateStore(
            const std::size_t node_index,
            std::shared_ptr<storedemo::DurableFile> durable_file = {})
        {
            const auto node_id = storedemo::test::MakeStorageNodeIdFixture(node_index);
            auto store = std::make_unique<storedemo::LocalDiskChunkStore>(
                storedemo::LocalDiskChunkStoreConfig{
                    .data_dir = temp_dir_.Path("store-" + std::to_string(node_index)),
                    .node_id = node_id,
                    .durable_file = std::move(durable_file),
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
            const std::uint32_t active_reads = 0,
            const bool write_overloaded = false,
            const std::uint64_t total_capacity_bytes = 64 * 1024,
            const std::uint64_t used_capacity_bytes = 8 * 1024)
        {
            storedemo::RegisterStorageNodeRequest request;
            request.node_id = storedemo::test::MakeStorageNodeIdFixture(node_index);
            request.endpoint = "127.0.0.1:" + std::to_string(7100 + node_index);
            request.observed_at_unix_ms = observed_at_unix_ms;
            request.facts = MakeRegistryFacts(health, disk_pressure, active_reads);
            request.facts.capacity.total_capacity_bytes = total_capacity_bytes;
            request.facts.capacity.used_capacity_bytes = used_capacity_bytes;
            request.facts.capacity.available_capacity_bytes =
                total_capacity_bytes >= used_capacity_bytes
                    ? total_capacity_bytes - used_capacity_bytes
                    : 0;
            request.facts.load.write_admission_overloaded = write_overloaded;
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

        std::map<storedemo::StorageNodeId, storedemo::ChunkStore *>
        RawChunkStoreMap()
        {
            std::map<storedemo::StorageNodeId, storedemo::ChunkStore *> map;
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

        storedemo::ScrubTask MakeScrubManagerTask(
            const std::string &task_id,
            const ScrubManifest &manifest,
            const std::uint64_t timeout_ms = 0) const
        {
            storedemo::ScrubTask task;
            task.task_id = task_id;
            task.manifest.identity = manifest.identity;
            task.manifest.expected_size = manifest.expected_size;
            task.manifest.expected_checksum = manifest.expected_checksum;
            task.manifest.replica_nodes = manifest.replica_nodes;
            task.manifest.desired_replica_count = manifest.desired_replica_count;
            task.context.timeout_ms = timeout_ms;
            return task;
        }

        storedemo::test::ScopedStoreTestDir temp_dir_{"storage_scrub_repair"};
        storedemo::StorageNodeRegistry registry_;
        RepairReplicaFactsLedger repair_facts_;
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

    TEST_F(StorageScrubRepairTest,
           RepairSelectsHealthySourceAndHealthyTargetAndUpdatesFactsAfterDurableWrite)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &overloaded_target = CreateStore(3);
        auto &high_pressure_target = CreateStore(4);
        auto &low_capacity_target = CreateStore(5);
        auto &stale_target = CreateStore(6);
        auto &healthy_target = CreateStore(7);

        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, 0, true);
        RegisterNode(4, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kHigh);
        RegisterNode(5, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, 0, false, 1024, 900);
        RegisterNode(6, 60);
        RegisterNode(7, 100);

        const auto identity = MakeIdentityOrThrow("repair-success", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(2048, "repair-success");
        WriteReplica(source_store, identity, payload, "repair-success-write-1");
        WriteReplica(peer_store, identity, payload, "repair-success-write-2");

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            3);
        const auto original_manifest = manifest;

        TestOnlyRepairRunner runner(RawStoreMap(), &registry_, &repair_facts_);
        const auto result = runner.Run(
            manifest,
            MakeRepairCandidate(
                manifest,
                {storedemo::test::MakeStorageNodeIdFixture(1),
                 storedemo::test::MakeStorageNodeIdFixture(2)}),
            110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.source_node, storedemo::test::MakeStorageNodeIdFixture(1));
        EXPECT_EQ(result.target_node, storedemo::test::MakeStorageNodeIdFixture(7));
        EXPECT_TRUE(result.target_durable);
        EXPECT_TRUE(result.facts_updated);
        EXPECT_FALSE(result.idempotent_success);
        EXPECT_TRUE(repair_facts_.HasDurableReplica(identity.chunk_id, result.target_node));
        EXPECT_EQ(repair_facts_.DurableReplicaCount(identity.chunk_id), 1U);

        const auto target_stat = healthy_target.StatChunk(
            MakeStatRequest(identity.chunk_id, "repair-success-target-stat"));
        ASSERT_EQ(target_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_stat.metadata.state, storedemo::ChunkState::kLive);
        EXPECT_EQ(target_stat.metadata.size, manifest.expected_size);
        EXPECT_TRUE(ChecksumEquals(target_stat.metadata.checksum,
                                   manifest.expected_checksum));
        ExpectManifestEq(manifest, original_manifest);

        (void)overloaded_target;
        (void)high_pressure_target;
        (void)low_capacity_target;
        (void)stale_target;
    }

    TEST_F(StorageScrubRepairTest,
           RepairRejectsQuarantinedStaleUnavailableAndUnhealthySources)
    {
        auto &healthy_source = CreateStore(1);
        auto &quarantined_source = CreateStore(2);
        auto &stale_source = CreateStore(3);
        auto &unavailable_source = CreateStore(4);
        auto &degraded_source = CreateStore(5);
        auto &target_store = CreateStore(6);

        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 60);
        RegisterNode(4, 100, storedemo::StorageNodeHealth::kUnavailable);
        RegisterNode(5, 100, storedemo::StorageNodeHealth::kDegraded);
        RegisterNode(6, 100);

        const auto identity = MakeIdentityOrThrow("repair-source-filter", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(64, "repair-source-filter");
        WriteReplica(healthy_source, identity, payload, "repair-source-write-1");
        WriteReplica(quarantined_source, identity, payload, "repair-source-write-2");
        WriteReplica(stale_source, identity, payload, "repair-source-write-3");
        WriteReplica(unavailable_source, identity, payload, "repair-source-write-4");
        WriteReplica(degraded_source, identity, payload, "repair-source-write-5");

        TamperReplica(quarantined_source,
                      identity,
                      storedemo::test::MakeChunkPayload(payload.size(), "tampered-source"));
        auto quarantine_stat = MakeStatRequest(identity.chunk_id,
                                               "repair-source-quarantine");
        quarantine_stat.verify_checksum = true;
        ASSERT_EQ(quarantined_source.StatChunk(quarantine_stat).status,
                  storedemo::StorageNodeStatusCode::kCorrupted);

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2),
             storedemo::test::MakeStorageNodeIdFixture(3),
             storedemo::test::MakeStorageNodeIdFixture(4),
             storedemo::test::MakeStorageNodeIdFixture(5)},
            6);
        TestOnlyRepairRunner runner(RawStoreMap(), &registry_, &repair_facts_);
        const auto result = runner.Run(
            manifest,
            MakeRepairCandidate(
                manifest,
                {storedemo::test::MakeStorageNodeIdFixture(2),
                 storedemo::test::MakeStorageNodeIdFixture(3),
                 storedemo::test::MakeStorageNodeIdFixture(4),
                 storedemo::test::MakeStorageNodeIdFixture(5),
                 storedemo::test::MakeStorageNodeIdFixture(1)}),
            110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.source_node, storedemo::test::MakeStorageNodeIdFixture(1));
        EXPECT_EQ(result.target_node, storedemo::test::MakeStorageNodeIdFixture(6));
        EXPECT_TRUE(repair_facts_.HasDurableReplica(identity.chunk_id, result.target_node));

        const auto source_state = quarantined_source.StatChunk(
            MakeStatRequest(identity.chunk_id, "repair-source-quarantine-state"));
        ASSERT_EQ(source_state.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(source_state.metadata.state, storedemo::ChunkState::kQuarantined);

        const auto target_stat = target_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "repair-source-target-stat"));
        ASSERT_EQ(target_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_stat.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageScrubRepairTest,
           RepairFailsWhenSourceChecksumVerificationFailsAndDoesNotUpdateFacts)
    {
        auto &source_store = CreateStore(1);
        auto &target_store = CreateStore(2);
        RegisterNode(1, 100);
        RegisterNode(2, 100);

        const auto identity = MakeIdentityOrThrow("repair-source-mismatch", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "repair-source-mismatch");
        WriteReplica(source_store, identity, payload, "repair-source-mismatch-write");
        TamperReplica(source_store,
                      identity,
                      storedemo::test::MakeChunkPayload(payload.size(), "tampered-source"));

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1)},
            2);
        TestOnlyRepairRunner runner(RawStoreMap(), &registry_, &repair_facts_);
        const auto result = runner.Run(
            manifest,
            MakeRepairCandidate(
                manifest, {storedemo::test::MakeStorageNodeIdFixture(1)}),
            110);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_FALSE(result.target_durable);
        EXPECT_FALSE(result.facts_updated);
        EXPECT_FALSE(repair_facts_.HasDurableReplica(identity.chunk_id,
                                                     storedemo::test::MakeStorageNodeIdFixture(2)));

        const auto source_state = source_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "repair-source-mismatch-state"));
        ASSERT_EQ(source_state.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(source_state.metadata.state, storedemo::ChunkState::kQuarantined);
        EXPECT_EQ(target_store.StatChunk(
                      MakeStatRequest(identity.chunk_id, "repair-source-mismatch-target"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
    }

    TEST_F(StorageScrubRepairTest,
           RepairDoesNotUpdateFactsWhenTargetDurableWriteFails)
    {
        auto &source_store = CreateStore(1);
        auto writer_state = std::make_shared<RecordingWriterState>();
        auto failing_durable_file = std::make_shared<RecordingDurableFile>(writer_state);
        failing_durable_file->publish_result.error =
            storedemo::DurableFileErrorCode::kAtomicPublishFailed;
        failing_durable_file->publish_result.error_detail = "publish failed";
        auto &target_store = CreateStore(2, failing_durable_file);

        RegisterNode(1, 100);
        RegisterNode(2, 100);

        const auto identity = MakeIdentityOrThrow("repair-target-fail", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(40, "repair-target-fail");
        WriteReplica(source_store, identity, payload, "repair-target-fail-write");

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1)},
            2);
        TestOnlyRepairRunner runner(RawStoreMap(), &registry_, &repair_facts_);
        const auto result = runner.Run(
            manifest,
            MakeRepairCandidate(
                manifest, {storedemo::test::MakeStorageNodeIdFixture(1)}),
            110);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kIoError);
        EXPECT_EQ(result.target_node, storedemo::test::MakeStorageNodeIdFixture(2));
        EXPECT_FALSE(result.target_durable);
        EXPECT_FALSE(result.facts_updated);
        EXPECT_FALSE(repair_facts_.HasDurableReplica(identity.chunk_id, result.target_node));
        EXPECT_EQ(target_store.StatChunk(
                      MakeStatRequest(identity.chunk_id, "repair-target-fail-stat"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
    }

    TEST_F(StorageScrubRepairTest,
           RepairTreatsExistingMatchingTargetAsIdempotentSuccess)
    {
        auto &source_store = CreateStore(1);
        auto &target_store = CreateStore(2);
        RegisterNode(1, 100);
        RegisterNode(2, 100);

        const auto identity = MakeIdentityOrThrow("repair-idempotent-target", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(56, "repair-idempotent-target");
        WriteReplica(source_store, identity, payload, "repair-idempotent-source");
        WriteReplica(target_store, identity, payload, "repair-idempotent-target");

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1)},
            2);
        TestOnlyRepairRunner runner(RawStoreMap(), &registry_, &repair_facts_);
        const auto result = runner.Run(
            manifest,
            MakeRepairCandidate(
                manifest, {storedemo::test::MakeStorageNodeIdFixture(1)}),
            110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_TRUE(result.target_durable);
        EXPECT_TRUE(result.idempotent_success);
        EXPECT_TRUE(result.facts_updated);
        EXPECT_TRUE(repair_facts_.HasDurableReplica(identity.chunk_id,
                                                    storedemo::test::MakeStorageNodeIdFixture(2)));
    }

    TEST_F(StorageScrubRepairTest, RepairFailsWhenNoUsableSourceExists)
    {
        auto &stale_source = CreateStore(1);
        auto &unavailable_source = CreateStore(2);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 60);
        RegisterNode(2, 100, storedemo::StorageNodeHealth::kUnavailable);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("repair-no-source", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(32, "repair-no-source");
        WriteReplica(stale_source, identity, payload, "repair-no-source-1");
        WriteReplica(unavailable_source, identity, payload, "repair-no-source-2");

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1)},
            2);
        TestOnlyRepairRunner runner(RawStoreMap(), &registry_, &repair_facts_);
        const auto result = runner.Run(
            manifest,
            MakeRepairCandidate(
                manifest,
                {storedemo::test::MakeStorageNodeIdFixture(1),
                 storedemo::test::MakeStorageNodeIdFixture(2)}),
            110);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_FALSE(result.target_durable);
        EXPECT_FALSE(result.facts_updated);
        EXPECT_EQ(target_store.StatChunk(
                      MakeStatRequest(identity.chunk_id, "repair-no-source-target-stat"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
    }

    TEST_F(StorageScrubRepairTest, RepairFailsWhenNoUsableTargetExists)
    {
        auto &source_store = CreateStore(1);
        auto &stale_target = CreateStore(2);
        auto &unavailable_target = CreateStore(3);
        auto &overloaded_target = CreateStore(4);
        auto &high_disk_target = CreateStore(5);
        auto &small_target = CreateStore(6);

        RegisterNode(1, 100);
        RegisterNode(2, 60);
        RegisterNode(3, 100, storedemo::StorageNodeHealth::kUnavailable);
        RegisterNode(4, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, 0, true);
        RegisterNode(5, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kFull);
        RegisterNode(6, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, 0, false, 1024, 900);

        const auto identity = MakeIdentityOrThrow("repair-no-target", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(2048, "repair-no-target");
        WriteReplica(source_store, identity, payload, "repair-no-target-source");

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1)},
            2);
        const auto original_manifest = manifest;

        TestOnlyRepairRunner runner(RawStoreMap(), &registry_, &repair_facts_);
        const auto result = runner.Run(
            manifest,
            MakeRepairCandidate(
                manifest, {storedemo::test::MakeStorageNodeIdFixture(1)}),
            110);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_FALSE(result.target_durable);
        EXPECT_FALSE(result.facts_updated);
        EXPECT_EQ(repair_facts_.DurableReplicaCount(identity.chunk_id), 0U);
        ExpectManifestEq(manifest, original_manifest);

        (void)stale_target;
        (void)unavailable_target;
        (void)overloaded_target;
        (void)high_disk_target;
        (void)small_target;
    }

    TEST_F(StorageScrubRepairTest,
           RepeatedRepairIsIdempotentAndDoesNotDuplicateFactsOrTouchRaft)
    {
        auto &source_store = CreateStore(1);
        auto &target_store = CreateStore(2);
        RegisterNode(1, 100);
        RegisterNode(2, 100);

        const auto identity = MakeIdentityOrThrow("repair-repeat", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(72, "repair-repeat");
        WriteReplica(source_store, identity, payload, "repair-repeat-source");

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1)},
            2);
        const auto original_manifest = manifest;

        std::size_t metadata_mutation_calls = 0;
        std::size_t raft_calls = 0;
        std::size_t payload_persist_calls = 0;
        TestOnlyRepairRunner runner(
            RawStoreMap(),
            &registry_,
            &repair_facts_,
            RepairObserver{
                .metadata_mutation_hook = [&metadata_mutation_calls]()
                { ++metadata_mutation_calls; },
                .raft_call_hook = [&raft_calls]()
                { ++raft_calls; },
                .payload_persist_hook = [&payload_persist_calls](std::string_view)
                { ++payload_persist_calls; }});

        const auto candidate = MakeRepairCandidate(
            manifest, {storedemo::test::MakeStorageNodeIdFixture(1)});
        const auto first = runner.Run(manifest, candidate, 110);
        const auto second = runner.Run(manifest, candidate, 120);

        ASSERT_EQ(first.status, storedemo::StorageNodeStatusCode::kOk)
            << first.error_detail;
        ASSERT_EQ(second.status, storedemo::StorageNodeStatusCode::kOk)
            << second.error_detail;
        EXPECT_TRUE(first.target_durable);
        EXPECT_TRUE(first.facts_updated);
        EXPECT_TRUE(second.target_durable);
        EXPECT_FALSE(second.facts_updated);
        EXPECT_TRUE(second.idempotent_success);
        EXPECT_EQ(first.target_node, storedemo::test::MakeStorageNodeIdFixture(2));
        EXPECT_EQ(second.target_node, first.target_node);
        EXPECT_EQ(repair_facts_.DurableReplicaCount(identity.chunk_id), 1U);
        ExpectManifestEq(manifest, original_manifest);
        EXPECT_EQ(metadata_mutation_calls, 0U);
        EXPECT_EQ(raft_calls, 0U);
        EXPECT_EQ(payload_persist_calls, 0U);

        const auto target_stat = target_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "repair-repeat-target-stat"));
        ASSERT_EQ(target_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_stat.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionScrubManagerCompletesHealthyTaskWithoutRepairCandidate)
    {
        auto &store = CreateStore(1);
        RegisterNode(1, 100);

        const auto identity = MakeIdentityOrThrow("scrub-manager-healthy", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(32, "scrub-manager-healthy");
        WriteReplica(store, identity, payload, "scrub-manager-healthy-write");

        const auto manifest = MakeManifest(
            identity, payload, {storedemo::test::MakeStorageNodeIdFixture(1)}, 1);
        storedemo::ScrubManager manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 4,
                .now_unix_ms = []()
                {
                    return 110;
                }});

        const auto submit_result =
            manager.SubmitTask(MakeScrubManagerTask("scrub-manager-healthy", manifest));
        ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;

        const auto drain_result = manager.Drain();
        ASSERT_TRUE(drain_result.drained) << drain_result.error_detail;

        const auto task = manager.FindTask("scrub-manager-healthy");
        ASSERT_TRUE(task.has_value());
        EXPECT_EQ(task->state, storedemo::ScrubTaskState::kCompleted);
        ASSERT_TRUE(task->result.has_value());
        EXPECT_EQ(task->result->status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_FALSE(task->result->repair_candidate.has_value());
        ASSERT_EQ(task->result->replica_facts.size(), 1U);
        EXPECT_TRUE(task->result->replica_facts.front().checksum_verified);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionScrubManagerQueueIsBoundedAndDoesNotBlockForegroundIo)
    {
        auto &store = CreateStore(1);
        RegisterNode(1, 100);

        const auto runner_started = std::make_shared<std::promise<void>>();
        const auto release_runner = std::make_shared<std::promise<void>>();
        const auto runner_started_once = std::make_shared<bool>(false);
        const auto runner_started_mutex = std::make_shared<std::mutex>();
        auto runner_started_future = runner_started->get_future();
        auto release_runner_future =
            std::make_shared<std::shared_future<void>>(
                release_runner->get_future().share());

        storedemo::ScrubManager manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 1,
                .now_unix_ms = []()
                {
                    return 110;
                }},
            [runner_started,
             runner_started_once,
             runner_started_mutex,
             release_runner_future](
                const storedemo::ScrubTask &) -> storedemo::ScrubTaskResult
            {
                {
                    std::lock_guard<std::mutex> lock(*runner_started_mutex);
                    if (!*runner_started_once)
                    {
                        *runner_started_once = true;
                        runner_started->set_value();
                    }
                }
                release_runner_future->wait();
                storedemo::ScrubTaskResult result;
                result.status = storedemo::StorageNodeStatusCode::kOk;
                return result;
            });

        auto first_task = MakeScrubManagerTask(
            "scrub-manager-queued-1",
            MakeManifest(MakeIdentityOrThrow("queued-1", 1, 0, 0),
                         storedemo::test::MakeChunkPayload(8, "queued-1"),
                         {storedemo::test::MakeStorageNodeIdFixture(1)},
                         1));
        auto second_task = MakeScrubManagerTask(
            "scrub-manager-queued-2",
            MakeManifest(MakeIdentityOrThrow("queued-2", 1, 0, 0),
                         storedemo::test::MakeChunkPayload(8, "queued-2"),
                         {storedemo::test::MakeStorageNodeIdFixture(1)},
                         1));
        auto third_task = MakeScrubManagerTask(
            "scrub-manager-queued-3",
            MakeManifest(MakeIdentityOrThrow("queued-3", 1, 0, 0),
                         storedemo::test::MakeChunkPayload(8, "queued-3"),
                         {storedemo::test::MakeStorageNodeIdFixture(1)},
                         1));

        ASSERT_TRUE(manager.SubmitTask(std::move(first_task)).accepted());
        ASSERT_EQ(runner_started_future.wait_for(std::chrono::milliseconds(200)),
                  std::future_status::ready);
        ASSERT_TRUE(manager.SubmitTask(std::move(second_task)).accepted());

        const auto overloaded_result = manager.SubmitTask(std::move(third_task));
        EXPECT_EQ(overloaded_result.code,
                  storedemo::ScrubManagerSubmitCode::kOverloaded);

        const auto foreground_identity =
            MakeIdentityOrThrow("foreground-io", 1, 0, 0);
        const auto foreground_payload =
            storedemo::test::MakeChunkPayload(24, "foreground-io");
        const auto write_response = store.WriteChunk(
            MakeWriteRequest(foreground_identity,
                             foreground_payload,
                             "foreground-io-write"));
        ASSERT_EQ(write_response.status, storedemo::StorageNodeStatusCode::kOk)
            << write_response.error_detail;

        auto read_request =
            MakeReadRequest(foreground_identity.chunk_id, "foreground-io-read");
        read_request.expected_checksum = ComputeChecksumOrThrow(foreground_payload);
        read_request.verify_checksum = true;
        const auto read_response = store.ReadChunk(read_request);
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, foreground_payload);

        release_runner->set_value();
        const auto drain_result = manager.Drain();
        EXPECT_TRUE(drain_result.drained);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionScrubManagerDrainWaitsForSubmittedTaskAndStopRejectsNewTasks)
    {
        auto &store = CreateStore(1);
        RegisterNode(1, 100);

        const auto runner_started = std::make_shared<std::promise<void>>();
        const auto release_runner = std::make_shared<std::promise<void>>();
        const auto runner_started_once = std::make_shared<bool>(false);
        const auto runner_started_mutex = std::make_shared<std::mutex>();
        auto runner_started_future = runner_started->get_future();
        auto release_runner_future =
            std::make_shared<std::shared_future<void>>(
                release_runner->get_future().share());

        storedemo::ScrubManager manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 2,
                .now_unix_ms = []()
                {
                    return 110;
                }},
            [runner_started,
             runner_started_once,
             runner_started_mutex,
             release_runner_future](
                const storedemo::ScrubTask &) -> storedemo::ScrubTaskResult
            {
                {
                    std::lock_guard<std::mutex> lock(*runner_started_mutex);
                    if (!*runner_started_once)
                    {
                        *runner_started_once = true;
                        runner_started->set_value();
                    }
                }
                release_runner_future->wait();
                storedemo::ScrubTaskResult result;
                result.status = storedemo::StorageNodeStatusCode::kOk;
                return result;
            });

        ASSERT_TRUE(manager.SubmitTask(
                              MakeScrubManagerTask(
                                  "scrub-manager-drain",
                                  MakeManifest(
                                      MakeIdentityOrThrow("drain", 1, 0, 0),
                                      storedemo::test::MakeChunkPayload(8, "drain"),
                                      {storedemo::test::MakeStorageNodeIdFixture(1)},
                                      1)))
                        .accepted());
        ASSERT_EQ(runner_started_future.wait_for(std::chrono::milliseconds(200)),
                  std::future_status::ready);

        auto drain_future = std::async(std::launch::async,
                                       [&manager]()
                                       {
                                           return manager.Drain();
                                       });
        EXPECT_EQ(drain_future.wait_for(std::chrono::milliseconds(50)),
                  std::future_status::timeout);

        release_runner->set_value();
        const auto drain_result = drain_future.get();
        EXPECT_TRUE(drain_result.drained);

        const auto stop_result = manager.Stop();
        EXPECT_TRUE(stop_result.stopped);

        const auto rejected_result = manager.SubmitTask(
            MakeScrubManagerTask(
                "scrub-manager-after-stop",
                MakeManifest(MakeIdentityOrThrow("after-stop", 1, 0, 0),
                             storedemo::test::MakeChunkPayload(8, "after-stop"),
                             {storedemo::test::MakeStorageNodeIdFixture(1)},
                             1)));
        EXPECT_EQ(rejected_result.code, storedemo::ScrubManagerSubmitCode::kStopped);

        (void)store;
    }

    TEST_F(StorageScrubRepairTest,
           ProductionScrubManagerCorruptedReplicaProducesRepairCandidate)
    {
        auto &healthy_store = CreateStore(1);
        auto &corrupted_store = CreateStore(2);
        RegisterNode(1, 100);
        RegisterNode(2, 100);

        const auto identity = MakeIdentityOrThrow("scrub-manager-corrupted", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(48, "scrub-manager-corrupted");
        WriteReplica(healthy_store, identity, payload, "scrub-manager-corrupted-1");
        WriteReplica(corrupted_store, identity, payload, "scrub-manager-corrupted-2");
        TamperReplica(corrupted_store,
                      identity,
                      storedemo::test::MakeChunkPayload(payload.size(), "tampered"));

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2);

        storedemo::ScrubManager manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 4,
                .now_unix_ms = []()
                {
                    return 110;
                }});
        ASSERT_TRUE(manager.SubmitTask(
                              MakeScrubManagerTask(
                                  "scrub-manager-corrupted-task", manifest))
                        .accepted());
        ASSERT_TRUE(manager.Drain().drained);

        const auto task = manager.FindTask("scrub-manager-corrupted-task");
        ASSERT_TRUE(task.has_value());
        EXPECT_EQ(task->state, storedemo::ScrubTaskState::kCompleted);
        ASSERT_TRUE(task->result.has_value());
        ASSERT_TRUE(task->result->repair_candidate.has_value());
        EXPECT_EQ(task->result->repair_candidate->bad_replicas,
                  std::vector<storedemo::StorageNodeId>{
                      storedemo::test::MakeStorageNodeIdFixture(2)});
        EXPECT_EQ(task->result->repair_candidate->healthy_source_replicas,
                  std::vector<storedemo::StorageNodeId>{
                      storedemo::test::MakeStorageNodeIdFixture(1)});

        const auto *corrupted_fact = FindFact(
            *task->result, storedemo::test::MakeStorageNodeIdFixture(2));
        ASSERT_NE(corrupted_fact, nullptr);
        EXPECT_TRUE(corrupted_fact->known_corrupted);
        EXPECT_TRUE(corrupted_fact->quarantined);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionScrubManagerProducesLostAndUnderReplicatedFacts)
    {
        auto &first_store = CreateStore(1);
        auto &second_store = CreateStore(2);
        auto &missing_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto lost_identity = MakeIdentityOrThrow("scrub-manager-lost", 1, 0, 0);
        const auto lost_payload =
            storedemo::test::MakeChunkPayload(24, "scrub-manager-lost");
        WriteReplica(first_store, lost_identity, lost_payload, "scrub-manager-lost-1");
        WriteReplica(second_store, lost_identity, lost_payload, "scrub-manager-lost-2");
        TamperReplica(first_store,
                      lost_identity,
                      storedemo::test::MakeChunkPayload(lost_payload.size(), "lost-a"));
        TamperReplica(second_store,
                      lost_identity,
                      storedemo::test::MakeChunkPayload(lost_payload.size(), "lost-b"));

        const auto under_identity =
            MakeIdentityOrThrow("scrub-manager-under", 1, 0, 0);
        const auto under_payload =
            storedemo::test::MakeChunkPayload(24, "scrub-manager-under");
        WriteReplica(first_store, under_identity, under_payload, "scrub-manager-under-1");
        WriteReplica(second_store, under_identity, under_payload, "scrub-manager-under-2");

        storedemo::ScrubManager lost_manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 4,
                .now_unix_ms = []()
                {
                    return 110;
                }});

        ASSERT_TRUE(lost_manager.SubmitTask(
                                   MakeScrubManagerTask(
                                       "scrub-manager-lost-task",
                                       MakeManifest(
                                           lost_identity,
                                           lost_payload,
                                           {storedemo::test::MakeStorageNodeIdFixture(1),
                                            storedemo::test::MakeStorageNodeIdFixture(2)},
                                           2)))
                        .accepted());
        ASSERT_TRUE(lost_manager.Drain().drained);

        const auto lost_task = lost_manager.FindTask("scrub-manager-lost-task");
        ASSERT_TRUE(lost_task.has_value());
        ASSERT_TRUE(lost_task->result.has_value());
        ASSERT_TRUE(lost_task->result->repair_candidate.has_value());
        EXPECT_TRUE(lost_task->result->repair_candidate->lost_or_unrecoverable);
        EXPECT_TRUE(lost_task->result->repair_candidate->healthy_source_replicas.empty());

        storedemo::ScrubManager under_manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 4,
                .now_unix_ms = []()
                {
                    return 110;
                }});
        const auto under_submit = under_manager.SubmitTask(
            MakeScrubManagerTask(
                "scrub-manager-under-task",
                MakeManifest(
                    under_identity,
                    under_payload,
                    {storedemo::test::MakeStorageNodeIdFixture(1),
                     storedemo::test::MakeStorageNodeIdFixture(2),
                     storedemo::test::MakeStorageNodeIdFixture(3)},
                    3)));
        ASSERT_TRUE(under_submit.accepted()) << under_submit.error_detail;
        ASSERT_TRUE(under_manager.Drain().drained);

        const auto under_task = under_manager.FindTask("scrub-manager-under-task");
        ASSERT_TRUE(under_task.has_value());
        ASSERT_TRUE(under_task->result.has_value());
        ASSERT_TRUE(under_task->result->repair_candidate.has_value());
        EXPECT_TRUE(under_task->result->repair_candidate->under_replicated);
        EXPECT_EQ(under_task->result->repair_candidate->bad_replicas,
                  std::vector<storedemo::StorageNodeId>{
                      storedemo::test::MakeStorageNodeIdFixture(3)});
        EXPECT_EQ(missing_store.StatChunk(
                      MakeStatRequest(under_identity.chunk_id, "scrub-manager-under-stat"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionScrubManagerFiltersUnhealthySourcesAndIsIdempotent)
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

        const auto identity =
            MakeIdentityOrThrow("scrub-manager-filtered", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(32, "scrub-manager-filtered");
        WriteReplica(healthy_store, identity, payload, "scrub-manager-filtered-1");
        WriteReplica(stale_store, identity, payload, "scrub-manager-filtered-2");
        WriteReplica(unavailable_store, identity, payload, "scrub-manager-filtered-3");
        WriteReplica(degraded_store, identity, payload, "scrub-manager-filtered-4");
        WriteReplica(corrupted_store, identity, payload, "scrub-manager-filtered-5");
        TamperReplica(corrupted_store,
                      identity,
                      storedemo::test::MakeChunkPayload(payload.size(), "tampered"));

        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2),
             storedemo::test::MakeStorageNodeIdFixture(3),
             storedemo::test::MakeStorageNodeIdFixture(4),
             storedemo::test::MakeStorageNodeIdFixture(5)},
            5);

        storedemo::ScrubManager manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 4,
                .now_unix_ms = []()
                {
                    return 110;
                }});
        ASSERT_TRUE(manager.SubmitTask(
                              MakeScrubManagerTask(
                                  "scrub-manager-filtered-1", manifest))
                        .accepted());
        ASSERT_TRUE(manager.SubmitTask(
                              MakeScrubManagerTask(
                                  "scrub-manager-filtered-2", manifest))
                        .accepted());
        ASSERT_TRUE(manager.Drain().drained);

        const auto first = manager.FindTask("scrub-manager-filtered-1");
        const auto second = manager.FindTask("scrub-manager-filtered-2");
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        ASSERT_TRUE(first->result.has_value());
        ASSERT_TRUE(second->result.has_value());
        ASSERT_TRUE(first->result->repair_candidate.has_value());
        ASSERT_TRUE(second->result->repair_candidate.has_value());
        EXPECT_EQ(first->result->repair_candidate->healthy_source_replicas,
                  std::vector<storedemo::StorageNodeId>{
                      storedemo::test::MakeStorageNodeIdFixture(1)});
        EXPECT_EQ(CandidateSummary(*first->result->repair_candidate),
                  CandidateSummary(*second->result->repair_candidate));

        const auto *first_corrupted = FindFact(
            *first->result, storedemo::test::MakeStorageNodeIdFixture(5));
        const auto *second_corrupted = FindFact(
            *second->result, storedemo::test::MakeStorageNodeIdFixture(5));
        ASSERT_NE(first_corrupted, nullptr);
        ASSERT_NE(second_corrupted, nullptr);
        EXPECT_TRUE(first_corrupted->known_corrupted);
        EXPECT_TRUE(second_corrupted->known_corrupted);
        EXPECT_TRUE(first_corrupted->quarantined);
        EXPECT_TRUE(second_corrupted->quarantined);
        EXPECT_EQ(first_corrupted->state_after, storedemo::ChunkState::kQuarantined);
        EXPECT_EQ(second_corrupted->state_after, storedemo::ChunkState::kQuarantined);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionScrubManagerFailedTaskRecordsAttemptsAndLastError)
    {
        auto &store = CreateStore(1);
        RegisterNode(1, 100);

        storedemo::ScrubManager manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 2,
                .now_unix_ms = []()
                {
                    return 110;
                }},
            [](const storedemo::ScrubTask &) -> storedemo::ScrubTaskResult
            {
                storedemo::ScrubTaskResult result;
                result.status = storedemo::StorageNodeStatusCode::kTimeout;
                result.error_detail = "forced scrub timeout";
                result.retry_after_ms = 25;
                return result;
            });

        ASSERT_TRUE(manager.SubmitTask(
                              MakeScrubManagerTask(
                                  "scrub-manager-failed",
                                  MakeManifest(
                                      MakeIdentityOrThrow("failed", 1, 0, 0),
                                      storedemo::test::MakeChunkPayload(8, "failed"),
                                      {storedemo::test::MakeStorageNodeIdFixture(1)},
                                      1),
                                  10))
                        .accepted());
        ASSERT_TRUE(manager.Drain().drained);

        const auto task = manager.FindTask("scrub-manager-failed");
        ASSERT_TRUE(task.has_value());
        EXPECT_EQ(task->state, storedemo::ScrubTaskState::kFailed);
        EXPECT_EQ(task->attempts, 1U);
        EXPECT_EQ(task->last_error, storedemo::StorageNodeStatusCode::kTimeout);
        EXPECT_EQ(task->last_error_detail, "forced scrub timeout");

        const auto stats = manager.SnapshotStats();
        EXPECT_EQ(stats.failed_tasks, 1U);
        EXPECT_EQ(stats.total_attempts, 1U);

        (void)store;
    }
} // namespace
