#include "view/view_registry.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <map>
#include <mutex>
#include <sstream>
#include <tuple>
#include <utility>

namespace viewdemo
{
    namespace
    {
        enum class SequenceDecision : std::uint8_t
        {
            kApply = 0,
            kIdempotent = 1,
            kStale = 2,
        };

        struct RecordKey
        {
            ClusterId cluster_id;
            NodeId node_id;

            [[nodiscard]] bool operator<(const RecordKey &other) const
            {
                return std::tie(cluster_id, node_id) <
                       std::tie(other.cluster_id, other.node_id);
            }
        };

        struct Record
        {
            NodeRegistration registration;
            std::uint64_t registered_at_unix_ms{0};
            std::uint64_t last_seen_unix_ms{0};
            std::uint64_t last_sequence{0};
        };

        using Records = std::map<RecordKey, Record>;

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

        bool IsValidClusterId(const std::string_view cluster_id)
        {
            return IsValidNodeId(cluster_id);
        }

        bool IsValidEndpoint(const std::string_view endpoint)
        {
            if (endpoint.empty())
            {
                return false;
            }

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
                const auto port_text = std::string(port);
                const auto parsed = std::stoul(port_text);
                return parsed > 0 && parsed <= 65535;
            }
            catch (const std::exception &)
            {
                return false;
            }
        }

        bool IsValidOptionalEndpoint(const std::string_view endpoint)
        {
            return endpoint.empty() || IsValidEndpoint(endpoint);
        }

        ViewRegistryConfig NormalizeConfig(ViewRegistryConfig config)
        {
            if (config.stale_timeout.count() <= 0)
            {
                config.stale_timeout = std::chrono::milliseconds{1};
            }
            if (config.suspect_timeout < config.stale_timeout)
            {
                config.suspect_timeout = config.stale_timeout;
            }
            if (config.dead_timeout < config.suspect_timeout)
            {
                config.dead_timeout = config.suspect_timeout;
            }
            return config;
        }

        ViewRegistryDiagnostic MakeDiagnostic(
            const ViewRegistryIssueCode code,
            std::string message,
            const RequestId &request_id,
            const ClusterId &cluster_id,
            const NodeId &node_id,
            const Endpoint &endpoint,
            const std::uint64_t sequence = 0)
        {
            return ViewRegistryDiagnostic{
                .code = code,
                .message = std::move(message),
                .request_id = request_id,
                .cluster_id = cluster_id,
                .node_id = node_id,
                .endpoint = endpoint,
                .sequence = sequence};
        }

        ViewRegistryResponseSummary MakeSummary(
            const ViewRegistryStatusCode status,
            std::string message,
            const RequestId &request_id,
            const ClusterId &cluster_id,
            const NodeId &node_id)
        {
            return ViewRegistryResponseSummary{
                .status = status,
                .message = std::move(message),
                .request_id = request_id,
                .cluster_id = cluster_id,
                .node_id = node_id};
        }

        void SetSummary(ViewRegistryResponseSummary *summary,
                        const ViewRegistryStatusCode status,
                        std::string message,
                        const RequestId &request_id,
                        const ClusterId &cluster_id,
                        const NodeId &node_id)
        {
            *summary = MakeSummary(status,
                                   std::move(message),
                                   request_id,
                                   cluster_id,
                                   node_id);
        }

        ViewRegistryStatusCode ValidateRegistration(
            const NodeRegistration &registration,
            std::string *message,
            ViewRegistryIssueCode *issue_code)
        {
            if (!IsValidClusterId(registration.cluster_id))
            {
                *message = "cluster_id must contain only alnum, '-' or '_'";
                *issue_code = ViewRegistryIssueCode::kMissingClusterId;
                return ViewRegistryStatusCode::kInvalidArgument;
            }
            if (!IsValidNodeId(registration.node_id))
            {
                *message = "node_id must contain only alnum, '-' or '_'";
                *issue_code = ViewRegistryIssueCode::kMissingNodeId;
                return ViewRegistryStatusCode::kInvalidArgument;
            }
            if (registration.node_type == ViewNodeType::kUnknown)
            {
                *message = "node_type must be view, metadata or storage";
                *issue_code = ViewRegistryIssueCode::kInvalidNodeType;
                return ViewRegistryStatusCode::kInvalidArgument;
            }
            if (!IsValidEndpoint(registration.endpoint))
            {
                *message = "endpoint must be host:port with a valid port";
                *issue_code = ViewRegistryIssueCode::kMissingEndpoint;
                return ViewRegistryStatusCode::kInvalidArgument;
            }
            if (!IsValidOptionalEndpoint(registration.control_plane_endpoint) ||
                !IsValidOptionalEndpoint(registration.data_plane_endpoint))
            {
                *message =
                    "control_plane_endpoint and data_plane_endpoint must be host:port when set";
                *issue_code = ViewRegistryIssueCode::kMissingEndpoint;
                return ViewRegistryStatusCode::kInvalidArgument;
            }
            if (registration.observed_at_unix_ms == 0)
            {
                *message = "observed_at_unix_ms must be greater than zero";
                *issue_code = ViewRegistryIssueCode::kStaleHeartbeat;
                return ViewRegistryStatusCode::kInvalidArgument;
            }
            if (registration.node_type == ViewNodeType::kStorage &&
                registration.capacity.total_capacity_bytes == 0)
            {
                *message = "storage node total_capacity_bytes must be greater than zero";
                *issue_code = ViewRegistryIssueCode::kCapacityInsufficient;
                return ViewRegistryStatusCode::kInvalidArgument;
            }
            if (registration.capacity.used_capacity_bytes >
                    registration.capacity.total_capacity_bytes ||
                registration.capacity.available_capacity_bytes >
                    registration.capacity.total_capacity_bytes)
            {
                *message =
                    "used_capacity_bytes and available_capacity_bytes must not exceed total_capacity_bytes";
                *issue_code = ViewRegistryIssueCode::kCapacityInsufficient;
                return ViewRegistryStatusCode::kInvalidArgument;
            }
            if (registration.capacity.used_capacity_bytes +
                    registration.capacity.available_capacity_bytes >
                registration.capacity.total_capacity_bytes)
            {
                *message =
                    "used_capacity_bytes + available_capacity_bytes must not exceed total_capacity_bytes";
                *issue_code = ViewRegistryIssueCode::kCapacityInsufficient;
                return ViewRegistryStatusCode::kInvalidArgument;
            }

            return ViewRegistryStatusCode::kOk;
        }

