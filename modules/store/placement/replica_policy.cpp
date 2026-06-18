#include "store/placement/replica_policy.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "store/node/storage_node_registry.h"

namespace storedemo
{
    namespace
    {
        constexpr std::uint64_t kFnv1a64OffsetBasis = 1469598103934665603ULL;
        constexpr std::uint64_t kFnv1a64Prime = 1099511628211ULL;
        constexpr std::uint64_t kMinimumCapacityTierQuantumBytes = 1024ULL;
        constexpr std::uint64_t kInflightTierQuantum = 2ULL;
        constexpr std::uint64_t kActiveWritesTierQuantum = 2ULL;
        constexpr std::uint64_t kActiveReadsTierQuantum = 2ULL;

        struct RankedCandidate
        {
            StorageNodePlacementCandidate candidate;
            std::size_t original_index{0};
            std::uint64_t capacity_tier{0};
            std::uint64_t inflight_tier{0};
            std::uint64_t active_writes_tier{0};
            std::uint64_t active_reads_tier{0};
            std::uint64_t chunk_scoped_jitter{0};
            std::uint64_t node_identity_hash{0};
        };

        struct RankedReadReplicaCandidate
        {
            ReadReplicaCandidate candidate;
            std::size_t manifest_index{0};
        };

        template <typename Decision>
        void AddExclusion(Decision *decision,
                          std::string_view node_id,
                          std::string reason)
        {
            if (decision == nullptr)
            {
                return;
            }
            decision->excluded_nodes.push_back(
                PlacementNodeExclusion{.node_id = std::string(node_id),
                                       .reason = std::move(reason)});
        }

        void AppendHashBytes(std::uint64_t *hash, std::string_view bytes)
        {
            if (hash == nullptr)
            {
                return;
            }

            for (const unsigned char byte : bytes)
            {
                *hash ^= static_cast<std::uint64_t>(byte);
                *hash *= kFnv1a64Prime;
            }
        }

        template <typename UInt>
        void AppendHashInteger(std::uint64_t *hash, const UInt value)
        {
            for (std::size_t index = 0; index < sizeof(UInt); ++index)
            {
                const auto byte = static_cast<unsigned char>(
                    (value >> (index * 8U)) & 0xFFU);
                *hash ^= static_cast<std::uint64_t>(byte);
                *hash *= kFnv1a64Prime;
            }
        }

        std::uint64_t ComputeStableNodeIdentityHash(std::string_view node_id)
        {
            std::uint64_t hash = kFnv1a64OffsetBasis;
            AppendHashBytes(&hash, "placement-node");
            AppendHashBytes(&hash, node_id);
            return hash;
        }

        std::uint64_t ComputeChunkScopedJitter(std::string_view chunk_id,
                                               const std::uint64_t decision_epoch,
                                               std::string_view node_id)
        {
            std::uint64_t hash = kFnv1a64OffsetBasis;
            AppendHashBytes(&hash, "placement-jitter");
            AppendHashBytes(&hash, chunk_id);
            AppendHashInteger(&hash, decision_epoch);
            AppendHashBytes(&hash, node_id);
            return hash;
        }

        std::uint64_t ResolveCapacityTierQuantumBytes(const PlacementRequest &request)
        {
            return std::max(kMinimumCapacityTierQuantumBytes,
                            request.chunk_size_bytes);
        }

        std::uint64_t ComputeCapacityTier(
            const StorageNodePlacementCandidate &candidate,
            const PlacementRequest &request)
        {
            const std::uint64_t required_with_reserve =
                request.chunk_size_bytes + request.policy.reserve_capacity_bytes;
            const std::uint64_t post_write_headroom =
                candidate.available_capacity_bytes > required_with_reserve
                    ? candidate.available_capacity_bytes - required_with_reserve
                    : 0ULL;
            return post_write_headroom / ResolveCapacityTierQuantumBytes(request);
        }

        std::uint64_t ComputeTier(const std::uint64_t value,
                                  const std::uint64_t quantum)
        {
            if (quantum == 0)
            {
                return value;
            }
            return value / quantum;
        }

