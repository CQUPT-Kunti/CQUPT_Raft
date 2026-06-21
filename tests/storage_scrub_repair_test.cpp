#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <future>
#include <filesystem>
#include <fstream>
#include <functional>
#include <chrono>
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

#include <grpcpp/grpcpp.h>

#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"
#include "store/maintenance/repair_manager.h"
#include "store/maintenance/scrub_manager.h"
#include "store/node/storage_node_client.h"
#include "store/node/storage_node_registry.h"
#include "store/node/storage_node_service.h"
#include "store/placement/placement_manager.h"
#include "store/placement/replica_policy.h"
#include "support/store_test_utils.h"

namespace
{
    using namespace std::chrono_literals;

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
        std::size_t healthy_replica_count{0};
        std::size_t required_replica_count{0};
        std::size_t missing_replica_count{0};
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

    class RunningStorageNodeService
    {
    public:
        explicit RunningStorageNodeService(
            std::shared_ptr<storedemo::StorageNodeService> service)
            : service_(std::move(service))
        {
            if (service_ == nullptr)
            {
                throw std::invalid_argument("service must not be null");
            }

            grpc::ServerBuilder builder;
            builder.AddListeningPort("127.0.0.1:0",
                                     grpc::InsecureServerCredentials(),
                                     &selected_port_);
            builder.RegisterService(service_.get());
            server_ = builder.BuildAndStart();
            if (!server_ || selected_port_ <= 0)
            {
                throw std::runtime_error("failed to start storage node service");
            }

            channel_ = grpc::CreateChannel(
                "127.0.0.1:" + std::to_string(selected_port_),
                grpc::InsecureChannelCredentials());
        }

        ~RunningStorageNodeService()
        {
            if (server_ != nullptr)
            {
                server_->Shutdown();
                server_->Wait();
            }
        }

        RunningStorageNodeService(const RunningStorageNodeService &) = delete;
        RunningStorageNodeService &operator=(const RunningStorageNodeService &) = delete;

        [[nodiscard]] std::shared_ptr<grpc::Channel> channel() const
        {
            return channel_;
        }

    private:
        std::shared_ptr<storedemo::StorageNodeService> service_;
        std::unique_ptr<grpc::Server> server_;
        std::shared_ptr<grpc::Channel> channel_;
        int selected_port_{0};
    };

    storedemo::RepairSourceReadResult ToRepairSourceReadResult(
        const storedemo::ReadChunkResponse &response)
    {
        storedemo::RepairSourceReadResult result;
        result.status = response.status;
        result.error_detail = response.error_detail;
        result.retry_after_ms = response.retry_after_ms;
        result.metadata = response.metadata;
        result.actual_checksum = response.actual_checksum;
        result.payload = response.payload;
        result.verified = response.verified;
        return result;
    }

    storedemo::RepairTargetWriteResult ToRepairTargetWriteResult(
        const storedemo::StorageNodeClientRepairChunkResponse &response)
    {
        storedemo::RepairTargetWriteResult result;
        result.status = response.status;
        result.error_detail = response.error_detail;
        result.retry_after_ms = response.retry_after_ms;
        result.metadata = response.metadata;
        result.source_node_id = response.source_node_id;
        result.source_state = response.source_state;
        result.target_state = response.target_state;
        result.expected_checksum = response.expected_checksum;
        result.observed_checksum = response.observed_checksum;
        result.expected_size = response.expected_size;
        result.observed_size = response.observed_size;
        result.source_checksum_verified = response.source_checksum_verified;
        result.source_unavailable = response.source_unavailable;
        result.target_durable = response.target_durable;
        result.already_exists = response.already_exists;
        result.repaired = response.repaired;
        result.retryable = response.retryable;
        return result;
    }

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

    void TamperChunkOrThrow(storedemo::LocalDiskChunkStore &store,
                            const storedemo::ChunkId &chunk_id,
                            const std::string_view payload)
    {
        WriteBinaryFileOrThrow(
            ResolveFinalPathOrThrow(store.paths().data_root, chunk_id),
            payload);
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
        summary += "healthy=" + std::to_string(candidate.healthy_replica_count) + ";";
        summary += "required=" + std::to_string(candidate.required_replica_count) + ";";
        summary += "missing=" + std::to_string(candidate.missing_replica_count) + ";";
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
        summary += "healthy=" + std::to_string(candidate.healthy_replica_count) + ";";
        summary += "required=" + std::to_string(candidate.required_replica_count) + ";";
        summary += "missing=" + std::to_string(candidate.missing_replica_count) + ";";
        summary += candidate.under_replicated ? "|under" : "|full";
        summary += candidate.lost_or_unrecoverable ? "|lost" : "|repairable";
        return summary;
    }

    ScrubRepairCandidate MakeRepairCandidate(
        const ScrubManifest &manifest,
        std::vector<storedemo::StorageNodeId> healthy_source_replicas,
        const bool under_replicated = true)
    {
        const auto healthy_replica_count = healthy_source_replicas.size();
        return ScrubRepairCandidate{
            .chunk_id = manifest.identity.chunk_id,
            .expected_size = manifest.expected_size,
            .expected_checksum = manifest.expected_checksum,
            .bad_replicas = {},
            .healthy_source_replicas = std::move(healthy_source_replicas),
            .healthy_replica_count = healthy_replica_count,
            .required_replica_count = manifest.desired_replica_count,
            .missing_replica_count =
                manifest.desired_replica_count > healthy_replica_count
                    ? manifest.desired_replica_count - healthy_replica_count
                    : 0U,
            .under_replicated = under_replicated,
            .lost_or_unrecoverable = false};
    }

    storedemo::ScrubRepairCandidate MakeManagerRepairCandidate(
        const ScrubManifest &manifest,
        std::vector<storedemo::StorageNodeId> healthy_source_replicas,
        std::vector<storedemo::StorageNodeId> bad_replicas = {},
        const bool under_replicated = false,
        const bool lost_or_unrecoverable = false)
    {
        const auto healthy_replica_count = healthy_source_replicas.size();
        return storedemo::ScrubRepairCandidate{
            .chunk_id = manifest.identity.chunk_id,
            .expected_size = manifest.expected_size,
            .expected_checksum = manifest.expected_checksum,
            .bad_replicas = std::move(bad_replicas),
            .healthy_source_replicas = std::move(healthy_source_replicas),
            .healthy_replica_count = healthy_replica_count,
            .required_replica_count = manifest.desired_replica_count,
            .missing_replica_count =
                manifest.desired_replica_count > healthy_replica_count
                    ? manifest.desired_replica_count - healthy_replica_count
                    : 0U,
            .under_replicated = under_replicated,
            .lost_or_unrecoverable = lost_or_unrecoverable};
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

        storedemo::RepairTaskRequest MakeRepairTaskRequest(
            const ScrubManifest &manifest,
            const storedemo::ScrubRepairCandidate &candidate,
            const std::uint64_t timeout_ms = 0) const
        {
            storedemo::RepairTaskRequest request;
            request.manifest.identity = manifest.identity;
            request.manifest.expected_size = manifest.expected_size;
            request.manifest.expected_checksum = manifest.expected_checksum;
            request.manifest.replica_nodes = manifest.replica_nodes;
            request.manifest.desired_replica_count = manifest.desired_replica_count;
            request.repair_candidate = candidate;
            request.context.timeout_ms = timeout_ms;
            return request;
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

    TEST_F(StorageScrubRepairTest,
           ProductionScrubManagerTracksHealthyReplicaCountForUnderReplicatedFact)
    {
        auto &healthy_a = CreateStore(1);
        auto &healthy_b = CreateStore(2);
        auto &stale_store = CreateStore(4);
        auto &degraded_store = CreateStore(5);

        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);
        RegisterNode(4, 60);
        RegisterNode(5, 100, storedemo::StorageNodeHealth::kDegraded);

        const auto identity =
            MakeIdentityOrThrow("scrub-manager-under-counts", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(52, "scrub-manager-under-counts");
        WriteReplica(healthy_a, identity, payload, "under-counts-healthy-a");
        WriteReplica(healthy_b, identity, payload, "under-counts-healthy-b");
        WriteReplica(stale_store, identity, payload, "under-counts-stale");
        WriteReplica(degraded_store, identity, payload, "under-counts-degraded");

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
        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2),
             storedemo::test::MakeStorageNodeIdFixture(3),
             storedemo::test::MakeStorageNodeIdFixture(4),
             storedemo::test::MakeStorageNodeIdFixture(5)},
            3);
        ASSERT_TRUE(manager.SubmitTask(
                              MakeScrubManagerTask(
                                  "scrub-manager-under-counts-task",
                                  manifest))
                        .accepted());
        ASSERT_TRUE(manager.Drain().drained);