        void FillInvalidRegisterResult(RegisterNodeResult *result,
                                       const RegisterNodeRequest &request,
                                       const ViewRegistryStatusCode status,
                                       const ViewRegistryIssueCode issue_code,
                                       std::string message)
        {
            SetSummary(&result->summary,
                       status,
                       message,
                       request.request_id,
                       request.registration.cluster_id,
                       request.registration.node_id);
            result->diagnostics.push_back(
                MakeDiagnostic(issue_code,
                               std::move(message),
                               request.request_id,
                               request.registration.cluster_id,
                               request.registration.node_id,
                               request.registration.endpoint));
        }

        void FillInvalidHeartbeatResult(HeartbeatNodeResult *result,
                                        const HeartbeatNodeRequest &request,
                                        const ViewRegistryStatusCode status,
                                        const ViewRegistryIssueCode issue_code,
                                        std::string message)
        {
            SetSummary(&result->summary,
                       status,
                       message,
                       request.request_id,
                       request.cluster_id,
                       request.node_id);
            result->diagnostics.push_back(
                MakeDiagnostic(issue_code,
                               std::move(message),
                               request.request_id,
                               request.cluster_id,
                               request.node_id,
                               request.observation.endpoint,
                               request.sequence));
        }

        std::vector<std::string_view> EndpointsOf(
            const NodeRegistration &registration)
        {
            std::vector<std::string_view> endpoints;
            endpoints.reserve(3);
            if (!registration.endpoint.empty())
            {
                endpoints.push_back(registration.endpoint);
            }
            if (!registration.control_plane_endpoint.empty())
            {
                endpoints.push_back(registration.control_plane_endpoint);
            }
            if (!registration.data_plane_endpoint.empty())
            {
                endpoints.push_back(registration.data_plane_endpoint);
            }
            return endpoints;
        }

        bool EndpointBelongsTo(const NodeRegistration &registration,
                               const std::string_view endpoint)
        {
            const auto endpoints = EndpointsOf(registration);
            return std::any_of(endpoints.begin(),
                               endpoints.end(),
                               [endpoint](const std::string_view candidate)
                               { return candidate == endpoint; });
        }

        const RecordKey *FindEndpointOwner(const Records &records,
                                           const ClusterId &cluster_id,
                                           const NodeId &ignored_node_id,
                                           const NodeRegistration &registration)
        {
            const auto incoming_endpoints = EndpointsOf(registration);
            for (const auto &[key, record] : records)
            {
                if (key.cluster_id != cluster_id)
                {
                    continue;
                }
                if (key.node_id == ignored_node_id)
                {
                    continue;
                }
                for (const auto endpoint : incoming_endpoints)
                {
                    if (EndpointBelongsTo(record.registration, endpoint))
                    {
                        return &key;
                    }
                }
            }
            return nullptr;
        }

        bool CompatibleOptionalField(const std::string &existing,
                                     const std::string &incoming)
        {
            return incoming.empty() || existing.empty() || existing == incoming;
        }

        bool IsCompatibleRegistration(const NodeRegistration &existing,
                                      const NodeRegistration &incoming,
                                      std::string *message,
                                      ViewRegistryIssueCode *issue_code)
        {
            if (existing.cluster_id != incoming.cluster_id)
            {
                *message = "node_id is already registered in a different cluster";
                *issue_code = ViewRegistryIssueCode::kClusterMismatch;
                return false;
            }
            if (existing.node_type != incoming.node_type)
            {
                *message = "node_id is already registered with a different node_type";
                *issue_code = ViewRegistryIssueCode::kNodeTypeMismatch;
                return false;
            }
            if (existing.endpoint != incoming.endpoint)
            {
                *message = "node_id is already registered with a different endpoint";
                *issue_code = ViewRegistryIssueCode::kEndpointConflict;
                return false;
            }
            if (!CompatibleOptionalField(existing.control_plane_endpoint,
                                         incoming.control_plane_endpoint) ||
                !CompatibleOptionalField(existing.data_plane_endpoint,
                                         incoming.data_plane_endpoint))
            {
                *message = "node_id is already registered with incompatible endpoints";
                *issue_code = ViewRegistryIssueCode::kEndpointConflict;
                return false;
            }
            if (!CompatibleOptionalField(existing.data_dir_fingerprint,
                                         incoming.data_dir_fingerprint))
            {
                *message =
                    "node_id is already registered with a different data_dir_fingerprint";
                *issue_code = ViewRegistryIssueCode::kDataDirFingerprintConflict;
                return false;
            }

            return true;
        }

        ViewNodeLivenessState DetermineLiveness(
            const std::uint64_t last_seen_unix_ms,
            const std::uint64_t now_unix_ms,
            const ViewRegistryConfig &config)
        {
            if (last_seen_unix_ms == 0)
            {
                return ViewNodeLivenessState::kDead;
            }
            if (now_unix_ms <= last_seen_unix_ms)
            {
                return ViewNodeLivenessState::kLive;
            }

            const auto elapsed =
                std::chrono::milliseconds{now_unix_ms - last_seen_unix_ms};
            if (elapsed <= config.stale_timeout)
            {
                return ViewNodeLivenessState::kLive;
            }
            if (elapsed <= config.suspect_timeout)
            {
                return ViewNodeLivenessState::kStale;
            }
            if (elapsed <= config.dead_timeout)
            {
                return ViewNodeLivenessState::kSuspect;
            }
            return ViewNodeLivenessState::kDead;
        }

