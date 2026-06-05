#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "store/chunk/local_disk_chunk_store.h"
#include "store/common/store_types.h"
#include "store/index/chunk_index.h"
#include "store/io/durable_file.h"
#include "store/maintenance/rebalance_manager.h"
#include "store/node/storage_node_registry.h"
#include "store/placement/placement_manager.h"
#include "support/store_test_utils.h"

namespace
{
    struct RebalanceManifest
    {
        storedemo::ChunkIdentity identity;
        std::uint64_t expected_size{0};
        storedemo::ChunkChecksum expected_checksum;
        std::vector<storedemo::StorageNodeId> replica_nodes;
        std::size_t desired_replica_count{0};
    };

    struct RebalanceIntent
    {
        std::optional<storedemo::StorageNodeId> preferred_source_node;
    };

    struct RebalanceObserver
    {
        std::function<void()> metadata_mutation_hook;
        std::function<void()> raft_call_hook;
        std::function<void(std::string_view)> payload_persist_hook;
    };

    struct CleanupCandidate
    {
        storedemo::ChunkId chunk_id;
        std::uint64_t expected_size{0};
        storedemo::ChunkChecksum expected_checksum;
        storedemo::StorageNodeId target_node;
        std::string reason;
    };

    struct SourceCleanupResult
    {
        storedemo::StorageNodeStatusCode status{
            storedemo::StorageNodeStatusCode::kOk};
        std::string error_detail;
        bool completed{false};
        bool already_missing{false};

        [[nodiscard]] bool ok() const
        {
            return status == storedemo::StorageNodeStatusCode::kOk;
        }
    };

    struct ManifestCoordinationResult
    {
        storedemo::StorageNodeStatusCode status{
            storedemo::StorageNodeStatusCode::kOk};
        std::string error_detail;
        bool updated{false};
        bool already_applied{false};

        [[nodiscard]] bool ok() const
        {
            return status == storedemo::StorageNodeStatusCode::kOk;
        }
    };

    struct RebalanceRunResult
    {
        storedemo::StorageNodeStatusCode status{
            storedemo::StorageNodeStatusCode::kOk};
        std::string error_detail;
        storedemo::StorageNodeId source_node;
        storedemo::StorageNodeId target_node;
        bool target_durable{false};
        bool target_already_present{false};
        bool manifest_update_attempted{false};
        bool manifest_updated{false};
        bool manifest_idempotent{false};
        bool source_cleanup_attempted{false};
        bool source_cleanup_completed{false};
        bool source_cleanup_retryable{false};
        bool orphan_candidate_created{false};
        bool idempotent_success{false};
        std::vector<std::string> stage_trace;
    };

    struct RecordingWriterState
    {
        storedemo::DurableFileResult append_result;
        storedemo::DurableFileResult flush_result{
            .durable_boundary_reached = true};
        storedemo::DurableFileResult close_result;
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

    storedemo::ReadChunkRequest MakeReadRequest(const storedemo::ChunkId &chunk_id,
                                                const storedemo::ChunkChecksum &checksum,
                                                const std::string &request_id)
    {
        return storedemo::ReadChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id,
            .expected_checksum = checksum,
            .verify_checksum = true};
    }

