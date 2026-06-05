#include "store/node/storage_node_registry.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>

namespace storedemo
{
    namespace
    {
        enum class SequenceDecision : std::uint8_t
        {
            kApply = 0,
            kIdempotent = 1,
            kStale = 2,
        };

        bool IsValidNodeId(const std::string_view node_id)
        {
            if (node_id.empty())
            {
                return false;
            }

            return std::all_of(
                node_id.begin(),
                node_id.end(),
                [](const unsigned char ch)
                { return std::isalnum(ch) != 0 || ch == '-' || ch == '_'; });
        }

        bool IsValidEndpoint(const std::string_view endpoint)
        {
            const auto separator = endpoint.rfind(':');
            if (separator == std::string_view::npos || separator == 0 ||
                separator + 1 >= endpoint.size())
            {
                return false;
            }

            const auto port = endpoint.substr(separator + 1);
            if (!std::all_of(
                    port.begin(),
                    port.end(),
                    [](const unsigned char ch) { return std::isdigit(ch) != 0; }))
            {
                return false;
            }

            try
            {
                const auto parsed = std::stoul(std::string(port));
                return parsed > 0 && parsed <= 65535;
            }
            catch (const std::exception &)
            {
                return false;
            }
        }

        StorageNodeRegistryConfig NormalizeConfig(StorageNodeRegistryConfig config)
        {
            if (config.stale_timeout_ms == 0)
            {
                config.stale_timeout_ms = 1;
            }
            if (config.dead_timeout_ms < config.stale_timeout_ms)
            {
                config.dead_timeout_ms = config.stale_timeout_ms;
            }
            return config;
        }