        MetadataMembershipObservedState MapObservedMembershipState(
            const MetadataNodeObservation &observation,
            const ViewNodeLivenessState liveness,
            const ViewRegistryHealthReport &health)
        {
            if (liveness == ViewNodeLivenessState::kDead ||
                health.health == ViewNodeHealth::kUnavailable)
            {
                return MetadataMembershipObservedState::kDown;
            }

            if (observation.membership_state !=
                MetadataMembershipObservedState::kUnknown)
            {
                return observation.membership_state;
            }

            if (observation.raft_role == MetadataRaftObservedRole::kLearner)
            {
                return MetadataMembershipObservedState::kLearner;
            }

            return MetadataMembershipObservedState::kRegistered;
        }

        std::optional<MetadataNodeObservation> NormalizeMetadataObservation(
            const ViewNodeType node_type,
            const ViewNodeLivenessState liveness,
            const ViewRegistryHealthReport &health,
            std::optional<MetadataNodeObservation> observation)
        {
            if (node_type != ViewNodeType::kMetadata)
            {
                return std::nullopt;
            }

            MetadataNodeObservation normalized =
                observation.value_or(MetadataNodeObservation{});
            // ViewNode 只补 discovery / observation status，不推导 Raft authority。
            normalized.membership_state =
                MapObservedMembershipState(normalized, liveness, health);
            return normalized;
        }

        ViewNodeSnapshot MakeSnapshot(const Record &record,
                                      const std::uint64_t now_unix_ms,
                                      const ViewRegistryConfig &config)
        {
            ViewNodeSnapshot snapshot{
                .cluster_id = record.registration.cluster_id,
                .node_id = record.registration.node_id,
                .node_type = record.registration.node_type,
                .endpoint = record.registration.endpoint,
                .control_plane_endpoint =
                    record.registration.control_plane_endpoint,
                .data_plane_endpoint = record.registration.data_plane_endpoint,
                .data_dir_fingerprint = record.registration.data_dir_fingerprint,
                .registered_at_unix_ms = record.registered_at_unix_ms,
                .last_seen_unix_ms = record.last_seen_unix_ms,
                .last_sequence = record.last_sequence,
                .liveness = DetermineLiveness(record.last_seen_unix_ms,
                                              now_unix_ms,
                                              config),
                .failure_domain = record.registration.failure_domain,
                .health = record.registration.health,
                .capacity = record.registration.capacity,
                .load = record.registration.load,
                .metadata = record.registration.metadata};

            snapshot.metadata = NormalizeMetadataObservation(
                snapshot.node_type,
                snapshot.liveness,
                snapshot.health,
                std::move(snapshot.metadata));
            return snapshot;
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

        bool IsLiveForDiscovery(const ViewNodeSnapshot &snapshot,
                                const bool live_only)
        {
            return !live_only ||
                   snapshot.liveness == ViewNodeLivenessState::kLive;
        }

        bool IsWritableStorageNode(const ViewNodeSnapshot &snapshot)
        {
            return snapshot.health.health != ViewNodeHealth::kUnavailable &&
                   snapshot.health.health != ViewNodeHealth::kReadOnly &&
                   snapshot.health.health != ViewNodeHealth::kDraining &&
                   snapshot.health.disk_pressure != ViewNodeDiskPressure::kFull &&
                   !snapshot.load.write_admission_overloaded;
        }

        bool PassesStorageFilters(const ViewNodeSnapshot &snapshot,
                                  const DiscoverStorageRequest &request)
        {
            if (snapshot.node_type != ViewNodeType::kStorage)
            {
                return false;
            }
            if (!IsLiveForDiscovery(snapshot, request.live_only))
            {
                return false;
            }
            if (snapshot.capacity.available_capacity_bytes <
                request.minimum_available_capacity_bytes)
            {
                return false;
            }
            if (!request.zone.empty() &&
                snapshot.failure_domain.zone != request.zone)
            {
                return false;
            }
            if (!request.rack.empty() &&
                snapshot.failure_domain.rack != request.rack)
            {
                return false;
            }
            if (request.require_writable && !IsWritableStorageNode(snapshot))
            {
                return false;
            }
            return true;
        }

        bool IsObservedLeader(const ViewNodeSnapshot &snapshot)
        {
            return snapshot.metadata.has_value() &&
                   snapshot.metadata->raft_role ==
                       MetadataRaftObservedRole::kLeader;
        }

        bool PreferLeaderHint(const MetadataLeaderHint &current,
                              const MetadataLeaderHint &candidate)
        {
            if (candidate.observed_term != current.observed_term)
            {
                return candidate.observed_term > current.observed_term;
            }
            return candidate.observed_at_unix_ms > current.observed_at_unix_ms;
        }

        void MaybeUpdateLeaderHint(const ViewNodeSnapshot &snapshot,
                                   std::optional<MetadataLeaderHint> *leader_hint)
        {
            if (!snapshot.metadata.has_value() ||
                !snapshot.metadata->leader_hint.has_value())
            {
                return;
            }

            const auto &candidate = *snapshot.metadata->leader_hint;
            if (!leader_hint->has_value() ||
                PreferLeaderHint(**leader_hint, candidate))
            {
                *leader_hint = candidate;
            }
        }

        bool PassesMetadataFilters(const ViewNodeSnapshot &snapshot,
                                   const DiscoverMetadataRequest &request)
        {
            return snapshot.node_type == ViewNodeType::kMetadata &&
                   IsLiveForDiscovery(snapshot, request.live_only);
        }

        void SortMetadataSnapshots(std::vector<ViewNodeSnapshot> *snapshots,
                                   const bool prefer_leader)
        {
            std::sort(
                snapshots->begin(),
                snapshots->end(),
                [prefer_leader](const auto &lhs, const auto &rhs)
                {
                    if (prefer_leader)
                    {
                        const auto lhs_leader = IsObservedLeader(lhs);
                        const auto rhs_leader = IsObservedLeader(rhs);
                        if (lhs_leader != rhs_leader)
                        {
                            return lhs_leader;
                        }
                    }
                    return std::tie(lhs.node_id, lhs.endpoint) <
                           std::tie(rhs.node_id, rhs.endpoint);
                });
        }

        void SortStorageSnapshots(std::vector<ViewNodeSnapshot> *snapshots)
        {
            std::sort(
                snapshots->begin(),
                snapshots->end(),
                [](const auto &lhs, const auto &rhs)
                {
                    return std::tie(lhs.failure_domain.zone,
                                    lhs.failure_domain.rack,
                                    lhs.node_id,
                                    lhs.endpoint) <
                           std::tie(rhs.failure_domain.zone,
                                    rhs.failure_domain.rack,
                                    rhs.node_id,
                                    rhs.endpoint);
                });
        }

        void ApplyLimit(std::vector<ViewNodeSnapshot> *snapshots,
                        const std::uint32_t limit)
        {
            if (limit > 0 && snapshots->size() > limit)
            {
                snapshots->resize(limit);
            }
        }

        NodeRegistration NormalizeHeartbeatObservation(
            const HeartbeatNodeRequest &request)
        {
            NodeRegistration observation = request.observation;
            if (observation.cluster_id.empty())
            {
                observation.cluster_id = request.cluster_id;
            }
            if (observation.node_id.empty())
            {
                observation.node_id = request.node_id;
            }
            if (observation.node_type == ViewNodeType::kUnknown)
            {
                observation.node_type = request.node_type;
            }
            return observation;
        }

        void MergeRegistrationFacts(Record *record,
                                    const NodeRegistration &incoming)
        {
            if (!incoming.control_plane_endpoint.empty())
            {
                record->registration.control_plane_endpoint =
                    incoming.control_plane_endpoint;
            }
            if (!incoming.data_plane_endpoint.empty())
            {
                record->registration.data_plane_endpoint =
                    incoming.data_plane_endpoint;
            }
            if (!incoming.data_dir_fingerprint.empty())
            {
                record->registration.data_dir_fingerprint =
                    incoming.data_dir_fingerprint;
            }
            record->registration.observed_at_unix_ms =
                incoming.observed_at_unix_ms;
            record->registration.failure_domain = incoming.failure_domain;
            record->registration.health = incoming.health;
            record->registration.capacity = incoming.capacity;
            record->registration.load = incoming.load;
            record->registration.metadata = incoming.metadata;
        }

        void AppendLivenessWarning(const ViewNodeSnapshot &snapshot,
                                   const RequestId &request_id,
                                   std::vector<ViewRegistryDiagnostic> *diagnostics)
        {
            if (snapshot.liveness == ViewNodeLivenessState::kLive)
            {
                return;
            }

            diagnostics->push_back(
                MakeDiagnostic(ViewRegistryIssueCode::kLivenessExcluded,
                               "node is not live",
                               request_id,
                               snapshot.cluster_id,
                               snapshot.node_id,
                               snapshot.endpoint,
                               snapshot.last_sequence));
        }

        const char *UnknownString()
        {
            return "unknown";
        }
    } // namespace