        StorageNodeStatusCode ResolveChunkId(const ChunkIdentity &identity,
                                             ChunkId *out_chunk_id,
                                             std::string *error_detail)
        {
            if (out_chunk_id == nullptr)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "chunk_id output must not be null";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (!identity.chunk_id.empty())
            {
                *out_chunk_id = identity.chunk_id;
                return ValidateChunkId(identity.chunk_id, error_detail);
            }

            if (identity.object_id.empty() || identity.version == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "placement request must include chunk_id or full identity";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return MakeChunkId(identity.object_id,
                               identity.version,
                               identity.chunk_index,
                               out_chunk_id,
                               error_detail);
        }

        std::optional<std::string> EvaluateCandidateEligibility(
            const StorageNodePlacementCandidate &candidate,
            const PlacementRequest &request,
            const std::unordered_set<std::string> &explicit_excluded)
        {
            if (candidate.node_id.empty())
            {
                return "candidate node_id must not be empty";
            }

            if (explicit_excluded.contains(candidate.node_id))
            {
                return "node is explicitly excluded";
            }

            if (!candidate.HasWritableHealth())
            {
                return std::string("node health is not writable: ") +
                       ToString(candidate.health);
            }

            if (candidate.disk_pressure == StorageNodeDiskPressure::kHigh ||
                candidate.disk_pressure == StorageNodeDiskPressure::kFull)
            {
                return std::string("node disk pressure is too high: ") +
                       ToString(candidate.disk_pressure);
            }

            if (candidate.write_admission_overloaded)
            {
                return "node write admission is overloaded";
            }

            if (!candidate.CanFit(request.chunk_size_bytes,
                                  request.policy.reserve_capacity_bytes))
            {
                return "node capacity is insufficient for requested chunk";
            }

            return std::nullopt;
        }

        bool PreferDistinctZone(const StorageNodePlacementCandidate &candidate,
                                const std::unordered_set<std::string> &selected_zones)
        {
            return !candidate.zone.empty() && !selected_zones.contains(candidate.zone);
        }

        std::optional<std::string> EvaluateReadReplicaEligibility(
            const ReadReplicaCandidate &candidate,
            const std::unordered_set<std::string> &explicit_excluded)
        {
            if (candidate.node_id.empty())
            {
                return "replica node_id must not be empty";
            }

            if (explicit_excluded.contains(candidate.node_id))
            {
                return "node is explicitly excluded";
            }

            if (candidate.known_corrupted)
            {
                return "node is marked corrupted for read";
            }

            if (candidate.known_missing)
            {
                return "node is known missing requested chunk";
            }

            if (candidate.stale)
            {
                return "node facts are stale";
            }

            if (candidate.read_admission_overloaded)
            {
                return "node read admission is overloaded";
            }

            if (candidate.health == StorageNodeHealth::kUnavailable ||
                candidate.health == StorageNodeHealth::kDraining)
            {
                return std::string("node health is not readable: ") +
                       ToString(candidate.health);
            }

            return std::nullopt;
        }

        std::uint8_t ReadHealthRank(const StorageNodeHealth health)
        {
            switch (health)
            {
            case StorageNodeHealth::kHealthy:
                return 0;
            case StorageNodeHealth::kReadOnly:
                return 1;
            case StorageNodeHealth::kDegraded:
                return 2;
            case StorageNodeHealth::kUnavailable:
                return 3;
            case StorageNodeHealth::kDraining:
                return 4;
            }

            return 5;
        }

        std::uint8_t ReadDiskPressureRank(const StorageNodeDiskPressure pressure)
        {
            switch (pressure)
            {
            case StorageNodeDiskPressure::kLow:
                return 0;
            case StorageNodeDiskPressure::kMedium:
                return 1;
            case StorageNodeDiskPressure::kHigh:
                return 2;
            case StorageNodeDiskPressure::kFull:
                return 3;
            }

            return 4;
        }

        ReadReplicaCandidate BuildReadReplicaCandidateFromRegistrySnapshot(
            const StorageNodeRegistryNodeSnapshot &snapshot)
        {
            ReadReplicaCandidate candidate;
            candidate.node_id = snapshot.node_id;
            candidate.health = snapshot.facts.health.health;
            candidate.disk_pressure = snapshot.facts.health.disk_pressure;
            candidate.load = snapshot.facts.load.load;
            candidate.stale = snapshot.liveness != StorageNodeRegistryLiveness::kLive;
            candidate.read_admission_overloaded =
                snapshot.facts.load.read_admission_overloaded;
            candidate.has_observed_facts = true;
            return candidate;
        }

        ReadReplicaCandidate MergeReadReplicaCandidates(
            const ReadReplicaCandidate &registry_candidate,
            const ReadReplicaCandidate &supplemental_candidate)
        {
            auto merged = registry_candidate;
            merged.known_corrupted = registry_candidate.known_corrupted ||
                                     supplemental_candidate.known_corrupted;
            merged.known_missing = registry_candidate.known_missing ||
                                   supplemental_candidate.known_missing;
            merged.stale = registry_candidate.stale || supplemental_candidate.stale;
            merged.read_admission_overloaded =
                registry_candidate.read_admission_overloaded ||
                supplemental_candidate.read_admission_overloaded;
            merged.has_observed_facts = registry_candidate.has_observed_facts ||
                                        supplemental_candidate.has_observed_facts;
            return merged;
        }
    }