        StorageNodeStatusCode ValidateNodeIdentity(
            const std::string_view node_id,
            const std::string_view endpoint,
            std::string *error_detail)
        {
            if (!IsValidNodeId(node_id))
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "node_id must contain only alnum, '-' or '_'";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            if (!IsValidEndpoint(endpoint))
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "endpoint must be host:port with a valid port";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ValidateObservedAt(const std::uint64_t observed_at_unix_ms,
                                                 std::string *error_detail)
        {
            if (observed_at_unix_ms == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "observed_at_unix_ms must be greater than zero";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ValidateSequence(const std::uint64_t sequence,
                                               std::string *error_detail)
        {
            if (sequence == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail = "sequence must be greater than zero";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ValidateCapacityFacts(
            const StorageNodeRegistryCapacityFacts &capacity,
            std::string *error_detail)
        {
            if (capacity.total_capacity_bytes == 0)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "total_capacity_bytes must be greater than zero";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
            if (capacity.used_capacity_bytes > capacity.total_capacity_bytes)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "used_capacity_bytes must not exceed total_capacity_bytes";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
            if (capacity.available_capacity_bytes > capacity.total_capacity_bytes)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "available_capacity_bytes must not exceed total_capacity_bytes";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }
            if (capacity.used_capacity_bytes + capacity.available_capacity_bytes >
                capacity.total_capacity_bytes)
            {
                if (error_detail != nullptr)
                {
                    *error_detail =
                        "used_capacity_bytes + available_capacity_bytes must not exceed total_capacity_bytes";
                }
                return StorageNodeStatusCode::kInvalidArgument;
            }

            return StorageNodeStatusCode::kOk;
        }

        StorageNodeStatusCode ValidateRegisterRequest(
            const RegisterStorageNodeRequest &request,
            std::string *error_detail)
        {
            auto status = ValidateNodeIdentity(request.node_id,
                                               request.endpoint,
                                               error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            status = ValidateObservedAt(request.observed_at_unix_ms, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            return ValidateCapacityFacts(request.facts.capacity, error_detail);
        }

        template <typename Request>
        StorageNodeStatusCode ValidateSequencedRequest(const Request &request,
                                                       std::string *error_detail)
        {
            auto status = ValidateNodeIdentity(request.node_id,
                                               request.endpoint,
                                               error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            status = ValidateObservedAt(request.observed_at_unix_ms, error_detail);
            if (status != StorageNodeStatusCode::kOk)
            {
                return status;
            }

            return ValidateSequence(request.sequence, error_detail);
        }

        SequenceDecision EvaluateSequenceDecision(
            const std::uint64_t last_sequence,
            const std::uint64_t last_seen_unix_ms,
            const std::uint64_t incoming_sequence,
            const std::uint64_t incoming_observed_at)
        {
            if (incoming_sequence < last_sequence)
            {
                return SequenceDecision::kStale;
            }
            if (incoming_sequence == last_sequence)
            {
                return SequenceDecision::kIdempotent;
            }
            if (incoming_observed_at < last_seen_unix_ms)
            {
                return SequenceDecision::kStale;
            }

            return SequenceDecision::kApply;
        }

        StorageNodeRegistryLiveness DetermineLiveness(
            const std::uint64_t last_seen_unix_ms,
            const std::uint64_t now_unix_ms,
            const StorageNodeRegistryConfig &config)
        {
            if (now_unix_ms <= last_seen_unix_ms)
            {
                return StorageNodeRegistryLiveness::kLive;
            }

            const auto elapsed = now_unix_ms - last_seen_unix_ms;
            if (elapsed <= config.stale_timeout_ms)
            {
                return StorageNodeRegistryLiveness::kLive;
            }
            if (elapsed <= config.dead_timeout_ms)
            {
                return StorageNodeRegistryLiveness::kStale;
            }

            return StorageNodeRegistryLiveness::kDead;
        }

        StorageNodeRegistryNodeSnapshot MakeNodeSnapshot(
            const StorageNodeId &node_id,
            const std::string &endpoint,
            const StorageNodeRegistryFacts &facts,
            const std::uint64_t last_sequence,
            const std::uint64_t last_seen_unix_ms,
            const std::uint64_t now_unix_ms,
            const StorageNodeRegistryConfig &config)
        {
            return StorageNodeRegistryNodeSnapshot{
                .node_id = node_id,
                .endpoint = endpoint,
                .last_sequence = last_sequence,
                .last_seen_unix_ms = last_seen_unix_ms,
                .liveness = DetermineLiveness(last_seen_unix_ms,
                                              now_unix_ms,
                                              config),
                .facts = facts};
        }

        template <typename Records>
        const StorageNodeId *FindNodeIdByEndpoint(const Records &records,
            const std::string_view endpoint)
        {
            const auto it =
                std::find_if(records.begin(),
                             records.end(),
                             [endpoint](const auto &entry)
                             { return entry.second.endpoint == endpoint; });
            if (it == records.end())
            {
                return nullptr;
            }

            return &it->first;
        }

        template <typename Record>
        void InitializeRegisteredRecord(Record *record,
                                        std::string endpoint,
                                        const StorageNodeRegistryFacts &facts,
                                        const std::uint64_t observed_at_unix_ms)
        {
            record->endpoint = std::move(endpoint);
            record->facts = facts;
            record->last_sequence = 0;
            record->last_seen_unix_ms = observed_at_unix_ms;
        }

        template <typename Record>
        void ApplyFullFacts(Record *record,
                            const StorageNodeRegistryFacts &facts,
                            const std::uint64_t sequence,
                            const std::uint64_t observed_at_unix_ms)
        {
            record->facts = facts;
            record->last_sequence = sequence;
            record->last_seen_unix_ms = observed_at_unix_ms;
        }

        template <typename Record>
        void MergeHealthFacts(Record *record,
                              const StorageNodeRegistryHealthFacts &health,
                              const std::uint64_t sequence,
                              const std::uint64_t observed_at_unix_ms)
        {
            record->facts.health = health;
            record->last_sequence = sequence;
            record->last_seen_unix_ms = observed_at_unix_ms;
        }

        template <typename Record>
        void MergeCapacityFacts(Record *record,
                                const StorageNodeRegistryCapacityFacts &capacity,
                                const std::uint64_t sequence,
                                const std::uint64_t observed_at_unix_ms)
        {
            record->facts.capacity = capacity;
            record->last_sequence = sequence;
            record->last_seen_unix_ms = observed_at_unix_ms;
        }

        template <typename Record>
        void MergeLoadFacts(Record *record,
                            const StorageNodeRegistryLoadFacts &load,
                            const std::uint64_t sequence,
                            const std::uint64_t observed_at_unix_ms)
        {
            record->facts.load = load;
            record->last_sequence = sequence;
            record->last_seen_unix_ms = observed_at_unix_ms;
        }

        template <typename Result>
        void FillConflict(Result *result, std::string message)
        {
            result->status = StorageNodeStatusCode::kConflict;
            result->error_detail = std::move(message);
        }

        template <typename Records>
        void AppendSortedSnapshots(
            const Records &records,
            const std::uint64_t now_unix_ms,
            const StorageNodeRegistryConfig &config,
            std::vector<StorageNodeRegistryNodeSnapshot> *out)
        {
            out->reserve(records.size());
            for (const auto &[node_id, record] : records)
            {
                out->push_back(MakeNodeSnapshot(node_id,
                                                record.endpoint,
                                                record.facts,
                                                record.last_sequence,
                                                record.last_seen_unix_ms,
                                                now_unix_ms,
                                                config));
            }
        }
    }

    StorageNodeRegistry::StorageNodeRegistry(StorageNodeRegistryConfig config)
        : config_(NormalizeConfig(std::move(config)))
    {
    }

    RegisterStorageNodeResult StorageNodeRegistry::RegisterStorageNode(
        const RegisterStorageNodeRequest &request)
    {
        RegisterStorageNodeResult result;
        result.status = ValidateRegisterRequest(request, &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        if (config_.enforce_unique_endpoints)
        {
            const auto *endpoint_owner =
                FindNodeIdByEndpoint(records_, request.endpoint);
            if (endpoint_owner != nullptr && *endpoint_owner != request.node_id)
            {
                FillConflict(&result,
                             "endpoint is already registered to a different node_id");
                return result;
            }
        }

        const auto existing = records_.find(request.node_id);
        if (existing != records_.end())
        {
            if (existing->second.endpoint != request.endpoint)
            {
                FillConflict(&result,
                             "node_id is already registered with a different endpoint");
                return result;
            }

            result.idempotent = true;
            result.snapshot = MakeNodeSnapshot(request.node_id,
                                               existing->second.endpoint,
                                               existing->second.facts,
                                               existing->second.last_sequence,
                                               existing->second.last_seen_unix_ms,
                                               request.observed_at_unix_ms,
                                               config_);
            return result;
        }

        Record record;
        InitializeRegisteredRecord(&record,
                                   request.endpoint,
                                   request.facts,
                                   request.observed_at_unix_ms);
        records_.emplace(request.node_id, record);

        result.created = true;
        result.snapshot = MakeNodeSnapshot(request.node_id,
                                           record.endpoint,
                                           record.facts,
                                           record.last_sequence,
                                           record.last_seen_unix_ms,
                                           request.observed_at_unix_ms,
                                           config_);
        return result;
    }

    StorageNodeRegistryUpdateResult StorageNodeRegistry::UpdateStorageNodeHeartbeat(
        const UpdateStorageNodeHeartbeatRequest &request)
    {
        StorageNodeRegistryUpdateResult result;
        result.status = ValidateSequencedRequest(request, &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        result.status = ValidateCapacityFacts(request.facts.capacity,
                                              &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = records_.find(request.node_id);
        if (existing == records_.end())
        {
            result.status = StorageNodeStatusCode::kNotFound;
            result.error_detail = "node_id is not registered";
            return result;
        }
        if (existing->second.endpoint != request.endpoint)
        {
            FillConflict(&result, "node_id heartbeat endpoint does not match registration");
            return result;
        }

        result.accepted_sequence = existing->second.last_sequence;
        const auto decision = EvaluateSequenceDecision(existing->second.last_sequence,
                                                       existing->second.last_seen_unix_ms,
                                                       request.sequence,
                                                       request.observed_at_unix_ms);
        if (decision == SequenceDecision::kStale)
        {
            result.status = StorageNodeStatusCode::kAlreadyExists;
            result.stale_ignored = true;
            result.snapshot = MakeNodeSnapshot(request.node_id,
                                               existing->second.endpoint,
                                               existing->second.facts,
                                               existing->second.last_sequence,
                                               existing->second.last_seen_unix_ms,
                                               request.observed_at_unix_ms,
                                               config_);
            return result;
        }
        if (decision == SequenceDecision::kIdempotent)
        {
            result.idempotent = true;
            result.snapshot = MakeNodeSnapshot(request.node_id,
                                               existing->second.endpoint,
                                               existing->second.facts,
                                               existing->second.last_sequence,
                                               existing->second.last_seen_unix_ms,
                                               request.observed_at_unix_ms,
                                               config_);
            return result;
        }

        ApplyFullFacts(&existing->second,
                       request.facts,
                       request.sequence,
                       request.observed_at_unix_ms);
        result.applied = true;
        result.accepted_sequence = request.sequence;
        result.snapshot = MakeNodeSnapshot(request.node_id,
                                           existing->second.endpoint,
                                           existing->second.facts,
                                           existing->second.last_sequence,
                                           existing->second.last_seen_unix_ms,
                                           request.observed_at_unix_ms,
                                           config_);
        return result;
    }

    StorageNodeRegistryUpdateResult StorageNodeRegistry::ReportHealth(
        const ReportHealthRequest &request)
    {
        StorageNodeRegistryUpdateResult result;
        result.status = ValidateSequencedRequest(request, &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = records_.find(request.node_id);
        if (existing == records_.end())
        {
            result.status = StorageNodeStatusCode::kNotFound;
            result.error_detail = "node_id is not registered";
            return result;
        }
        if (existing->second.endpoint != request.endpoint)
        {
            FillConflict(&result, "node_id health endpoint does not match registration");
            return result;
        }

        result.accepted_sequence = existing->second.last_sequence;
        const auto decision = EvaluateSequenceDecision(existing->second.last_sequence,
                                                       existing->second.last_seen_unix_ms,
                                                       request.sequence,
                                                       request.observed_at_unix_ms);
        if (decision == SequenceDecision::kStale)
        {
            result.status = StorageNodeStatusCode::kAlreadyExists;
            result.stale_ignored = true;
            result.snapshot = MakeNodeSnapshot(request.node_id,
                                               existing->second.endpoint,
                                               existing->second.facts,
                                               existing->second.last_sequence,
                                               existing->second.last_seen_unix_ms,
                                               request.observed_at_unix_ms,
                                               config_);
            return result;
        }
        if (decision == SequenceDecision::kIdempotent)
        {
            result.idempotent = true;
            result.snapshot = MakeNodeSnapshot(request.node_id,
                                               existing->second.endpoint,
                                               existing->second.facts,
                                               existing->second.last_sequence,
                                               existing->second.last_seen_unix_ms,
                                               request.observed_at_unix_ms,
                                               config_);
            return result;
        }

        MergeHealthFacts(&existing->second,
                         request.health,
                         request.sequence,
                         request.observed_at_unix_ms);
        result.applied = true;
        result.accepted_sequence = request.sequence;
        result.snapshot = MakeNodeSnapshot(request.node_id,
                                           existing->second.endpoint,
                                           existing->second.facts,
                                           existing->second.last_sequence,
                                           existing->second.last_seen_unix_ms,
                                           request.observed_at_unix_ms,
                                           config_);
        return result;
    }

    StorageNodeRegistryUpdateResult StorageNodeRegistry::ReportCapacity(
        const ReportCapacityRequest &request)
    {
        StorageNodeRegistryUpdateResult result;
        result.status = ValidateSequencedRequest(request, &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        result.status =
            ValidateCapacityFacts(request.capacity, &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = records_.find(request.node_id);
        if (existing == records_.end())
        {
            result.status = StorageNodeStatusCode::kNotFound;
            result.error_detail = "node_id is not registered";
            return result;
        }
        if (existing->second.endpoint != request.endpoint)
        {
            FillConflict(&result,
                         "node_id capacity endpoint does not match registration");
            return result;
        }

        result.accepted_sequence = existing->second.last_sequence;
        const auto decision = EvaluateSequenceDecision(existing->second.last_sequence,
                                                       existing->second.last_seen_unix_ms,
                                                       request.sequence,
                                                       request.observed_at_unix_ms);
        if (decision == SequenceDecision::kStale)
        {
            result.status = StorageNodeStatusCode::kAlreadyExists;
            result.stale_ignored = true;
            result.snapshot = MakeNodeSnapshot(request.node_id,
                                               existing->second.endpoint,
                                               existing->second.facts,
                                               existing->second.last_sequence,
                                               existing->second.last_seen_unix_ms,
                                               request.observed_at_unix_ms,
                                               config_);
            return result;
        }
        if (decision == SequenceDecision::kIdempotent)
        {
            result.idempotent = true;
            result.snapshot = MakeNodeSnapshot(request.node_id,
                                               existing->second.endpoint,
                                               existing->second.facts,
                                               existing->second.last_sequence,
                                               existing->second.last_seen_unix_ms,
                                               request.observed_at_unix_ms,
                                               config_);
            return result;
        }

        MergeCapacityFacts(&existing->second,
                           request.capacity,
                           request.sequence,
                           request.observed_at_unix_ms);
        result.applied = true;
        result.accepted_sequence = request.sequence;
        result.snapshot = MakeNodeSnapshot(request.node_id,
                                           existing->second.endpoint,
                                           existing->second.facts,
                                           existing->second.last_sequence,
                                           existing->second.last_seen_unix_ms,
                                           request.observed_at_unix_ms,
                                           config_);
        return result;
    }

    StorageNodeRegistryUpdateResult StorageNodeRegistry::ReportLoad(
        const ReportLoadRequest &request)
    {
        StorageNodeRegistryUpdateResult result;
        result.status = ValidateSequencedRequest(request, &result.error_detail);
        if (!result.ok())
        {
            return result;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = records_.find(request.node_id);
        if (existing == records_.end())
        {
            result.status = StorageNodeStatusCode::kNotFound;
            result.error_detail = "node_id is not registered";
            return result;
        }
        if (existing->second.endpoint != request.endpoint)
        {
            FillConflict(&result, "node_id load endpoint does not match registration");
            return result;
        }

        result.accepted_sequence = existing->second.last_sequence;
        const auto decision = EvaluateSequenceDecision(existing->second.last_sequence,
                                                       existing->second.last_seen_unix_ms,
                                                       request.sequence,
                                                       request.observed_at_unix_ms);
        if (decision == SequenceDecision::kStale)
        {
            result.status = StorageNodeStatusCode::kAlreadyExists;
            result.stale_ignored = true;
            result.snapshot = MakeNodeSnapshot(request.node_id,
                                               existing->second.endpoint,
                                               existing->second.facts,
                                               existing->second.last_sequence,
                                               existing->second.last_seen_unix_ms,
                                               request.observed_at_unix_ms,
                                               config_);
            return result;
        }
        if (decision == SequenceDecision::kIdempotent)
        {
            result.idempotent = true;
            result.snapshot = MakeNodeSnapshot(request.node_id,
                                               existing->second.endpoint,
                                               existing->second.facts,
                                               existing->second.last_sequence,
                                               existing->second.last_seen_unix_ms,
                                               request.observed_at_unix_ms,
                                               config_);
            return result;
        }

        MergeLoadFacts(&existing->second,
                       request.load,
                       request.sequence,
                       request.observed_at_unix_ms);
        result.applied = true;
        result.accepted_sequence = request.sequence;
        result.snapshot = MakeNodeSnapshot(request.node_id,
                                           existing->second.endpoint,
                                           existing->second.facts,
                                           existing->second.last_sequence,
                                           existing->second.last_seen_unix_ms,
                                           request.observed_at_unix_ms,
                                           config_);
        return result;
    }

    StorageNodeRegistryLookupResult StorageNodeRegistry::LookupNode(
        const std::string_view node_id,
        const std::uint64_t now_unix_ms) const
    {
        StorageNodeRegistryLookupResult result;
        if (!IsValidNodeId(node_id))
        {
            result.status = StorageNodeStatusCode::kInvalidArgument;
            result.error_detail = "node_id must contain only alnum, '-' or '_'";
            return result;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = records_.find(node_id);
        if (existing == records_.end())
        {
            result.status = StorageNodeStatusCode::kNotFound;
            result.error_detail = "node_id is not registered";
            return result;
        }

        result.snapshot =
            MakeNodeSnapshot(existing->first,
                             existing->second.endpoint,
                             existing->second.facts,
                             existing->second.last_sequence,
                             existing->second.last_seen_unix_ms,
                             now_unix_ms,
                             config_);
        return result;
    }

    StorageNodeRegistryListResult StorageNodeRegistry::ListNodes(
        const std::uint64_t now_unix_ms) const
    {
        StorageNodeRegistryListResult result;
        std::lock_guard<std::mutex> lock(mutex_);
        AppendSortedSnapshots(records_, now_unix_ms, config_, &result.nodes);
        return result;
    }

    StorageNodeRegistrySnapshotResult StorageNodeRegistry::Snapshot(
        const std::uint64_t now_unix_ms) const
    {
        StorageNodeRegistrySnapshotResult result;
        std::lock_guard<std::mutex> lock(mutex_);
        result.generated_at_unix_ms = now_unix_ms;
        AppendSortedSnapshots(records_, now_unix_ms, config_, &result.nodes);
        return result;
    }

    std::size_t StorageNodeRegistry::size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return records_.size();
    }

    const StorageNodeRegistryConfig &StorageNodeRegistry::config() const
    {
        return config_;
    }
}