    storedemo::StatChunkRequest MakeStatRequest(const storedemo::ChunkId &chunk_id,
                                                const std::string &request_id)
    {
        return storedemo::StatChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id};
    }

    storedemo::RebalanceTaskRequest MakeRebalanceTaskRequest(
        const storedemo::ChunkIdentity &identity,
        const std::string &payload,
        const storedemo::StorageNodeId &source_node,
        const storedemo::StorageNodeId &target_node,
        const storedemo::RebalanceTaskReason reason =
            storedemo::RebalanceTaskReason::kCapacityImbalance,
        const storedemo::ChunkState source_state = storedemo::ChunkState::kLive)
    {
        return storedemo::RebalanceTaskRequest{
            .identity = identity,
            .source_node = source_node,
            .target_node = target_node,
            .reason = reason,
            .expected_checksum = ComputeChecksumOrThrow(payload),
            .expected_size = static_cast<std::uint64_t>(payload.size()),
            .source_state = source_state};
    }

    storedemo::DeleteChunkRequest MakeDeleteRequest(
        const storedemo::ChunkId &chunk_id,
        const storedemo::ChunkChecksum &checksum,
        const std::string &request_id)
    {
        return storedemo::DeleteChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id,
            .reason = "rebalance-source-cleanup",
            .metadata_boundary = "manifest_updated",
            .expected_checksum = checksum};
    }

    storedemo::StorageNodeRegistryFacts MakeRegistryFacts(
        const storedemo::StorageNodeHealth health =
            storedemo::StorageNodeHealth::kHealthy,
        const storedemo::StorageNodeDiskPressure disk_pressure =
            storedemo::StorageNodeDiskPressure::kLow)
    {
        storedemo::StorageNodeRegistryFacts facts;
        facts.capacity.total_capacity_bytes = 64 * 1024;
        facts.capacity.used_capacity_bytes = 8 * 1024;
        facts.capacity.available_capacity_bytes = 56 * 1024;
        facts.capacity.chunk_count = 1;
        facts.health.health = health;
        facts.health.disk_pressure = disk_pressure;
        facts.health.io_error_count = 0;
        facts.load.load.active_reads = 0;
        facts.load.load.active_writes = 0;
        facts.load.load.queued_ops = 0;
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

    bool ContainsNode(const std::vector<storedemo::StorageNodeId> &nodes,
                      const storedemo::StorageNodeId &node_id)
    {
        return std::find(nodes.begin(), nodes.end(), node_id) != nodes.end();
    }

    std::string MoveKey(const storedemo::ChunkId &chunk_id,
                        const storedemo::StorageNodeId &source_node)
    {
        return chunk_id + "|" + source_node;
    }

    class TestOnlyCleanupCandidateLedger
    {
    public:
        bool Record(const RebalanceManifest &manifest,
                    const storedemo::StorageNodeId &target_node,
                    std::string reason)
        {
            for (const auto &candidate : candidates_)
            {
                if (candidate.chunk_id == manifest.identity.chunk_id &&
                    candidate.target_node == target_node)
                {
                    return false;
                }
            }

            candidates_.push_back(CleanupCandidate{
                .chunk_id = manifest.identity.chunk_id,
                .expected_size = manifest.expected_size,
                .expected_checksum = manifest.expected_checksum,
                .target_node = target_node,
                .reason = std::move(reason)});
            return true;
        }

        [[nodiscard]] bool HasCandidate(const storedemo::ChunkId &chunk_id,
                                        const storedemo::StorageNodeId &target_node) const
        {
            return std::any_of(candidates_.begin(),
                               candidates_.end(),
                               [&](const CleanupCandidate &candidate)
                               {
                                   return candidate.chunk_id == chunk_id &&
                                          candidate.target_node == target_node;
                               });
        }

        [[nodiscard]] const std::vector<CleanupCandidate> &candidates() const
        {
            return candidates_;
        }

    private:
        std::vector<CleanupCandidate> candidates_;
    };

    class TestOnlyManifestLedger
    {
    public:
        explicit TestOnlyManifestLedger(RebalanceManifest manifest)
            : manifest_(std::move(manifest))
        {
        }

        void FailNextUpdate(const storedemo::StorageNodeStatusCode status,
                            std::string error_detail)
        {
            next_update_failure_status_ = status;
            next_update_failure_detail_ = std::move(error_detail);
        }

        ManifestCoordinationResult CoordinateMove(
            const storedemo::StorageNodeId &source_node,
            const storedemo::StorageNodeId &target_node)
        {
            ++update_attempts_;

            if (next_update_failure_status_ != storedemo::StorageNodeStatusCode::kOk)
            {
                ManifestCoordinationResult failure;
                failure.status = next_update_failure_status_;
                failure.error_detail = next_update_failure_detail_;
                next_update_failure_status_ = storedemo::StorageNodeStatusCode::kOk;
                next_update_failure_detail_.clear();
                return failure;
            }

            const auto source_it =
                std::find(manifest_.replica_nodes.begin(),
                          manifest_.replica_nodes.end(),
                          source_node);
            const bool target_present =
                ContainsNode(manifest_.replica_nodes, target_node);

            if (source_it == manifest_.replica_nodes.end() && target_present)
            {
                completed_moves_[MoveKey(manifest_.identity.chunk_id, source_node)] =
                    target_node;
                return ManifestCoordinationResult{
                    .status = storedemo::StorageNodeStatusCode::kOk,
                    .updated = false,
                    .already_applied = true};
            }

            if (source_it == manifest_.replica_nodes.end())
            {
                return ManifestCoordinationResult{
                    .status = storedemo::StorageNodeStatusCode::kConflict,
                    .error_detail = "rebalance source is absent from manifest"};
            }

            manifest_.replica_nodes.erase(source_it);
            if (!target_present)
            {
                manifest_.replica_nodes.push_back(target_node);
            }

            ++successful_updates_;
            completed_moves_[MoveKey(manifest_.identity.chunk_id, source_node)] =
                target_node;
            return ManifestCoordinationResult{
                .status = storedemo::StorageNodeStatusCode::kOk,
                .updated = true,
                .already_applied = false};
        }

        [[nodiscard]] const RebalanceManifest &manifest() const
        {
            return manifest_;
        }

        [[nodiscard]] std::optional<storedemo::StorageNodeId> FindRecordedTarget(
            const storedemo::ChunkId &chunk_id,
            const storedemo::StorageNodeId &source_node) const
        {
            const auto it = completed_moves_.find(MoveKey(chunk_id, source_node));
            if (it == completed_moves_.end())
            {
                return std::nullopt;
            }
            return it->second;
        }

        [[nodiscard]] std::size_t update_attempts() const
        {
            return update_attempts_;
        }

        [[nodiscard]] std::size_t successful_updates() const
        {
            return successful_updates_;
        }

    private:
        RebalanceManifest manifest_;
        storedemo::StorageNodeStatusCode next_update_failure_status_{
            storedemo::StorageNodeStatusCode::kOk};
        std::string next_update_failure_detail_;
        std::size_t update_attempts_{0};
        std::size_t successful_updates_{0};
        std::map<std::string, storedemo::StorageNodeId> completed_moves_;
    };

    class TestOnlySourceCleanupLedger
    {
    public:
        void FailCleanupFor(const storedemo::StorageNodeId &node_id,
                            const storedemo::StorageNodeStatusCode status,
                            std::string error_detail)
        {
            failures_[node_id] = std::make_pair(status, std::move(error_detail));
        }

        SourceCleanupResult Cleanup(storedemo::LocalDiskChunkStore &store,
                                    const storedemo::ChunkId &chunk_id,
                                    const storedemo::ChunkChecksum &checksum,
                                    const storedemo::StorageNodeId &source_node)
        {
            ++attempts_[source_node];

            const auto failure_it = failures_.find(source_node);
            if (failure_it != failures_.end())
            {
                return SourceCleanupResult{
                    .status = failure_it->second.first,
                    .error_detail = failure_it->second.second,
                    .completed = false,
                    .already_missing = false};
            }

            const auto response = store.DeleteChunk(
                MakeDeleteRequest(chunk_id,
                                  checksum,
                                  "rebalance-clean-" + source_node));
            if (!response.ok())
            {
                return SourceCleanupResult{
                    .status = response.status,
                    .error_detail = response.error_detail,
                    .completed = false,
                    .already_missing = false};
            }

            ++completions_[source_node];
            return SourceCleanupResult{
                .status = response.status,
                .error_detail = response.error_detail,
                .completed = response.deleted || response.already_missing,
                .already_missing = response.already_missing};
        }

        [[nodiscard]] std::size_t AttemptCount(
            const storedemo::StorageNodeId &node_id) const
        {
            const auto it = attempts_.find(node_id);
            return it == attempts_.end() ? 0U : it->second;
        }

        [[nodiscard]] std::size_t CompletionCount(
            const storedemo::StorageNodeId &node_id) const
        {
            const auto it = completions_.find(node_id);
            return it == completions_.end() ? 0U : it->second;
        }

    private:
        std::map<storedemo::StorageNodeId,
                 std::pair<storedemo::StorageNodeStatusCode, std::string>>
            failures_;
        std::map<storedemo::StorageNodeId, std::size_t> attempts_;
        std::map<storedemo::StorageNodeId, std::size_t> completions_;
    };

    storedemo::RebalanceManagerConfig MakeProductionRebalanceManagerConfig(
        const std::map<storedemo::StorageNodeId, storedemo::LocalDiskChunkStore *> &stores,
        TestOnlyManifestLedger *manifest_ledger,
        TestOnlyCleanupCandidateLedger *cleanup_candidate_ledger,
        TestOnlySourceCleanupLedger *source_cleanup_ledger,
        std::uint64_t *now_unix_ms,
        std::size_t *manifest_calls = nullptr,
        std::size_t *cleanup_candidate_calls = nullptr,
        std::size_t *source_cleanup_calls = nullptr)
    {
        return storedemo::RebalanceManagerConfig{
            .max_active_tasks = 8,
            .max_tasks = 8,
            .now_unix_ms = [now_unix_ms]()
            {
                if (now_unix_ms == nullptr)
                {
                    return std::uint64_t{0};
                }
                return *now_unix_ms;
            },
            .source_reader =
                [stores](const storedemo::RebalanceTask &task,
                         const storedemo::StorageTaskContext &)
                {
                    storedemo::RebalanceSourceReadResult result;
                    const auto store_it = stores.find(task.source_node);
                    if (store_it == stores.end() || store_it->second == nullptr)
                    {
                        result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                        result.error_detail = "rebalance source store is unavailable";
                        return result;
                    }

                    const auto read_response = store_it->second->ReadChunk(
                        MakeReadRequest(task.chunk_id,
                                        task.expected_checksum,
                                        "rebalance-manager-read-" + task.source_node));
                    result.status = read_response.status;
                    result.error_detail = read_response.error_detail;
                    result.retry_after_ms = read_response.retry_after_ms;
                    result.metadata = read_response.metadata;
                    result.actual_checksum = read_response.actual_checksum;
                    result.payload = read_response.payload;
                    result.verified = read_response.verified;
                    return result;
                },
            .target_writer =
                [stores](const storedemo::RebalanceTask &task,
                         std::string_view payload,
                         const storedemo::StorageTaskContext &)
                {
                    storedemo::RebalanceTargetWriteResult result;
                    const auto store_it = stores.find(task.target_node);
                    if (store_it == stores.end() || store_it->second == nullptr)
                    {
                        result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                        result.error_detail = "rebalance target store is unavailable";
                        result.retryable = true;
                        return result;
                    }

                    const auto write_response = store_it->second->WriteChunk(
                        MakeWriteRequest(task.identity,
                                         std::string(payload),
                                         "rebalance-manager-write-" + task.target_node));
                    result.status = write_response.status;
                    result.error_detail = write_response.error_detail;
                    result.retry_after_ms = write_response.retry_after_ms;
                    result.metadata = write_response.metadata;
                    result.target_state = write_response.metadata.state;
                    result.expected_checksum = task.expected_checksum;
                    result.observed_checksum = write_response.metadata.checksum;
                    result.expected_size = task.expected_size;
                    result.observed_size = write_response.metadata.size;
                    result.target_durable = write_response.durable;
                    result.already_exists = write_response.already_exists;
                    result.repaired =
                        write_response.ok() && write_response.durable &&
                        !write_response.already_exists;
                    result.retryable =
                        storedemo::IsRetriableStatus(write_response.status);
                    return result;
                },
            .target_verifier =
                [stores](const storedemo::RebalanceTask &task,
                         const storedemo::StorageTaskContext &)
                {
                    storedemo::RebalanceTargetVerifyResult result;
                    const auto store_it = stores.find(task.target_node);
                    if (store_it == stores.end() || store_it->second == nullptr)
                    {
                        result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                        result.error_detail = "rebalance target store is unavailable";
                        result.retryable = true;
                        return result;
                    }

                    const auto read_response = store_it->second->ReadChunk(
                        MakeReadRequest(task.chunk_id,
                                        task.expected_checksum,
                                        "rebalance-manager-verify-" +
                                            task.target_node));
                    result.status = read_response.status;
                    result.error_detail = read_response.error_detail;
                    result.retry_after_ms = read_response.retry_after_ms;
                    result.metadata = read_response.metadata;
                    result.actual_checksum = read_response.actual_checksum;
                    result.actual_size = read_response.metadata.size;
                    result.verified = read_response.verified;
                    result.retryable =
                        storedemo::IsRetriableStatus(read_response.status);
                    return result;
                },
            .manifest_coordinator =
                [manifest_ledger, manifest_calls](
                    const storedemo::RebalanceTask &task,
                    const storedemo::StorageTaskContext &)
                {
                    storedemo::RebalanceManifestCoordinationResult result;
                    if (manifest_calls != nullptr)
                    {
                        ++(*manifest_calls);
                    }
                    if (manifest_ledger == nullptr)
                    {
                        result.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
                        result.error_detail =
                            "rebalance manifest ledger is unavailable";
                        return result;
                    }

                    const auto coordination = manifest_ledger->CoordinateMove(
                        task.source_node, task.target_node);
                    result.status = coordination.status;
                    result.error_detail = coordination.error_detail;
                    result.updated = coordination.updated;
                    result.already_applied = coordination.already_applied;
                    result.retryable =
                        storedemo::IsRetriableStatus(coordination.status);
                    return result;
                },
            .source_cleanup_handler =
                [stores, source_cleanup_ledger, source_cleanup_calls](
                    const storedemo::RebalanceTask &task,
                    const storedemo::StorageTaskContext &)
                {
                    storedemo::RebalanceSourceCleanupResult result;
                    if (source_cleanup_calls != nullptr)
                    {
                        ++(*source_cleanup_calls);
                    }
                    const auto store_it = stores.find(task.source_node);
                    if (store_it == stores.end() || store_it->second == nullptr)
                    {
                        result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                        result.error_detail = "rebalance source store is unavailable";
                        result.retryable = true;
                        return result;
                    }
                    if (source_cleanup_ledger == nullptr)
                    {
                        result.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
                        result.error_detail =
                            "rebalance source cleanup ledger is unavailable";
                        return result;
                    }

                    const auto cleanup = source_cleanup_ledger->Cleanup(
                        *store_it->second,
                        task.chunk_id,
                        task.expected_checksum,
                        task.source_node);
                    result.status = cleanup.status;
                    result.error_detail = cleanup.error_detail;
                    result.completed = cleanup.completed;
                    result.already_missing = cleanup.already_missing;
                    result.retryable = storedemo::IsRetriableStatus(cleanup.status);
                    return result;
                },
            .cleanup_candidate_recorder =
                [cleanup_candidate_ledger, cleanup_candidate_calls](
                    const storedemo::RebalanceTask &task,
                    std::string_view reason,
                    const storedemo::StorageTaskContext &)
                {
                    storedemo::RebalanceCleanupCandidateResult result;
                    if (cleanup_candidate_calls != nullptr)
                    {
                        ++(*cleanup_candidate_calls);
                    }
                    if (cleanup_candidate_ledger == nullptr)
                    {
                        result.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
                        result.error_detail =
                            "rebalance cleanup candidate ledger is unavailable";
                        return result;
                    }

                    const bool recorded = cleanup_candidate_ledger->Record(
                        RebalanceManifest{
                            .identity = task.identity,
                            .expected_size = task.expected_size,
                            .expected_checksum = task.expected_checksum,
                            .replica_nodes = {task.source_node, task.target_node},
                            .desired_replica_count = 0},
                        task.target_node,
                        std::string(reason));
                    result.status = storedemo::StorageNodeStatusCode::kOk;
                    result.recorded = recorded;
                    result.already_exists = !recorded;
                    return result;
                }};
    }

    class TestOnlyRebalanceRunner
    {
    public:
        TestOnlyRebalanceRunner(
            std::map<storedemo::StorageNodeId, storedemo::LocalDiskChunkStore *> stores,
            const storedemo::StorageNodeRegistry *registry,
            TestOnlyManifestLedger *manifest_ledger,
            TestOnlyCleanupCandidateLedger *cleanup_candidate_ledger,
            TestOnlySourceCleanupLedger *source_cleanup_ledger,
            RebalanceObserver observer = {})
            : stores_(std::move(stores))
            , registry_(registry)
            , manifest_ledger_(manifest_ledger)
            , cleanup_candidate_ledger_(cleanup_candidate_ledger)
            , source_cleanup_ledger_(source_cleanup_ledger)
            , observer_(std::move(observer))
        {
        }

        RebalanceRunResult Run(const RebalanceIntent &intent,
                               const std::uint64_t now_unix_ms) const
        {
            RebalanceRunResult result;
            if (registry_ == nullptr || manifest_ledger_ == nullptr ||
                cleanup_candidate_ledger_ == nullptr ||
                source_cleanup_ledger_ == nullptr)
            {
                result.status = storedemo::StorageNodeStatusCode::kInvalidArgument;
                result.error_detail =
                    "rebalance runner requires registry, manifest ledger and cleanup ledgers";
                return result;
            }

            const auto snapshot = registry_->Snapshot(now_unix_ms);
            if (!snapshot.ok())
            {
                result.status = snapshot.status;
                result.error_detail = snapshot.error_detail;
                return result;
            }

            const auto &manifest = manifest_ledger_->manifest();
            std::map<storedemo::StorageNodeId, storedemo::StorageNodeRegistryNodeSnapshot>
                snapshot_by_node;
            for (const auto &node : snapshot.nodes)
            {
                snapshot_by_node.emplace(node.node_id, node);
            }

            if (intent.preferred_source_node.has_value())
            {
                const auto recorded_target = manifest_ledger_->FindRecordedTarget(
                    manifest.identity.chunk_id, *intent.preferred_source_node);
                if (!ContainsNode(manifest.replica_nodes, *intent.preferred_source_node) &&
                    recorded_target.has_value() &&
                    ContainsNode(manifest.replica_nodes, *recorded_target))
                {
                    result.status = storedemo::StorageNodeStatusCode::kOk;
                    result.source_node = *intent.preferred_source_node;
                    result.target_node = *recorded_target;
                    result.manifest_idempotent = true;
                    result.idempotent_success = true;
                    return result;
                }
            }

            std::vector<storedemo::StorageNodeId> candidate_sources;
            if (intent.preferred_source_node.has_value())
            {
                candidate_sources.push_back(*intent.preferred_source_node);
            }
            else
            {
                candidate_sources = manifest.replica_nodes;
            }

            std::string source_payload;
            for (const auto &node_id : candidate_sources)
            {
                const auto snapshot_it = snapshot_by_node.find(node_id);
                if (snapshot_it == snapshot_by_node.end())
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

                const auto store_it = stores_.find(node_id);
                if (store_it == stores_.end() || store_it->second == nullptr)
                {
                    continue;
                }

                const auto read_response =
                    store_it->second->ReadChunk(MakeReadRequest(
                        manifest.identity.chunk_id,
                        manifest.expected_checksum,
                        "rebalance-read-source-" + node_id));
                if (!read_response.ok())
                {
                    continue;
                }
                if (read_response.metadata.size != manifest.expected_size ||
                    !ChecksumEquals(read_response.actual_checksum,
                                    manifest.expected_checksum))
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
                result.error_detail = "no healthy rebalance source is available";
                return result;
            }

            storedemo::PlacementManager placement_manager;
            storedemo::PlacementRequest placement_request;
            placement_request.identity = manifest.identity;
            placement_request.chunk_size_bytes = manifest.expected_size;
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
            const auto target_store_it = stores_.find(result.target_node);
            if (target_store_it == stores_.end() || target_store_it->second == nullptr)
            {
                result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                result.error_detail = "selected rebalance target store is unavailable";
                return result;
            }

            const auto write_response = target_store_it->second->WriteChunk(
                MakeWriteRequest(manifest.identity,
                                 source_payload,
                                 "rebalance-write-target-" + result.target_node));
            if (!write_response.ok())
            {
                result.status = write_response.status;
                result.error_detail = write_response.error_detail;
                return result;
            }

            result.target_durable = true;
            result.target_already_present = write_response.already_exists;
            result.stage_trace.push_back("target_durable");

            result.manifest_update_attempted = true;
            result.stage_trace.push_back("manifest_update");
            const auto coordination =
                manifest_ledger_->CoordinateMove(result.source_node, result.target_node);
            if (!coordination.ok())
            {
                result.status = coordination.status;
                result.error_detail = coordination.error_detail;
                result.orphan_candidate_created = cleanup_candidate_ledger_->Record(
                    manifest,
                    result.target_node,
                    "manifest_update_failed_after_target_durable");
                return result;
            }

            result.manifest_updated = coordination.updated;
            result.manifest_idempotent = coordination.already_applied;

            const auto source_store_it = stores_.find(result.source_node);
            if (source_store_it == stores_.end() || source_store_it->second == nullptr)
            {
                result.status = storedemo::StorageNodeStatusCode::kNodeUnavailable;
                result.error_detail = "selected rebalance source store is unavailable";
                return result;
            }

            result.source_cleanup_attempted = true;
            result.stage_trace.push_back("source_cleanup");
            const auto cleanup = source_cleanup_ledger_->Cleanup(
                *source_store_it->second,
                manifest.identity.chunk_id,
                manifest.expected_checksum,
                result.source_node);
            if (!cleanup.ok())
            {
                result.status = cleanup.status;
                result.error_detail = cleanup.error_detail;
                result.source_cleanup_retryable = true;
                return result;
            }

            result.source_cleanup_completed = cleanup.completed;
            result.idempotent_success =
                result.target_already_present || coordination.already_applied;
            return result;
        }

    private:
        std::map<storedemo::StorageNodeId, storedemo::LocalDiskChunkStore *> stores_;
        const storedemo::StorageNodeRegistry *registry_;
        TestOnlyManifestLedger *manifest_ledger_;
        TestOnlyCleanupCandidateLedger *cleanup_candidate_ledger_;
        TestOnlySourceCleanupLedger *source_cleanup_ledger_;
        RebalanceObserver observer_;
    };

    class StorageRebalanceTest : public ::testing::Test
    {
    protected:
        StorageRebalanceTest()
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
            const bool write_overloaded = false,
            const std::uint64_t total_capacity_bytes = 64 * 1024,
            const std::uint64_t used_capacity_bytes = 8 * 1024)
        {
            storedemo::RegisterStorageNodeRequest request;
            request.node_id = storedemo::test::MakeStorageNodeIdFixture(node_index);
            request.endpoint = "127.0.0.1:" + std::to_string(7200 + node_index);
            request.observed_at_unix_ms = observed_at_unix_ms;
            request.facts = MakeRegistryFacts(health, disk_pressure);
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

        RebalanceManifest MakeManifest(
            const storedemo::ChunkIdentity &identity,
            const std::string &payload,
            std::vector<storedemo::StorageNodeId> replica_nodes,
            const std::size_t desired_replica_count) const
        {
            return RebalanceManifest{
                .identity = identity,
                .expected_size = static_cast<std::uint64_t>(payload.size()),
                .expected_checksum = ComputeChecksumOrThrow(payload),
                .replica_nodes = std::move(replica_nodes),
                .desired_replica_count = desired_replica_count};
        }

        storedemo::test::ScopedStoreTestDir temp_dir_{"storage_rebalance"};
        storedemo::StorageNodeRegistry registry_;
        std::map<storedemo::StorageNodeId,
                 std::unique_ptr<storedemo::LocalDiskChunkStore>>
            stores_;
    };

    TEST_F(StorageRebalanceTest,
           TargetDurableCompletesBeforeManifestUpdateAndSourceCleanupFollowsManifest)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-happy", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(96, "rebalance-happy");
        WriteReplica(source_store, identity, payload, "rebalance-happy-source");
        WriteReplica(peer_store, identity, payload, "rebalance-happy-peer");

        TestOnlyManifestLedger manifest_ledger(MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2));
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        TestOnlyRebalanceRunner runner(RawStoreMap(),
                                       &registry_,
                                       &manifest_ledger,
                                       &cleanup_candidates,
                                       &cleanup_ledger);

        const auto result = runner.Run(
            RebalanceIntent{
                .preferred_source_node =
                    storedemo::test::MakeStorageNodeIdFixture(1)},
            110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.source_node, storedemo::test::MakeStorageNodeIdFixture(1));
        EXPECT_EQ(result.target_node, storedemo::test::MakeStorageNodeIdFixture(3));
        EXPECT_TRUE(result.target_durable);
        EXPECT_TRUE(result.manifest_update_attempted);
        EXPECT_TRUE(result.manifest_updated);
        EXPECT_TRUE(result.source_cleanup_attempted);
        EXPECT_TRUE(result.source_cleanup_completed);
        EXPECT_EQ(result.stage_trace,
                  std::vector<std::string>(
                      {"target_durable", "manifest_update", "source_cleanup"}));
        const std::vector<storedemo::StorageNodeId> expected_manifest_nodes{
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::test::MakeStorageNodeIdFixture(3)};
        EXPECT_EQ(manifest_ledger.manifest().replica_nodes,
                  expected_manifest_nodes);
        EXPECT_EQ(cleanup_ledger.AttemptCount(
                      storedemo::test::MakeStorageNodeIdFixture(1)),
                  1U);
        EXPECT_EQ(cleanup_ledger.CompletionCount(
                      storedemo::test::MakeStorageNodeIdFixture(1)),
                  1U);
        EXPECT_TRUE(cleanup_candidates.candidates().empty());

        const auto source_stat = source_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-happy-source-stat"));
        ASSERT_EQ(source_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(source_stat.metadata.state, storedemo::ChunkState::kDeleted);

        const auto target_stat = target_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-happy-target-stat"));
        ASSERT_EQ(target_stat.status, storedemo::StorageNodeStatusCode::kOk)
            << target_stat.error_detail;
        EXPECT_EQ(target_stat.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageRebalanceTest,
           TargetDurableFailureDoesNotUpdateManifestOrCleanupSource)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto writer_state = std::make_shared<RecordingWriterState>();
        auto failing_durable_file =
            std::make_shared<RecordingDurableFile>(writer_state);
        failing_durable_file->publish_result.error =
            storedemo::DurableFileErrorCode::kIoError;
        auto &target_store = CreateStore(3, failing_durable_file);
        (void)target_store;

        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-target-fail", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(80, "rebalance-target-fail");
        WriteReplica(source_store, identity, payload, "rebalance-target-fail-source");
        WriteReplica(peer_store, identity, payload, "rebalance-target-fail-peer");

        const auto original_manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2);
        TestOnlyManifestLedger manifest_ledger(original_manifest);
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        TestOnlyRebalanceRunner runner(RawStoreMap(),
                                       &registry_,
                                       &manifest_ledger,
                                       &cleanup_candidates,
                                       &cleanup_ledger);

        const auto result = runner.Run(
            RebalanceIntent{
                .preferred_source_node =
                    storedemo::test::MakeStorageNodeIdFixture(1)},
            110);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kIoError);
        EXPECT_FALSE(result.target_durable);
        EXPECT_FALSE(result.manifest_update_attempted);
        EXPECT_FALSE(result.source_cleanup_attempted);
        EXPECT_FALSE(result.orphan_candidate_created);
        EXPECT_TRUE(result.stage_trace.empty());
        EXPECT_EQ(manifest_ledger.manifest().replica_nodes,
                  original_manifest.replica_nodes);
        EXPECT_TRUE(cleanup_candidates.candidates().empty());
        EXPECT_EQ(cleanup_ledger.AttemptCount(
                      storedemo::test::MakeStorageNodeIdFixture(1)),
                  0U);

        const auto source_stat = source_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-target-fail-source-stat"));
        ASSERT_EQ(source_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(source_stat.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageRebalanceTest,
           ManifestUpdateFailureLeavesDurableTargetAsCleanupCandidateAndKeepsSource)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-manifest-fail", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(88, "rebalance-manifest-fail");
        WriteReplica(source_store, identity, payload, "rebalance-manifest-fail-source");
        WriteReplica(peer_store, identity, payload, "rebalance-manifest-fail-peer");

        const auto original_manifest = MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2);
        TestOnlyManifestLedger manifest_ledger(original_manifest);
        manifest_ledger.FailNextUpdate(storedemo::StorageNodeStatusCode::kConflict,
                                       "manifest coordination rejected move");
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        TestOnlyRebalanceRunner runner(RawStoreMap(),
                                       &registry_,
                                       &manifest_ledger,
                                       &cleanup_candidates,
                                       &cleanup_ledger);

        const auto result = runner.Run(
            RebalanceIntent{
                .preferred_source_node =
                    storedemo::test::MakeStorageNodeIdFixture(1)},
            110);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kConflict);
        EXPECT_TRUE(result.target_durable);
        EXPECT_TRUE(result.manifest_update_attempted);
        EXPECT_FALSE(result.manifest_updated);
        EXPECT_FALSE(result.source_cleanup_attempted);
        EXPECT_TRUE(result.orphan_candidate_created);
        EXPECT_EQ(result.stage_trace,
                  std::vector<std::string>(
                      {"target_durable", "manifest_update"}));
        EXPECT_EQ(manifest_ledger.manifest().replica_nodes,
                  original_manifest.replica_nodes);
        ASSERT_TRUE(cleanup_candidates.HasCandidate(
            identity.chunk_id, storedemo::test::MakeStorageNodeIdFixture(3)));
        ASSERT_EQ(cleanup_candidates.candidates().size(), 1U);
        EXPECT_EQ(cleanup_candidates.candidates().front().expected_size,
                  static_cast<std::uint64_t>(payload.size()));
        EXPECT_TRUE(ChecksumEquals(cleanup_candidates.candidates().front().expected_checksum,
                                   ComputeChecksumOrThrow(payload)));

        const auto source_stat = source_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-manifest-fail-source-stat"));
        ASSERT_EQ(source_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(source_stat.metadata.state, storedemo::ChunkState::kLive);

        const auto target_stat = target_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-manifest-fail-target-stat"));
        ASSERT_EQ(target_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_stat.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageRebalanceTest,
           SourceCleanupFailureReturnsRetryableStatusWithoutRollingBackManifest)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-cleanup-fail", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(72, "rebalance-cleanup-fail");
        WriteReplica(source_store, identity, payload, "rebalance-cleanup-fail-source");
        WriteReplica(peer_store, identity, payload, "rebalance-cleanup-fail-peer");

        TestOnlyManifestLedger manifest_ledger(MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2));
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        cleanup_ledger.FailCleanupFor(storedemo::test::MakeStorageNodeIdFixture(1),
                                      storedemo::StorageNodeStatusCode::kIoError,
                                      "source cleanup is temporarily blocked");
        TestOnlyRebalanceRunner runner(RawStoreMap(),
                                       &registry_,
                                       &manifest_ledger,
                                       &cleanup_candidates,
                                       &cleanup_ledger);

        const auto result = runner.Run(
            RebalanceIntent{
                .preferred_source_node =
                    storedemo::test::MakeStorageNodeIdFixture(1)},
            110);

        EXPECT_EQ(result.status, storedemo::StorageNodeStatusCode::kIoError);
        EXPECT_TRUE(result.target_durable);
        EXPECT_TRUE(result.manifest_update_attempted);
        EXPECT_TRUE(result.manifest_updated);
        EXPECT_TRUE(result.source_cleanup_attempted);
        EXPECT_FALSE(result.source_cleanup_completed);
        EXPECT_TRUE(result.source_cleanup_retryable);
        EXPECT_EQ(result.stage_trace,
                  std::vector<std::string>(
                      {"target_durable", "manifest_update", "source_cleanup"}));
        const std::vector<storedemo::StorageNodeId> expected_manifest_nodes{
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::test::MakeStorageNodeIdFixture(3)};
        EXPECT_EQ(manifest_ledger.manifest().replica_nodes,
                  expected_manifest_nodes);
        EXPECT_EQ(cleanup_ledger.AttemptCount(
                      storedemo::test::MakeStorageNodeIdFixture(1)),
                  1U);
        EXPECT_EQ(cleanup_ledger.CompletionCount(
                      storedemo::test::MakeStorageNodeIdFixture(1)),
                  0U);
        EXPECT_TRUE(cleanup_candidates.candidates().empty());

        const auto source_stat = source_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-cleanup-fail-source-stat"));
        ASSERT_EQ(source_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(source_stat.metadata.state, storedemo::ChunkState::kLive);

        const auto target_stat = target_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-cleanup-fail-target-stat"));
        ASSERT_EQ(target_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_stat.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageRebalanceTest,
           SourceAndTargetSelectionRejectsBadCandidatesBeforeMigrating)
    {
        auto &corrupted_source = CreateStore(1);
        auto &stale_source = CreateStore(2);
        auto &unavailable_source = CreateStore(3);
        auto &healthy_source = CreateStore(4);
        auto &healthy_peer = CreateStore(5);
        auto &overloaded_target = CreateStore(6);
        auto &high_disk_target = CreateStore(7);
        auto &small_target = CreateStore(8);
        auto &stale_target = CreateStore(9);
        auto &healthy_target = CreateStore(10);
        (void)overloaded_target;
        (void)high_disk_target;
        (void)small_target;
        (void)stale_target;

        RegisterNode(1, 100);
        RegisterNode(2, 60);
        RegisterNode(3, 100, storedemo::StorageNodeHealth::kUnavailable);
        RegisterNode(4, 100);
        RegisterNode(5, 100);
        RegisterNode(6, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, true);
        RegisterNode(7, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kHigh);
        RegisterNode(8, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, false, 2048, 1664);
        RegisterNode(9, 60);
        RegisterNode(10, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-filter", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(512, "rebalance-filter");
        WriteReplica(corrupted_source, identity, payload, "rebalance-filter-corrupted");
        WriteReplica(stale_source, identity, payload, "rebalance-filter-stale");
        WriteReplica(unavailable_source, identity, payload, "rebalance-filter-unavailable");
        WriteReplica(healthy_source, identity, payload, "rebalance-filter-healthy");
        WriteReplica(healthy_peer, identity, payload, "rebalance-filter-peer");
        TamperReplica(corrupted_source,
                      identity,
                      storedemo::test::MakeChunkPayload(payload.size(),
                                                        "rebalance-filter-bad"));

        TestOnlyManifestLedger manifest_ledger(MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2),
             storedemo::test::MakeStorageNodeIdFixture(3),
             storedemo::test::MakeStorageNodeIdFixture(4),
             storedemo::test::MakeStorageNodeIdFixture(5)},
            5));
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        TestOnlyRebalanceRunner runner(RawStoreMap(),
                                       &registry_,
                                       &manifest_ledger,
                                       &cleanup_candidates,
                                       &cleanup_ledger);

        const auto result = runner.Run(RebalanceIntent{}, 110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_EQ(result.source_node, storedemo::test::MakeStorageNodeIdFixture(4));
        EXPECT_EQ(result.target_node, storedemo::test::MakeStorageNodeIdFixture(10));
        EXPECT_TRUE(result.target_durable);
        EXPECT_TRUE(result.manifest_updated);
        EXPECT_TRUE(result.source_cleanup_completed);
        EXPECT_TRUE(cleanup_candidates.candidates().empty());

        const auto corrupted_stat = corrupted_source.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-filter-corrupted-stat"));
        ASSERT_EQ(corrupted_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(corrupted_stat.metadata.state, storedemo::ChunkState::kQuarantined);
    }

    TEST_F(StorageRebalanceTest,
           ExistingTargetReplicaAllowsIdempotentDurableWriteBeforeManifestUpdate)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-existing-target", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(104, "rebalance-existing-target");
        WriteReplica(source_store, identity, payload, "rebalance-existing-source");
        WriteReplica(peer_store, identity, payload, "rebalance-existing-peer");
        WriteReplica(target_store, identity, payload, "rebalance-existing-target");

        TestOnlyManifestLedger manifest_ledger(MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2));
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        TestOnlyRebalanceRunner runner(RawStoreMap(),
                                       &registry_,
                                       &manifest_ledger,
                                       &cleanup_candidates,
                                       &cleanup_ledger);

        const auto result = runner.Run(
            RebalanceIntent{
                .preferred_source_node =
                    storedemo::test::MakeStorageNodeIdFixture(1)},
            110);

        ASSERT_EQ(result.status, storedemo::StorageNodeStatusCode::kOk)
            << result.error_detail;
        EXPECT_TRUE(result.target_durable);
        EXPECT_TRUE(result.target_already_present);
        EXPECT_TRUE(result.manifest_updated);
        EXPECT_TRUE(result.source_cleanup_completed);
        const std::vector<storedemo::StorageNodeId> expected_manifest_nodes{
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::test::MakeStorageNodeIdFixture(3)};
        EXPECT_EQ(manifest_ledger.manifest().replica_nodes,
                  expected_manifest_nodes);
        EXPECT_TRUE(cleanup_candidates.candidates().empty());
    }

    TEST_F(StorageRebalanceTest,
           RepeatedRebalanceIsIdempotentAndDoesNotTouchRaftOrPersistPayloadIntoMetadata)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-repeat", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(112, "rebalance-repeat");
        WriteReplica(source_store, identity, payload, "rebalance-repeat-source");
        WriteReplica(peer_store, identity, payload, "rebalance-repeat-peer");

        TestOnlyManifestLedger manifest_ledger(MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2));
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;

        std::size_t metadata_mutation_calls = 0;
        std::size_t raft_calls = 0;
        std::size_t payload_persist_calls = 0;
        TestOnlyRebalanceRunner runner(
            RawStoreMap(),
            &registry_,
            &manifest_ledger,
            &cleanup_candidates,
            &cleanup_ledger,
            RebalanceObserver{
                .metadata_mutation_hook = [&metadata_mutation_calls]()
                { ++metadata_mutation_calls; },
                .raft_call_hook = [&raft_calls]()
                { ++raft_calls; },
                .payload_persist_hook = [&payload_persist_calls](std::string_view)
                { ++payload_persist_calls; }});

        const RebalanceIntent intent{
            .preferred_source_node = storedemo::test::MakeStorageNodeIdFixture(1)};
        const auto first = runner.Run(intent, 110);
        const auto manifest_after_first = manifest_ledger.manifest().replica_nodes;
        const auto second = runner.Run(intent, 120);

        ASSERT_EQ(first.status, storedemo::StorageNodeStatusCode::kOk)
            << first.error_detail;
        ASSERT_EQ(second.status, storedemo::StorageNodeStatusCode::kOk)
            << second.error_detail;
        EXPECT_TRUE(second.idempotent_success);
        EXPECT_TRUE(second.manifest_idempotent);
        EXPECT_FALSE(second.target_durable);
        EXPECT_FALSE(second.manifest_update_attempted);
        EXPECT_FALSE(second.source_cleanup_attempted);
        EXPECT_TRUE(second.stage_trace.empty());
        EXPECT_EQ(manifest_ledger.manifest().replica_nodes, manifest_after_first);
        EXPECT_EQ(manifest_ledger.update_attempts(), 1U);
        EXPECT_EQ(cleanup_ledger.AttemptCount(
                      storedemo::test::MakeStorageNodeIdFixture(1)),
                  1U);
        EXPECT_EQ(metadata_mutation_calls, 0U);
        EXPECT_EQ(raft_calls, 0U);
        EXPECT_EQ(payload_persist_calls, 0U);

        const auto source_stat = source_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-repeat-source-stat"));
        ASSERT_EQ(source_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(source_stat.metadata.state, storedemo::ChunkState::kDeleted);

        const auto target_stat = target_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-repeat-target-stat"));
        ASSERT_EQ(target_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_stat.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageRebalanceTest,
           ProductionRebalanceManagerCreatesReasonScopedTasks)
    {
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);
        RegisterNode(4, 100);
        RegisterNode(5, 100);
        RegisterNode(6, 100);

        const auto payload =
            storedemo::test::MakeChunkPayload(96, "rebalance-task-model");
        const auto checksum = ComputeChecksumOrThrow(payload);
        std::uint64_t now_unix_ms = 110;
        storedemo::RebalanceManager manager(
            &registry_,
            storedemo::RebalanceManagerConfig{
                .max_active_tasks = 8,
                .max_tasks = 8,
                .now_unix_ms = [&now_unix_ms]()
                { return now_unix_ms; }});

        const auto capacity_result = manager.SubmitTask(MakeRebalanceTaskRequest(
            MakeIdentityOrThrow("rebalance-capacity-task", 1, 0, 0),
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::RebalanceTaskReason::kCapacityImbalance));
        const auto hotspot_result = manager.SubmitTask(MakeRebalanceTaskRequest(
            MakeIdentityOrThrow("rebalance-hotspot-task", 1, 0, 0),
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(3),
            storedemo::RebalanceTaskReason::kHotspot));
        const auto new_node_result = manager.SubmitTask(MakeRebalanceTaskRequest(
            MakeIdentityOrThrow("rebalance-new-node-task", 1, 0, 0),
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(4),
            storedemo::RebalanceTaskReason::kNewNodeJoin));
        const auto draining_result = manager.SubmitTask(MakeRebalanceTaskRequest(
            MakeIdentityOrThrow("rebalance-draining-task", 1, 0, 0),
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(5),
            storedemo::RebalanceTaskReason::kDraining));
        const auto maintenance_result =
            manager.SubmitTask(MakeRebalanceTaskRequest(
                MakeIdentityOrThrow("rebalance-maintenance-task", 1, 0, 0),
                payload,
                storedemo::test::MakeStorageNodeIdFixture(1),
                storedemo::test::MakeStorageNodeIdFixture(6),
                storedemo::RebalanceTaskReason::kMaintenance));

        ASSERT_TRUE(capacity_result.accepted()) << capacity_result.error_detail;
        ASSERT_TRUE(hotspot_result.accepted()) << hotspot_result.error_detail;
        ASSERT_TRUE(new_node_result.accepted()) << new_node_result.error_detail;
        ASSERT_TRUE(draining_result.accepted()) << draining_result.error_detail;
        ASSERT_TRUE(maintenance_result.accepted())
            << maintenance_result.error_detail;

        ASSERT_TRUE(capacity_result.task.has_value());
        EXPECT_EQ(capacity_result.task->source_node,
                  storedemo::test::MakeStorageNodeIdFixture(1));
        EXPECT_EQ(capacity_result.task->target_node,
                  storedemo::test::MakeStorageNodeIdFixture(2));
        EXPECT_EQ(capacity_result.task->reason,
                  storedemo::RebalanceTaskReason::kCapacityImbalance);
        EXPECT_EQ(capacity_result.task->chunk_id,
                  capacity_result.task->identity.chunk_id);
        EXPECT_TRUE(
            ChecksumEquals(capacity_result.task->expected_checksum, checksum));
        EXPECT_EQ(capacity_result.task->expected_size,
                  static_cast<std::uint64_t>(payload.size()));
        EXPECT_EQ(capacity_result.task->state,
                  storedemo::RebalanceTaskState::kQueued);
        EXPECT_EQ(capacity_result.task->progress_percent, 0U);
        EXPECT_EQ(capacity_result.task->attempts, 0U);
        EXPECT_EQ(capacity_result.task->last_error,
                  storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(capacity_result.task->last_error_detail.empty());
        EXPECT_EQ(capacity_result.task->submitted_at_unix_ms, now_unix_ms);

        ASSERT_TRUE(hotspot_result.task.has_value());
        EXPECT_EQ(hotspot_result.task->reason,
                  storedemo::RebalanceTaskReason::kHotspot);
        ASSERT_TRUE(new_node_result.task.has_value());
        EXPECT_EQ(new_node_result.task->reason,
                  storedemo::RebalanceTaskReason::kNewNodeJoin);
        ASSERT_TRUE(draining_result.task.has_value());
        EXPECT_EQ(draining_result.task->reason,
                  storedemo::RebalanceTaskReason::kDraining);
        ASSERT_TRUE(maintenance_result.task.has_value());
        EXPECT_EQ(maintenance_result.task->reason,
                  storedemo::RebalanceTaskReason::kMaintenance);

        const auto listed_tasks = manager.ListTasks();
        ASSERT_EQ(listed_tasks.size(), 5U);
        EXPECT_TRUE(std::is_sorted(
            listed_tasks.begin(),
            listed_tasks.end(),
            [](const storedemo::RebalanceTask &lhs,
               const storedemo::RebalanceTask &rhs)
            { return lhs.task_id < rhs.task_id; }));

        const auto stats = manager.SnapshotStats();
        EXPECT_EQ(stats.total_tasks, 5U);
        EXPECT_EQ(stats.queued_tasks, 5U);
        EXPECT_EQ(stats.submitted_tasks, 5U);
        EXPECT_EQ(stats.rejected_tasks, 0U);
    }

    TEST_F(StorageRebalanceTest,
           ProductionRebalanceManagerRejectsInvalidFactsAndBadSourceTarget)
    {
        RegisterNode(1, 100);
        RegisterNode(2, 60);
        RegisterNode(3, 100, storedemo::StorageNodeHealth::kUnavailable);
        RegisterNode(4, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, true);
        RegisterNode(5, 100, storedemo::StorageNodeHealth::kHealthy,
                     storedemo::StorageNodeDiskPressure::kLow, false, 1024, 960);
        RegisterNode(6, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-invalid-task", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(128, "rebalance-invalid-task");

        std::uint64_t now_unix_ms = 110;
        storedemo::RebalanceManager manager(
            &registry_,
            storedemo::RebalanceManagerConfig{
                .max_active_tasks = 8,
                .max_tasks = 8,
                .now_unix_ms = [&now_unix_ms]()
                { return now_unix_ms; }});

        auto missing_chunk_request = MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(6));
        missing_chunk_request.identity = {};
        const auto missing_chunk_result = manager.SubmitTask(missing_chunk_request);
        EXPECT_EQ(missing_chunk_result.code,
                  storedemo::RebalanceManagerSubmitCode::kInvalidArgument);

        auto missing_checksum_request = MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(6));
        missing_checksum_request.expected_checksum = {};
        const auto missing_checksum_result =
            manager.SubmitTask(missing_checksum_request);
        EXPECT_EQ(missing_checksum_result.code,
                  storedemo::RebalanceManagerSubmitCode::kInvalidArgument);

        auto missing_size_request = MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(6));
        missing_size_request.expected_size = 0;
        const auto missing_size_result = manager.SubmitTask(missing_size_request);
        EXPECT_EQ(missing_size_result.code,
                  storedemo::RebalanceManagerSubmitCode::kInvalidArgument);

        auto same_node_request = MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(1));
        const auto same_node_result = manager.SubmitTask(same_node_request);
        EXPECT_EQ(same_node_result.code,
                  storedemo::RebalanceManagerSubmitCode::kInvalidArgument);

        auto quarantined_source_request = MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(6),
            storedemo::RebalanceTaskReason::kCapacityImbalance,
            storedemo::ChunkState::kQuarantined);
        const auto quarantined_source_result =
            manager.SubmitTask(quarantined_source_request);
        EXPECT_EQ(quarantined_source_result.code,
                  storedemo::RebalanceManagerSubmitCode::kInvalidArgument);

        auto stale_source_request = MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::test::MakeStorageNodeIdFixture(6));
        const auto stale_source_result = manager.SubmitTask(stale_source_request);
        EXPECT_EQ(stale_source_result.code,
                  storedemo::RebalanceManagerSubmitCode::kInvalidArgument);

        auto unhealthy_target_request = MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(3));
        const auto unhealthy_target_result =
            manager.SubmitTask(unhealthy_target_request);
        EXPECT_EQ(unhealthy_target_result.code,
                  storedemo::RebalanceManagerSubmitCode::kInvalidArgument);

        auto overloaded_target_request = MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(4));
        const auto overloaded_target_result =
            manager.SubmitTask(overloaded_target_request);
        EXPECT_EQ(overloaded_target_result.code,
                  storedemo::RebalanceManagerSubmitCode::kOverloaded);

        auto low_capacity_target_request = MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(5));
        const auto low_capacity_target_result =
            manager.SubmitTask(low_capacity_target_request);
        EXPECT_EQ(low_capacity_target_result.code,
                  storedemo::RebalanceManagerSubmitCode::kInvalidArgument);
    }

    TEST_F(StorageRebalanceTest,
           ProductionRebalanceManagerDeduplicatesTracksLifecycleAndDoesNotAutoCopy)
    {
        auto &source_store = CreateStore(1);
        auto &target_store = CreateStore(2);
        (void)target_store;
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity_a = MakeIdentityOrThrow("rebalance-dup-a", 1, 0, 0);
        const auto identity_b = MakeIdentityOrThrow("rebalance-dup-b", 1, 0, 0);
        const auto payload_a = storedemo::test::MakeChunkPayload(96, "rebalance-dup-a");
        const auto payload_b = storedemo::test::MakeChunkPayload(96, "rebalance-dup-b");
        WriteReplica(source_store, identity_a, payload_a, "rebalance-dup-source-a");

        std::uint64_t now_unix_ms = 110;
        storedemo::RebalanceManager manager(
            &registry_,
            storedemo::RebalanceManagerConfig{
                .max_active_tasks = 2,
                .max_tasks = 2,
                .now_unix_ms = [&now_unix_ms]()
                { return now_unix_ms; }});

        const auto first_submit = manager.SubmitTask(MakeRebalanceTaskRequest(
            identity_a,
            payload_a,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::RebalanceTaskReason::kCapacityImbalance));
        ASSERT_TRUE(first_submit.accepted()) << first_submit.error_detail;
        ASSERT_TRUE(first_submit.task.has_value());

        const auto duplicate_submit = manager.SubmitTask(MakeRebalanceTaskRequest(
            identity_a,
            payload_a,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::RebalanceTaskReason::kCapacityImbalance));
        EXPECT_EQ(duplicate_submit.code,
                  storedemo::RebalanceManagerSubmitCode::kAlreadyExists);
        ASSERT_TRUE(duplicate_submit.task.has_value());
        EXPECT_EQ(duplicate_submit.task->task_id, first_submit.task->task_id);

        const auto second_submit = manager.SubmitTask(MakeRebalanceTaskRequest(
            identity_b,
            payload_b,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(3),
            storedemo::RebalanceTaskReason::kHotspot));
        ASSERT_TRUE(second_submit.accepted()) << second_submit.error_detail;
        ASSERT_TRUE(second_submit.task.has_value());

        const auto overloaded_submit = manager.SubmitTask(MakeRebalanceTaskRequest(
            MakeIdentityOrThrow("rebalance-overloaded-c", 1, 0, 0),
            payload_b,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::RebalanceTaskReason::kNewNodeJoin));
        EXPECT_EQ(overloaded_submit.code,
                  storedemo::RebalanceManagerSubmitCode::kOverloaded);

        const auto missing_target_stat = target_store.StatChunk(
            MakeStatRequest(identity_a.chunk_id, "rebalance-no-auto-copy-target"));
        EXPECT_EQ(missing_target_stat.status, storedemo::StorageNodeStatusCode::kNotFound);

        const auto listed_before = manager.ListTasks();
        ASSERT_EQ(listed_before.size(), 2U);
        EXPECT_TRUE(std::is_sorted(
            listed_before.begin(),
            listed_before.end(),
            [](const storedemo::RebalanceTask &lhs,
               const storedemo::RebalanceTask &rhs)
            { return lhs.task_id < rhs.task_id; }));
        EXPECT_TRUE(manager.FindTask(first_submit.task->task_id).has_value());

        now_unix_ms = 111;
        const auto running_result = manager.MarkTaskRunning(first_submit.task->task_id);
        ASSERT_TRUE(running_result.ok()) << running_result.error_detail;
        ASSERT_TRUE(running_result.task.has_value());
        EXPECT_EQ(running_result.task->state, storedemo::RebalanceTaskState::kRunning);
        EXPECT_EQ(running_result.task->attempts, 1U);

        const auto progress_result =
            manager.UpdateTaskProgress(first_submit.task->task_id, 35);
        ASSERT_TRUE(progress_result.ok()) << progress_result.error_detail;
        ASSERT_TRUE(progress_result.task.has_value());
        EXPECT_EQ(progress_result.task->progress_percent, 35U);

        now_unix_ms = 112;
        const auto complete_result = manager.CompleteTask(first_submit.task->task_id);
        EXPECT_EQ(complete_result.code,
                  storedemo::RebalanceTaskOperationCode::kConflict);
        ASSERT_TRUE(complete_result.task.has_value());
        EXPECT_EQ(complete_result.task->state,
                  storedemo::RebalanceTaskState::kRunning);

        const auto cancel_completed =
            manager.CancelTask(first_submit.task->task_id);
        EXPECT_EQ(cancel_completed.code,
                  storedemo::RebalanceTaskOperationCode::kConflict);

        const auto fail_result = manager.FailTask(second_submit.task->task_id,
                                                  storedemo::StorageNodeStatusCode::kIoError,
                                                  "manifest coordination is not wired",
                                                  false);
        ASSERT_TRUE(fail_result.ok()) << fail_result.error_detail;
        ASSERT_TRUE(fail_result.task.has_value());
        EXPECT_EQ(fail_result.task->state, storedemo::RebalanceTaskState::kFailed);
        EXPECT_EQ(fail_result.task->last_error,
                  storedemo::StorageNodeStatusCode::kIoError);
        EXPECT_EQ(fail_result.task->last_error_detail,
                  "manifest coordination is not wired");

        const auto retry_result = manager.RetryTask(second_submit.task->task_id);
        ASSERT_TRUE(retry_result.ok()) << retry_result.error_detail;
        ASSERT_TRUE(retry_result.task.has_value());
        EXPECT_EQ(retry_result.task->state,
                  storedemo::RebalanceTaskState::kRetryPending);

        now_unix_ms = 113;
        const auto cancel_retry_pending =
            manager.CancelTask(second_submit.task->task_id);
        ASSERT_TRUE(cancel_retry_pending.ok()) << cancel_retry_pending.error_detail;
        ASSERT_TRUE(cancel_retry_pending.task.has_value());
        EXPECT_EQ(cancel_retry_pending.task->state,
                  storedemo::RebalanceTaskState::kCancelled);

        const auto stats = manager.SnapshotStats();
        EXPECT_EQ(stats.submitted_tasks, 2U);
        EXPECT_EQ(stats.rejected_tasks, 2U);
        EXPECT_EQ(stats.completed_tasks, 0U);
        EXPECT_EQ(stats.cancelled_tasks, 1U);
        EXPECT_EQ(stats.failed_tasks, 0U);
        EXPECT_EQ(stats.total_attempts, 1U);
        EXPECT_EQ(stats.last_error_detail, "rebalance task cancelled");
    }

    TEST_F(StorageRebalanceTest,
           ProductionRebalanceManagerRunTaskCompletesHappyPathInOrder)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-run-happy", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(96, "rebalance-run-happy");
        WriteReplica(source_store, identity, payload, "rebalance-run-happy-source");
        WriteReplica(peer_store, identity, payload, "rebalance-run-happy-peer");

        TestOnlyManifestLedger manifest_ledger(MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2));
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        std::size_t manifest_calls = 0;
        std::size_t cleanup_candidate_calls = 0;
        std::size_t source_cleanup_calls = 0;
        std::uint64_t now_unix_ms = 110;
        auto config = MakeProductionRebalanceManagerConfig(RawStoreMap(),
                                                           &manifest_ledger,
                                                           &cleanup_candidates,
                                                           &cleanup_ledger,
                                                           &now_unix_ms,
                                                           &manifest_calls,
                                                           &cleanup_candidate_calls,
                                                           &source_cleanup_calls);
        storedemo::RebalanceManager manager(&registry_, std::move(config));

        const auto submit_result = manager.SubmitTask(MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(3),
            storedemo::RebalanceTaskReason::kCapacityImbalance));
        ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;

        const auto run_result = manager.RunTask(submit_result.task->task_id);
        ASSERT_EQ(run_result.status, storedemo::StorageNodeStatusCode::kOk)
            << run_result.error_detail;
        ASSERT_TRUE(run_result.task.has_value());
        EXPECT_TRUE(run_result.source_verified);
        EXPECT_TRUE(run_result.target_durable);
        EXPECT_TRUE(run_result.target_verified);
        EXPECT_TRUE(run_result.manifest_coordination_attempted);
        EXPECT_TRUE(run_result.manifest_updated);
        EXPECT_TRUE(run_result.source_cleanup_attempted);
        EXPECT_TRUE(run_result.source_cleanup_completed);
        EXPECT_FALSE(run_result.orphan_candidate_created);
        EXPECT_EQ(run_result.task->state, storedemo::RebalanceTaskState::kCompleted);
        EXPECT_EQ(run_result.task->progress_percent, 100U);
        EXPECT_TRUE(run_result.task->source_payload_verified);
        EXPECT_TRUE(run_result.task->target_durable);
        EXPECT_TRUE(run_result.task->target_verified);
        EXPECT_TRUE(run_result.task->manifest_coordinated);
        EXPECT_TRUE(run_result.task->source_cleanup_completed);
        EXPECT_EQ(manifest_calls, 1U);
        EXPECT_EQ(cleanup_candidate_calls, 0U);
        EXPECT_EQ(source_cleanup_calls, 1U);
        EXPECT_TRUE(cleanup_candidates.candidates().empty());

        const std::vector<storedemo::StorageNodeId> expected_manifest_nodes{
            storedemo::test::MakeStorageNodeIdFixture(2),
            storedemo::test::MakeStorageNodeIdFixture(3)};
        EXPECT_EQ(manifest_ledger.manifest().replica_nodes, expected_manifest_nodes);

        const auto source_stat = source_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-run-happy-source-stat"));
        ASSERT_EQ(source_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(source_stat.metadata.state, storedemo::ChunkState::kDeleted);

        const auto target_stat = target_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-run-happy-target-stat"));
        ASSERT_EQ(target_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_stat.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageRebalanceTest,
           ProductionRebalanceManagerRunTaskStopsAfterTargetDurableFailure)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto writer_state = std::make_shared<RecordingWriterState>();
        auto failing_durable_file =
            std::make_shared<RecordingDurableFile>(writer_state);
        failing_durable_file->publish_result.error =
            storedemo::DurableFileErrorCode::kIoError;
        auto &target_store = CreateStore(3, failing_durable_file);
        (void)target_store;
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-run-target-fail", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(96, "rebalance-run-target-fail");
        WriteReplica(source_store, identity, payload, "rebalance-run-target-fail-source");
        WriteReplica(peer_store, identity, payload, "rebalance-run-target-fail-peer");

        TestOnlyManifestLedger manifest_ledger(MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2));
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        std::size_t manifest_calls = 0;
        std::size_t cleanup_candidate_calls = 0;
        std::size_t source_cleanup_calls = 0;
        std::uint64_t now_unix_ms = 110;
        storedemo::RebalanceManager manager(
            &registry_,
            MakeProductionRebalanceManagerConfig(RawStoreMap(),
                                                 &manifest_ledger,
                                                 &cleanup_candidates,
                                                 &cleanup_ledger,
                                                 &now_unix_ms,
                                                 &manifest_calls,
                                                 &cleanup_candidate_calls,
                                                 &source_cleanup_calls));

        const auto submit_result = manager.SubmitTask(MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(3)));
        ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;

        const auto run_result = manager.RunTask(submit_result.task->task_id);
        EXPECT_EQ(run_result.status, storedemo::StorageNodeStatusCode::kIoError);
        ASSERT_TRUE(run_result.task.has_value());
        EXPECT_EQ(run_result.task->state, storedemo::RebalanceTaskState::kRetryPending);
        EXPECT_TRUE(run_result.source_verified);
        EXPECT_FALSE(run_result.target_durable);
        EXPECT_FALSE(run_result.target_verified);
        EXPECT_FALSE(run_result.manifest_coordination_attempted);
        EXPECT_FALSE(run_result.source_cleanup_attempted);
        EXPECT_EQ(manifest_calls, 0U);
        EXPECT_EQ(cleanup_candidate_calls, 0U);
        EXPECT_EQ(source_cleanup_calls, 0U);
    }

    TEST_F(StorageRebalanceTest,
           ProductionRebalanceManagerRunTaskStopsAfterVerifyFailure)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &target_store = CreateStore(3);
        (void)target_store;
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity = MakeIdentityOrThrow("rebalance-run-verify-fail", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(96, "rebalance-run-verify-fail");
        WriteReplica(source_store, identity, payload, "rebalance-run-verify-fail-source");
        WriteReplica(peer_store, identity, payload, "rebalance-run-verify-fail-peer");

        TestOnlyManifestLedger manifest_ledger(MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2));
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        std::size_t manifest_calls = 0;
        std::size_t cleanup_candidate_calls = 0;
        std::size_t source_cleanup_calls = 0;
        std::uint64_t now_unix_ms = 110;
        auto config = MakeProductionRebalanceManagerConfig(RawStoreMap(),
                                                           &manifest_ledger,
                                                           &cleanup_candidates,
                                                           &cleanup_ledger,
                                                           &now_unix_ms,
                                                           &manifest_calls,
                                                           &cleanup_candidate_calls,
                                                           &source_cleanup_calls);
        config.target_verifier = [](const storedemo::RebalanceTask &task,
                                    const storedemo::StorageTaskContext &)
        {
            storedemo::RebalanceTargetVerifyResult result;
            result.status = storedemo::StorageNodeStatusCode::kChecksumMismatch;
            result.error_detail = "target verify checksum mismatch";
            result.actual_checksum = task.expected_checksum;
            result.actual_size = task.expected_size;
            result.verified = false;
            return result;
        };
        storedemo::RebalanceManager manager(&registry_, std::move(config));

        const auto submit_result = manager.SubmitTask(MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(3)));
        ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;

        const auto run_result = manager.RunTask(submit_result.task->task_id);
        EXPECT_EQ(run_result.status,
                  storedemo::StorageNodeStatusCode::kChecksumMismatch);
        ASSERT_TRUE(run_result.task.has_value());
        EXPECT_EQ(run_result.task->state, storedemo::RebalanceTaskState::kFailed);
        EXPECT_TRUE(run_result.target_durable);
        EXPECT_FALSE(run_result.target_verified);
        EXPECT_FALSE(run_result.manifest_coordination_attempted);
        EXPECT_FALSE(run_result.source_cleanup_attempted);
        EXPECT_EQ(manifest_calls, 0U);
        EXPECT_EQ(cleanup_candidate_calls, 0U);
        EXPECT_EQ(source_cleanup_calls, 0U);
    }

    TEST_F(StorageRebalanceTest,
           ProductionRebalanceManagerRunTaskRecordsCleanupCandidateWhenManifestFails)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity =
            MakeIdentityOrThrow("rebalance-run-manifest-fail", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(96, "rebalance-run-manifest-fail");
        WriteReplica(source_store,
                     identity,
                     payload,
                     "rebalance-run-manifest-fail-source");
        WriteReplica(peer_store,
                     identity,
                     payload,
                     "rebalance-run-manifest-fail-peer");

        TestOnlyManifestLedger manifest_ledger(MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2));
        manifest_ledger.FailNextUpdate(storedemo::StorageNodeStatusCode::kConflict,
                                       "manifest coordination rejected move");
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        std::size_t manifest_calls = 0;
        std::size_t cleanup_candidate_calls = 0;
        std::size_t source_cleanup_calls = 0;
        std::uint64_t now_unix_ms = 110;
        storedemo::RebalanceManager manager(
            &registry_,
            MakeProductionRebalanceManagerConfig(RawStoreMap(),
                                                 &manifest_ledger,
                                                 &cleanup_candidates,
                                                 &cleanup_ledger,
                                                 &now_unix_ms,
                                                 &manifest_calls,
                                                 &cleanup_candidate_calls,
                                                 &source_cleanup_calls));

        const auto submit_result = manager.SubmitTask(MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(3)));
        ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;

        const auto run_result = manager.RunTask(submit_result.task->task_id);
        EXPECT_EQ(run_result.status, storedemo::StorageNodeStatusCode::kConflict);
        ASSERT_TRUE(run_result.task.has_value());
        EXPECT_EQ(run_result.task->state, storedemo::RebalanceTaskState::kFailed);
        EXPECT_TRUE(run_result.target_durable);
        EXPECT_TRUE(run_result.target_verified);
        EXPECT_TRUE(run_result.manifest_coordination_attempted);
        EXPECT_FALSE(run_result.manifest_updated);
        EXPECT_FALSE(run_result.source_cleanup_attempted);
        EXPECT_TRUE(run_result.orphan_candidate_created);
        EXPECT_EQ(manifest_calls, 1U);
        EXPECT_EQ(cleanup_candidate_calls, 1U);
        EXPECT_EQ(source_cleanup_calls, 0U);
        EXPECT_TRUE(cleanup_candidates.HasCandidate(
            identity.chunk_id, storedemo::test::MakeStorageNodeIdFixture(3)));

        const auto source_stat = source_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-run-manifest-fail-source-stat"));
        ASSERT_EQ(source_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(source_stat.metadata.state, storedemo::ChunkState::kLive);

        const auto target_stat = target_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-run-manifest-fail-target-stat"));
        ASSERT_EQ(target_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_stat.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageRebalanceTest,
           ProductionRebalanceManagerRunTaskReturnsCleanupRetryableAndCanResume)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &target_store = CreateStore(3);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);

        const auto identity =
            MakeIdentityOrThrow("rebalance-run-cleanup-retry", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(96, "rebalance-run-cleanup-retry");
        WriteReplica(source_store,
                     identity,
                     payload,
                     "rebalance-run-cleanup-retry-source");
        WriteReplica(peer_store,
                     identity,
                     payload,
                     "rebalance-run-cleanup-retry-peer");

        TestOnlyManifestLedger manifest_ledger(MakeManifest(
            identity,
            payload,
            {storedemo::test::MakeStorageNodeIdFixture(1),
             storedemo::test::MakeStorageNodeIdFixture(2)},
            2));
        TestOnlyCleanupCandidateLedger cleanup_candidates;
        TestOnlySourceCleanupLedger cleanup_ledger;
        cleanup_ledger.FailCleanupFor(storedemo::test::MakeStorageNodeIdFixture(1),
                                      storedemo::StorageNodeStatusCode::kIoError,
                                      "cleanup retry required");
        std::size_t manifest_calls = 0;
        std::size_t cleanup_candidate_calls = 0;
        std::size_t source_cleanup_calls = 0;
        std::uint64_t now_unix_ms = 110;
        storedemo::RebalanceManager manager(
            &registry_,
            MakeProductionRebalanceManagerConfig(RawStoreMap(),
                                                 &manifest_ledger,
                                                 &cleanup_candidates,
                                                 &cleanup_ledger,
                                                 &now_unix_ms,
                                                 &manifest_calls,
                                                 &cleanup_candidate_calls,
                                                 &source_cleanup_calls));

        const auto submit_result = manager.SubmitTask(MakeRebalanceTaskRequest(
            identity,
            payload,
            storedemo::test::MakeStorageNodeIdFixture(1),
            storedemo::test::MakeStorageNodeIdFixture(3)));
        ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;

        const auto first_run = manager.RunTask(submit_result.task->task_id);
        EXPECT_EQ(first_run.status, storedemo::StorageNodeStatusCode::kIoError);
        ASSERT_TRUE(first_run.task.has_value());
        EXPECT_EQ(first_run.task->state, storedemo::RebalanceTaskState::kRetryPending);
        EXPECT_TRUE(first_run.target_durable);
        EXPECT_TRUE(first_run.target_verified);
        EXPECT_TRUE(first_run.manifest_updated);
        EXPECT_TRUE(first_run.source_cleanup_attempted);
        EXPECT_FALSE(first_run.source_cleanup_completed);
        EXPECT_EQ(manifest_calls, 1U);
        EXPECT_EQ(source_cleanup_calls, 1U);

        cleanup_ledger = TestOnlySourceCleanupLedger{};
        now_unix_ms = 111;
        const auto second_run = manager.RunTask(submit_result.task->task_id);
        EXPECT_EQ(second_run.status, storedemo::StorageNodeStatusCode::kOk);
        ASSERT_TRUE(second_run.task.has_value());
        EXPECT_EQ(second_run.task->state, storedemo::RebalanceTaskState::kCompleted);
        EXPECT_TRUE(second_run.source_cleanup_attempted);
        EXPECT_TRUE(second_run.source_cleanup_completed);
        EXPECT_EQ(manifest_calls, 1U);
        EXPECT_EQ(source_cleanup_calls, 2U);
        EXPECT_EQ(cleanup_candidate_calls, 0U);

        const auto source_stat = source_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-run-cleanup-retry-source-stat"));
        ASSERT_EQ(source_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(source_stat.metadata.state, storedemo::ChunkState::kDeleted);

        const auto target_stat = target_store.StatChunk(
            MakeStatRequest(identity.chunk_id, "rebalance-run-cleanup-retry-target-stat"));
        ASSERT_EQ(target_stat.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_EQ(target_stat.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageRebalanceTest,
           ProductionRebalanceManagerRunTaskHandlesAlreadyExistsSourceMismatchAndIdempotency)
    {
        auto &source_store = CreateStore(1);
        auto &peer_store = CreateStore(2);
        auto &idempotent_target_store = CreateStore(3);
        auto &conflict_target_store = CreateStore(4);
        RegisterNode(1, 100);
        RegisterNode(2, 100);
        RegisterNode(3, 100);
        RegisterNode(4, 100);

        const auto idempotent_identity =
            MakeIdentityOrThrow("rebalance-run-idempotent", 1, 0, 0);
        const auto conflict_identity =
            MakeIdentityOrThrow("rebalance-run-conflict", 1, 0, 0);
        const auto mismatch_identity =
            MakeIdentityOrThrow("rebalance-run-source-mismatch", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(96, "rebalance-run-idempotent");
        const auto mismatch_payload =
            storedemo::test::MakeChunkPayload(96, "rebalance-run-source-mismatch");

        WriteReplica(source_store, idempotent_identity, payload, "rebalance-idempotent-source");
        WriteReplica(peer_store, idempotent_identity, payload, "rebalance-idempotent-peer");
        WriteReplica(idempotent_target_store, idempotent_identity, payload, "rebalance-idempotent-target");

        WriteReplica(source_store, conflict_identity, payload, "rebalance-conflict-source");
        WriteReplica(peer_store, conflict_identity, payload, "rebalance-conflict-peer");
        WriteReplica(conflict_target_store,
                     conflict_identity,
                     storedemo::test::MakeChunkPayload(96, "rebalance-conflict-target"),
                     "rebalance-conflict-target");

        WriteReplica(source_store, mismatch_identity, mismatch_payload, "rebalance-mismatch-source");
        WriteReplica(peer_store, mismatch_identity, mismatch_payload, "rebalance-mismatch-peer");
        TamperReplica(source_store,
                      mismatch_identity,
                      storedemo::test::MakeChunkPayload(mismatch_payload.size(),
                                                        "rebalance-mismatch-bad"));

        {
            TestOnlyManifestLedger manifest_ledger(MakeManifest(
                idempotent_identity,
                payload,
                {storedemo::test::MakeStorageNodeIdFixture(1),
                 storedemo::test::MakeStorageNodeIdFixture(2)},
                2));
            TestOnlyCleanupCandidateLedger cleanup_candidates;
            TestOnlySourceCleanupLedger cleanup_ledger;
            std::size_t manifest_calls = 0;
            std::size_t cleanup_candidate_calls = 0;
            std::size_t source_cleanup_calls = 0;
            std::uint64_t now_unix_ms = 110;
            storedemo::RebalanceManager manager(
                &registry_,
                MakeProductionRebalanceManagerConfig(RawStoreMap(),
                                                     &manifest_ledger,
                                                     &cleanup_candidates,
                                                     &cleanup_ledger,
                                                     &now_unix_ms,
                                                     &manifest_calls,
                                                     &cleanup_candidate_calls,
                                                     &source_cleanup_calls));

            const auto submit_result = manager.SubmitTask(MakeRebalanceTaskRequest(
                idempotent_identity,
                payload,
                storedemo::test::MakeStorageNodeIdFixture(1),
                storedemo::test::MakeStorageNodeIdFixture(3)));
            ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;

            const auto first_run = manager.RunTask(submit_result.task->task_id);
            EXPECT_EQ(first_run.status, storedemo::StorageNodeStatusCode::kOk);
            ASSERT_TRUE(first_run.task.has_value());
            EXPECT_TRUE(first_run.target_durable);
            EXPECT_TRUE(first_run.target_already_exists);
            EXPECT_EQ(first_run.task->state, storedemo::RebalanceTaskState::kCompleted);
            EXPECT_EQ(manifest_calls, 1U);
            EXPECT_EQ(source_cleanup_calls, 1U);

            const auto second_run = manager.RunTask(submit_result.task->task_id);
            EXPECT_EQ(second_run.status, storedemo::StorageNodeStatusCode::kOk);
            EXPECT_TRUE(second_run.idempotent_success);
            EXPECT_EQ(manifest_calls, 1U);
            EXPECT_EQ(source_cleanup_calls, 1U);
            EXPECT_EQ(cleanup_candidate_calls, 0U);
        }

        {
            TestOnlyManifestLedger manifest_ledger(MakeManifest(
                conflict_identity,
                payload,
                {storedemo::test::MakeStorageNodeIdFixture(1),
                 storedemo::test::MakeStorageNodeIdFixture(2)},
                2));
            TestOnlyCleanupCandidateLedger cleanup_candidates;
            TestOnlySourceCleanupLedger cleanup_ledger;
            std::size_t manifest_calls = 0;
            std::size_t cleanup_candidate_calls = 0;
            std::size_t source_cleanup_calls = 0;
            std::uint64_t now_unix_ms = 110;
            storedemo::RebalanceManager manager(
                &registry_,
                MakeProductionRebalanceManagerConfig(RawStoreMap(),
                                                     &manifest_ledger,
                                                     &cleanup_candidates,
                                                     &cleanup_ledger,
                                                     &now_unix_ms,
                                                     &manifest_calls,
                                                     &cleanup_candidate_calls,
                                                     &source_cleanup_calls));

            const auto submit_result = manager.SubmitTask(MakeRebalanceTaskRequest(
                conflict_identity,
                payload,
                storedemo::test::MakeStorageNodeIdFixture(1),
                storedemo::test::MakeStorageNodeIdFixture(4)));
            ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;

            const auto run_result = manager.RunTask(submit_result.task->task_id);
            EXPECT_EQ(run_result.status, storedemo::StorageNodeStatusCode::kConflict);
            ASSERT_TRUE(run_result.task.has_value());
            EXPECT_EQ(run_result.task->state, storedemo::RebalanceTaskState::kFailed);
            EXPECT_FALSE(run_result.target_durable);
            EXPECT_FALSE(run_result.manifest_coordination_attempted);
            EXPECT_FALSE(run_result.source_cleanup_attempted);
            EXPECT_EQ(manifest_calls, 0U);
            EXPECT_EQ(source_cleanup_calls, 0U);
            EXPECT_EQ(cleanup_candidate_calls, 0U);
        }

        {
            TestOnlyManifestLedger manifest_ledger(MakeManifest(
                mismatch_identity,
                mismatch_payload,
                {storedemo::test::MakeStorageNodeIdFixture(1),
                 storedemo::test::MakeStorageNodeIdFixture(2)},
                2));
            TestOnlyCleanupCandidateLedger cleanup_candidates;
            TestOnlySourceCleanupLedger cleanup_ledger;
            std::size_t manifest_calls = 0;
            std::size_t cleanup_candidate_calls = 0;
            std::size_t source_cleanup_calls = 0;
            std::uint64_t now_unix_ms = 110;
            auto config = MakeProductionRebalanceManagerConfig(RawStoreMap(),
                                                               &manifest_ledger,
                                                               &cleanup_candidates,
                                                               &cleanup_ledger,
                                                               &now_unix_ms,
                                                               &manifest_calls,
                                                               &cleanup_candidate_calls,
                                                               &source_cleanup_calls);
            config.source_reader = [mismatch_identity,
                                    mismatch_payload](const storedemo::RebalanceTask &task,
                                                      const storedemo::StorageTaskContext &)
            {
                storedemo::RebalanceSourceReadResult result;
                result.status = storedemo::StorageNodeStatusCode::kChecksumMismatch;
                result.error_detail = "source checksum mismatch";
                result.metadata.identity = mismatch_identity;
                result.metadata.node_id = task.source_node;
                result.metadata.size =
                    static_cast<std::uint64_t>(mismatch_payload.size());
                result.metadata.state = storedemo::ChunkState::kLive;
                result.actual_checksum = ComputeChecksumOrThrow(
                    storedemo::test::MakeChunkPayload(mismatch_payload.size(),
                                                      "rebalance-mismatch-bad"));
                result.payload = mismatch_payload;
                result.verified = false;
                return result;
            };
            storedemo::RebalanceManager manager(&registry_, std::move(config));

            const auto submit_result = manager.SubmitTask(MakeRebalanceTaskRequest(
                mismatch_identity,
                mismatch_payload,
                storedemo::test::MakeStorageNodeIdFixture(1),
                storedemo::test::MakeStorageNodeIdFixture(3)));
            ASSERT_TRUE(submit_result.accepted()) << submit_result.error_detail;

            const auto run_result = manager.RunTask(submit_result.task->task_id);
            EXPECT_EQ(run_result.status,
                      storedemo::StorageNodeStatusCode::kChecksumMismatch);
            ASSERT_TRUE(run_result.task.has_value());
            EXPECT_EQ(run_result.task->state, storedemo::RebalanceTaskState::kFailed);
            EXPECT_FALSE(run_result.target_durable);
            EXPECT_FALSE(run_result.manifest_coordination_attempted);
            EXPECT_FALSE(run_result.source_cleanup_attempted);
            EXPECT_EQ(manifest_calls, 0U);
            EXPECT_EQ(source_cleanup_calls, 0U);
            EXPECT_EQ(cleanup_candidate_calls, 0U);
        }
    }
}