    const char *ToString(const StorageNodeHealth health)
    {
        switch (health)
        {
        case StorageNodeHealth::kHealthy:
            return "Healthy";
        case StorageNodeHealth::kDegraded:
            return "Degraded";
        case StorageNodeHealth::kReadOnly:
            return "ReadOnly";
        case StorageNodeHealth::kUnavailable:
            return "Unavailable";
        case StorageNodeHealth::kDraining:
            return "Draining";
        }

        return "Unknown";
    }

    const char *ToString(const StorageNodeDiskPressure pressure)
    {
        switch (pressure)
        {
        case StorageNodeDiskPressure::kLow:
            return "Low";
        case StorageNodeDiskPressure::kMedium:
            return "Medium";
        case StorageNodeDiskPressure::kHigh:
            return "High";
        case StorageNodeDiskPressure::kFull:
            return "Full";
        }

        return "Unknown";
    }

    std::uint64_t StorageNodeLoadSnapshot::TotalInflight() const
    {
        return static_cast<std::uint64_t>(active_reads) +
               static_cast<std::uint64_t>(active_writes) +
               static_cast<std::uint64_t>(queued_ops);
    }

    bool StorageNodePlacementCandidate::CanFit(const std::uint64_t required_bytes,
                                               const std::uint64_t reserve_bytes) const
    {
        if (required_bytes > available_capacity_bytes)
        {
            return false;
        }

        return available_capacity_bytes - required_bytes >= reserve_bytes;
    }

    bool StorageNodePlacementCandidate::HasWritableHealth() const
    {
        return health == StorageNodeHealth::kHealthy;
    }

    PlacementDecisionResult ReplicaPolicySelector::SelectReplicas(
        const PlacementRequest &request,
        const std::span<const StorageNodePlacementCandidate> candidates) const
    {
        PlacementDecisionResult result;
        result.decision.required_replica_count = request.policy.replica_count;
        result.decision.minimum_successful_writes =
            request.policy.minimum_successful_writes;
        result.decision.decision_epoch = request.decision_epoch;

        result.status = ResolveChunkId(request.identity,
                                       &result.decision.chunk_id,
                                       &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        if (request.chunk_size_bytes == 0)
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "placement request chunk_size_bytes must be greater than zero";
            return result;
        }

        if (request.policy.replica_count == 0)
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "replica_count must be greater than zero";
            return result;
        }

        if (request.policy.minimum_successful_writes == 0)
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "minimum_successful_writes must be greater than zero";
            return result;
        }

