#include "view/view_service_impl.h"

#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace viewdemo
{
    namespace
    {
        struct StorageNodeIdResolution
        {
            ViewRegistryStatusCode status{ViewRegistryStatusCode::kOk};
            NodeRegistration registration;
            std::vector<ViewRegistryDiagnostic> diagnostics;
            bool conflict{false};
            bool confirmed_existing{false};
            bool generated_new{false};

            [[nodiscard]] bool ok() const
            {
                return status == ViewRegistryStatusCode::kOk;
            }
        };

        std::uint64_t SystemNowUnixMs()
        {
            const auto now = std::chrono::system_clock::now();
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch())
                    .count());
        }

        std::uint64_t ResolveNowUnixMs(const ViewNodeServiceImplConfig &config)
        {
            if (!config.now_unix_ms)
            {
                return SystemNowUnixMs();
            }

            const auto now_unix_ms = config.now_unix_ms();
            return now_unix_ms == 0 ? SystemNowUnixMs() : now_unix_ms;
        }

        std::uint64_t Fnv1a64(const std::string_view text)
        {
            constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
            constexpr std::uint64_t kPrime = 1099511628211ull;

            std::uint64_t hash = kOffsetBasis;
            for (const unsigned char ch : text)
            {
                hash ^= static_cast<std::uint64_t>(ch);
                hash *= kPrime;
            }
            return hash;
        }

        std::string ToLowerHex(const std::uint64_t value)
        {
            constexpr char kDigits[] = "0123456789abcdef";
            std::string text(16, '0');
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const auto shift =
                    static_cast<unsigned>((text.size() - 1 - i) * 4);
                text[i] = kDigits[(value >> shift) & 0x0fU];
            }
            return text;
        }

        std::string AllocateStorageNodeId(const std::string_view cluster_id,
                                          const std::string_view fingerprint)
        {
            std::string seed;
            seed.reserve(cluster_id.size() + fingerprint.size() + 1);
            seed.append(cluster_id);
            seed.push_back('\n');
            seed.append(fingerprint);
            return "store-" + ToLowerHex(Fnv1a64(seed));
        }

        ViewRegistryDiagnostic MakeServiceDiagnostic(
            const ViewRegistryIssueCode code,
            std::string message,
            const RequestId &request_id,
            const ClusterId &cluster_id,
            const NodeId &node_id,
            const Endpoint &endpoint)
        {
            return ViewRegistryDiagnostic{
                .code = code,
                .message = std::move(message),
                .request_id = request_id,
                .cluster_id = cluster_id,
                .node_id = node_id,
                .endpoint = endpoint,
                .sequence = 0};
        }

        ViewNodeType FromProtoNodeType(const ::view::ViewNodeType node_type)
        {
            switch (node_type)
            {
            case ::view::VIEW_NODE_TYPE_VIEW:
                return ViewNodeType::kView;
            case ::view::VIEW_NODE_TYPE_METADATA:
                return ViewNodeType::kMetadata;
            case ::view::VIEW_NODE_TYPE_STORAGE:
                return ViewNodeType::kStorage;
            case ::view::VIEW_NODE_TYPE_UNSPECIFIED:
                return ViewNodeType::kUnknown;
            }
            return ViewNodeType::kUnknown;
        }

        ::view::ViewNodeType ToProtoNodeType(const ViewNodeType node_type)
        {
            switch (node_type)
            {
            case ViewNodeType::kView:
                return ::view::VIEW_NODE_TYPE_VIEW;
            case ViewNodeType::kMetadata:
                return ::view::VIEW_NODE_TYPE_METADATA;
            case ViewNodeType::kStorage:
                return ::view::VIEW_NODE_TYPE_STORAGE;
            case ViewNodeType::kUnknown:
                return ::view::VIEW_NODE_TYPE_UNSPECIFIED;
            }
            return ::view::VIEW_NODE_TYPE_UNSPECIFIED;
        }

        ViewNodeLivenessState FromProtoLiveness(
            const ::view::ViewNodeLivenessState state)
        {
            switch (state)
            {
            case ::view::VIEW_NODE_LIVENESS_STATE_LIVE:
                return ViewNodeLivenessState::kLive;
            case ::view::VIEW_NODE_LIVENESS_STATE_STALE:
                return ViewNodeLivenessState::kStale;
            case ::view::VIEW_NODE_LIVENESS_STATE_SUSPECT:
                return ViewNodeLivenessState::kSuspect;
            case ::view::VIEW_NODE_LIVENESS_STATE_DEAD:
                return ViewNodeLivenessState::kDead;
            case ::view::VIEW_NODE_LIVENESS_STATE_UNSPECIFIED:
                return ViewNodeLivenessState::kUnknown;
            }
            return ViewNodeLivenessState::kUnknown;
        }

        ::view::ViewNodeLivenessState ToProtoLiveness(
            const ViewNodeLivenessState state)
        {
            switch (state)
            {
            case ViewNodeLivenessState::kLive:
                return ::view::VIEW_NODE_LIVENESS_STATE_LIVE;
            case ViewNodeLivenessState::kStale:
                return ::view::VIEW_NODE_LIVENESS_STATE_STALE;
            case ViewNodeLivenessState::kSuspect:
                return ::view::VIEW_NODE_LIVENESS_STATE_SUSPECT;
            case ViewNodeLivenessState::kDead:
                return ::view::VIEW_NODE_LIVENESS_STATE_DEAD;
            case ViewNodeLivenessState::kUnknown:
                return ::view::VIEW_NODE_LIVENESS_STATE_UNSPECIFIED;
            }
            return ::view::VIEW_NODE_LIVENESS_STATE_UNSPECIFIED;
        }

        ViewNodeHealth FromProtoHealth(const ::view::ViewNodeHealth health)
        {
            switch (health)
            {
            case ::view::VIEW_NODE_HEALTH_HEALTHY:
                return ViewNodeHealth::kHealthy;
            case ::view::VIEW_NODE_HEALTH_DEGRADED:
                return ViewNodeHealth::kDegraded;
            case ::view::VIEW_NODE_HEALTH_READ_ONLY:
                return ViewNodeHealth::kReadOnly;
            case ::view::VIEW_NODE_HEALTH_DRAINING:
                return ViewNodeHealth::kDraining;
            case ::view::VIEW_NODE_HEALTH_UNAVAILABLE:
                return ViewNodeHealth::kUnavailable;
            case ::view::VIEW_NODE_HEALTH_UNSPECIFIED:
                return ViewNodeHealth::kUnknown;
            }
            return ViewNodeHealth::kUnknown;
        }

        ::view::ViewNodeHealth ToProtoHealth(const ViewNodeHealth health)
        {
            switch (health)
            {
            case ViewNodeHealth::kHealthy:
                return ::view::VIEW_NODE_HEALTH_HEALTHY;
            case ViewNodeHealth::kDegraded:
                return ::view::VIEW_NODE_HEALTH_DEGRADED;
            case ViewNodeHealth::kReadOnly:
                return ::view::VIEW_NODE_HEALTH_READ_ONLY;
            case ViewNodeHealth::kDraining:
                return ::view::VIEW_NODE_HEALTH_DRAINING;
            case ViewNodeHealth::kUnavailable:
                return ::view::VIEW_NODE_HEALTH_UNAVAILABLE;
            case ViewNodeHealth::kUnknown:
                return ::view::VIEW_NODE_HEALTH_UNSPECIFIED;
            }
            return ::view::VIEW_NODE_HEALTH_UNSPECIFIED;
        }

        ViewNodeDiskPressure FromProtoDiskPressure(
            const ::view::ViewNodeDiskPressure pressure)
        {
            switch (pressure)
            {
            case ::view::VIEW_NODE_DISK_PRESSURE_LOW:
                return ViewNodeDiskPressure::kLow;
            case ::view::VIEW_NODE_DISK_PRESSURE_MEDIUM:
                return ViewNodeDiskPressure::kMedium;
            case ::view::VIEW_NODE_DISK_PRESSURE_HIGH:
                return ViewNodeDiskPressure::kHigh;
            case ::view::VIEW_NODE_DISK_PRESSURE_FULL:
                return ViewNodeDiskPressure::kFull;
            case ::view::VIEW_NODE_DISK_PRESSURE_UNSPECIFIED:
                return ViewNodeDiskPressure::kUnknown;
            }
            return ViewNodeDiskPressure::kUnknown;
        }

        ::view::ViewNodeDiskPressure ToProtoDiskPressure(
            const ViewNodeDiskPressure pressure)
        {
            switch (pressure)
            {
            case ViewNodeDiskPressure::kLow:
                return ::view::VIEW_NODE_DISK_PRESSURE_LOW;
            case ViewNodeDiskPressure::kMedium:
                return ::view::VIEW_NODE_DISK_PRESSURE_MEDIUM;
            case ViewNodeDiskPressure::kHigh:
                return ::view::VIEW_NODE_DISK_PRESSURE_HIGH;
            case ViewNodeDiskPressure::kFull:
                return ::view::VIEW_NODE_DISK_PRESSURE_FULL;
            case ViewNodeDiskPressure::kUnknown:
                return ::view::VIEW_NODE_DISK_PRESSURE_UNSPECIFIED;
            }
            return ::view::VIEW_NODE_DISK_PRESSURE_UNSPECIFIED;
        }

        MetadataMembershipObservedState FromProtoMembershipState(
            const ::view::MetadataMembershipObservedState state)
        {
            switch (state)
            {
            case ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_REGISTERED:
                return MetadataMembershipObservedState::kRegistered;
            case ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_JOINING:
                return MetadataMembershipObservedState::kJoining;
            case ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_LEARNER:
                return MetadataMembershipObservedState::kLearner;
            case ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_VOTER:
                return MetadataMembershipObservedState::kVoter;
            case ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_DOWN:
                return MetadataMembershipObservedState::kDown;
            case ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_UNSPECIFIED:
                return MetadataMembershipObservedState::kUnknown;
            }
            return MetadataMembershipObservedState::kUnknown;
        }

        ::view::MetadataMembershipObservedState ToProtoMembershipState(
            const MetadataMembershipObservedState state)
        {
            switch (state)
            {
            case MetadataMembershipObservedState::kRegistered:
                return ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_REGISTERED;
            case MetadataMembershipObservedState::kJoining:
                return ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_JOINING;
            case MetadataMembershipObservedState::kLearner:
                return ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_LEARNER;
            case MetadataMembershipObservedState::kVoter:
                return ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_VOTER;
            case MetadataMembershipObservedState::kDown:
                return ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_DOWN;
            case MetadataMembershipObservedState::kUnknown:
                return ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_UNSPECIFIED;
            }
            return ::view::METADATA_MEMBERSHIP_OBSERVED_STATE_UNSPECIFIED;
        }

        MetadataRaftObservedRole FromProtoRaftRole(
            const ::view::MetadataRaftObservedRole role)
        {
            switch (role)
            {
            case ::view::METADATA_RAFT_OBSERVED_ROLE_FOLLOWER:
                return MetadataRaftObservedRole::kFollower;
            case ::view::METADATA_RAFT_OBSERVED_ROLE_CANDIDATE:
                return MetadataRaftObservedRole::kCandidate;
            case ::view::METADATA_RAFT_OBSERVED_ROLE_LEADER:
                return MetadataRaftObservedRole::kLeader;
            case ::view::METADATA_RAFT_OBSERVED_ROLE_LEARNER:
                return MetadataRaftObservedRole::kLearner;
            case ::view::METADATA_RAFT_OBSERVED_ROLE_OBSERVER:
                return MetadataRaftObservedRole::kObserver;
            case ::view::METADATA_RAFT_OBSERVED_ROLE_UNSPECIFIED:
                return MetadataRaftObservedRole::kUnknown;
            }
            return MetadataRaftObservedRole::kUnknown;
        }

        ::view::MetadataRaftObservedRole ToProtoRaftRole(
            const MetadataRaftObservedRole role)
        {
            switch (role)
            {
            case MetadataRaftObservedRole::kFollower:
                return ::view::METADATA_RAFT_OBSERVED_ROLE_FOLLOWER;
            case MetadataRaftObservedRole::kCandidate:
                return ::view::METADATA_RAFT_OBSERVED_ROLE_CANDIDATE;
            case MetadataRaftObservedRole::kLeader:
                return ::view::METADATA_RAFT_OBSERVED_ROLE_LEADER;
            case MetadataRaftObservedRole::kLearner:
                return ::view::METADATA_RAFT_OBSERVED_ROLE_LEARNER;
            case MetadataRaftObservedRole::kObserver:
                return ::view::METADATA_RAFT_OBSERVED_ROLE_OBSERVER;
            case MetadataRaftObservedRole::kUnknown:
                return ::view::METADATA_RAFT_OBSERVED_ROLE_UNSPECIFIED;
            }
            return ::view::METADATA_RAFT_OBSERVED_ROLE_UNSPECIFIED;
        }

        ::view::ViewNodeStatusCode ToProtoStatusCode(
            const ViewRegistryStatusCode status)
        {
            switch (status)
            {
            case ViewRegistryStatusCode::kOk:
                return ::view::VIEW_NODE_STATUS_CODE_OK;
            case ViewRegistryStatusCode::kIdempotentReplay:
                return ::view::VIEW_NODE_STATUS_CODE_IDEMPOTENT_REPLAY;
            case ViewRegistryStatusCode::kInvalidArgument:
                return ::view::VIEW_NODE_STATUS_CODE_INVALID_ARGUMENT;
            case ViewRegistryStatusCode::kNotFound:
                return ::view::VIEW_NODE_STATUS_CODE_NOT_FOUND;
            case ViewRegistryStatusCode::kConflict:
                return ::view::VIEW_NODE_STATUS_CODE_CONFLICT;
            case ViewRegistryStatusCode::kStaleIgnored:
                return ::view::VIEW_NODE_STATUS_CODE_STALE_IGNORED;
            case ViewRegistryStatusCode::kInternalError:
                return ::view::VIEW_NODE_STATUS_CODE_INTERNAL_ERROR;
            case ViewRegistryStatusCode::kTimeout:
                return ::view::VIEW_NODE_STATUS_CODE_TIMEOUT;
            case ViewRegistryStatusCode::kOverloaded:
                return ::view::VIEW_NODE_STATUS_CODE_OVERLOADED;
            case ViewRegistryStatusCode::kServiceUnavailable:
                return ::view::VIEW_NODE_STATUS_CODE_SERVICE_UNAVAILABLE;
            case ViewRegistryStatusCode::kUnsupported:
                return ::view::VIEW_NODE_STATUS_CODE_UNSUPPORTED;
            }
            return ::view::VIEW_NODE_STATUS_CODE_UNSPECIFIED;
        }

        MetadataLeaderHint FromProtoLeaderHint(
            const ::view::MetadataLeaderHint &hint)
        {
            MetadataLeaderHint result;
            result.node_id = hint.node_id();
            if (hint.raft_id() != 0)
            {
                result.raft_id = hint.raft_id();
            }
            result.endpoint = hint.endpoint();
            result.observed_term = hint.observed_term();
            result.observed_at_unix_ms = hint.observed_at_unix_ms();
            return result;
        }

        MetadataNodeObservation FromProtoMetadataObservation(
            const ::view::MetadataNodeObservation &observation)
        {
            MetadataNodeObservation result;
            if (observation.raft_id() != 0)
            {
                result.raft_id = observation.raft_id();
            }
            result.raft_role = FromProtoRaftRole(observation.raft_role());
            result.membership_state =
                FromProtoMembershipState(observation.membership_state());
            if (observation.has_leader_hint())
            {
                result.leader_hint =
                    FromProtoLeaderHint(observation.leader_hint());
            }
            result.observed_term = observation.observed_term();
            result.commit_index = observation.commit_index();
            result.membership_epoch = observation.membership_epoch();
            return result;
        }

        NodeRegistration FromProtoRegistration(
            const ::view::ViewNodeRegistration &registration)
        {
            NodeRegistration result;
            result.cluster_id = registration.cluster_id();
            result.node_id = registration.node_id();
            result.node_type = FromProtoNodeType(registration.node_type());
            result.endpoint = registration.endpoint();
            result.control_plane_endpoint =
                registration.control_plane_endpoint();
            result.data_plane_endpoint = registration.data_plane_endpoint();
            result.data_dir_fingerprint = registration.data_dir_fingerprint();
            result.observed_at_unix_ms = registration.observed_at_unix_ms();
            result.failure_domain.zone = registration.failure_domain().zone();
            result.failure_domain.rack = registration.failure_domain().rack();
            result.health.health =
                FromProtoHealth(registration.health().health());
            result.health.disk_pressure =
                FromProtoDiskPressure(registration.health().disk_pressure());
            result.health.io_error_count =
                registration.health().io_error_count();
            result.capacity.total_capacity_bytes =
                registration.capacity().total_capacity_bytes();
            result.capacity.used_capacity_bytes =
                registration.capacity().used_capacity_bytes();
            result.capacity.available_capacity_bytes =
                registration.capacity().available_capacity_bytes();
            result.capacity.chunk_count = registration.capacity().chunk_count();
            result.load.active_reads = registration.load().active_reads();
            result.load.active_writes = registration.load().active_writes();
            result.load.queued_ops = registration.load().queued_ops();
            result.load.write_admission_overloaded =
                registration.load().write_admission_overloaded();
            result.load.read_admission_overloaded =
                registration.load().read_admission_overloaded();
            if (registration.has_metadata())
            {
                result.metadata =
                    FromProtoMetadataObservation(registration.metadata());
            }
            return result;
        }

        std::vector<ViewNodeSnapshot> FindStorageNodesByFingerprint(
            const ViewNodeRegistry &registry,
            const ClusterId &cluster_id,
            const std::string_view data_dir_fingerprint,
            const std::uint64_t now_unix_ms)
        {
            if (cluster_id.empty() || data_dir_fingerprint.empty())
            {
                return {};
            }

            const auto cluster_view = registry.GetClusterView(
                GetClusterViewRequest{.request_id = {},
                                      .cluster_id = cluster_id,
                                      .include_dead_nodes = true,
                                      .include_warnings = false},
                now_unix_ms);
            if (!cluster_view.ok())
            {
                return {};
            }

            std::vector<ViewNodeSnapshot> matches;
            for (const auto &snapshot : cluster_view.snapshot.storage_nodes)
            {
                if (snapshot.data_dir_fingerprint == data_dir_fingerprint)
                {
                    matches.push_back(snapshot);
                }
            }
            return matches;
        }

        StorageNodeIdResolution ResolveStorageNodeIdForRegistration(
            const RegisterNodeRequest &request,
            const ViewNodeRegistry &registry,
            const std::uint64_t now_unix_ms)
        {
            StorageNodeIdResolution resolution;
            resolution.registration = request.registration;

            if (resolution.registration.node_type != ViewNodeType::kStorage)
            {
                return resolution;
            }

            const auto &cluster_id = resolution.registration.cluster_id;
            const auto &fingerprint =
                resolution.registration.data_dir_fingerprint;
            const auto &endpoint = resolution.registration.endpoint;

            const auto fingerprint_matches = FindStorageNodesByFingerprint(
                registry,
                cluster_id,
                fingerprint,
                now_unix_ms);
            if (fingerprint_matches.size() > 1)
            {
                resolution.status = ViewRegistryStatusCode::kConflict;
                resolution.conflict = true;
                resolution.diagnostics.push_back(
                    MakeServiceDiagnostic(
                        ViewRegistryIssueCode::kDataDirFingerprintConflict,
                        "data_dir_fingerprint is already associated with multiple storage node_ids",
                        request.request_id,
                        cluster_id,
                        resolution.registration.node_id,
                        endpoint));
                return resolution;
            }

            if (!fingerprint_matches.empty())
            {
                const auto &matched = fingerprint_matches.front();
                if (!resolution.registration.node_id.empty() &&
                    resolution.registration.node_id != matched.node_id)
                {
                    resolution.status = ViewRegistryStatusCode::kConflict;
                    resolution.conflict = true;
                    resolution.diagnostics.push_back(
                        MakeServiceDiagnostic(
                            ViewRegistryIssueCode::kDataDirFingerprintConflict,
                            "data_dir_fingerprint is already registered to a different node_id",
                            request.request_id,
                            cluster_id,
                            resolution.registration.node_id,
                            endpoint));
                    return resolution;
                }

                resolution.registration.node_id = matched.node_id;
                resolution.confirmed_existing = true;
                return resolution;
            }

            if (!resolution.registration.node_id.empty())
            {
                return resolution;
            }

            if (resolution.registration.data_dir_fingerprint.empty())
            {
                resolution.status = ViewRegistryStatusCode::kInvalidArgument;
                resolution.diagnostics.push_back(
                    MakeServiceDiagnostic(
                        ViewRegistryIssueCode::kMissingNodeId,
                        "storage first registration with empty node_id requires non-empty data_dir_fingerprint",
                        request.request_id,
                        cluster_id,
                        resolution.registration.node_id,
                        endpoint));
                return resolution;
            }

            // ViewNode 这里只分配稳定的 discovery identity，不授予任何 metadata authority。
            const auto allocated_node_id =
                AllocateStorageNodeId(cluster_id, fingerprint);
            const auto existing =
                registry.LookupNode(cluster_id, allocated_node_id, now_unix_ms);
            if (existing.summary.status == ViewRegistryStatusCode::kOk &&
                existing.snapshot.has_value() &&
                existing.snapshot->data_dir_fingerprint != fingerprint)
            {
                resolution.status = ViewRegistryStatusCode::kConflict;
                resolution.conflict = true;
                resolution.diagnostics.push_back(
                    MakeServiceDiagnostic(
                        ViewRegistryIssueCode::kNodeIdConflict,
                        "allocated node_id is already registered to a different storage identity",
                        request.request_id,
                        cluster_id,
                        allocated_node_id,
                        endpoint));
                return resolution;
            }

            resolution.registration.node_id = allocated_node_id;
            resolution.generated_new = true;
            return resolution;
        }

        void FillProtoSummary(const ViewRegistryResponseSummary &summary,
                              ::view::ViewNodeResponseSummary *proto_summary)
        {
            proto_summary->set_code(ToProtoStatusCode(summary.status));
            proto_summary->set_message(summary.message);
            proto_summary->set_request_id(summary.request_id);
            proto_summary->set_cluster_id(summary.cluster_id);
            proto_summary->set_node_id(summary.node_id);
            proto_summary->set_retry_after_ms(summary.retry_after_ms);
        }

        void FillProtoLeaderHint(const MetadataLeaderHint &hint,
                                 ::view::MetadataLeaderHint *proto_hint)
        {
            proto_hint->set_node_id(hint.node_id);
            if (hint.raft_id.has_value())
            {
                proto_hint->set_raft_id(*hint.raft_id);
            }
            else
            {
                proto_hint->clear_raft_id();
            }
            proto_hint->set_endpoint(hint.endpoint);
            proto_hint->set_observed_term(hint.observed_term);
            proto_hint->set_observed_at_unix_ms(hint.observed_at_unix_ms);
        }

        void FillProtoMetadataObservation(
            const MetadataNodeObservation &observation,
            ::view::MetadataNodeObservation *proto_observation)
        {
            if (observation.raft_id.has_value())
            {
                proto_observation->set_raft_id(*observation.raft_id);
            }
            else
            {
                proto_observation->clear_raft_id();
            }
            proto_observation->set_raft_role(
                ToProtoRaftRole(observation.raft_role));
            proto_observation->set_membership_state(
                ToProtoMembershipState(observation.membership_state));
            if (observation.leader_hint.has_value())
            {
                FillProtoLeaderHint(*observation.leader_hint,
                                    proto_observation->mutable_leader_hint());
            }
            else
            {
                proto_observation->clear_leader_hint();
            }
            proto_observation->set_observed_term(observation.observed_term);
            proto_observation->set_commit_index(observation.commit_index);
            proto_observation->set_membership_epoch(
                observation.membership_epoch);
        }

        void FillProtoSnapshot(const viewdemo::ViewNodeSnapshot &snapshot,
                               ::view::ViewNodeSnapshot *proto_snapshot)
        {
            proto_snapshot->set_cluster_id(snapshot.cluster_id);
            proto_snapshot->set_node_id(snapshot.node_id);
            proto_snapshot->set_node_type(ToProtoNodeType(snapshot.node_type));
            proto_snapshot->set_endpoint(snapshot.endpoint);
            proto_snapshot->set_control_plane_endpoint(
                snapshot.control_plane_endpoint);
            proto_snapshot->set_data_plane_endpoint(snapshot.data_plane_endpoint);
            proto_snapshot->set_data_dir_fingerprint(
                snapshot.data_dir_fingerprint);
            proto_snapshot->set_registered_at_unix_ms(
                snapshot.registered_at_unix_ms);
            proto_snapshot->set_last_seen_unix_ms(snapshot.last_seen_unix_ms);
            proto_snapshot->set_last_sequence(snapshot.last_sequence);
            proto_snapshot->set_incarnation_id(snapshot.incarnation_id);
            proto_snapshot->set_liveness(ToProtoLiveness(snapshot.liveness));
            proto_snapshot->mutable_failure_domain()->set_zone(
                snapshot.failure_domain.zone);
            proto_snapshot->mutable_failure_domain()->set_rack(
                snapshot.failure_domain.rack);
            proto_snapshot->mutable_health()->set_health(
                ToProtoHealth(snapshot.health.health));
            proto_snapshot->mutable_health()->set_disk_pressure(
                ToProtoDiskPressure(snapshot.health.disk_pressure));
            proto_snapshot->mutable_health()->set_io_error_count(
                snapshot.health.io_error_count);
            proto_snapshot->mutable_capacity()->set_total_capacity_bytes(
                snapshot.capacity.total_capacity_bytes);
            proto_snapshot->mutable_capacity()->set_used_capacity_bytes(
                snapshot.capacity.used_capacity_bytes);
            proto_snapshot->mutable_capacity()->set_available_capacity_bytes(
                snapshot.capacity.available_capacity_bytes);
            proto_snapshot->mutable_capacity()->set_chunk_count(
                snapshot.capacity.chunk_count);
            proto_snapshot->mutable_load()->set_active_reads(
                snapshot.load.active_reads);
            proto_snapshot->mutable_load()->set_active_writes(
                snapshot.load.active_writes);
            proto_snapshot->mutable_load()->set_queued_ops(
                snapshot.load.queued_ops);
            proto_snapshot->mutable_load()->set_write_admission_overloaded(
                snapshot.load.write_admission_overloaded);
            proto_snapshot->mutable_load()->set_read_admission_overloaded(
                snapshot.load.read_admission_overloaded);
            if (snapshot.metadata.has_value())
            {
                FillProtoMetadataObservation(
                    *snapshot.metadata,
                    proto_snapshot->mutable_metadata());
            }
            else
            {
                proto_snapshot->clear_metadata();
            }
        }

        void FillProtoPeerSyncSnapshot(const ViewRegistryPeerSnapshot &snapshot,
                                       ::view::ViewPeerSyncSnapshot *proto_snapshot)
        {
            if (proto_snapshot == nullptr)
            {
                return;
            }

            proto_snapshot->set_cluster_id(snapshot.cluster_id);
            proto_snapshot->set_generated_at_unix_ms(
                snapshot.generated_at_unix_ms);
            for (const auto &view_node : snapshot.view_nodes)
            {
                FillProtoSnapshot(view_node, proto_snapshot->add_view_nodes());
            }
            for (const auto &metadata_node : snapshot.metadata_nodes)
            {
                FillProtoSnapshot(metadata_node,
                                  proto_snapshot->add_metadata_nodes());
            }
            for (const auto &storage_node : snapshot.storage_nodes)
            {
                FillProtoSnapshot(storage_node,
                                  proto_snapshot->add_storage_nodes());
            }
            if (snapshot.leader_hint.has_value())
            {
                FillProtoLeaderHint(*snapshot.leader_hint,
                                    proto_snapshot->mutable_leader_hint());
            }
            else
            {
                proto_snapshot->clear_leader_hint();
            }
        }

        template <typename Response>
        void AppendWarnings(
            const std::vector<ViewRegistryDiagnostic> &diagnostics,
            Response *response)
        {
            for (const auto &diagnostic : diagnostics)
            {
                auto *warning = response->add_warnings();
                warning->set_code(ToString(diagnostic.code));
                warning->set_message(
                    diagnostic.message.empty()
                        ? DescribeViewRegistryDiagnostic(diagnostic)
                        : diagnostic.message);
                warning->set_node_id(diagnostic.node_id);
                warning->set_endpoint(diagnostic.endpoint);
                warning->set_sequence(diagnostic.sequence);
            }
        }

        template <typename Response>
        void AppendNonAuthorityBoundaryWarning(const ClusterId &cluster_id,
                                               const RequestId &request_id,
                                               Response *response)
        {
            if (response == nullptr)
            {
                return;
            }

            auto *warning = response->add_warnings();
            warning->set_code(ToString(ViewRegistryIssueCode::kNonAuthorityBoundary));
            warning->set_message(
                "peer sync exchanges observed registry state only; it does not change Raft membership, quorum, or committed object visibility");
            warning->clear_node_id();
            warning->clear_endpoint();
            warning->set_sequence(0);
            static_cast<void>(cluster_id);
            static_cast<void>(request_id);
        }

        std::string BuildViewSelfRefreshDiagnosticMessage(
            const viewdemo::ViewNodeSnapshot &snapshot)
        {
            std::string message = "self_refresh_state";
            const auto append_text_field =
                [&message](const std::string_view key, const std::string_view value)
            {
                message.push_back(' ');
                message.append(key);
                message.push_back('=');
                message.append(value);
            };
            const auto append_u64_field =
                [&append_text_field](const std::string_view key,
                                     const std::uint64_t value)
            {
                append_text_field(key, std::to_string(value));
            };

            const bool refreshed = !snapshot.incarnation_id.empty() ||
                                   snapshot.last_sequence != 0;
            append_text_field("source",
                              refreshed ? "self_refresh" : "registration_only");
            append_text_field("node_id",
                              snapshot.node_id.empty() ? "<unknown>"
                                                       : snapshot.node_id);
            append_text_field("endpoint",
                              snapshot.endpoint.empty() ? "<unknown>"
                                                        : snapshot.endpoint);
            append_text_field("incarnation",
                              snapshot.incarnation_id.empty() ? "<none>"
                                                              : snapshot.incarnation_id);
            append_u64_field("sequence", snapshot.last_sequence);
            append_u64_field("last_seen_unix_ms", snapshot.last_seen_unix_ms);
            append_text_field("health", ToString(snapshot.health.health));
            append_text_field("liveness", ToString(snapshot.liveness));
            return message;
        }

        void AppendViewSelfRefreshDiagnostics(
            const ClusterViewSnapshot &snapshot,
            ::view::GetClusterViewResponse *response)
        {
            if (response == nullptr)
            {
                return;
            }

            for (const auto &view_node : snapshot.view_nodes)
            {
                auto *warning = response->add_warnings();
                warning->set_code("self_refresh_state");
                warning->set_message(
                    BuildViewSelfRefreshDiagnosticMessage(view_node));
                warning->set_node_id(view_node.node_id);
                warning->set_endpoint(view_node.endpoint);
                warning->set_sequence(view_node.observed_state.sequence);
            }
        }

        template <typename Response>
        void SetInternalSummary(Response *response,
                                std::string_view message,
                                std::string_view request_id = {},
                                std::string_view cluster_id = {},
                                std::string_view node_id = {})
        {
            if (response == nullptr)
            {
                return;
            }

            auto *summary = response->mutable_summary();
            summary->set_code(::view::VIEW_NODE_STATUS_CODE_INTERNAL_ERROR);
            summary->set_message(std::string(message));
            summary->set_request_id(std::string(request_id));
            summary->set_cluster_id(std::string(cluster_id));
            summary->set_node_id(std::string(node_id));
        }

        std::string AppendServiceNote(std::string base_message,
                                      const std::string_view note)
        {
            if (note.empty())
            {
                return base_message;
            }
            if (base_message.empty())
            {
                return std::string(note);
            }
            base_message.append("; service_note=");
            base_message.append(note);
            return base_message;
        }

        template <typename Response>
        ::grpc::Status ValidateRpcState(Response *response,
                                        const ViewNodeRegistry *registry,
                                        std::string_view request_id = {},
                                        std::string_view cluster_id = {},
                                        std::string_view node_id = {})
        {
            if (response == nullptr)
            {
                return ::grpc::Status(
                    ::grpc::StatusCode::INVALID_ARGUMENT,
                    "response must not be null");
            }
            if (registry == nullptr)
            {
                SetInternalSummary(response,
                                   "view registry is not configured",
                                   request_id,
                                   cluster_id,
                                   node_id);
                return ::grpc::Status(
                    ::grpc::StatusCode::FAILED_PRECONDITION,
                    "view registry is not configured");
            }
            return ::grpc::Status::OK;
        }

        ViewNodeSnapshot FromProtoSnapshot(
            const ::view::ViewNodeSnapshot &snapshot,
            const ClusterId &fallback_cluster_id)
        {
            ViewNodeSnapshot result;
            result.cluster_id = snapshot.cluster_id().empty()
                                    ? fallback_cluster_id
                                    : snapshot.cluster_id();
            result.node_id = snapshot.node_id();
            result.node_type = FromProtoNodeType(snapshot.node_type());
            result.incarnation_id = snapshot.incarnation_id();
            result.endpoint = snapshot.endpoint();
            result.control_plane_endpoint =
                snapshot.control_plane_endpoint();
            result.data_plane_endpoint =
                snapshot.data_plane_endpoint();
            result.data_dir_fingerprint =
                snapshot.data_dir_fingerprint();
            result.observed_state.incarnation_id = result.incarnation_id;
            result.observed_state.sequence = snapshot.last_sequence();
            result.observed_state.observed_at_unix_ms =
                snapshot.last_seen_unix_ms();
            result.registered_at_unix_ms = snapshot.registered_at_unix_ms();
            result.last_seen_unix_ms = snapshot.last_seen_unix_ms();
            result.last_sequence = snapshot.last_sequence();
            result.liveness = FromProtoLiveness(snapshot.liveness());
            result.failure_domain.zone =
                snapshot.failure_domain().zone();
            result.failure_domain.rack =
                snapshot.failure_domain().rack();
            result.health.health =
                FromProtoHealth(snapshot.health().health());
            result.health.disk_pressure =
                FromProtoDiskPressure(snapshot.health().disk_pressure());
            result.health.io_error_count =
                snapshot.health().io_error_count();
            result.capacity.total_capacity_bytes =
                snapshot.capacity().total_capacity_bytes();
            result.capacity.used_capacity_bytes =
                snapshot.capacity().used_capacity_bytes();
            result.capacity.available_capacity_bytes =
                snapshot.capacity().available_capacity_bytes();
            result.capacity.chunk_count =
                snapshot.capacity().chunk_count();
            result.load.active_reads = snapshot.load().active_reads();
            result.load.active_writes = snapshot.load().active_writes();
            result.load.queued_ops = snapshot.load().queued_ops();
            result.load.write_admission_overloaded =
                snapshot.load().write_admission_overloaded();
            result.load.read_admission_overloaded =
                snapshot.load().read_admission_overloaded();
            if (snapshot.has_metadata())
            {
                result.metadata =
                    FromProtoMetadataObservation(snapshot.metadata());
            }
            return result;
        }

        ViewRegistryPeerSnapshot FromProtoPeerSyncSnapshot(
            const ::view::ViewPeerSyncSnapshot &snapshot,
            const ClusterId &fallback_cluster_id)
        {
            ViewRegistryPeerSnapshot result;
            result.cluster_id = snapshot.cluster_id().empty()
                                    ? fallback_cluster_id
                                    : snapshot.cluster_id();
            result.generated_at_unix_ms = snapshot.generated_at_unix_ms();
            result.view_nodes.reserve(
                static_cast<std::size_t>(snapshot.view_nodes_size()));
            result.metadata_nodes.reserve(
                static_cast<std::size_t>(snapshot.metadata_nodes_size()));
            result.storage_nodes.reserve(
                static_cast<std::size_t>(snapshot.storage_nodes_size()));
            for (const auto &view_node : snapshot.view_nodes())
            {
                result.view_nodes.push_back(
                    FromProtoSnapshot(view_node, result.cluster_id));
            }
            for (const auto &metadata_node : snapshot.metadata_nodes())
            {
                result.metadata_nodes.push_back(
                    FromProtoSnapshot(metadata_node, result.cluster_id));
            }
            for (const auto &storage_node : snapshot.storage_nodes())
            {
                result.storage_nodes.push_back(
                    FromProtoSnapshot(storage_node, result.cluster_id));
            }
            if (snapshot.has_leader_hint())
            {
                result.leader_hint =
                    FromProtoLeaderHint(snapshot.leader_hint());
            }
            return result;
        }

        ::grpc::Status MakeInternalStatus(const std::string_view rpc_name,
                                          const std::exception &ex)
        {
            return ::grpc::Status(
                ::grpc::StatusCode::INTERNAL,
                std::string(rpc_name) + " failed inside view service adapter: " +
                    ex.what());
        }

        ::grpc::Status MakeUnknownInternalStatus(
            const std::string_view rpc_name)
        {
            return ::grpc::Status(
                ::grpc::StatusCode::INTERNAL,
                std::string(rpc_name) +
                    " failed inside view service adapter");
        }

        void FillServiceRegisterFailure(
            const RegisterNodeRequest &request,
            const StorageNodeIdResolution &resolution,
            ::view::RegisterNodeResponse *response)
        {
            FillProtoSummary(
                ViewRegistryResponseSummary{
                    .status = resolution.status,
                    .message = resolution.diagnostics.empty()
                                   ? "storage node registration failed before registry apply"
                                   : resolution.diagnostics.front().message,
                    .request_id = request.request_id,
                    .cluster_id = request.registration.cluster_id,
                    .node_id = resolution.registration.node_id,
                    .retry_after_ms = 0},
                response->mutable_summary());
            response->set_created(false);
            response->set_idempotent(false);
            response->set_conflict(resolution.conflict);
            response->clear_snapshot();
            AppendWarnings(resolution.diagnostics, response);
        }
    } // namespace

    ViewNodeServiceImpl::ViewNodeServiceImpl(
        std::shared_ptr<ViewNodeRegistry> registry,
        ViewNodeServiceImplConfig config)
        : registry_(std::move(registry)),
          config_(std::move(config))
    {
    }

    ::grpc::Status ViewNodeServiceImpl::RegisterNode(
        ::grpc::ServerContext *context,
        const ::view::RegisterNodeRequest *request,
        ::view::RegisterNodeResponse *response)
    {
        static_cast<void>(context);
        if (request == nullptr)
        {
            SetInternalSummary(response, "request must not be null");
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "request must not be null");
        }

        const auto state = ValidateRpcState(response,
                                            registry_.get(),
                                            request->request_id(),
                                            request->registration().cluster_id(),
                                            request->registration().node_id());
        if (!state.ok())
        {
            return state;
        }

        try
        {
            const RegisterNodeRequest register_request{
                .request_id = request->request_id(),
                .registration = FromProtoRegistration(request->registration())};
            // service adapter 只补 first registration 的 node_id 分配/确认边界；
            // ViewNode 仍然只是 discovery / observation 组件，不决定对象可见性。
            const auto resolution = ResolveStorageNodeIdForRegistration(
                register_request,
                *registry_,
                ResolveNowUnixMs(config_));
            if (!resolution.ok())
            {
                FillServiceRegisterFailure(register_request, resolution, response);
                return ::grpc::Status::OK;
            }

            const auto result = registry_->RegisterNode(
                RegisterNodeRequest{.request_id = register_request.request_id,
                                    .registration = resolution.registration});

            FillProtoSummary(result.summary, response->mutable_summary());
            if (resolution.generated_new)
            {
                response->mutable_summary()->set_message(AppendServiceNote(
                    response->mutable_summary()->message(),
                    "storage node_id allocated by view service and registration applied"));
            }
            else if (resolution.confirmed_existing)
            {
                response->mutable_summary()->set_message(AppendServiceNote(
                    response->mutable_summary()->message(),
                    "storage node_id confirmed from existing view registration"));
            }
            response->set_created(result.created);
            response->set_idempotent(result.idempotent);
            response->set_conflict(result.conflict);
            if (result.snapshot.has_value())
            {
                FillProtoSnapshot(*result.snapshot, response->mutable_snapshot());
            }
            else
            {
                response->clear_snapshot();
            }
            AppendWarnings(resolution.diagnostics, response);
            AppendWarnings(result.diagnostics, response);
            return ::grpc::Status::OK;
        }
        catch (const std::exception &ex)
        {
            SetInternalSummary(response,
                               ex.what(),
                               request->request_id(),
                               request->registration().cluster_id(),
                               request->registration().node_id());
            return MakeInternalStatus("RegisterNode", ex);
        }
        catch (...)
        {
            SetInternalSummary(response,
                               "unknown internal error",
                               request->request_id(),
                               request->registration().cluster_id(),
                               request->registration().node_id());
            return MakeUnknownInternalStatus("RegisterNode");
        }
    }

    ::grpc::Status ViewNodeServiceImpl::HeartbeatNode(
        ::grpc::ServerContext *context,
        const ::view::HeartbeatNodeRequest *request,
        ::view::HeartbeatNodeResponse *response)
    {
        static_cast<void>(context);
        if (request == nullptr)
        {
            SetInternalSummary(response, "request must not be null");
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "request must not be null");
        }

        const auto state = ValidateRpcState(response,
                                            registry_.get(),
                                            request->request_id(),
                                            request->cluster_id(),
                                            request->node_id());
        if (!state.ok())
        {
            return state;
        }

        try
        {
            const auto result = registry_->HeartbeatNode(HeartbeatNodeRequest{
                .request_id = request->request_id(),
                .cluster_id = request->cluster_id(),
                .node_id = request->node_id(),
                .node_type = FromProtoNodeType(request->node_type()),
                .incarnation_id = request->incarnation_id(),
                .sequence = request->sequence(),
                .observation = FromProtoRegistration(request->observation())});

            FillProtoSummary(result.summary, response->mutable_summary());
            response->set_accepted_sequence(result.accepted_sequence);
            response->set_applied(result.applied);
            response->set_idempotent(result.idempotent);
            response->set_stale_ignored(result.stale_ignored);
            if (result.snapshot.has_value())
            {
                FillProtoSnapshot(*result.snapshot, response->mutable_snapshot());
            }
            else
            {
                response->clear_snapshot();
            }
            return ::grpc::Status::OK;
        }
        catch (const std::exception &ex)
        {
            SetInternalSummary(response,
                               ex.what(),
                               request->request_id(),
                               request->cluster_id(),
                               request->node_id());
            return MakeInternalStatus("HeartbeatNode", ex);
        }
        catch (...)
        {
            SetInternalSummary(response,
                               "unknown internal error",
                               request->request_id(),
                               request->cluster_id(),
                               request->node_id());
            return MakeUnknownInternalStatus("HeartbeatNode");
        }
    }

    ::grpc::Status ViewNodeServiceImpl::DiscoverMetadata(
        ::grpc::ServerContext *context,
        const ::view::DiscoverMetadataRequest *request,
        ::view::DiscoverMetadataResponse *response)
    {
        static_cast<void>(context);
        if (request == nullptr)
        {
            SetInternalSummary(response, "request must not be null");
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "request must not be null");
        }

        const auto state = ValidateRpcState(response,
                                            registry_.get(),
                                            request->request_id(),
                                            request->cluster_id());
        if (!state.ok())
        {
            return state;
        }

        try
        {
            // leader hint 只作为观测提示返回，调用方仍必须处理 NOT_LEADER。
            const auto result = registry_->DiscoverMetadata(
                DiscoverMetadataRequest{.request_id = request->request_id(),
                                        .cluster_id = request->cluster_id(),
                                        .prefer_leader = request->prefer_leader(),
                                        .live_only = request->live_only(),
                                        .limit = request->limit()},
                ResolveNowUnixMs(config_));

            FillProtoSummary(result.summary, response->mutable_summary());
            response->set_observed_at_unix_ms(result.observed_at_unix_ms);
            response->set_membership_epoch(result.membership_epoch);
            for (const auto &snapshot : result.metadata_nodes)
            {
                FillProtoSnapshot(snapshot, response->add_metadata_nodes());
            }
            if (result.leader_hint.has_value())
            {
                FillProtoLeaderHint(*result.leader_hint,
                                    response->mutable_leader_hint());
            }
            else
            {
                response->clear_leader_hint();
            }
            AppendWarnings(result.diagnostics, response);
            return ::grpc::Status::OK;
        }
        catch (const std::exception &ex)
        {
            SetInternalSummary(response,
                               ex.what(),
                               request->request_id(),
                               request->cluster_id());
            return MakeInternalStatus("DiscoverMetadata", ex);
        }
        catch (...)
        {
            SetInternalSummary(response,
                               "unknown internal error",
                               request->request_id(),
                               request->cluster_id());
            return MakeUnknownInternalStatus("DiscoverMetadata");
        }
    }

    ::grpc::Status ViewNodeServiceImpl::DiscoverStorage(
        ::grpc::ServerContext *context,
        const ::view::DiscoverStorageRequest *request,
        ::view::DiscoverStorageResponse *response)
    {
        static_cast<void>(context);
        if (request == nullptr)
        {
            SetInternalSummary(response, "request must not be null");
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "request must not be null");
        }

        const auto state = ValidateRpcState(response,
                                            registry_.get(),
                                            request->request_id(),
                                            request->cluster_id());
        if (!state.ok())
        {
            return state;
        }

        try
        {
            const auto result = registry_->DiscoverStorage(
                DiscoverStorageRequest{
                    .request_id = request->request_id(),
                    .cluster_id = request->cluster_id(),
                    .live_only = request->live_only(),
                    .minimum_available_capacity_bytes =
                        request->minimum_available_capacity_bytes(),
                    .zone = request->zone(),
                    .rack = request->rack(),
                    .limit = request->limit(),
                    .require_writable = request->require_writable()},
                ResolveNowUnixMs(config_));

            FillProtoSummary(result.summary, response->mutable_summary());
            response->set_observed_at_unix_ms(result.observed_at_unix_ms);
            for (const auto &snapshot : result.storage_nodes)
            {
                FillProtoSnapshot(snapshot, response->add_storage_nodes());
            }
            AppendWarnings(result.diagnostics, response);
            return ::grpc::Status::OK;
        }
        catch (const std::exception &ex)
        {
            SetInternalSummary(response,
                               ex.what(),
                               request->request_id(),
                               request->cluster_id());
            return MakeInternalStatus("DiscoverStorage", ex);
        }
        catch (...)
        {
            SetInternalSummary(response,
                               "unknown internal error",
                               request->request_id(),
                               request->cluster_id());
            return MakeUnknownInternalStatus("DiscoverStorage");
        }
    }

    ::grpc::Status ViewNodeServiceImpl::GetClusterView(
        ::grpc::ServerContext *context,
        const ::view::GetClusterViewRequest *request,
        ::view::GetClusterViewResponse *response)
    {
        static_cast<void>(context);
        if (request == nullptr)
        {
            SetInternalSummary(response, "request must not be null");
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "request must not be null");
        }

        const auto state = ValidateRpcState(response,
                                            registry_.get(),
                                            request->request_id(),
                                            request->cluster_id());
        if (!state.ok())
        {
            return state;
        }

        try
        {
            const auto result = registry_->GetClusterView(
                GetClusterViewRequest{.request_id = request->request_id(),
                                      .cluster_id = request->cluster_id(),
                                      .include_dead_nodes =
                                          request->include_dead_nodes(),
                                      .include_warnings =
                                          request->include_warnings()},
                ResolveNowUnixMs(config_));

            FillProtoSummary(result.summary, response->mutable_summary());
            response->set_observed_at_unix_ms(
                result.snapshot.observed_at_unix_ms);
            for (const auto &snapshot : result.snapshot.view_nodes)
            {
                FillProtoSnapshot(snapshot, response->add_view_nodes());
            }
            for (const auto &snapshot : result.snapshot.metadata_nodes)
            {
                FillProtoSnapshot(snapshot, response->add_metadata_nodes());
            }
            for (const auto &snapshot : result.snapshot.storage_nodes)
            {
                FillProtoSnapshot(snapshot, response->add_storage_nodes());
            }
            if (result.snapshot.leader_hint.has_value())
            {
                FillProtoLeaderHint(*result.snapshot.leader_hint,
                                    response->mutable_leader_hint());
            }
            else
            {
                response->clear_leader_hint();
            }
            AppendWarnings(result.snapshot.diagnostics, response);
            if (request->include_warnings())
            {
                AppendViewSelfRefreshDiagnostics(result.snapshot, response);
            }
            return ::grpc::Status::OK;
        }
        catch (const std::exception &ex)
        {
            SetInternalSummary(response,
                               ex.what(),
                               request->request_id(),
                               request->cluster_id());
            return MakeInternalStatus("GetClusterView", ex);
        }
        catch (...)
        {
            SetInternalSummary(response,
                               "unknown internal error",
                               request->request_id(),
                               request->cluster_id());
            return MakeUnknownInternalStatus("GetClusterView");
        }
    }

    ::grpc::Status ViewNodeServiceImpl::PullPeerViewSnapshot(
        ::grpc::ServerContext *context,
        const ::view::PullPeerViewSnapshotRequest *request,
        ::view::PullPeerViewSnapshotResponse *response)
    {
        static_cast<void>(context);
        if (request == nullptr)
        {
            SetInternalSummary(response, "request must not be null");
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "request must not be null");
        }

        const auto state = ValidateRpcState(response,
                                            registry_.get(),
                                            request->request_id(),
                                            request->cluster_id());
        if (!state.ok())
        {
            return state;
        }

        try
        {
            const auto result = registry_->ExportPeerSnapshot(
                ExportPeerSnapshotRequest{
                    .request_id = request->request_id(),
                    .cluster_id = request->cluster_id(),
                    .include_dead_nodes = request->include_dead_nodes(),
                    .include_warnings = request->include_warnings()},
                ResolveNowUnixMs(config_));

            FillProtoSummary(result.summary, response->mutable_summary());
            FillProtoPeerSyncSnapshot(result.snapshot,
                                     response->mutable_snapshot());
            AppendWarnings(result.diagnostics, response);
            AppendNonAuthorityBoundaryWarning(request->cluster_id(),
                                              request->request_id(),
                                              response);
            return ::grpc::Status::OK;
        }
        catch (const std::exception &ex)
        {
            SetInternalSummary(response,
                               ex.what(),
                               request->request_id(),
                               request->cluster_id());
            return MakeInternalStatus("PullPeerViewSnapshot", ex);
        }
        catch (...)
        {
            SetInternalSummary(response,
                               "unknown internal error",
                               request->request_id(),
                               request->cluster_id());
            return MakeUnknownInternalStatus("PullPeerViewSnapshot");
        }
    }

    ::grpc::Status ViewNodeServiceImpl::PushPeerViewSnapshot(
        ::grpc::ServerContext *context,
        const ::view::PushPeerViewSnapshotRequest *request,
        ::view::PushPeerViewSnapshotResponse *response)
    {
        static_cast<void>(context);
        if (request == nullptr)
        {
            SetInternalSummary(response, "request must not be null");
            return ::grpc::Status(::grpc::StatusCode::INVALID_ARGUMENT,
                                  "request must not be null");
        }

        const auto state = ValidateRpcState(response,
                                            registry_.get(),
                                            request->request_id(),
                                            request->cluster_id());
        if (!state.ok())
        {
            return state;
        }

        try
        {
            const auto result = registry_->ImportPeerSnapshot(
                ImportPeerSnapshotRequest{
                    .request_id = request->request_id(),
                    .cluster_id = request->cluster_id(),
                    .snapshot = FromProtoPeerSyncSnapshot(request->snapshot(),
                                                          request->cluster_id())});

            FillProtoSummary(result.summary, response->mutable_summary());
            response->set_received_node_count(result.received_node_count);
            response->set_accepted_node_count(result.accepted_node_count);
            response->set_applied_node_count(result.applied_node_count);
            response->set_stale_ignored_node_count(
                result.stale_ignored_node_count);
            response->set_conflict_node_count(result.conflict_node_count);
            AppendWarnings(result.diagnostics, response);
            AppendNonAuthorityBoundaryWarning(request->cluster_id(),
                                              request->request_id(),
                                              response);
            return ::grpc::Status::OK;
        }
        catch (const std::exception &ex)
        {
            SetInternalSummary(response,
                               ex.what(),
                               request->request_id(),
                               request->cluster_id());
            return MakeInternalStatus("PushPeerViewSnapshot", ex);
        }
        catch (...)
        {
            SetInternalSummary(response,
                               "unknown internal error",
                               request->request_id(),
                               request->cluster_id());
            return MakeUnknownInternalStatus("PushPeerViewSnapshot");
        }
    }

    const std::shared_ptr<ViewNodeRegistry> &ViewNodeServiceImpl::registry() const
    {
        return registry_;
    }

    const ViewNodeServiceImplConfig &ViewNodeServiceImpl::config() const
    {
        return config_;
    }

} // namespace viewdemo