    struct ViewNodeRegistry::Impl
    {
        explicit Impl(ViewRegistryConfig input_config)
            : config(NormalizeConfig(std::move(input_config)))
        {
        }

        ViewRegistryConfig config;
        mutable std::mutex mutex;
        Records records;
    };

    ViewNodeRegistry::ViewNodeRegistry(ViewRegistryConfig config)
        : impl_(std::make_unique<Impl>(std::move(config)))
    {
    }

    ViewNodeRegistry::~ViewNodeRegistry() = default;

    ViewNodeRegistry::ViewNodeRegistry(ViewNodeRegistry &&) noexcept = default;

    ViewNodeRegistry &ViewNodeRegistry::operator=(
        ViewNodeRegistry &&) noexcept = default;

    RegisterNodeResult ViewNodeRegistry::RegisterNode(
        const RegisterNodeRequest &request)
    {
        RegisterNodeResult result;
        std::string message;
        ViewRegistryIssueCode issue_code{ViewRegistryIssueCode::kUnknown};
        const auto status =
            ValidateRegistration(request.registration, &message, &issue_code);
        if (status != ViewRegistryStatusCode::kOk)
        {
            FillInvalidRegisterResult(&result,
                                      request,
                                      status,
                                      issue_code,
                                      std::move(message));
            return result;
        }

        std::lock_guard<std::mutex> lock(impl_->mutex);
        const RecordKey key{request.registration.cluster_id,
                            request.registration.node_id};

        if (impl_->config.enforce_unique_endpoints)
        {
            const auto *endpoint_owner = FindEndpointOwner(impl_->records,
                                                           key.cluster_id,
                                                           key.node_id,
                                                           request.registration);
            if (endpoint_owner != nullptr)
            {
                message = "endpoint is already registered to a different node_id";
                result.conflict = true;
                SetSummary(&result.summary,
                           ViewRegistryStatusCode::kConflict,
                           message,
                           request.request_id,
                           key.cluster_id,
                           key.node_id);
                result.diagnostics.push_back(
                    MakeDiagnostic(ViewRegistryIssueCode::kEndpointConflict,
                                   message,
                                   request.request_id,
                                   key.cluster_id,
                                   key.node_id,
                                   request.registration.endpoint));
                return result;
            }
        }

        const auto existing = impl_->records.find(key);
        if (existing != impl_->records.end())
        {
            if (!IsCompatibleRegistration(existing->second.registration,
                                          request.registration,
                                          &message,
                                          &issue_code))
            {
                result.conflict = true;
                SetSummary(&result.summary,
                           ViewRegistryStatusCode::kConflict,
                           message,
                           request.request_id,
                           key.cluster_id,
                           key.node_id);
                result.diagnostics.push_back(
                    MakeDiagnostic(issue_code,
                                   message,
                                   request.request_id,
                                   key.cluster_id,
                                   key.node_id,
                                   request.registration.endpoint));
                return result;
            }

            result.idempotent = true;
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kIdempotentReplay,
                       "node registration is compatible with existing record",
                       request.request_id,
                       key.cluster_id,
                       key.node_id);
            result.snapshot = MakeSnapshot(existing->second,
                                           request.registration.observed_at_unix_ms,
                                           impl_->config);
            return result;
        }