        const auto task = manager.FindTask("scrub-manager-under-counts-task");
        ASSERT_TRUE(task.has_value());
        ASSERT_TRUE(task->result.has_value());
        ASSERT_TRUE(task->result->repair_candidate.has_value());
        const auto &candidate = *task->result->repair_candidate;

        EXPECT_TRUE(candidate.under_replicated);
        EXPECT_FALSE(candidate.lost_or_unrecoverable);
        EXPECT_EQ(candidate.healthy_source_replicas,
                  std::vector<storedemo::StorageNodeId>(
                      {storedemo::test::MakeStorageNodeIdFixture(1),
                       storedemo::test::MakeStorageNodeIdFixture(2)}));
        EXPECT_EQ(candidate.bad_replicas,
                  std::vector<storedemo::StorageNodeId>(
                      {storedemo::test::MakeStorageNodeIdFixture(3)}));
        EXPECT_EQ(candidate.healthy_replica_count, 2U);
        EXPECT_EQ(candidate.required_replica_count, 3U);
        EXPECT_EQ(candidate.missing_replica_count, 1U);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerCreatesUnderReplicatedTaskFromScrubAndRepeatedScanIsIdempotent)
    {
        auto &source_a = CreateStore(1);
        auto &source_b = CreateStore(2);
        auto &target_store = CreateStore(4);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);
        RegisterNode(4, 100);

        const auto identity =
            MakeIdentityOrThrow("repair-manager-under-from-scrub", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(88, "repair-manager-under-from-scrub");
        WriteReplica(source_a, identity, payload, "under-from-scrub-source-a");
        WriteReplica(source_b, identity, payload, "under-from-scrub-source-b");

        auto source_a_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[source_a.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            source_a.config().node_id);
        auto source_b_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[source_b.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            source_b.config().node_id);
        auto target_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[target_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            target_store.config().node_id);
        RunningStorageNodeService source_a_server(source_a_service);
        RunningStorageNodeService source_b_server(source_b_service);
        RunningStorageNodeService target_server(target_service);

        std::map<storedemo::StorageNodeId, std::unique_ptr<storedemo::StorageNodeClient>> clients;
        clients.emplace(source_a.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            source_a_server.channel()));
        clients.emplace(source_b.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            source_b_server.channel()));
        clients.emplace(target_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            target_server.channel()));

        storedemo::ScrubManager scrub_manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 4,
                .now_unix_ms = []()
                {
                    return 110;
                }});
        const auto manifest = MakeManifest(
            identity,
            payload,
            {source_a.config().node_id,
             source_b.config().node_id,
             storedemo::test::MakeStorageNodeIdFixture(3)},
            3);
        ASSERT_TRUE(scrub_manager.SubmitTask(
                                    MakeScrubManagerTask("under-scan-1", manifest))
                        .accepted());
        ASSERT_TRUE(scrub_manager.SubmitTask(
                                    MakeScrubManagerTask("under-scan-2", manifest))
                        .accepted());
        ASSERT_TRUE(scrub_manager.Drain().drained);

        const auto first_scrub = scrub_manager.FindTask("under-scan-1");
        const auto second_scrub = scrub_manager.FindTask("under-scan-2");
        ASSERT_TRUE(first_scrub.has_value());
        ASSERT_TRUE(second_scrub.has_value());
        ASSERT_TRUE(first_scrub->result.has_value());
        ASSERT_TRUE(first_scrub->result->repair_candidate.has_value());
        ASSERT_TRUE(second_scrub->result.has_value());
        ASSERT_TRUE(second_scrub->result->repair_candidate.has_value());
        EXPECT_TRUE(first_scrub->result->repair_candidate->under_replicated);
        EXPECT_EQ(first_scrub->result->repair_candidate->healthy_replica_count, 2U);
        EXPECT_EQ(first_scrub->result->repair_candidate->required_replica_count, 3U);

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 8,
                .default_timeout_ms = 1500,
                .now_unix_ms = []()
                {
                    return 120;
                },
                .source_reader =
                    [&clients](const storedemo::RepairTask &task,
                               const storedemo::StorageTaskContext &context)
                {
                    storedemo::ReadChunkRequest request;
                    request.request_id = task.task_id + "/source-read";
                    request.chunk_id = task.chunk_id;
                    request.expected_checksum = task.expected_checksum;
                    request.verify_checksum = true;
                    return ToRepairSourceReadResult(
                        clients.at(task.source_node)->ReadChunk(request, {.context = context}));
                },
                .target_writer =
                    [&clients](const storedemo::RepairTask &task,
                               const std::string_view repair_payload,
                               const storedemo::StorageTaskContext &context)
                {
                    storedemo::StorageNodeClientRepairChunkRequest request;
                    request.request_id = task.task_id + "/target-repair";
                    request.chunk_id = task.chunk_id;
                    request.object_id = task.identity.object_id;
                    request.version = task.identity.version;
                    request.chunk_index = task.identity.chunk_index;
                    request.offset = task.identity.offset;
                    request.expected_size = task.expected_size;
                    request.expected_checksum = task.expected_checksum;
                    request.source_node_id = task.source_node;
                    request.source_size = task.expected_size;
                    request.source_checksum = task.expected_checksum;
                    request.source_state = storedemo::ChunkState::kLive;
                    request.source_checksum_verified = true;
                    request.payload = std::string(repair_payload);
                    return ToRepairTargetWriteResult(
                        clients.at(task.target_node)->RepairChunk(request, {.context = context}));
                }});

        const auto first_submit = manager.SubmitUnderReplicatedTask(*first_scrub);
        ASSERT_TRUE(first_submit.accepted()) << first_submit.error_detail;
        ASSERT_TRUE(first_submit.task.has_value());
        EXPECT_EQ(first_submit.task->source_node,
                  storedemo::test::MakeStorageNodeIdFixture(1));
        EXPECT_EQ(first_submit.task->target_node,
                  storedemo::test::MakeStorageNodeIdFixture(4));
        EXPECT_EQ(first_submit.task->chunk_id, identity.chunk_id);
        EXPECT_EQ(first_submit.task->expected_size, manifest.expected_size);
        EXPECT_TRUE(
            ChecksumEquals(first_submit.task->expected_checksum, manifest.expected_checksum));

        const auto second_submit = manager.SubmitUnderReplicatedTask(*second_scrub);
        EXPECT_EQ(second_submit.code,
                  storedemo::UnderReplicatedTaskSubmitCode::kAlreadyExists);
        ASSERT_TRUE(second_submit.task.has_value());
        EXPECT_EQ(second_submit.task->task_id, first_submit.task->task_id);

        const auto run_result = manager.RunTask(first_submit.task->task_id);
        ASSERT_EQ(run_result.status, storedemo::StorageNodeStatusCode::kOk)
            << run_result.error_detail;
        ASSERT_TRUE(run_result.task.has_value());
        EXPECT_EQ(run_result.task->state, storedemo::RepairTaskState::kCompleted);
        EXPECT_TRUE(run_result.target_durable);
        EXPECT_TRUE(run_result.repaired);

        const auto target_read = target_store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "under-from-scrub-target-read"));
        ASSERT_EQ(target_read.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_read.payload, payload);

        EXPECT_EQ(first_scrub->manifest.replica_nodes.size(), 3U);
        EXPECT_EQ(first_scrub->manifest.replica_nodes.back(),
                  storedemo::test::MakeStorageNodeIdFixture(3));
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerUnderReplicatedSubmitReportsLostAndNoTarget)
    {
        auto &lost_a = CreateStore(1);
        auto &lost_b = CreateStore(2);
        RegisterNode(1, 100);
        RegisterNode(2, 100);

        const auto lost_identity =
            MakeIdentityOrThrow("repair-manager-under-lost", 1, 0, 0);
        const auto lost_payload =
            storedemo::test::MakeChunkPayload(36, "repair-manager-under-lost");
        WriteReplica(lost_a, lost_identity, lost_payload, "under-lost-a");
        WriteReplica(lost_b, lost_identity, lost_payload, "under-lost-b");
        TamperReplica(lost_a,
                      lost_identity,
                      storedemo::test::MakeChunkPayload(lost_payload.size(), "under-lost-ta"));
        TamperReplica(lost_b,
                      lost_identity,
                      storedemo::test::MakeChunkPayload(lost_payload.size(), "under-lost-tb"));

        storedemo::ScrubManager lost_scrub_manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 4,
                .now_unix_ms = []()
                {
                    return 110;
                }});
        const auto lost_manifest = MakeManifest(
            lost_identity,
            lost_payload,
            {lost_a.config().node_id, lost_b.config().node_id},
            2);
        ASSERT_TRUE(lost_scrub_manager.SubmitTask(
                                         MakeScrubManagerTask("under-lost-scan", lost_manifest))
                        .accepted());
        ASSERT_TRUE(lost_scrub_manager.Drain().drained);

        const auto lost_scrub = lost_scrub_manager.FindTask("under-lost-scan");
        ASSERT_TRUE(lost_scrub.has_value());
        ASSERT_TRUE(lost_scrub->result.has_value());
        ASSERT_TRUE(lost_scrub->result->repair_candidate.has_value());
        EXPECT_TRUE(lost_scrub->result->repair_candidate->lost_or_unrecoverable);

        storedemo::RepairManager lost_manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 8,
                .now_unix_ms = []()
                {
                    return 120;
                }});
        const auto lost_submit = lost_manager.SubmitUnderReplicatedTask(*lost_scrub);
        EXPECT_EQ(lost_submit.code,
                  storedemo::UnderReplicatedTaskSubmitCode::kLostOrUnrecoverable);

        auto &source_store = CreateStore(6);
        RegisterNode(6, 200);
        RegisterNode(7, 200);
        RegisterNode(8, 200, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, 0, true);
        RegisterNode(9, 200, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kHigh);
        RegisterNode(10, 200, storedemo::StorageNodeHealth::kUnavailable);

        const auto no_target_identity =
            MakeIdentityOrThrow("repair-manager-under-no-target", 1, 0, 0);
        const auto no_target_payload =
            storedemo::test::MakeChunkPayload(44, "repair-manager-under-no-target");
        WriteReplica(source_store,
                     no_target_identity,
                     no_target_payload,
                     "under-no-target-source");

        storedemo::ScrubManager no_target_scrub_manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 4,
                .now_unix_ms = []()
                {
                    return 210;
                }});
        const auto no_target_manifest = MakeManifest(
            no_target_identity,
            no_target_payload,
            {source_store.config().node_id,
             storedemo::test::MakeStorageNodeIdFixture(7)},
            3);
        ASSERT_TRUE(no_target_scrub_manager.SubmitTask(
                                              MakeScrubManagerTask("under-no-target-scan",
                                                                   no_target_manifest))
                        .accepted());
        ASSERT_TRUE(no_target_scrub_manager.Drain().drained);

        const auto no_target_scrub =
            no_target_scrub_manager.FindTask("under-no-target-scan");
        ASSERT_TRUE(no_target_scrub.has_value());
        ASSERT_TRUE(no_target_scrub->result.has_value());
        ASSERT_TRUE(no_target_scrub->result->repair_candidate.has_value());
        EXPECT_TRUE(no_target_scrub->result->repair_candidate->under_replicated);
        EXPECT_EQ(no_target_scrub->result->repair_candidate->healthy_source_replicas,
                  std::vector<storedemo::StorageNodeId>{source_store.config().node_id});

        storedemo::RepairManager no_target_manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 8,
                .now_unix_ms = []()
                {
                    return 210;
                }});
        const auto no_target_submit =
            no_target_manager.SubmitUnderReplicatedTask(*no_target_scrub);
        EXPECT_EQ(no_target_submit.code,
                  storedemo::UnderReplicatedTaskSubmitCode::kNoHealthyTarget);
        EXPECT_NE(no_target_submit.error_detail.find(no_target_identity.chunk_id),
                  std::string::npos);
        EXPECT_NE(no_target_submit.error_detail.find(source_store.config().node_id),
                  std::string::npos);
        EXPECT_NE(no_target_submit.error_detail.find(
                      storedemo::test::MakeStorageNodeIdFixture(7)),
                  std::string::npos);
        EXPECT_NE(no_target_submit.error_detail.find(
                      storedemo::test::MakeStorageNodeIdFixture(8) +
                      ":node write admission is overloaded"),
                  std::string::npos);
        EXPECT_NE(no_target_submit.error_detail.find(
                      storedemo::test::MakeStorageNodeIdFixture(9) +
                      ":node disk pressure is too high: High"),
                  std::string::npos);
        EXPECT_NE(no_target_submit.error_detail.find(
                      storedemo::test::MakeStorageNodeIdFixture(10) +
                      ":node health is not writable: Unavailable"),
                  std::string::npos);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerUnderReplicatedTaskTreatsExistingTargetAsIdempotent)
    {
        auto &source_store = CreateStore(1);
        auto &target_store = CreateStore(4);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(4, 100);

        const auto identity =
            MakeIdentityOrThrow("repair-manager-under-idempotent", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(60, "repair-manager-under-idempotent");
        WriteReplica(source_store, identity, payload, "under-idempotent-source");
        WriteReplica(target_store, identity, payload, "under-idempotent-target");

        auto source_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[source_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            source_store.config().node_id);
        auto target_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[target_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            target_store.config().node_id);
        RunningStorageNodeService source_server(source_service);
        RunningStorageNodeService target_server(target_service);

        std::map<storedemo::StorageNodeId, std::unique_ptr<storedemo::StorageNodeClient>> clients;
        clients.emplace(source_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            source_server.channel()));
        clients.emplace(target_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            target_server.channel()));

        storedemo::ScrubManager scrub_manager(
            RawChunkStoreMap(),
            &registry_,
            storedemo::ScrubManagerConfig{
                .worker_count = 1,
                .queue_capacity = 4,
                .now_unix_ms = []()
                {
                    return 110;
                }});
        const auto manifest = MakeManifest(
            identity,
            payload,
            {source_store.config().node_id,
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2);
        ASSERT_TRUE(scrub_manager.SubmitTask(
                                    MakeScrubManagerTask("under-idempotent-scan", manifest))
                        .accepted());
        ASSERT_TRUE(scrub_manager.Drain().drained);

        const auto scrub_task = scrub_manager.FindTask("under-idempotent-scan");
        ASSERT_TRUE(scrub_task.has_value());
        ASSERT_TRUE(scrub_task->result.has_value());
        ASSERT_TRUE(scrub_task->result->repair_candidate.has_value());
        EXPECT_TRUE(scrub_task->result->repair_candidate->under_replicated);

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 8,
                .default_timeout_ms = 1500,
                .now_unix_ms = []()
                {
                    return 120;
                },
                .source_reader =
                    [&clients](const storedemo::RepairTask &task,
                               const storedemo::StorageTaskContext &context)
                {
                    storedemo::ReadChunkRequest request;
                    request.request_id = task.task_id + "/source-read";
                    request.chunk_id = task.chunk_id;
                    request.expected_checksum = task.expected_checksum;
                    request.verify_checksum = true;
                    return ToRepairSourceReadResult(
                        clients.at(task.source_node)->ReadChunk(request, {.context = context}));
                },
                .target_writer =
                    [&clients](const storedemo::RepairTask &task,
                               const std::string_view repair_payload,
                               const storedemo::StorageTaskContext &context)
                {
                    storedemo::StorageNodeClientRepairChunkRequest request;
                    request.request_id = task.task_id + "/target-repair";
                    request.chunk_id = task.chunk_id;
                    request.object_id = task.identity.object_id;
                    request.version = task.identity.version;
                    request.chunk_index = task.identity.chunk_index;
                    request.offset = task.identity.offset;
                    request.expected_size = task.expected_size;
                    request.expected_checksum = task.expected_checksum;
                    request.source_node_id = task.source_node;
                    request.source_size = task.expected_size;
                    request.source_checksum = task.expected_checksum;
                    request.source_state = storedemo::ChunkState::kLive;
                    request.source_checksum_verified = true;
                    request.payload = std::string(repair_payload);
                    return ToRepairTargetWriteResult(
                        clients.at(task.target_node)->RepairChunk(request, {.context = context}));
                }});

        const auto submit_result = manager.SubmitUnderReplicatedTask(*scrub_task);
        ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;
        ASSERT_TRUE(submit_result.task.has_value());
        EXPECT_EQ(submit_result.task->target_node, target_store.config().node_id);

        const auto run_result = manager.RunTask(submit_result.task->task_id);
        EXPECT_EQ(run_result.status, storedemo::StorageNodeStatusCode::kOk);
        ASSERT_TRUE(run_result.task.has_value());
        EXPECT_EQ(run_result.task->state, storedemo::RepairTaskState::kCompleted);
        EXPECT_TRUE(run_result.already_exists);
        EXPECT_TRUE(run_result.target_durable);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerCreatesTaskFromCandidateAndRecordsPlan)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("repair-manager-create", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(48, "repair-manager-create");
        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            3);
        const auto candidate = MakeManagerRepairCandidate(
            manifest,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            {storedemo::test::MakeStorageNodeIdFixture(4)},
            true,
            false);

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 8,
                .now_unix_ms = []()
                {
                    return 110;
                }});
        const auto submit_result =
            manager.SubmitTask(MakeRepairTaskRequest(manifest, candidate, 10));
        ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;
        ASSERT_TRUE(submit_result.task.has_value());

        const auto task = manager.FindTask(submit_result.task->task_id);
        ASSERT_TRUE(task.has_value());
        EXPECT_EQ(task->source_node, storedemo::test::MakeStorageNodeIdFixture(1));
        EXPECT_EQ(task->target_node, storedemo::test::MakeStorageNodeIdFixture(3));
        EXPECT_EQ(task->chunk_id, identity.chunk_id);
        EXPECT_TRUE(ChecksumEquals(task->expected_checksum, manifest.expected_checksum));
        EXPECT_EQ(task->expected_size, manifest.expected_size);
        EXPECT_EQ(task->state, storedemo::RepairTaskState::kQueued);
        EXPECT_EQ(task->progress_percent, 0U);
        EXPECT_EQ(task->attempts, 0U);
        EXPECT_EQ(task->last_error, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(task->last_error_detail.empty());
        EXPECT_EQ(task->existing_replica_nodes, manifest.replica_nodes);
        EXPECT_EQ(task->healthy_source_replicas,
                  std::vector<storedemo::StorageNodeId>(
                      {storedemo::test::MakeStorageNodeIdFixture(1),
                       storedemo::test::MakeStorageNodeIdFixture(2)}));
        EXPECT_EQ(task->bad_replicas,
                  std::vector<storedemo::StorageNodeId>(
                      {storedemo::test::MakeStorageNodeIdFixture(4)}));
        EXPECT_NE(task->target_node, task->source_node);
        EXPECT_EQ(std::find(task->existing_replica_nodes.begin(),
                            task->existing_replica_nodes.end(),
                            task->target_node),
                  task->existing_replica_nodes.end());
        EXPECT_EQ(task->replacement_decision.chunk_id, identity.chunk_id);
        EXPECT_EQ(task->replacement_decision.decision_epoch, 110U);
        ASSERT_EQ(task->replacement_decision.replica_nodes.size(), 1U);
        EXPECT_EQ(task->replacement_decision.replica_nodes.front().node_id,
                  task->target_node);
        EXPECT_FALSE(task->excluded_nodes.empty());

        EXPECT_EQ(target_store.StatChunk(
                      MakeStatRequest(identity.chunk_id, "repair-manager-create-target"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);

        (void)source_store;
        (void)peer_store;
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerPlanningDoesNotExecuteRepairIoDuringSubmit)
    {
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity =
            MakeIdentityOrThrow("repair-manager-plan-only", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(40, "repair-manager-plan-only");
        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            3);
        const auto candidate = MakeManagerRepairCandidate(
            manifest,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            {storedemo::test::MakeStorageNodeIdFixture(4)},
            true,
            false);

        std::size_t source_reads = 0;
        std::size_t target_writes = 0;
        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 8,
                .now_unix_ms = []()
                {
                    return 110;
                },
                .source_reader =
                    [&source_reads](const storedemo::RepairTask &,
                                    const storedemo::StorageTaskContext &)
                {
                    ++source_reads;
                    return storedemo::RepairSourceReadResult{};
                },
                .target_writer =
                    [&target_writes](const storedemo::RepairTask &,
                                     const std::string_view,
                                     const storedemo::StorageTaskContext &)
                {
                    ++target_writes;
                    return storedemo::RepairTargetWriteResult{};
                }});

        const auto submit_result =
            manager.SubmitTask(MakeRepairTaskRequest(manifest, candidate, 10));
        ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;
        ASSERT_TRUE(submit_result.task.has_value());
        EXPECT_EQ(source_reads, 0U);
        EXPECT_EQ(target_writes, 0U);
        EXPECT_EQ(submit_result.task->state, storedemo::RepairTaskState::kQueued);
        EXPECT_EQ(submit_result.task->healthy_source_replicas,
                  std::vector<storedemo::StorageNodeId>(
                      {storedemo::test::MakeStorageNodeIdFixture(1),
                       storedemo::test::MakeStorageNodeIdFixture(2)}));
        ASSERT_EQ(submit_result.task->replacement_decision.replica_nodes.size(), 1U);
        EXPECT_EQ(submit_result.task->replacement_decision.replica_nodes.front().node_id,
                  storedemo::test::MakeStorageNodeIdFixture(3));
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerReplacementPlanningDoesNotForceOriginalReplicaBack)
    {
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);
        RegisterNode(4,
                     100,
                     storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kHigh);

        const auto identity =
            MakeIdentityOrThrow("repair-manager-no-force-back", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(40, "repair-manager-no-force-back");
        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2),
             storedemo::test::MakeStorageNodeIdFixture(4)},
            3);
        const auto candidate = MakeManagerRepairCandidate(
            manifest,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            {storedemo::test::MakeStorageNodeIdFixture(4)},
            true,
            false);

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 8,
                .now_unix_ms = []()
                {
                    return 110;
                }});

        const auto submit_result =
            manager.SubmitTask(MakeRepairTaskRequest(manifest, candidate, 10));
        ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;
        ASSERT_TRUE(submit_result.task.has_value());
        EXPECT_EQ(submit_result.task->target_node,
                  storedemo::test::MakeStorageNodeIdFixture(3));
        EXPECT_NE(submit_result.task->target_node,
                  storedemo::test::MakeStorageNodeIdFixture(4));
        EXPECT_EQ(submit_result.task->healthy_source_replicas,
                  std::vector<storedemo::StorageNodeId>(
                      {storedemo::test::MakeStorageNodeIdFixture(1),
                       storedemo::test::MakeStorageNodeIdFixture(2)}));
        EXPECT_EQ(std::find(submit_result.task->existing_replica_nodes.begin(),
                            submit_result.task->existing_replica_nodes.end(),
                            submit_result.task->target_node),
                  submit_result.task->existing_replica_nodes.end());
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerRejectsManifestExternalHealthySourceAuthorityLeak)
    {
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity =
            MakeIdentityOrThrow("repair-manager-source-authority", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(40, "repair-manager-source-authority");
        const auto manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            3);
        const auto candidate = MakeManagerRepairCandidate(
            manifest,
            {storedemo::test::MakeStorageNodeIdFixture(3)},
            {storedemo::test::MakeStorageNodeIdFixture(2)},
            true,
            false);

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 8,
                .now_unix_ms = []()
                {
                    return 110;
                }});

        const auto submit_result =
            manager.SubmitTask(MakeRepairTaskRequest(manifest, candidate, 10));
        EXPECT_EQ(submit_result.code,
                  storedemo::RepairManagerSubmitCode::kInvalidArgument);
        EXPECT_NE(submit_result.error_detail.find(
                      "healthy repair source is not in committed manifest replicas"),
                  std::string::npos);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerRejectsDuplicateAndReturnsOverloadedForQueueAndCapacity)
    {
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);
        RegisterNode(4, 100);

        const auto first_identity = MakeIdentityOrThrow("repair-manager-dupe-a", 1, 0, 0);
        const auto second_identity = MakeIdentityOrThrow("repair-manager-dupe-b", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(32, "repair-manager-dupe");

        const auto first_manifest = MakeManifest(
            first_identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1)},
            2);
        const auto second_manifest = MakeManifest(
            second_identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(2)},
            2);

        storedemo::RepairManager queue_manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 1,
                .max_tasks = 2,
                .now_unix_ms = []()
                {
                    return 110;
                }});
        const auto first_submit = queue_manager.SubmitTask(
            MakeRepairTaskRequest(
                first_manifest,
                MakeManagerRepairCandidate(
                    first_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(1)})));
        ASSERT_TRUE(first_submit.accepted()) << first_submit.error_detail;

        const auto duplicate_submit = queue_manager.SubmitTask(
            MakeRepairTaskRequest(
                first_manifest,
                MakeManagerRepairCandidate(
                    first_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(1)})));
        EXPECT_EQ(duplicate_submit.code,
                  storedemo::RepairManagerSubmitCode::kAlreadyExists);

        const auto queue_full_submit = queue_manager.SubmitTask(
            MakeRepairTaskRequest(
                second_manifest,
                MakeManagerRepairCandidate(
                    second_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(2)})));
        EXPECT_EQ(queue_full_submit.code,
                  storedemo::RepairManagerSubmitCode::kOverloaded);

        storedemo::RepairManager capacity_manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 1,
                .max_tasks = 1,
                .now_unix_ms = []()
                {
                    return 110;
                }});
        const auto capacity_first = capacity_manager.SubmitTask(
            MakeRepairTaskRequest(
                first_manifest,
                MakeManagerRepairCandidate(
                    first_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(1)})));
        ASSERT_TRUE(capacity_first.accepted()) << capacity_first.error_detail;
        ASSERT_TRUE(capacity_manager.MarkTaskRunning(
                                      capacity_first.task->task_id)
                        .ok());
        ASSERT_TRUE(capacity_manager.CompleteTask(
                                      capacity_first.task->task_id)
                        .ok());

        const auto capacity_full_submit = capacity_manager.SubmitTask(
            MakeRepairTaskRequest(
                second_manifest,
                MakeManagerRepairCandidate(
                    second_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(2)})));
        EXPECT_EQ(capacity_full_submit.code,
                  storedemo::RepairManagerSubmitCode::kOverloaded);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerRejectsInvalidSourceTargetAndMissingFacts)
    {
        RegisterNode(1, 60);
        RegisterNode(2, 100, storedemo::StorageNodeHealth::kUnavailable);
        RegisterNode(3, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kFull);
        RegisterNode(4, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, 0, true);
        RegisterNode(5, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, 0, false, 1024, 900);
        RegisterNode(6, 100);

        const auto payload =
            storedemo::test::MakeChunkPayload(2048, "repair-manager-invalid");
        const auto invalid_source_identity =
            MakeIdentityOrThrow("repair-manager-invalid-source", 1, 0, 0);
        const auto invalid_target_identity =
            MakeIdentityOrThrow("repair-manager-invalid-target", 1, 0, 0);

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 8,
                .now_unix_ms = []()
                {
                    return 110;
                }});

        const auto invalid_source_manifest = MakeManifest(
            invalid_source_identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1)},
            2);
        const auto invalid_source = manager.SubmitTask(
            MakeRepairTaskRequest(
                invalid_source_manifest,
                MakeManagerRepairCandidate(
                    invalid_source_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(1)})));
        EXPECT_EQ(invalid_source.code,
                  storedemo::RepairManagerSubmitCode::kInvalidArgument);

        const auto invalid_target_manifest = MakeManifest(
            invalid_target_identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(6)},
            2);
        const auto invalid_target = manager.SubmitTask(
            MakeRepairTaskRequest(
                invalid_target_manifest,
                MakeManagerRepairCandidate(
                    invalid_target_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(6)})));
        EXPECT_EQ(invalid_target.code,
                  storedemo::RepairManagerSubmitCode::kInvalidArgument);

        auto missing_checksum_manifest = invalid_target_manifest;
        missing_checksum_manifest.expected_checksum = {};
        auto missing_checksum_candidate =
            MakeManagerRepairCandidate(invalid_target_manifest,
                                       {storedemo::test::MakeStorageNodeIdFixture(6)});
        missing_checksum_candidate.expected_checksum = {};
        const auto missing_checksum = manager.SubmitTask(
            MakeRepairTaskRequest(missing_checksum_manifest, missing_checksum_candidate));
        EXPECT_EQ(missing_checksum.code,
                  storedemo::RepairManagerSubmitCode::kInvalidArgument);

        auto missing_size_manifest = invalid_target_manifest;
        missing_size_manifest.expected_size = 0;
        auto missing_size_candidate =
            MakeManagerRepairCandidate(invalid_target_manifest,
                                       {storedemo::test::MakeStorageNodeIdFixture(6)});
        missing_size_candidate.expected_size = 0;
        const auto missing_size = manager.SubmitTask(
            MakeRepairTaskRequest(missing_size_manifest, missing_size_candidate));
        EXPECT_EQ(missing_size.code,
                  storedemo::RepairManagerSubmitCode::kInvalidArgument);

        storedemo::RepairTaskRequest missing_chunk_request;
        missing_chunk_request.repair_candidate.expected_checksum =
            ComputeChecksumOrThrow(payload);
        missing_chunk_request.repair_candidate.expected_size =
            payload.size();
        missing_chunk_request.repair_candidate.healthy_source_replicas = {
            storedemo::test::MakeStorageNodeIdFixture(2)};
        const auto missing_chunk = manager.SubmitTask(missing_chunk_request);
        EXPECT_EQ(missing_chunk.code,
                  storedemo::RepairManagerSubmitCode::kInvalidArgument);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerCancelRetryFailCompleteAndListSemanticsAreStable)
    {
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);
        RegisterNode(4, 100);

        const auto payload =
            storedemo::test::MakeChunkPayload(40, "repair-manager-lifecycle");
        const auto queued_identity =
            MakeIdentityOrThrow("repair-manager-queued", 1, 0, 0);
        const auto running_identity =
            MakeIdentityOrThrow("repair-manager-running", 1, 0, 0);
        const auto completed_identity =
            MakeIdentityOrThrow("repair-manager-completed", 1, 0, 0);
        const auto failed_identity =
            MakeIdentityOrThrow("repair-manager-failed", 1, 0, 0);

        const auto queued_manifest = MakeManifest(
            queued_identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            3);
        const auto running_manifest = MakeManifest(
            running_identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            3);
        const auto completed_manifest = MakeManifest(
            completed_identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            3);
        const auto failed_manifest = MakeManifest(
            failed_identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            3);

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 8,
                .max_tasks = 8,
                .now_unix_ms = []()
                {
                    return 110;
                }});

        const auto queued_submit = manager.SubmitTask(
            MakeRepairTaskRequest(
                queued_manifest,
                MakeManagerRepairCandidate(
                    queued_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(1)})));
        const auto running_submit = manager.SubmitTask(
            MakeRepairTaskRequest(
                running_manifest,
                MakeManagerRepairCandidate(
                    running_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(1)})));
        const auto completed_submit = manager.SubmitTask(
            MakeRepairTaskRequest(
                completed_manifest,
                MakeManagerRepairCandidate(
                    completed_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(1)})));
        const auto failed_submit = manager.SubmitTask(
            MakeRepairTaskRequest(
                failed_manifest,
                MakeManagerRepairCandidate(
                    failed_manifest,
                    {storedemo::test::MakeStorageNodeIdFixture(1)})));

        ASSERT_TRUE(queued_submit.accepted());
        ASSERT_TRUE(running_submit.accepted());
        ASSERT_TRUE(completed_submit.accepted());
        ASSERT_TRUE(failed_submit.accepted());

        const auto cancel_queued =
            manager.CancelTask(queued_submit.task->task_id);
        ASSERT_TRUE(cancel_queued.ok());
        EXPECT_EQ(cancel_queued.task->state, storedemo::RepairTaskState::kCancelled);

        ASSERT_TRUE(manager.MarkTaskRunning(running_submit.task->task_id).ok());
        const auto cancel_running =
            manager.CancelTask(running_submit.task->task_id);
        EXPECT_EQ(cancel_running.code, storedemo::RepairTaskOperationCode::kConflict);

        ASSERT_TRUE(manager.MarkTaskRunning(completed_submit.task->task_id).ok());
        ASSERT_TRUE(manager.UpdateTaskProgress(completed_submit.task->task_id, 55).ok());
        const auto complete_result =
            manager.CompleteTask(completed_submit.task->task_id);
        ASSERT_TRUE(complete_result.ok());
        EXPECT_EQ(complete_result.task->state, storedemo::RepairTaskState::kCompleted);
        EXPECT_EQ(complete_result.task->progress_percent, 100U);
        EXPECT_EQ(target_store.StatChunk(
                      MakeStatRequest(completed_identity.chunk_id,
                                      "repair-manager-complete-target"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
        const auto cancel_completed =
            manager.CancelTask(completed_submit.task->task_id);
        EXPECT_EQ(cancel_completed.code, storedemo::RepairTaskOperationCode::kConflict);

        ASSERT_TRUE(manager.MarkTaskRunning(failed_submit.task->task_id).ok());
        const auto fail_result = manager.FailTask(
            failed_submit.task->task_id,
            storedemo::StorageNodeStatusCode::kTimeout,
            "repair task timed out",
            false,
            0);
        ASSERT_TRUE(fail_result.ok());
        EXPECT_EQ(fail_result.task->state, storedemo::RepairTaskState::kFailed);
        EXPECT_EQ(fail_result.task->last_error,
                  storedemo::StorageNodeStatusCode::kTimeout);
        EXPECT_EQ(fail_result.task->last_error_detail, "repair task timed out");
        const auto cancel_failed =
            manager.CancelTask(failed_submit.task->task_id);
        EXPECT_EQ(cancel_failed.code, storedemo::RepairTaskOperationCode::kConflict);

        const auto retry_result =
            manager.RetryTask(failed_submit.task->task_id);
        ASSERT_TRUE(retry_result.ok());
        EXPECT_EQ(retry_result.task->state, storedemo::RepairTaskState::kRetryPending);
        EXPECT_EQ(retry_result.task->progress_percent, 0U);
        const auto retry_task = manager.FindTask(failed_submit.task->task_id);
        ASSERT_TRUE(retry_task.has_value());
        EXPECT_EQ(retry_task->attempts, 1U);

        const auto rerun_result =
            manager.MarkTaskRunning(failed_submit.task->task_id);
        ASSERT_TRUE(rerun_result.ok());
        EXPECT_EQ(rerun_result.task->attempts, 2U);

        const auto listed_tasks = manager.ListTasks();
        ASSERT_EQ(listed_tasks.size(), 4U);
        std::vector<std::string> task_ids;
        for (const auto &task : listed_tasks)
        {
            task_ids.push_back(task.task_id);
        }
        auto sorted_task_ids = task_ids;
        std::sort(sorted_task_ids.begin(), sorted_task_ids.end());
        EXPECT_EQ(task_ids, sorted_task_ids);

        const auto stats = manager.SnapshotStats();
        EXPECT_EQ(stats.cancelled_tasks, 1U);
        EXPECT_EQ(stats.completed_tasks, 1U);
        EXPECT_EQ(stats.running_tasks, 2U);
        EXPECT_EQ(stats.total_attempts, 4U);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerRunsRepairCopyFlowAndCompletesAfterTargetDurable)
    {
        auto &source_store = CreateStore(1);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(3, 100);

        const auto payload =
            storedemo::test::MakeChunkPayload(128, "repair-manager-copy-success");
        const auto identity = MakeIdentityOrThrow("repair-manager-copy-success", 1, 0, 0);
        ASSERT_EQ(source_store.WriteChunk(
                      MakeWriteRequest(identity, payload, "repair-manager-source-write"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto source_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[source_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            source_store.config().node_id);
        auto target_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[target_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            target_store.config().node_id);
        RunningStorageNodeService source_server(source_service);
        RunningStorageNodeService target_server(target_service);

        std::map<storedemo::StorageNodeId, std::unique_ptr<storedemo::StorageNodeClient>> clients;
        clients.emplace(source_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            source_server.channel()));
        clients.emplace(target_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            target_server.channel()));

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 4,
                .default_timeout_ms = 1500,
                .now_unix_ms = []()
                {
                    return 120;
                },
                .source_reader =
                    [&clients](const storedemo::RepairTask &task,
                               const storedemo::StorageTaskContext &context)
                {
                    const auto client_it = clients.find(task.source_node);
                    if (client_it == clients.end())
                    {
                        storedemo::RepairSourceReadResult result;
                        result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                        result.error_detail = "repair source client is unavailable";
                        return result;
                    }

                    storedemo::ReadChunkRequest request;
                    request.request_id = task.task_id + "/source-read";
                    request.chunk_id = task.chunk_id;
                    request.expected_checksum = task.expected_checksum;
                    request.verify_checksum = true;
                    return ToRepairSourceReadResult(
                        client_it->second->ReadChunk(request, {.context = context}));
                },
                .target_writer =
                    [&clients](const storedemo::RepairTask &task,
                               const std::string_view repair_payload,
                               const storedemo::StorageTaskContext &context)
                {
                    const auto client_it = clients.find(task.target_node);
                    if (client_it == clients.end())
                    {
                        storedemo::RepairTargetWriteResult result;
                        result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                        result.error_detail = "repair target client is unavailable";
                        return result;
                    }

                    storedemo::StorageNodeClientRepairChunkRequest request;
                    request.request_id = task.task_id + "/target-repair";
                    request.chunk_id = task.chunk_id;
                    request.object_id = task.identity.object_id;
                    request.version = task.identity.version;
                    request.chunk_index = task.identity.chunk_index;
                    request.offset = task.identity.offset;
                    request.expected_size = task.expected_size;
                    request.expected_checksum = task.expected_checksum;
                    request.source_node_id = task.source_node;
                    request.source_size = task.expected_size;
                    request.source_checksum = task.expected_checksum;
                    request.source_state = storedemo::ChunkState::kLive;
                    request.source_checksum_verified = true;
                    request.payload = std::string(repair_payload);
                    return ToRepairTargetWriteResult(
                        client_it->second->RepairChunk(request, {.context = context}));
                }});

        const auto manifest =
            MakeManifest(identity, payload, {source_store.config().node_id}, 2);
        const auto submit_result = manager.SubmitTask(
            MakeRepairTaskRequest(
                manifest,
                MakeManagerRepairCandidate(manifest,
                                           {source_store.config().node_id},
                                           {},
                                           true)));
        ASSERT_TRUE(submit_result.accepted());

        const auto run_result = manager.RunTask(submit_result.task->task_id);
        ASSERT_EQ(run_result.status, storedemo::StorageNodeStatusCode::kOk)
            << run_result.error_detail;
        ASSERT_TRUE(run_result.task.has_value());
        EXPECT_EQ(run_result.task->state, storedemo::RepairTaskState::kCompleted);
        EXPECT_EQ(run_result.task->progress_percent, 100U);
        EXPECT_TRUE(run_result.target_durable);
        EXPECT_TRUE(run_result.repaired);
        EXPECT_FALSE(run_result.already_exists);
        EXPECT_EQ(run_result.target_checksum.value,
                  ComputeChecksumOrThrow(payload).value);

        const auto target_read = target_store.ReadChunk(
            MakeReadRequest(identity.chunk_id, "repair-manager-target-read"));
        ASSERT_EQ(target_read.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_read.payload, payload);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerRejectsCorruptedSourceAndKeepsTargetMissing)
    {
        auto &source_store = CreateStore(1);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(3, 100);

        const auto payload =
            storedemo::test::MakeChunkPayload(96, "repair-manager-source-corrupted");
        const auto identity =
            MakeIdentityOrThrow("repair-manager-source-corrupted", 1, 0, 0);
        ASSERT_EQ(source_store.WriteChunk(
                      MakeWriteRequest(identity, payload, "repair-corrupted-source-write"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_NO_THROW(TamperChunkOrThrow(
            source_store,
            identity.chunk_id,
            storedemo::test::MakeChunkPayload(payload.size(), "repair-corrupted-tamper")));

        auto source_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[source_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            source_store.config().node_id);
        auto target_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[target_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            target_store.config().node_id);
        RunningStorageNodeService source_server(source_service);
        RunningStorageNodeService target_server(target_service);

        std::map<storedemo::StorageNodeId, std::unique_ptr<storedemo::StorageNodeClient>> clients;
        clients.emplace(source_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            source_server.channel()));
        clients.emplace(target_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            target_server.channel()));

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 4,
                .default_timeout_ms = 1500,
                .now_unix_ms = []()
                {
                    return 120;
                },
                .source_reader =
                    [&clients](const storedemo::RepairTask &task,
                               const storedemo::StorageTaskContext &context)
                {
                    storedemo::ReadChunkRequest request;
                    request.request_id = task.task_id + "/source-read";
                    request.chunk_id = task.chunk_id;
                    request.expected_checksum = task.expected_checksum;
                    request.verify_checksum = true;
                    return ToRepairSourceReadResult(
                        clients.at(task.source_node)->ReadChunk(request, {.context = context}));
                },
                .target_writer =
                    [&clients](const storedemo::RepairTask &task,
                               const std::string_view repair_payload,
                               const storedemo::StorageTaskContext &context)
                {
                    storedemo::StorageNodeClientRepairChunkRequest request;
                    request.request_id = task.task_id + "/target-repair";
                    request.chunk_id = task.chunk_id;
                    request.object_id = task.identity.object_id;
                    request.version = task.identity.version;
                    request.chunk_index = task.identity.chunk_index;
                    request.offset = task.identity.offset;
                    request.expected_size = task.expected_size;
                    request.expected_checksum = task.expected_checksum;
                    request.source_node_id = task.source_node;
                    request.source_size = task.expected_size;
                    request.source_checksum = task.expected_checksum;
                    request.source_state = storedemo::ChunkState::kLive;
                    request.source_checksum_verified = true;
                    request.payload = std::string(repair_payload);
                    return ToRepairTargetWriteResult(
                        clients.at(task.target_node)->RepairChunk(request, {.context = context}));
                }});

        const auto manifest =
            MakeManifest(identity, payload, {source_store.config().node_id}, 2);
        const auto submit_result = manager.SubmitTask(
            MakeRepairTaskRequest(
                manifest,
                MakeManagerRepairCandidate(manifest,
                                           {source_store.config().node_id},
                                           {},
                                           true)));
        ASSERT_TRUE(submit_result.accepted());

        const auto run_result = manager.RunTask(submit_result.task->task_id);
        EXPECT_EQ(run_result.status, storedemo::StorageNodeStatusCode::kCorrupted);
        ASSERT_TRUE(run_result.task.has_value());
        EXPECT_EQ(run_result.task->state, storedemo::RepairTaskState::kFailed);
        EXPECT_EQ(run_result.task->last_error,
                  storedemo::StorageNodeStatusCode::kCorrupted);
        EXPECT_EQ(target_store.StatChunk(
                      MakeStatRequest(identity.chunk_id, "repair-corrupted-target-stat"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerKeepsTaskPendingWhenTargetDurableWriteFails)
    {
        auto &source_store = CreateStore(1);
        auto writer_state = std::make_shared<RecordingWriterState>();
        auto durable_file = std::make_shared<RecordingDurableFile>(writer_state);
        durable_file->publish_result.error = storedemo::DurableFileErrorCode::kIoError;
        durable_file->publish_result.error_detail = "simulated publish failure";
        auto &target_store = CreateStore(3, durable_file);
        RegisterNode(1, 100);
        RegisterNode(3, 100);

        const auto payload =
            storedemo::test::MakeChunkPayload(88, "repair-manager-target-fail");
        const auto identity =
            MakeIdentityOrThrow("repair-manager-target-fail", 1, 0, 0);
        ASSERT_EQ(source_store.WriteChunk(
                      MakeWriteRequest(identity, payload, "repair-target-fail-source"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto source_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[source_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            source_store.config().node_id);
        auto target_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[target_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            target_store.config().node_id);
        RunningStorageNodeService source_server(source_service);
        RunningStorageNodeService target_server(target_service);

        std::map<storedemo::StorageNodeId, std::unique_ptr<storedemo::StorageNodeClient>> clients;
        clients.emplace(source_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            source_server.channel()));
        clients.emplace(target_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            target_server.channel()));

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 4,
                .default_timeout_ms = 1500,
                .now_unix_ms = []()
                {
                    return 120;
                },
                .source_reader =
                    [&clients](const storedemo::RepairTask &task,
                               const storedemo::StorageTaskContext &context)
                {
                    storedemo::ReadChunkRequest request;
                    request.request_id = task.task_id + "/source-read";
                    request.chunk_id = task.chunk_id;
                    request.expected_checksum = task.expected_checksum;
                    request.verify_checksum = true;
                    return ToRepairSourceReadResult(
                        clients.at(task.source_node)->ReadChunk(request, {.context = context}));
                },
                .target_writer =
                    [&clients](const storedemo::RepairTask &task,
                               const std::string_view repair_payload,
                               const storedemo::StorageTaskContext &context)
                {
                    storedemo::StorageNodeClientRepairChunkRequest request;
                    request.request_id = task.task_id + "/target-repair";
                    request.chunk_id = task.chunk_id;
                    request.object_id = task.identity.object_id;
                    request.version = task.identity.version;
                    request.chunk_index = task.identity.chunk_index;
                    request.offset = task.identity.offset;
                    request.expected_size = task.expected_size;
                    request.expected_checksum = task.expected_checksum;
                    request.source_node_id = task.source_node;
                    request.source_size = task.expected_size;
                    request.source_checksum = task.expected_checksum;
                    request.source_state = storedemo::ChunkState::kLive;
                    request.source_checksum_verified = true;
                    request.payload = std::string(repair_payload);
                    return ToRepairTargetWriteResult(
                        clients.at(task.target_node)->RepairChunk(request, {.context = context}));
                }});

        const auto manifest =
            MakeManifest(identity, payload, {source_store.config().node_id}, 2);
        const auto submit_result = manager.SubmitTask(
            MakeRepairTaskRequest(
                manifest,
                MakeManagerRepairCandidate(manifest,
                                           {source_store.config().node_id},
                                           {},
                                           true)));
        ASSERT_TRUE(submit_result.accepted());

        const auto run_result = manager.RunTask(submit_result.task->task_id);
        EXPECT_EQ(run_result.status, storedemo::StorageNodeStatusCode::kIoError);
        ASSERT_TRUE(run_result.task.has_value());
        EXPECT_EQ(run_result.task->state, storedemo::RepairTaskState::kRetryPending);
        EXPECT_EQ(run_result.task->last_error,
                  storedemo::StorageNodeStatusCode::kIoError);
        EXPECT_FALSE(run_result.target_durable);
        EXPECT_EQ(target_store.StatChunk(
                      MakeStatRequest(identity.chunk_id, "repair-target-fail-stat"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerTreatsExistingTargetAsIdempotentAndRejectsConflict)
    {
        auto &source_store = CreateStore(1);
        auto &idempotent_target_store = CreateStore(3);
        auto &conflict_target_store = CreateStore(4);
        RegisterNode(1, 150);
        RegisterNode(3, 150);
        RegisterNode(4, 150);

        const auto payload =
            storedemo::test::MakeChunkPayload(72, "repair-manager-idempotent");
        const auto different_payload =
            storedemo::test::MakeChunkPayload(72, "repair-manager-conflict");
        const auto idempotent_identity =
            MakeIdentityOrThrow("repair-manager-idempotent", 1, 0, 0);
        const auto conflict_identity =
            MakeIdentityOrThrow("repair-manager-conflict", 1, 0, 0);
        ASSERT_EQ(source_store.WriteChunk(
                      MakeWriteRequest(idempotent_identity, payload, "repair-idempotent-source"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(source_store.WriteChunk(
                      MakeWriteRequest(conflict_identity, payload, "repair-conflict-source"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(idempotent_target_store.WriteChunk(
                      MakeWriteRequest(idempotent_identity, payload, "repair-idempotent-target"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);
        ASSERT_EQ(conflict_target_store.WriteChunk(
                      MakeWriteRequest(conflict_identity,
                                       different_payload,
                                       "repair-conflict-target"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto source_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[source_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            source_store.config().node_id);
        auto idempotent_target_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[idempotent_target_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            idempotent_target_store.config().node_id);
        auto conflict_target_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[conflict_target_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            conflict_target_store.config().node_id);
        RunningStorageNodeService source_server(source_service);
        RunningStorageNodeService idempotent_target_server(idempotent_target_service);
        RunningStorageNodeService conflict_target_server(conflict_target_service);

        std::map<storedemo::StorageNodeId, std::unique_ptr<storedemo::StorageNodeClient>> clients;
        clients.emplace(source_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            source_server.channel()));
        clients.emplace(idempotent_target_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            idempotent_target_server.channel()));
        clients.emplace(conflict_target_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            conflict_target_server.channel()));

        auto make_manager = [&clients, this](const std::uint64_t now_unix_ms)
        {
            return storedemo::RepairManager(
                &registry_,
                storedemo::RepairManagerConfig{
                    .max_active_tasks = 8,
                    .max_tasks = 8,
                    .default_timeout_ms = 1500,
                    .now_unix_ms = [now_unix_ms]()
                    {
                        return now_unix_ms;
                    },
                    .source_reader =
                        [&clients](const storedemo::RepairTask &task,
                                   const storedemo::StorageTaskContext &context)
                    {
                        storedemo::ReadChunkRequest request;
                        request.request_id = task.task_id + "/source-read";
                        request.chunk_id = task.chunk_id;
                        request.expected_checksum = task.expected_checksum;
                        request.verify_checksum = true;
                        return ToRepairSourceReadResult(
                            clients.at(task.source_node)->ReadChunk(
                                request,
                                {.context = context}));
                    },
                    .target_writer =
                        [&clients](const storedemo::RepairTask &task,
                                   const std::string_view repair_payload,
                                   const storedemo::StorageTaskContext &context)
                    {
                        storedemo::StorageNodeClientRepairChunkRequest request;
                        request.request_id = task.task_id + "/target-repair";
                        request.chunk_id = task.chunk_id;
                        request.object_id = task.identity.object_id;
                        request.version = task.identity.version;
                        request.chunk_index = task.identity.chunk_index;
                        request.offset = task.identity.offset;
                        request.expected_size = task.expected_size;
                        request.expected_checksum = task.expected_checksum;
                        request.source_node_id = task.source_node;
                        request.source_size = task.expected_size;
                        request.source_checksum = task.expected_checksum;
                        request.source_state = storedemo::ChunkState::kLive;
                        request.source_checksum_verified = true;
                        request.payload = std::string(repair_payload);
                        return ToRepairTargetWriteResult(
                            clients.at(task.target_node)->RepairChunk(
                                request,
                                {.context = context}));
                    }});
        };

        auto idempotent_manager = make_manager(160);
        const auto idempotent_manifest = MakeManifest(
            idempotent_identity,
            payload,
            {source_store.config().node_id},
            2);
        const auto idempotent_submit = idempotent_manager.SubmitTask(
            MakeRepairTaskRequest(
                idempotent_manifest,
                MakeManagerRepairCandidate(idempotent_manifest,
                                           {source_store.config().node_id},
                                           {},
                                           true)));
        ASSERT_TRUE(idempotent_submit.accepted());
        const auto idempotent_run =
            idempotent_manager.RunTask(idempotent_submit.task->task_id);
        EXPECT_EQ(idempotent_run.status, storedemo::StorageNodeStatusCode::kOk);
        ASSERT_TRUE(idempotent_run.task.has_value());
        EXPECT_EQ(idempotent_run.task->state, storedemo::RepairTaskState::kCompleted);
        EXPECT_TRUE(idempotent_run.already_exists);

        storedemo::ReportLoadRequest steer_conflict_target_request;
        steer_conflict_target_request.node_id = idempotent_target_store.config().node_id;
        steer_conflict_target_request.endpoint = "127.0.0.1:7103";
        steer_conflict_target_request.sequence = 1;
        steer_conflict_target_request.observed_at_unix_ms = 160;
        steer_conflict_target_request.load.write_admission_overloaded = true;
        const auto steer_conflict_target_result =
            registry_.ReportLoad(steer_conflict_target_request);
        ASSERT_EQ(steer_conflict_target_result.status,
                  storedemo::StorageNodeStatusCode::kOk)
            << steer_conflict_target_result.error_detail;

        auto conflict_manager = make_manager(161);
        const auto conflict_manifest = MakeManifest(
            conflict_identity,
            payload,
            {source_store.config().node_id},
            2);
        const auto conflict_submit = conflict_manager.SubmitTask(
            MakeRepairTaskRequest(
                conflict_manifest,
                MakeManagerRepairCandidate(conflict_manifest,
                                           {source_store.config().node_id},
                                           {},
                                           true)));
        ASSERT_TRUE(conflict_submit.accepted());
        const auto conflict_run =
            conflict_manager.RunTask(conflict_submit.task->task_id);
        EXPECT_EQ(conflict_run.status, storedemo::StorageNodeStatusCode::kConflict);
        ASSERT_TRUE(conflict_run.task.has_value());
        EXPECT_EQ(conflict_run.task->state, storedemo::RepairTaskState::kFailed);
        EXPECT_FALSE(conflict_run.target_durable);
    }

    TEST_F(StorageScrubRepairTest,
           ProductionRepairManagerRejectsTargetThatBecomesOverloadedBeforeCopy)
    {
        auto &source_store = CreateStore(1);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 150);
        RegisterNode(3, 150);

        const auto payload =
            storedemo::test::MakeChunkPayload(64, "repair-manager-overloaded");
        const auto identity =
            MakeIdentityOrThrow("repair-manager-overloaded", 1, 0, 0);
        ASSERT_EQ(source_store.WriteChunk(
                      MakeWriteRequest(identity, payload, "repair-overloaded-source"))
                      .status,
                  storedemo::StorageNodeStatusCode::kOk);

        auto source_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[source_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            source_store.config().node_id);
        auto target_service = std::make_shared<storedemo::StorageNodeService>(
            std::shared_ptr<storedemo::ChunkStore>(stores_[target_store.config().node_id].get(),
                                                   [](storedemo::ChunkStore *) {}),
            target_store.config().node_id);
        RunningStorageNodeService source_server(source_service);
        RunningStorageNodeService target_server(target_service);

        std::map<storedemo::StorageNodeId, std::unique_ptr<storedemo::StorageNodeClient>> clients;
        clients.emplace(source_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            source_server.channel()));
        clients.emplace(target_store.config().node_id,
                        std::make_unique<storedemo::StorageNodeClient>(
                            target_server.channel()));

        storedemo::RepairManager manager(
            &registry_,
            storedemo::RepairManagerConfig{
                .max_active_tasks = 4,
                .max_tasks = 4,
                .default_timeout_ms = 1500,
                .now_unix_ms = []()
                {
                    return 161;
                },
                .source_reader =
                    [&clients](const storedemo::RepairTask &task,
                               const storedemo::StorageTaskContext &context)
                {
                    storedemo::ReadChunkRequest request;
                    request.request_id = task.task_id + "/source-read";
                    request.chunk_id = task.chunk_id;
                    request.expected_checksum = task.expected_checksum;
                    request.verify_checksum = true;
                    return ToRepairSourceReadResult(
                        clients.at(task.source_node)->ReadChunk(request, {.context = context}));
                },
                .target_writer =
                    [&clients](const storedemo::RepairTask &task,
                               const std::string_view repair_payload,
                               const storedemo::StorageTaskContext &context)
                {
                    storedemo::StorageNodeClientRepairChunkRequest request;
                    request.request_id = task.task_id + "/target-repair";
                    request.chunk_id = task.chunk_id;
                    request.object_id = task.identity.object_id;
                    request.version = task.identity.version;
                    request.chunk_index = task.identity.chunk_index;
                    request.offset = task.identity.offset;
                    request.expected_size = task.expected_size;
                    request.expected_checksum = task.expected_checksum;
                    request.source_node_id = task.source_node;
                    request.source_size = task.expected_size;
                    request.source_checksum = task.expected_checksum;
                    request.source_state = storedemo::ChunkState::kLive;
                    request.source_checksum_verified = true;
                    request.payload = std::string(repair_payload);
                    return ToRepairTargetWriteResult(
                        clients.at(task.target_node)->RepairChunk(request, {.context = context}));
                }});

        const auto manifest =
            MakeManifest(identity, payload, {source_store.config().node_id}, 2);
        const auto submit_result = manager.SubmitTask(
            MakeRepairTaskRequest(
                manifest,
                MakeManagerRepairCandidate(manifest,
                                           {source_store.config().node_id},
                                           {},
                                           true)));
        ASSERT_TRUE(submit_result.accepted());

        storedemo::ReportLoadRequest overload_request;
        overload_request.node_id = target_store.config().node_id;
        overload_request.endpoint = "127.0.0.1:7103";
        overload_request.sequence = 1;
        overload_request.observed_at_unix_ms = 160;
        overload_request.load.write_admission_overloaded = true;
        overload_request.load.read_admission_overloaded = false;
        const auto overload_result = registry_.ReportLoad(overload_request);
        ASSERT_EQ(overload_result.status, storedemo::StorageNodeStatusCode::kOk)
            << overload_result.error_detail;

        const auto run_result = manager.RunTask(submit_result.task->task_id);
        EXPECT_EQ(run_result.status, storedemo::StorageNodeStatusCode::kOverloaded);
        ASSERT_TRUE(run_result.task.has_value());
        EXPECT_EQ(run_result.task->state, storedemo::RepairTaskState::kRetryPending);
        EXPECT_EQ(target_store.StatChunk(
                      MakeStatRequest(identity.chunk_id, "repair-overloaded-target-stat"))
                      .status,
                  storedemo::StorageNodeStatusCode::kNotFound);
    }
} // namespace
