#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "raft/state_machine/metadata_state_machine.h"
#include "store/node/storage_node_client.h"
#include "store/placement/replica_policy.h"
#include "support/storage_upload_test_utils.h"

namespace storedemo::test
{
    class CountingReplicaReader
    {
    public:
        using Handler = std::function<ReadChunkResponse(
            const StorageNodeId &node_id,
            const ReadChunkRequest &request)>;

        explicit CountingReplicaReader(Handler handler)
            : handler_(std::move(handler))
        {
        }

        ReadChunkResponse ReadChunk(const StorageNodeId &node_id,
                                    const ReadChunkRequest &request)
        {
            ++read_calls_;
            read_node_ids_.push_back(node_id);
            read_chunk_ids_.push_back(request.chunk_id);
            ++per_node_calls_[node_id];
            return handler_(node_id, request);
        }

        [[nodiscard]] std::size_t read_calls() const
        {
            return read_calls_;
        }

        [[nodiscard]] const std::vector<std::string> &read_chunk_ids() const
        {
            return read_chunk_ids_;
        }

        [[nodiscard]] const std::vector<std::string> &read_node_ids() const
        {
            return read_node_ids_;
        }

        [[nodiscard]] std::size_t calls_for_node(const StorageNodeId &node_id) const
        {
            const auto it = per_node_calls_.find(node_id);
            return it == per_node_calls_.end() ? 0U : it->second;
        }

    private:
        Handler handler_;
        std::size_t read_calls_{0};
        std::vector<std::string> read_node_ids_;
        std::vector<std::string> read_chunk_ids_;
        std::unordered_map<std::string, std::size_t> per_node_calls_;
    };

    using ReadReplicaCandidateResolver =
        std::function<std::vector<ReadReplicaCandidate>(const raftdemo::ChunkRef &)>;
    using ReadReplicaRegistrySnapshotResolver =
        std::function<StorageNodeRegistrySnapshotResult()>;

    struct ReadObjectByManifestRequest
    {
        std::string bucket;
        std::string object_key;
        std::string request_id_prefix{"storage-read"};
        ReadReplicaCandidateResolver candidate_resolver;
        ReadReplicaRegistrySnapshotResolver registry_snapshot_resolver;
    };

    struct ReadObjectByManifestResult
    {
        StorageNodeStatusCode status{StorageNodeStatusCode::kOk};
        std::string error_detail;
        std::string payload;
        std::vector<ReadReplicaFallbackResult> chunk_results;
    };

    inline ReadObjectByManifestResult ReadObjectByManifest(
        const raftdemo::MetadataStateMachine &machine,
        CountingReplicaReader &reader,
        const ReadObjectByManifestRequest &request)
    {
        const auto head = machine.HeadObject(
            {.bucket = request.bucket, .object_key = request.object_key});
        if (head.result.code != raftdemo::MetadataStatusCode::kOk ||
            !head.record.has_value() ||
            !head.record->IsCommitted())
        {
            return ReadObjectByManifestResult{
                .status = MapMetadataStatusCode(head.result.code),
                .error_detail = head.result.summary.message};
        }

        const auto manifest = machine.FindChunkRefs(request.bucket, request.object_key);
        if (!manifest.has_value())
        {
            return ReadObjectByManifestResult{
                .status = StorageNodeStatusCode::kNotFound,
                .error_detail = "committed object manifest not found"};
        }

        std::vector<raftdemo::ChunkRef> ordered_manifest = *manifest;
        std::stable_sort(ordered_manifest.begin(),
                         ordered_manifest.end(),
                         [](const raftdemo::ChunkRef &lhs, const raftdemo::ChunkRef &rhs)
                         {
                             if (lhs.offset != rhs.offset)
                             {
                                 return lhs.offset < rhs.offset;
                             }
                             return lhs.chunk_id < rhs.chunk_id;
                         });

        ReadObjectByManifestResult result;
        result.chunk_results.reserve(ordered_manifest.size());
        ReplicaPolicySelector selector;
        std::optional<StorageNodeRegistrySnapshotResult> registry_snapshot;
        if (request.registry_snapshot_resolver)
        {
            registry_snapshot = request.registry_snapshot_resolver();
        }
        for (std::size_t index = 0; index < ordered_manifest.size(); ++index)
        {
            const auto &chunk_ref = ordered_manifest.at(index);
            if (chunk_ref.replica_nodes.empty())
            {
                return ReadObjectByManifestResult{
                    .status = StorageNodeStatusCode::kInvalidArgument,
                    .error_detail = "manifest chunk is missing replica_nodes"};
            }

            std::vector<ReadReplicaCandidate> read_candidates;
            if (request.candidate_resolver)
            {
                read_candidates = request.candidate_resolver(chunk_ref);
            }

            const auto selection =
                registry_snapshot.has_value()
                    ? selector.SelectReadReplicas(
                          ReadReplicaSelectionRequest{
                              .chunk_id = chunk_ref.chunk_id,
                              .replica_nodes = chunk_ref.replica_nodes},
                          *registry_snapshot,
                          read_candidates)
                    : selector.SelectReadReplicas(
                          ReadReplicaSelectionRequest{
                              .chunk_id = chunk_ref.chunk_id,
                              .replica_nodes = chunk_ref.replica_nodes},
                          read_candidates);
            if (!selection.ok())
            {
                return ReadObjectByManifestResult{
                    .status = selection.status,
                    .error_detail = selection.error_detail};
            }

            std::vector<StorageNodeId> ordered_replicas;
            ordered_replicas.reserve(selection.decision.ordered_replicas.size());
            for (const auto &candidate : selection.decision.ordered_replicas)
            {
                ordered_replicas.push_back(candidate.node_id);
            }

            const auto read_request = MakeReadChunkRequestForCommittedManifestReplica(
                request.request_id_prefix + "-" + std::to_string(index),
                chunk_ref.chunk_id,
                chunk_ref.size,
                chunk_ref.checksum);
            const auto fallback = ReadChunkWithReplicaFallback(
                ordered_replicas,
                read_request,
                {},
                [&](const StorageNodeId &node_id,
                    const ReadChunkRequest &chunk_request,
                    const StorageNodeClientReadChunkOptions &)
                {
                    return reader.ReadChunk(node_id, chunk_request);
                });
            result.chunk_results.push_back(fallback);

            const auto &read = result.chunk_results.back().response;
            if (read.status != StorageNodeStatusCode::kOk)
            {
                result.status = read.status;
                result.error_detail = read.error_detail;
                return result;
            }

            if (read.metadata.size != chunk_ref.size)
            {
                result.status = StorageNodeStatusCode::kCorrupted;
                result.error_detail =
                    "manifest size does not match local chunk facts";
                return result;
            }

            if (read.metadata.checksum.value != chunk_ref.checksum)
            {
                result.status = StorageNodeStatusCode::kChecksumMismatch;
                result.error_detail =
                    "manifest checksum does not match local chunk facts";
                return result;
            }

            result.payload.append(read.payload);
        }

        return result;
    }
}