        if (request.policy.minimum_successful_writes > request.policy.replica_count)
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail =
                "minimum_successful_writes must not exceed replica_count";
            return result;
        }

        if (candidates.empty())
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "placement requires at least one candidate node";
            return result;
        }

        std::unordered_set<std::string> explicit_excluded(
            request.excluded_nodes.begin(),
            request.excluded_nodes.end());

        std::vector<RankedCandidate> eligible_candidates;
        eligible_candidates.reserve(candidates.size());

        for (std::size_t index = 0; index < candidates.size(); ++index)
        {
            const auto &candidate = candidates[index];
            const auto rejection_reason =
                EvaluateCandidateEligibility(candidate, request, explicit_excluded);
            if (rejection_reason.has_value())
            {
                AddExclusion(&result.decision, candidate.node_id, *rejection_reason);
                continue;
            }

            eligible_candidates.push_back(RankedCandidate{
                .candidate = candidate,
                .original_index = index,
                .capacity_tier = ComputeCapacityTier(candidate, request),
                .inflight_tier = ComputeTier(candidate.load.TotalInflight(),
                                             kInflightTierQuantum),
                .active_writes_tier = ComputeTier(candidate.load.active_writes,
                                                  kActiveWritesTierQuantum),
                .active_reads_tier = ComputeTier(candidate.load.active_reads,
                                                kActiveReadsTierQuantum),
                .chunk_scoped_jitter = ComputeChunkScopedJitter(
                    result.decision.chunk_id,
                    request.decision_epoch,
                    candidate.node_id),
                .node_identity_hash = ComputeStableNodeIdentityHash(
                    candidate.node_id)});
        }

        std::sort(eligible_candidates.begin(),
                  eligible_candidates.end(),
                  [](const RankedCandidate &lhs, const RankedCandidate &rhs)
                  {
                      if (lhs.capacity_tier != rhs.capacity_tier)
                      {
                          return lhs.capacity_tier > rhs.capacity_tier;
                      }

                      if (lhs.inflight_tier != rhs.inflight_tier)
                      {
                          return lhs.inflight_tier < rhs.inflight_tier;
                      }

                      if (lhs.active_writes_tier != rhs.active_writes_tier)
                      {
                          return lhs.active_writes_tier < rhs.active_writes_tier;
                      }

                      if (lhs.active_reads_tier != rhs.active_reads_tier)
                      {
                          return lhs.active_reads_tier < rhs.active_reads_tier;
                      }

                      if (lhs.chunk_scoped_jitter != rhs.chunk_scoped_jitter)
                      {
                          return lhs.chunk_scoped_jitter < rhs.chunk_scoped_jitter;
                      }

                      if (lhs.node_identity_hash != rhs.node_identity_hash)
                      {
                          return lhs.node_identity_hash < rhs.node_identity_hash;
                      }

                      return lhs.original_index < rhs.original_index;
                  });

        std::unordered_set<std::string> selected_node_ids;
        std::unordered_set<std::string> selected_zones;

        auto try_select = [&](const RankedCandidate &ranked_candidate,
                              const bool require_new_zone) -> bool
        {
            const auto &candidate = ranked_candidate.candidate;
            if (request.policy.avoid_same_node &&
                selected_node_ids.contains(candidate.node_id))
            {
                return false;
            }

            if (request.policy.prefer_distinct_zones && require_new_zone &&
                !PreferDistinctZone(candidate, selected_zones))
            {
                return false;
            }

            if (selected_node_ids.contains(candidate.node_id))
            {
                AddExclusion(&result.decision,
                             candidate.node_id,
                             "duplicate node_id skipped during selection");
                return false;
            }

            result.decision.replica_nodes.push_back(candidate);
            selected_node_ids.insert(candidate.node_id);
            if (!candidate.zone.empty())
            {
                selected_zones.insert(candidate.zone);
            }
            return true;
        };

        if (request.policy.prefer_distinct_zones)
        {
            for (const auto &candidate : eligible_candidates)
            {
                if (result.decision.replica_nodes.size() >= request.policy.replica_count)
                {
                    break;
                }
                (void)try_select(candidate, true);
            }
            result.decision.reasons.push_back(
                "prefer_distinct_zones enabled; selector first spreads replicas across zones when possible");
        }

        for (const auto &candidate : eligible_candidates)
        {
            if (result.decision.replica_nodes.size() >= request.policy.replica_count)
            {
                break;
            }
            (void)try_select(candidate, false);
        }

        if (result.decision.replica_nodes.size() < request.policy.replica_count)
        {
            result.status = StorageNodeStatusCode::kNodeUnavailable;
            result.error_detail =
                "eligible storage nodes are fewer than requested replica_count";
            result.decision.reasons.push_back(
                "selection failed because eligible nodes were insufficient");
            return result;
        }

        result.decision.reasons.push_back(
            "replicas are ordered by resource tiers first, then chunk-scoped deterministic jitter");
        return result;
    }

    ReadReplicaSelectionResult ReplicaPolicySelector::SelectReadReplicas(
        const ReadReplicaSelectionRequest &request,
        const std::span<const ReadReplicaCandidate> candidates) const
    {
        ReadReplicaSelectionResult result;
        result.decision.chunk_id = request.chunk_id;

        result.status = ValidateChunkId(request.chunk_id, &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        if (request.replica_nodes.empty())
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "read replica selection requires at least one replica node";
            return result;
        }

        std::unordered_set<std::string> explicit_excluded(request.excluded_nodes.begin(),
                                                          request.excluded_nodes.end());
        std::unordered_map<std::string, ReadReplicaCandidate> observed_candidates;
        observed_candidates.reserve(candidates.size());
        for (const auto &candidate : candidates)
        {
            if (candidate.node_id.empty())
            {
                continue;
            }

            ReadReplicaCandidate observed = candidate;
            observed.has_observed_facts = true;
            observed_candidates.insert_or_assign(observed.node_id, std::move(observed));
        }

        std::unordered_set<std::string> seen_nodes;
        std::vector<RankedReadReplicaCandidate> eligible_candidates;
        eligible_candidates.reserve(request.replica_nodes.size());

        for (std::size_t index = 0; index < request.replica_nodes.size(); ++index)
        {
            const auto &node_id = request.replica_nodes[index];
            if (!seen_nodes.insert(node_id).second)
            {
                AddExclusion(&result.decision, node_id, "duplicate node_id in replica_nodes");
                continue;
            }

            ReadReplicaCandidate candidate;
            const auto observed = observed_candidates.find(node_id);
            if (observed != observed_candidates.end())
            {
                candidate = observed->second;
            }
            else
            {
                candidate.node_id = node_id;
                candidate.has_observed_facts = false;
            }

            const auto rejection_reason =
                EvaluateReadReplicaEligibility(candidate, explicit_excluded);
            if (rejection_reason.has_value())
            {
                AddExclusion(&result.decision, node_id, *rejection_reason);
                continue;
            }

            eligible_candidates.push_back(RankedReadReplicaCandidate{
                .candidate = std::move(candidate),
                .manifest_index = index});
        }

        std::stable_sort(eligible_candidates.begin(),
                         eligible_candidates.end(),
                         [](const RankedReadReplicaCandidate &lhs,
                            const RankedReadReplicaCandidate &rhs)
                         {
                             if (lhs.candidate.has_observed_facts != rhs.candidate.has_observed_facts)
                             {
                                 return lhs.candidate.has_observed_facts;
                             }

                             if (ReadHealthRank(lhs.candidate.health) !=
                                 ReadHealthRank(rhs.candidate.health))
                             {
                                 return ReadHealthRank(lhs.candidate.health) <
                                        ReadHealthRank(rhs.candidate.health);
                             }

                             if (ReadDiskPressureRank(lhs.candidate.disk_pressure) !=
                                 ReadDiskPressureRank(rhs.candidate.disk_pressure))
                             {
                                 return ReadDiskPressureRank(lhs.candidate.disk_pressure) <
                                        ReadDiskPressureRank(rhs.candidate.disk_pressure);
                             }

                             if (lhs.candidate.load.active_reads != rhs.candidate.load.active_reads)
                             {
                                 return lhs.candidate.load.active_reads <
                                        rhs.candidate.load.active_reads;
                             }

                             if (lhs.candidate.load.TotalInflight() !=
                                 rhs.candidate.load.TotalInflight())
                             {
                                 return lhs.candidate.load.TotalInflight() <
                                        rhs.candidate.load.TotalInflight();
                             }

                             return lhs.manifest_index < rhs.manifest_index;
                         });

        for (auto &candidate : eligible_candidates)
        {
            result.decision.ordered_replicas.push_back(std::move(candidate.candidate));
        }

        if (result.decision.ordered_replicas.empty())
        {
            result.status = StorageNodeStatusCode::kNodeUnavailable;
            result.error_detail = "no readable replicas remain after selection filtering";
            result.decision.reasons.push_back(
                "all manifest replicas were filtered by explicit exclusions or read facts");
            return result;
        }

        result.decision.reasons.push_back(
            "read replicas are ordered by observed health, lower disk pressure, lower active_reads, lower inflight load, then manifest order");
        if (observed_candidates.size() < request.replica_nodes.size())
        {
            result.decision.reasons.push_back(
                "replicas without observed facts remain eligible and preserve manifest order as neutral fallback candidates");
        }
        return result;
    }

    ReadReplicaSelectionResult ReplicaPolicySelector::SelectReadReplicas(
        const ReadReplicaSelectionRequest &request,
        const StorageNodeRegistrySnapshotResult &registry_snapshot,
        const std::span<const ReadReplicaCandidate> supplemental_candidates) const
    {
        ReadReplicaSelectionResult result;
        result.decision.chunk_id = request.chunk_id;

        if (!registry_snapshot.ok())
        {
            result.status = registry_snapshot.status;
            result.error_detail = registry_snapshot.error_detail;
            result.decision.reasons.push_back(
                "read replica selection could not consume registry snapshot");
            return result;
        }

        std::unordered_map<std::string, ReadReplicaCandidate> merged_candidates_by_node;
        merged_candidates_by_node.reserve(registry_snapshot.nodes.size() +
                                          supplemental_candidates.size());
        for (const auto &snapshot : registry_snapshot.nodes)
        {
            if (snapshot.node_id.empty())
            {
                continue;
            }

            merged_candidates_by_node.insert_or_assign(
                snapshot.node_id,
                BuildReadReplicaCandidateFromRegistrySnapshot(snapshot));
        }

        for (const auto &candidate : supplemental_candidates)
        {
            if (candidate.node_id.empty())
            {
                continue;
            }

            const auto existing = merged_candidates_by_node.find(candidate.node_id);
            if (existing == merged_candidates_by_node.end())
            {
                auto observed = candidate;
                observed.has_observed_facts = true;
                merged_candidates_by_node.insert_or_assign(candidate.node_id,
                                                           std::move(observed));
                continue;
            }

            existing->second =
                MergeReadReplicaCandidates(existing->second, candidate);
        }

        std::vector<ReadReplicaCandidate> merged_candidates;
        merged_candidates.reserve(merged_candidates_by_node.size());
        for (auto &[node_id, candidate] : merged_candidates_by_node)
        {
            (void)node_id;
            merged_candidates.push_back(std::move(candidate));
        }

        result = SelectReadReplicas(request, merged_candidates);
        result.decision.reasons.insert(
            result.decision.reasons.begin(),
            "read replica selection consumed " +
                std::to_string(registry_snapshot.nodes.size()) +
                " registry snapshot nodes");
        result.decision.reasons.insert(
            result.decision.reasons.begin() + 1,
            "registry snapshot facts override node health/load/disk pressure while preserving chunk-specific corruption and missing facts");
        return result;
    }
}
