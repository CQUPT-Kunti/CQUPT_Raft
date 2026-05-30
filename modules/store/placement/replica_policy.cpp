#include "store/placement/replica_policy.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <unordered_set>

namespace storedemo
{
    namespace
    {
        struct RankedCandidate
        {
            StorageNodePlacementCandidate candidate;
            std::size_t original_index{0};
        };

        void AddExclusion(PlacementDecision *decision,
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
                .original_index = index});
        }

        std::sort(eligible_candidates.begin(),
                  eligible_candidates.end(),
                  [](const RankedCandidate &lhs, const RankedCandidate &rhs)
                  {
                      if (lhs.candidate.available_capacity_bytes !=
                          rhs.candidate.available_capacity_bytes)
                      {
                          return lhs.candidate.available_capacity_bytes >
                                 rhs.candidate.available_capacity_bytes;
                      }

                      if (lhs.candidate.load.TotalInflight() !=
                          rhs.candidate.load.TotalInflight())
                      {
                          return lhs.candidate.load.TotalInflight() <
                                 rhs.candidate.load.TotalInflight();
                      }

                      if (lhs.candidate.load.active_writes !=
                          rhs.candidate.load.active_writes)
                      {
                          return lhs.candidate.load.active_writes <
                                 rhs.candidate.load.active_writes;
                      }

                      if (lhs.candidate.load.active_reads !=
                          rhs.candidate.load.active_reads)
                      {
                          return lhs.candidate.load.active_reads <
                                 rhs.candidate.load.active_reads;
                      }

                      if (lhs.candidate.node_id != rhs.candidate.node_id)
                      {
                          return lhs.candidate.node_id < rhs.candidate.node_id;
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
            "replicas are ordered by available capacity, lower inflight load, then node_id");
        return result;
    }
}