        Record record;
        record.registration = request.registration;
        record.registered_at_unix_ms = request.registration.observed_at_unix_ms;
        record.last_seen_unix_ms = request.registration.observed_at_unix_ms;
        record.last_sequence = 0;

        const auto inserted = impl_->records.emplace(key, std::move(record));
        result.created = true;
        SetSummary(&result.summary,
                   ViewRegistryStatusCode::kOk,
                   "node registered",
                   request.request_id,
                   key.cluster_id,
                   key.node_id);
        result.snapshot = MakeSnapshot(inserted.first->second,
                                       request.registration.observed_at_unix_ms,
                                       impl_->config);
        return result;
    }

    HeartbeatNodeResult ViewNodeRegistry::HeartbeatNode(
        const HeartbeatNodeRequest &request)
    {
        HeartbeatNodeResult result;
        if (!IsValidClusterId(request.cluster_id))
        {
            FillInvalidHeartbeatResult(
                &result,
                request,
                ViewRegistryStatusCode::kInvalidArgument,
                ViewRegistryIssueCode::kMissingClusterId,
                "cluster_id must contain only alnum, '-' or '_'");
            return result;
        }
        if (!IsValidNodeId(request.node_id))
        {
            FillInvalidHeartbeatResult(
                &result,
                request,
                ViewRegistryStatusCode::kInvalidArgument,
                ViewRegistryIssueCode::kMissingNodeId,
                "node_id must contain only alnum, '-' or '_'");
            return result;
        }
        if (request.sequence == 0)
        {
            FillInvalidHeartbeatResult(
                &result,
                request,
                ViewRegistryStatusCode::kInvalidArgument,
                ViewRegistryIssueCode::kStaleHeartbeat,
                "heartbeat sequence must be greater than zero");
            return result;
        }

        auto observation = NormalizeHeartbeatObservation(request);
        std::string message;
        ViewRegistryIssueCode issue_code{ViewRegistryIssueCode::kUnknown};
        const auto status =
            ValidateRegistration(observation, &message, &issue_code);
        if (status != ViewRegistryStatusCode::kOk)
        {
            FillInvalidHeartbeatResult(&result,
                                       request,
                                       status,
                                       issue_code,
                                       std::move(message));
            return result;
        }
        if (observation.cluster_id != request.cluster_id ||
            observation.node_id != request.node_id ||
            observation.node_type != request.node_type)
        {
            FillInvalidHeartbeatResult(
                &result,
                request,
                ViewRegistryStatusCode::kConflict,
                ViewRegistryIssueCode::kNodeIdConflict,
                "heartbeat observation identity does not match request identity");
            return result;
        }

        std::lock_guard<std::mutex> lock(impl_->mutex);
        const RecordKey key{request.cluster_id, request.node_id};
        const auto existing = impl_->records.find(key);
        if (existing == impl_->records.end())
        {
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kNotFound,
                       "node_id is not registered",
                       request.request_id,
                       request.cluster_id,
                       request.node_id);
            result.diagnostics.push_back(
                MakeDiagnostic(ViewRegistryIssueCode::kNodeUnavailable,
                               "node_id is not registered",
                               request.request_id,
                               request.cluster_id,
                               request.node_id,
                               observation.endpoint,
                               request.sequence));
            return result;
        }
        if (!IsCompatibleRegistration(existing->second.registration,
                                      observation,
                                      &message,
                                      &issue_code))
        {
            result.summary = MakeSummary(ViewRegistryStatusCode::kConflict,
                                         message,
                                         request.request_id,
                                         request.cluster_id,
                                         request.node_id);
            result.diagnostics.push_back(
                MakeDiagnostic(issue_code,
                               message,
                               request.request_id,
                               request.cluster_id,
                               request.node_id,
                               observation.endpoint,
                               request.sequence));
            return result;
        }

        result.accepted_sequence = existing->second.last_sequence;
        const auto decision = EvaluateSequenceDecision(
            existing->second.last_sequence,
            existing->second.last_seen_unix_ms,
            request.sequence,
            observation.observed_at_unix_ms);

        if (decision == SequenceDecision::kStale)
        {
            result.stale_ignored = true;
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kStaleIgnored,
                       "heartbeat is older than the latest accepted observation",
                       request.request_id,
                       request.cluster_id,
                       request.node_id);
            result.diagnostics.push_back(
                MakeDiagnostic(ViewRegistryIssueCode::kStaleHeartbeat,
                               "stale heartbeat ignored",
                               request.request_id,
                               request.cluster_id,
                               request.node_id,
                               observation.endpoint,
                               request.sequence));
            result.snapshot = MakeSnapshot(existing->second,
                                           observation.observed_at_unix_ms,
                                           impl_->config);
            return result;
        }
        if (decision == SequenceDecision::kIdempotent)
        {
            result.idempotent = true;
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kIdempotentReplay,
                       "heartbeat sequence already accepted",
                       request.request_id,
                       request.cluster_id,
                       request.node_id);
            result.snapshot = MakeSnapshot(existing->second,
                                           observation.observed_at_unix_ms,
                                           impl_->config);
            return result;
        }

        MergeRegistrationFacts(&existing->second, observation);
        existing->second.last_sequence = request.sequence;
        existing->second.last_seen_unix_ms = observation.observed_at_unix_ms;

        result.applied = true;
        result.accepted_sequence = request.sequence;
        SetSummary(&result.summary,
                   ViewRegistryStatusCode::kOk,
                   "heartbeat applied",
                   request.request_id,
                   request.cluster_id,
                   request.node_id);
        result.snapshot = MakeSnapshot(existing->second,
                                       observation.observed_at_unix_ms,
                                       impl_->config);
        return result;
    }

    HeartbeatNodeResult ViewNodeRegistry::RefreshSelfNode(
        const HeartbeatNodeRequest &request)
    {
        // self refresh 只是 ViewNode 对自己 observed state 的周期性更新，
        // 故意复用 heartbeat 的 sequence / observed_at / liveness 语义，
        // 避免把 self record 变成绕过 TTL 的永久 LIVE 特权。
        return HeartbeatNode(request);
    }

    LookupNodeResult ViewNodeRegistry::LookupNode(
        const std::string_view cluster_id,
        const std::string_view node_id,
        const std::uint64_t now_unix_ms) const
    {
        LookupNodeResult result;
        const ClusterId cluster_id_text(cluster_id);
        const NodeId node_id_text(node_id);
        if (!IsValidClusterId(cluster_id))
        {
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kInvalidArgument,
                       "cluster_id must contain only alnum, '-' or '_'",
                       {},
                       cluster_id_text,
                       node_id_text);
            return result;
        }
        if (!IsValidNodeId(node_id))
        {
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kInvalidArgument,
                       "node_id must contain only alnum, '-' or '_'",
                       {},
                       cluster_id_text,
                       node_id_text);
            return result;
        }

        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto existing =
            impl_->records.find(RecordKey{cluster_id_text, node_id_text});
        if (existing == impl_->records.end())
        {
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kNotFound,
                       "node_id is not registered",
                       {},
                       cluster_id_text,
                       node_id_text);
            result.diagnostics.push_back(
                MakeDiagnostic(ViewRegistryIssueCode::kNodeUnavailable,
                               "node_id is not registered",
                               {},
                               cluster_id_text,
                               node_id_text,
                               {}));
            return result;
        }

        SetSummary(&result.summary,
                   ViewRegistryStatusCode::kOk,
                   "node found",
                   {},
                   cluster_id_text,
                   node_id_text);
        result.snapshot = MakeSnapshot(existing->second,
                                       now_unix_ms,
                                       impl_->config);
        return result;
    }

    DiscoverMetadataResult ViewNodeRegistry::DiscoverMetadata(
        const DiscoverMetadataRequest &request,
        const std::uint64_t now_unix_ms) const
    {
        DiscoverMetadataResult result;
        if (!IsValidClusterId(request.cluster_id))
        {
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kInvalidArgument,
                       "cluster_id must contain only alnum, '-' or '_'",
                       request.request_id,
                       request.cluster_id,
                       {});
            return result;
        }

        std::lock_guard<std::mutex> lock(impl_->mutex);
        result.observed_at_unix_ms = now_unix_ms;
        for (const auto &[key, record] : impl_->records)
        {
            if (key.cluster_id != request.cluster_id)
            {
                continue;
            }

            auto snapshot = MakeSnapshot(record, now_unix_ms, impl_->config);
            if (snapshot.node_type != ViewNodeType::kMetadata)
            {
                continue;
            }
            if (!IsLiveForDiscovery(snapshot, request.live_only))
            {
                AppendLivenessWarning(snapshot,
                                      request.request_id,
                                      &result.diagnostics);
                continue;
            }

            if (snapshot.metadata.has_value())
            {
                result.membership_epoch =
                    std::max(result.membership_epoch,
                             snapshot.metadata->membership_epoch);
            }
            MaybeUpdateLeaderHint(snapshot, &result.leader_hint);
            if (PassesMetadataFilters(snapshot, request))
            {
                result.metadata_nodes.push_back(std::move(snapshot));
            }
        }

        SortMetadataSnapshots(&result.metadata_nodes, request.prefer_leader);
        ApplyLimit(&result.metadata_nodes, request.limit);
        if (!result.metadata_nodes.empty())
        {
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kOk,
                       "metadata discovery snapshot generated",
                       request.request_id,
                       request.cluster_id,
                       {});
            return result;
        }

        SetSummary(&result.summary,
                   ViewRegistryStatusCode::kNotFound,
                   "no metadata nodes matched discovery filters",
                   request.request_id,
                   request.cluster_id,
                   {});
        result.diagnostics.push_back(
            MakeDiagnostic(ViewRegistryIssueCode::kNodeUnavailable,
                           "no metadata nodes matched discovery filters",
                           request.request_id,
                           request.cluster_id,
                           {},
                           {}));
        return result;
    }

    DiscoverStorageResult ViewNodeRegistry::DiscoverStorage(
        const DiscoverStorageRequest &request,
        const std::uint64_t now_unix_ms) const
    {
        DiscoverStorageResult result;
        if (!IsValidClusterId(request.cluster_id))
        {
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kInvalidArgument,
                       "cluster_id must contain only alnum, '-' or '_'",
                       request.request_id,
                       request.cluster_id,
                       {});
            return result;
        }

        std::lock_guard<std::mutex> lock(impl_->mutex);
        result.observed_at_unix_ms = now_unix_ms;
        for (const auto &[key, record] : impl_->records)
        {
            if (key.cluster_id != request.cluster_id)
            {
                continue;
            }

            auto snapshot = MakeSnapshot(record, now_unix_ms, impl_->config);
            if (snapshot.node_type != ViewNodeType::kStorage)
            {
                continue;
            }
            if (!PassesStorageFilters(snapshot, request))
            {
                if (!IsLiveForDiscovery(snapshot, request.live_only))
                {
                    AppendLivenessWarning(snapshot,
                                          request.request_id,
                                          &result.diagnostics);
                }
                else if (snapshot.capacity.available_capacity_bytes <
                         request.minimum_available_capacity_bytes)
                {
                    result.diagnostics.push_back(
                        MakeDiagnostic(ViewRegistryIssueCode::kCapacityInsufficient,
                                       "storage node capacity is below discovery minimum",
                                       request.request_id,
                                       snapshot.cluster_id,
                                       snapshot.node_id,
                                       snapshot.endpoint,
                                       snapshot.last_sequence));
                }
                else if (request.require_writable &&
                         !IsWritableStorageNode(snapshot))
                {
                    result.diagnostics.push_back(
                        MakeDiagnostic(ViewRegistryIssueCode::kHealthExcluded,
                                       "storage node is not writable",
                                       request.request_id,
                                       snapshot.cluster_id,
                                       snapshot.node_id,
                                       snapshot.endpoint,
                                       snapshot.last_sequence));
                }
                continue;
            }
            result.storage_nodes.push_back(std::move(snapshot));
        }

        SortStorageSnapshots(&result.storage_nodes);
        ApplyLimit(&result.storage_nodes, request.limit);
        if (!result.storage_nodes.empty())
        {
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kOk,
                       "storage discovery snapshot generated",
                       request.request_id,
                       request.cluster_id,
                       {});
            return result;
        }

        SetSummary(&result.summary,
                   ViewRegistryStatusCode::kNotFound,
                   "no storage nodes matched discovery filters",
                   request.request_id,
                   request.cluster_id,
                   {});
        result.diagnostics.push_back(
            MakeDiagnostic(ViewRegistryIssueCode::kNodeUnavailable,
                           "no storage nodes matched discovery filters",
                           request.request_id,
                           request.cluster_id,
                           {},
                           {}));
        return result;
    }

    GetClusterViewResult ViewNodeRegistry::GetClusterView(
        const GetClusterViewRequest &request,
        const std::uint64_t now_unix_ms) const
    {
        GetClusterViewResult result;
        if (!IsValidClusterId(request.cluster_id))
        {
            SetSummary(&result.summary,
                       ViewRegistryStatusCode::kInvalidArgument,
                       "cluster_id must contain only alnum, '-' or '_'",
                       request.request_id,
                       request.cluster_id,
                       {});
            return result;
        }

        std::lock_guard<std::mutex> lock(impl_->mutex);
        result.snapshot.observed_at_unix_ms = now_unix_ms;
        for (const auto &[key, record] : impl_->records)
        {
            if (key.cluster_id != request.cluster_id)
            {
                continue;
            }

            auto snapshot = MakeSnapshot(record, now_unix_ms, impl_->config);
            if (!request.include_dead_nodes &&
                snapshot.liveness == ViewNodeLivenessState::kDead)
            {
                if (request.include_warnings)
                {
                    AppendLivenessWarning(snapshot,
                                          request.request_id,
                                          &result.snapshot.diagnostics);
                }
                continue;
            }
            if (request.include_warnings)
            {
                AppendLivenessWarning(snapshot,
                                      request.request_id,
                                      &result.snapshot.diagnostics);
            }
            MaybeUpdateLeaderHint(snapshot, &result.snapshot.leader_hint);

            switch (snapshot.node_type)
            {
            case ViewNodeType::kView:
                result.snapshot.view_nodes.push_back(std::move(snapshot));
                break;
            case ViewNodeType::kMetadata:
                result.snapshot.metadata_nodes.push_back(std::move(snapshot));
                break;
            case ViewNodeType::kStorage:
                result.snapshot.storage_nodes.push_back(std::move(snapshot));
                break;
            case ViewNodeType::kUnknown:
                break;
            }
        }

        SortMetadataSnapshots(&result.snapshot.metadata_nodes, true);
        SortStorageSnapshots(&result.snapshot.storage_nodes);
        std::sort(result.snapshot.view_nodes.begin(),
                  result.snapshot.view_nodes.end(),
                  [](const auto &lhs, const auto &rhs)
                  { return std::tie(lhs.node_id, lhs.endpoint) <
                           std::tie(rhs.node_id, rhs.endpoint); });

        SetSummary(&result.summary,
                   ViewRegistryStatusCode::kOk,
                   "cluster view snapshot generated",
                   request.request_id,
                   request.cluster_id,
                   {});
        return result;
    }

    std::size_t ViewNodeRegistry::size() const
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        return impl_->records.size();
    }

    const ViewRegistryConfig &ViewNodeRegistry::config() const
    {
        return impl_->config;
    }

    const char *ToString(const ViewNodeType node_type)
    {
        switch (node_type)
        {
        case ViewNodeType::kView:
            return "view";
        case ViewNodeType::kMetadata:
            return "metadata";
        case ViewNodeType::kStorage:
            return "storage";
        case ViewNodeType::kUnknown:
            return UnknownString();
        }
        return UnknownString();
    }

    const char *ToString(const ViewNodeLivenessState liveness)
    {
        switch (liveness)
        {
        case ViewNodeLivenessState::kLive:
            return "live";
        case ViewNodeLivenessState::kStale:
            return "stale";
        case ViewNodeLivenessState::kSuspect:
            return "suspect";
        case ViewNodeLivenessState::kDead:
            return "dead";
        case ViewNodeLivenessState::kUnknown:
            return UnknownString();
        }
        return UnknownString();
    }

    const char *ToString(const ViewNodeHealth health)
    {
        switch (health)
        {
        case ViewNodeHealth::kHealthy:
            return "healthy";
        case ViewNodeHealth::kDegraded:
            return "degraded";
        case ViewNodeHealth::kReadOnly:
            return "read_only";
        case ViewNodeHealth::kDraining:
            return "draining";
        case ViewNodeHealth::kUnavailable:
            return "unavailable";
        case ViewNodeHealth::kUnknown:
            return UnknownString();
        }
        return UnknownString();
    }

    const char *ToString(const ViewNodeDiskPressure pressure)
    {
        switch (pressure)
        {
        case ViewNodeDiskPressure::kLow:
            return "low";
        case ViewNodeDiskPressure::kMedium:
            return "medium";
        case ViewNodeDiskPressure::kHigh:
            return "high";
        case ViewNodeDiskPressure::kFull:
            return "full";
        case ViewNodeDiskPressure::kUnknown:
            return UnknownString();
        }
        return UnknownString();
    }

    const char *ToString(const MetadataMembershipObservedState state)
    {
        switch (state)
        {
        case MetadataMembershipObservedState::kRegistered:
            return "registered";
        case MetadataMembershipObservedState::kJoining:
            return "joining";
        case MetadataMembershipObservedState::kLearner:
            return "learner";
        case MetadataMembershipObservedState::kVoter:
            return "voter";
        case MetadataMembershipObservedState::kDown:
            return "down";
        case MetadataMembershipObservedState::kUnknown:
            return UnknownString();
        }
        return UnknownString();
    }

    const char *ToString(const MetadataRaftObservedRole role)
    {
        switch (role)
        {
        case MetadataRaftObservedRole::kFollower:
            return "follower";
        case MetadataRaftObservedRole::kCandidate:
            return "candidate";
        case MetadataRaftObservedRole::kLeader:
            return "leader";
        case MetadataRaftObservedRole::kLearner:
            return "learner";
        case MetadataRaftObservedRole::kObserver:
            return "observer";
        case MetadataRaftObservedRole::kUnknown:
            return UnknownString();
        }
        return UnknownString();
    }

    const char *ToString(const ViewRegistryStatusCode status)
    {
        switch (status)
        {
        case ViewRegistryStatusCode::kOk:
            return "ok";
        case ViewRegistryStatusCode::kIdempotentReplay:
            return "idempotent_replay";
        case ViewRegistryStatusCode::kInvalidArgument:
            return "invalid_argument";
        case ViewRegistryStatusCode::kNotFound:
            return "not_found";
        case ViewRegistryStatusCode::kConflict:
            return "conflict";
        case ViewRegistryStatusCode::kStaleIgnored:
            return "stale_ignored";
        case ViewRegistryStatusCode::kInternalError:
            return "internal_error";
        case ViewRegistryStatusCode::kTimeout:
            return "timeout";
        case ViewRegistryStatusCode::kOverloaded:
            return "overloaded";
        case ViewRegistryStatusCode::kServiceUnavailable:
            return "service_unavailable";
        case ViewRegistryStatusCode::kUnsupported:
            return "unsupported";
        }
        return UnknownString();
    }

    const char *ToString(const ViewRegistryIssueCode code)
    {
        switch (code)
        {
        case ViewRegistryIssueCode::kMissingClusterId:
            return "missing_cluster_id";
        case ViewRegistryIssueCode::kMissingNodeId:
            return "missing_node_id";
        case ViewRegistryIssueCode::kInvalidNodeType:
            return "invalid_node_type";
        case ViewRegistryIssueCode::kMissingEndpoint:
            return "missing_endpoint";
        case ViewRegistryIssueCode::kEndpointConflict:
            return "endpoint_conflict";
        case ViewRegistryIssueCode::kNodeIdConflict:
            return "node_id_conflict";
        case ViewRegistryIssueCode::kClusterMismatch:
            return "cluster_mismatch";
        case ViewRegistryIssueCode::kNodeTypeMismatch:
            return "node_type_mismatch";
        case ViewRegistryIssueCode::kDataDirFingerprintConflict:
            return "data_dir_fingerprint_conflict";
        case ViewRegistryIssueCode::kStaleHeartbeat:
            return "stale_heartbeat";
        case ViewRegistryIssueCode::kNodeUnavailable:
            return "node_unavailable";
        case ViewRegistryIssueCode::kLivenessExcluded:
            return "liveness_excluded";
        case ViewRegistryIssueCode::kCapacityInsufficient:
            return "capacity_insufficient";
        case ViewRegistryIssueCode::kHealthExcluded:
            return "health_excluded";
        case ViewRegistryIssueCode::kLeaderHintStale:
            return "leader_hint_stale";
        case ViewRegistryIssueCode::kNonAuthorityBoundary:
            return "non_authority_boundary";
        case ViewRegistryIssueCode::kUnknown:
            return UnknownString();
        }
        return UnknownString();
    }

    std::string DescribeViewRegistryDiagnostic(
        const ViewRegistryDiagnostic &diagnostic)
    {
        std::ostringstream out;
        out << ToString(diagnostic.code);
        if (!diagnostic.message.empty())
        {
            out << ": " << diagnostic.message;
        }
        if (!diagnostic.cluster_id.empty())
        {
            out << " cluster_id=" << diagnostic.cluster_id;
        }
        if (!diagnostic.node_id.empty())
        {
            out << " node_id=" << diagnostic.node_id;
        }
        if (!diagnostic.endpoint.empty())
        {
            out << " endpoint=" << diagnostic.endpoint;
        }
        if (diagnostic.sequence != 0)
        {
            out << " sequence=" << diagnostic.sequence;
        }
        return out.str();
    }
} // namespace viewdemo
