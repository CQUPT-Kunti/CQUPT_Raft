#include "view/view_client.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace viewdemo
{
    namespace
    {
        ViewRegistryStatusCode FromProtoStatusCode(
            const view::ViewNodeStatusCode code)
        {
            switch (code)
            {
            case view::VIEW_NODE_STATUS_CODE_OK:
                return ViewRegistryStatusCode::kOk;
            case view::VIEW_NODE_STATUS_CODE_IDEMPOTENT_REPLAY:
                return ViewRegistryStatusCode::kIdempotentReplay;
            case view::VIEW_NODE_STATUS_CODE_INVALID_ARGUMENT:
                return ViewRegistryStatusCode::kInvalidArgument;
            case view::VIEW_NODE_STATUS_CODE_NOT_FOUND:
                return ViewRegistryStatusCode::kNotFound;
            case view::VIEW_NODE_STATUS_CODE_CONFLICT:
                return ViewRegistryStatusCode::kConflict;
            case view::VIEW_NODE_STATUS_CODE_STALE_IGNORED:
                return ViewRegistryStatusCode::kStaleIgnored;
            case view::VIEW_NODE_STATUS_CODE_TIMEOUT:
                return ViewRegistryStatusCode::kTimeout;
            case view::VIEW_NODE_STATUS_CODE_OVERLOADED:
                return ViewRegistryStatusCode::kOverloaded;
            case view::VIEW_NODE_STATUS_CODE_SERVICE_UNAVAILABLE:
                return ViewRegistryStatusCode::kServiceUnavailable;
            case view::VIEW_NODE_STATUS_CODE_UNSUPPORTED:
                return ViewRegistryStatusCode::kUnsupported;
            case view::VIEW_NODE_STATUS_CODE_INTERNAL_ERROR:
            case view::VIEW_NODE_STATUS_CODE_UNSPECIFIED:
            default:
                return ViewRegistryStatusCode::kInternalError;
            }
        }

        view::ViewNodeType ToProtoNodeType(const ViewNodeType node_type)
        {
            switch (node_type)
            {
            case ViewNodeType::kView:
                return view::VIEW_NODE_TYPE_VIEW;
            case ViewNodeType::kMetadata:
                return view::VIEW_NODE_TYPE_METADATA;
            case ViewNodeType::kStorage:
                return view::VIEW_NODE_TYPE_STORAGE;
            case ViewNodeType::kUnknown:
            default:
                return view::VIEW_NODE_TYPE_UNSPECIFIED;
            }
        }

        ViewNodeType FromProtoNodeType(const view::ViewNodeType node_type)
        {
            switch (node_type)
            {
            case view::VIEW_NODE_TYPE_VIEW:
                return ViewNodeType::kView;
            case view::VIEW_NODE_TYPE_METADATA:
                return ViewNodeType::kMetadata;
            case view::VIEW_NODE_TYPE_STORAGE:
                return ViewNodeType::kStorage;
            case view::VIEW_NODE_TYPE_UNSPECIFIED:
            default:
                return ViewNodeType::kUnknown;
            }
        }

        ViewNodeLivenessState FromProtoLiveness(
            const view::ViewNodeLivenessState liveness)
        {
            switch (liveness)
            {
            case view::VIEW_NODE_LIVENESS_STATE_LIVE:
                return ViewNodeLivenessState::kLive;
            case view::VIEW_NODE_LIVENESS_STATE_STALE:
                return ViewNodeLivenessState::kStale;
            case view::VIEW_NODE_LIVENESS_STATE_SUSPECT:
                return ViewNodeLivenessState::kSuspect;
            case view::VIEW_NODE_LIVENESS_STATE_DEAD:
                return ViewNodeLivenessState::kDead;
            case view::VIEW_NODE_LIVENESS_STATE_UNSPECIFIED:
            default:
                return ViewNodeLivenessState::kUnknown;
            }
        }

        view::ViewNodeLivenessState ToProtoLiveness(
            const ViewNodeLivenessState liveness)
        {
            switch (liveness)
            {
            case ViewNodeLivenessState::kLive:
                return view::VIEW_NODE_LIVENESS_STATE_LIVE;
            case ViewNodeLivenessState::kStale:
                return view::VIEW_NODE_LIVENESS_STATE_STALE;
            case ViewNodeLivenessState::kSuspect:
                return view::VIEW_NODE_LIVENESS_STATE_SUSPECT;
            case ViewNodeLivenessState::kDead:
                return view::VIEW_NODE_LIVENESS_STATE_DEAD;
            case ViewNodeLivenessState::kUnknown:
            default:
                return view::VIEW_NODE_LIVENESS_STATE_UNSPECIFIED;
            }
        }

        view::ViewNodeHealth ToProtoHealth(const ViewNodeHealth health)
        {
            switch (health)
            {
            case ViewNodeHealth::kHealthy:
                return view::VIEW_NODE_HEALTH_HEALTHY;
            case ViewNodeHealth::kDegraded:
                return view::VIEW_NODE_HEALTH_DEGRADED;
            case ViewNodeHealth::kReadOnly:
                return view::VIEW_NODE_HEALTH_READ_ONLY;
            case ViewNodeHealth::kDraining:
                return view::VIEW_NODE_HEALTH_DRAINING;
            case ViewNodeHealth::kUnavailable:
                return view::VIEW_NODE_HEALTH_UNAVAILABLE;
            case ViewNodeHealth::kUnknown:
            default:
                return view::VIEW_NODE_HEALTH_UNSPECIFIED;
            }
        }

        ViewNodeHealth FromProtoHealth(const view::ViewNodeHealth health)
        {
            switch (health)
            {
            case view::VIEW_NODE_HEALTH_HEALTHY:
                return ViewNodeHealth::kHealthy;
            case view::VIEW_NODE_HEALTH_DEGRADED:
                return ViewNodeHealth::kDegraded;
            case view::VIEW_NODE_HEALTH_READ_ONLY:
                return ViewNodeHealth::kReadOnly;
            case view::VIEW_NODE_HEALTH_DRAINING:
                return ViewNodeHealth::kDraining;
            case view::VIEW_NODE_HEALTH_UNAVAILABLE:
                return ViewNodeHealth::kUnavailable;
            case view::VIEW_NODE_HEALTH_UNSPECIFIED:
            default:
                return ViewNodeHealth::kUnknown;
            }
        }

        view::ViewNodeDiskPressure ToProtoDiskPressure(
            const ViewNodeDiskPressure pressure)
        {
            switch (pressure)
            {
            case ViewNodeDiskPressure::kLow:
                return view::VIEW_NODE_DISK_PRESSURE_LOW;
            case ViewNodeDiskPressure::kMedium:
                return view::VIEW_NODE_DISK_PRESSURE_MEDIUM;
            case ViewNodeDiskPressure::kHigh:
                return view::VIEW_NODE_DISK_PRESSURE_HIGH;
            case ViewNodeDiskPressure::kFull:
                return view::VIEW_NODE_DISK_PRESSURE_FULL;
            case ViewNodeDiskPressure::kUnknown:
            default:
                return view::VIEW_NODE_DISK_PRESSURE_UNSPECIFIED;
            }
        }

        ViewNodeDiskPressure FromProtoDiskPressure(
            const view::ViewNodeDiskPressure pressure)
        {
            switch (pressure)
            {
            case view::VIEW_NODE_DISK_PRESSURE_LOW:
                return ViewNodeDiskPressure::kLow;
            case view::VIEW_NODE_DISK_PRESSURE_MEDIUM:
                return ViewNodeDiskPressure::kMedium;
            case view::VIEW_NODE_DISK_PRESSURE_HIGH:
                return ViewNodeDiskPressure::kHigh;
            case view::VIEW_NODE_DISK_PRESSURE_FULL:
                return ViewNodeDiskPressure::kFull;
            case view::VIEW_NODE_DISK_PRESSURE_UNSPECIFIED:
            default:
                return ViewNodeDiskPressure::kUnknown;
            }
        }

        view::MetadataMembershipObservedState ToProtoMembershipState(
            const MetadataMembershipObservedState state)
        {
            switch (state)
            {
            case MetadataMembershipObservedState::kRegistered:
                return view::METADATA_MEMBERSHIP_OBSERVED_STATE_REGISTERED;
            case MetadataMembershipObservedState::kJoining:
                return view::METADATA_MEMBERSHIP_OBSERVED_STATE_JOINING;
            case MetadataMembershipObservedState::kLearner:
                return view::METADATA_MEMBERSHIP_OBSERVED_STATE_LEARNER;
            case MetadataMembershipObservedState::kVoter:
                return view::METADATA_MEMBERSHIP_OBSERVED_STATE_VOTER;
            case MetadataMembershipObservedState::kDown:
                return view::METADATA_MEMBERSHIP_OBSERVED_STATE_DOWN;
            case MetadataMembershipObservedState::kUnknown:
            default:
                return view::METADATA_MEMBERSHIP_OBSERVED_STATE_UNSPECIFIED;
            }
        }

        MetadataMembershipObservedState FromProtoMembershipState(
            const view::MetadataMembershipObservedState state)
        {
            switch (state)
            {
            case view::METADATA_MEMBERSHIP_OBSERVED_STATE_REGISTERED:
                return MetadataMembershipObservedState::kRegistered;
            case view::METADATA_MEMBERSHIP_OBSERVED_STATE_JOINING:
                return MetadataMembershipObservedState::kJoining;
            case view::METADATA_MEMBERSHIP_OBSERVED_STATE_LEARNER:
                return MetadataMembershipObservedState::kLearner;
            case view::METADATA_MEMBERSHIP_OBSERVED_STATE_VOTER:
                return MetadataMembershipObservedState::kVoter;
            case view::METADATA_MEMBERSHIP_OBSERVED_STATE_DOWN:
                return MetadataMembershipObservedState::kDown;
            case view::METADATA_MEMBERSHIP_OBSERVED_STATE_UNSPECIFIED:
            default:
                return MetadataMembershipObservedState::kUnknown;
            }
        }

        view::MetadataRaftObservedRole ToProtoRaftRole(
            const MetadataRaftObservedRole role)
        {
            switch (role)
            {
            case MetadataRaftObservedRole::kFollower:
                return view::METADATA_RAFT_OBSERVED_ROLE_FOLLOWER;
            case MetadataRaftObservedRole::kCandidate:
                return view::METADATA_RAFT_OBSERVED_ROLE_CANDIDATE;
            case MetadataRaftObservedRole::kLeader:
                return view::METADATA_RAFT_OBSERVED_ROLE_LEADER;
            case MetadataRaftObservedRole::kLearner:
                return view::METADATA_RAFT_OBSERVED_ROLE_LEARNER;
            case MetadataRaftObservedRole::kObserver:
                return view::METADATA_RAFT_OBSERVED_ROLE_OBSERVER;
            case MetadataRaftObservedRole::kUnknown:
            default:
                return view::METADATA_RAFT_OBSERVED_ROLE_UNSPECIFIED;
            }
        }

        MetadataRaftObservedRole FromProtoRaftRole(
            const view::MetadataRaftObservedRole role)
        {
            switch (role)
            {
            case view::METADATA_RAFT_OBSERVED_ROLE_FOLLOWER:
                return MetadataRaftObservedRole::kFollower;
            case view::METADATA_RAFT_OBSERVED_ROLE_CANDIDATE:
                return MetadataRaftObservedRole::kCandidate;
            case view::METADATA_RAFT_OBSERVED_ROLE_LEADER:
                return MetadataRaftObservedRole::kLeader;
            case view::METADATA_RAFT_OBSERVED_ROLE_LEARNER:
                return MetadataRaftObservedRole::kLearner;
            case view::METADATA_RAFT_OBSERVED_ROLE_OBSERVER:
                return MetadataRaftObservedRole::kObserver;
            case view::METADATA_RAFT_OBSERVED_ROLE_UNSPECIFIED:
            default:
                return MetadataRaftObservedRole::kUnknown;
            }
        }

        ViewRegistryIssueCode ParseIssueCode(const std::string_view code)
        {
            if (code == "missing_cluster_id")
            {
                return ViewRegistryIssueCode::kMissingClusterId;
            }
            if (code == "missing_node_id")
            {
                return ViewRegistryIssueCode::kMissingNodeId;
            }
            if (code == "invalid_node_type")
            {
                return ViewRegistryIssueCode::kInvalidNodeType;
            }
            if (code == "missing_endpoint")
            {
                return ViewRegistryIssueCode::kMissingEndpoint;
            }
            if (code == "endpoint_conflict")
            {
                return ViewRegistryIssueCode::kEndpointConflict;
            }
            if (code == "node_id_conflict")
            {
                return ViewRegistryIssueCode::kNodeIdConflict;
            }
            if (code == "cluster_mismatch")
            {
                return ViewRegistryIssueCode::kClusterMismatch;
            }
            if (code == "node_type_mismatch")
            {
                return ViewRegistryIssueCode::kNodeTypeMismatch;
            }
            if (code == "data_dir_fingerprint_conflict")
            {
                return ViewRegistryIssueCode::kDataDirFingerprintConflict;
            }
            if (code == "stale_heartbeat")
            {
                return ViewRegistryIssueCode::kStaleHeartbeat;
            }
            if (code == "node_unavailable")
            {
                return ViewRegistryIssueCode::kNodeUnavailable;
            }
            if (code == "liveness_excluded")
            {
                return ViewRegistryIssueCode::kLivenessExcluded;
            }
            if (code == "capacity_insufficient")
            {
                return ViewRegistryIssueCode::kCapacityInsufficient;
            }
            if (code == "health_excluded")
            {
                return ViewRegistryIssueCode::kHealthExcluded;
            }
            if (code == "leader_hint_stale")
            {
                return ViewRegistryIssueCode::kLeaderHintStale;
            }
            if (code == "non_authority_boundary")
            {
                return ViewRegistryIssueCode::kNonAuthorityBoundary;
            }
            return ViewRegistryIssueCode::kUnknown;
        }

        ViewRegistryStatusCode MapGrpcStatusCode(const grpc::StatusCode code)
        {
            switch (code)
            {
            case grpc::StatusCode::OK:
                return ViewRegistryStatusCode::kOk;
            case grpc::StatusCode::NOT_FOUND:
                return ViewRegistryStatusCode::kNotFound;
            case grpc::StatusCode::INVALID_ARGUMENT:
                return ViewRegistryStatusCode::kInvalidArgument;
            case grpc::StatusCode::ALREADY_EXISTS:
            case grpc::StatusCode::FAILED_PRECONDITION:
                return ViewRegistryStatusCode::kConflict;
            case grpc::StatusCode::DEADLINE_EXCEEDED:
                return ViewRegistryStatusCode::kTimeout;
            case grpc::StatusCode::RESOURCE_EXHAUSTED:
                return ViewRegistryStatusCode::kOverloaded;
            case grpc::StatusCode::UNAVAILABLE:
                return ViewRegistryStatusCode::kServiceUnavailable;
            case grpc::StatusCode::UNIMPLEMENTED:
                return ViewRegistryStatusCode::kUnsupported;
            case grpc::StatusCode::CANCELLED:
            case grpc::StatusCode::PERMISSION_DENIED:
            case grpc::StatusCode::UNAUTHENTICATED:
            case grpc::StatusCode::INTERNAL:
            case grpc::StatusCode::UNKNOWN:
            case grpc::StatusCode::DATA_LOSS:
            default:
                return ViewRegistryStatusCode::kInternalError;
            }
        }

        bool IsRetryableGrpcFailure(const grpc::StatusCode code)
        {
            return code == grpc::StatusCode::DEADLINE_EXCEEDED ||
                   code == grpc::StatusCode::UNAVAILABLE ||
                   code == grpc::StatusCode::RESOURCE_EXHAUSTED;
        }

        bool IsRetryableRegistryStatus(const ViewRegistryStatusCode status)
        {
            return status == ViewRegistryStatusCode::kTimeout ||
                   status == ViewRegistryStatusCode::kOverloaded ||
                   status == ViewRegistryStatusCode::kServiceUnavailable;
        }

        std::chrono::milliseconds ResolveTimeout(
            const std::chrono::milliseconds default_timeout,
            const ViewNodeClientCallOptions &options)
        {
            if (options.timeout.has_value())
            {
                return *options.timeout;
            }
            return default_timeout;
        }

        bool ResolveWaitForReady(const bool default_wait_for_ready,
                                 const ViewNodeClientCallOptions &options)
        {
            if (options.wait_for_ready.has_value())
            {
                return *options.wait_for_ready;
            }
            return default_wait_for_ready;
        }

        void ApplyRpcOptions(const std::chrono::milliseconds timeout,
                             const bool wait_for_ready,
                             grpc::ClientContext *context)
        {
            if (context == nullptr)
            {
                return;
            }
            if (timeout.count() > 0)
            {
                context->set_deadline(std::chrono::system_clock::now() + timeout);
            }
            context->set_wait_for_ready(wait_for_ready);
        }

        ViewNodeClientCallDiagnostics MakeRpcDiagnostics(
            const RequestId &request_id,
            const ClusterId &cluster_id,
            const NodeId &node_id,
            const Endpoint &target_endpoint,
            const std::chrono::milliseconds effective_timeout,
            const bool wait_for_ready)
        {
            ViewNodeClientCallDiagnostics diagnostics;
            diagnostics.request_id = request_id;
            diagnostics.cluster_id = cluster_id;
            diagnostics.node_id = node_id;
            diagnostics.target_endpoint = target_endpoint;
            diagnostics.effective_timeout = effective_timeout;
            diagnostics.wait_for_ready = wait_for_ready;
            return diagnostics;
        }

        void FillRpcFailureDiagnostics(const grpc::Status &grpc_status,
                                       ViewNodeClientCallDiagnostics *diagnostics)
        {
            if (diagnostics == nullptr)
            {
                return;
            }
            diagnostics->grpc_status_code = grpc_status.error_code();
            diagnostics->grpc_error_message = grpc_status.error_message();
            diagnostics->grpc_error_details = grpc_status.error_details();
            diagnostics->retryable = IsRetryableGrpcFailure(grpc_status.error_code());
        }

        void FillResponseSummary(const view::ViewNodeResponseSummary &proto_summary,
                                 ViewRegistryResponseSummary *summary)
        {
            if (summary == nullptr)
            {
                return;
            }
            summary->status = FromProtoStatusCode(proto_summary.code());
            summary->message = proto_summary.message();
            summary->request_id = proto_summary.request_id();
            summary->cluster_id = proto_summary.cluster_id();
            summary->node_id = proto_summary.node_id();
            summary->retry_after_ms = proto_summary.retry_after_ms();
        }

        void FillProtoLeaderHint(const MetadataLeaderHint &leader_hint,
                                 view::MetadataLeaderHint *proto)
        {
            if (proto == nullptr)
            {
                return;
            }

            proto->set_node_id(leader_hint.node_id);
            if (leader_hint.raft_id.has_value())
            {
                proto->set_raft_id(*leader_hint.raft_id);
            }
            proto->set_endpoint(leader_hint.endpoint);
            proto->set_observed_term(leader_hint.observed_term);
            proto->set_observed_at_unix_ms(leader_hint.observed_at_unix_ms);
        }

        MetadataLeaderHint FromProtoLeaderHint(
            const view::MetadataLeaderHint &proto)
        {
            MetadataLeaderHint leader_hint;
            leader_hint.node_id = proto.node_id();
            if (proto.raft_id() != 0)
            {
                leader_hint.raft_id = proto.raft_id();
            }
            leader_hint.endpoint = proto.endpoint();
            leader_hint.observed_term = proto.observed_term();
            leader_hint.observed_at_unix_ms = proto.observed_at_unix_ms();
            return leader_hint;
        }

        void FillProtoMetadataObservation(const MetadataNodeObservation &observation,
                                          view::MetadataNodeObservation *proto)
        {
            if (proto == nullptr)
            {
                return;
            }

            if (observation.raft_id.has_value())
            {
                proto->set_raft_id(*observation.raft_id);
            }
            proto->set_raft_role(ToProtoRaftRole(observation.raft_role));
            proto->set_membership_state(
                ToProtoMembershipState(observation.membership_state));
            if (observation.leader_hint.has_value())
            {
                FillProtoLeaderHint(*observation.leader_hint,
                                    proto->mutable_leader_hint());
            }
            proto->set_observed_term(observation.observed_term);
            proto->set_commit_index(observation.commit_index);
            proto->set_membership_epoch(observation.membership_epoch);
        }

        MetadataNodeObservation FromProtoMetadataObservation(
            const view::MetadataNodeObservation &proto)
        {
            MetadataNodeObservation observation;
            if (proto.raft_id() != 0)
            {
                observation.raft_id = proto.raft_id();
            }
            observation.raft_role = FromProtoRaftRole(proto.raft_role());
            observation.membership_state =
                FromProtoMembershipState(proto.membership_state());
            if (proto.has_leader_hint())
            {
                observation.leader_hint =
                    FromProtoLeaderHint(proto.leader_hint());
            }
            observation.observed_term = proto.observed_term();
            observation.commit_index = proto.commit_index();
            observation.membership_epoch = proto.membership_epoch();
            return observation;
        }

        void FillProtoRegistration(const NodeRegistration &registration,
                                   view::ViewNodeRegistration *proto)
        {
            if (proto == nullptr)
            {
                return;
            }

            proto->set_cluster_id(registration.cluster_id);
            proto->set_node_id(registration.node_id);
            proto->set_node_type(ToProtoNodeType(registration.node_type));
            proto->set_endpoint(registration.endpoint);
            proto->set_control_plane_endpoint(
                registration.control_plane_endpoint);
            proto->set_data_plane_endpoint(registration.data_plane_endpoint);
            proto->set_data_dir_fingerprint(registration.data_dir_fingerprint);
            proto->set_observed_at_unix_ms(registration.observed_at_unix_ms);
            proto->mutable_failure_domain()->set_zone(
                registration.failure_domain.zone);
            proto->mutable_failure_domain()->set_rack(
                registration.failure_domain.rack);
            proto->mutable_health()->set_health(
                ToProtoHealth(registration.health.health));
            proto->mutable_health()->set_disk_pressure(
                ToProtoDiskPressure(registration.health.disk_pressure));
            proto->mutable_health()->set_io_error_count(
                registration.health.io_error_count);
            proto->mutable_capacity()->set_total_capacity_bytes(
                registration.capacity.total_capacity_bytes);
            proto->mutable_capacity()->set_used_capacity_bytes(
                registration.capacity.used_capacity_bytes);
            proto->mutable_capacity()->set_available_capacity_bytes(
                registration.capacity.available_capacity_bytes);
            proto->mutable_capacity()->set_chunk_count(
                registration.capacity.chunk_count);
            proto->mutable_load()->set_active_reads(
                registration.load.active_reads);
            proto->mutable_load()->set_active_writes(
                registration.load.active_writes);
            proto->mutable_load()->set_queued_ops(
                registration.load.queued_ops);
            proto->mutable_load()->set_write_admission_overloaded(
                registration.load.write_admission_overloaded);
            proto->mutable_load()->set_read_admission_overloaded(
                registration.load.read_admission_overloaded);
            if (registration.metadata.has_value())
            {
                FillProtoMetadataObservation(*registration.metadata,
                                             proto->mutable_metadata());
            }
        }

        void FillProtoSnapshot(const ViewNodeSnapshot &snapshot,
                               view::ViewNodeSnapshot *proto)
        {
            if (proto == nullptr)
            {
                return;
            }

            proto->set_cluster_id(snapshot.cluster_id);
            proto->set_node_id(snapshot.node_id);
            proto->set_node_type(ToProtoNodeType(snapshot.node_type));
            proto->set_incarnation_id(snapshot.incarnation_id);
            proto->set_endpoint(snapshot.endpoint);
            proto->set_control_plane_endpoint(snapshot.control_plane_endpoint);
            proto->set_data_plane_endpoint(snapshot.data_plane_endpoint);
            proto->set_data_dir_fingerprint(snapshot.data_dir_fingerprint);
            proto->set_registered_at_unix_ms(snapshot.registered_at_unix_ms);
            proto->set_last_seen_unix_ms(snapshot.last_seen_unix_ms);
            proto->set_last_sequence(snapshot.last_sequence);
            proto->set_liveness(ToProtoLiveness(snapshot.liveness));
            proto->mutable_failure_domain()->set_zone(
                snapshot.failure_domain.zone);
            proto->mutable_failure_domain()->set_rack(
                snapshot.failure_domain.rack);
            proto->mutable_health()->set_health(
                ToProtoHealth(snapshot.health.health));
            proto->mutable_health()->set_disk_pressure(
                ToProtoDiskPressure(snapshot.health.disk_pressure));
            proto->mutable_health()->set_io_error_count(
                snapshot.health.io_error_count);
            proto->mutable_capacity()->set_total_capacity_bytes(
                snapshot.capacity.total_capacity_bytes);
            proto->mutable_capacity()->set_used_capacity_bytes(
                snapshot.capacity.used_capacity_bytes);
            proto->mutable_capacity()->set_available_capacity_bytes(
                snapshot.capacity.available_capacity_bytes);
            proto->mutable_capacity()->set_chunk_count(
                snapshot.capacity.chunk_count);
            proto->mutable_load()->set_active_reads(snapshot.load.active_reads);
            proto->mutable_load()->set_active_writes(
                snapshot.load.active_writes);
            proto->mutable_load()->set_queued_ops(snapshot.load.queued_ops);
            proto->mutable_load()->set_write_admission_overloaded(
                snapshot.load.write_admission_overloaded);
            proto->mutable_load()->set_read_admission_overloaded(
                snapshot.load.read_admission_overloaded);
            if (snapshot.metadata.has_value())
            {
                FillProtoMetadataObservation(*snapshot.metadata,
                                             proto->mutable_metadata());
            }
        }

        ViewNodeSnapshot FromProtoSnapshot(const view::ViewNodeSnapshot &proto)
        {
            ViewNodeSnapshot snapshot;
            snapshot.cluster_id = proto.cluster_id();
            snapshot.node_id = proto.node_id();
            snapshot.node_type = FromProtoNodeType(proto.node_type());
            snapshot.incarnation_id = proto.incarnation_id();
            snapshot.endpoint = proto.endpoint();
            snapshot.control_plane_endpoint = proto.control_plane_endpoint();
            snapshot.data_plane_endpoint = proto.data_plane_endpoint();
            snapshot.data_dir_fingerprint = proto.data_dir_fingerprint();
            snapshot.registered_at_unix_ms = proto.registered_at_unix_ms();
            snapshot.last_seen_unix_ms = proto.last_seen_unix_ms();
            snapshot.last_sequence = proto.last_sequence();
            snapshot.observed_state.incarnation_id = snapshot.incarnation_id;
            snapshot.observed_state.sequence = snapshot.last_sequence;
            snapshot.observed_state.observed_at_unix_ms =
                snapshot.last_seen_unix_ms;
            snapshot.liveness = FromProtoLiveness(proto.liveness());
            snapshot.failure_domain.zone = proto.failure_domain().zone();
            snapshot.failure_domain.rack = proto.failure_domain().rack();
            snapshot.health.health = FromProtoHealth(proto.health().health());
            snapshot.health.disk_pressure =
                FromProtoDiskPressure(proto.health().disk_pressure());
            snapshot.health.io_error_count = proto.health().io_error_count();
            snapshot.capacity.total_capacity_bytes =
                proto.capacity().total_capacity_bytes();
            snapshot.capacity.used_capacity_bytes =
                proto.capacity().used_capacity_bytes();
            snapshot.capacity.available_capacity_bytes =
                proto.capacity().available_capacity_bytes();
            snapshot.capacity.chunk_count = proto.capacity().chunk_count();
            snapshot.load.active_reads = proto.load().active_reads();
            snapshot.load.active_writes = proto.load().active_writes();
            snapshot.load.queued_ops = proto.load().queued_ops();
            snapshot.load.write_admission_overloaded =
                proto.load().write_admission_overloaded();
            snapshot.load.read_admission_overloaded =
                proto.load().read_admission_overloaded();
            if (proto.has_metadata())
            {
                snapshot.metadata = FromProtoMetadataObservation(proto.metadata());
            }
            return snapshot;
        }

        ViewRegistryDiagnostic WarningToDiagnostic(
            const view::ClusterViewWarning &warning,
            const RequestId &request_id,
            const ClusterId &cluster_id,
            const NodeId &node_id)
        {
            ViewRegistryDiagnostic diagnostic;
            diagnostic.code = ParseIssueCode(warning.code());
            diagnostic.message = warning.message();
            diagnostic.request_id = request_id;
            diagnostic.cluster_id = cluster_id;
            diagnostic.node_id =
                warning.node_id().empty() ? node_id : warning.node_id();
            diagnostic.endpoint = warning.endpoint();
            diagnostic.sequence = warning.sequence();
            return diagnostic;
        }

        template <typename WarningContainer>
        std::vector<ViewRegistryDiagnostic> WarningsToDiagnostics(
            const WarningContainer &warnings,
            const RequestId &request_id,
            const ClusterId &cluster_id,
            const NodeId &node_id)
        {
            std::vector<ViewRegistryDiagnostic> diagnostics;
            diagnostics.reserve(static_cast<std::size_t>(warnings.size()));
            for (const auto &warning : warnings)
            {
                diagnostics.push_back(
                    WarningToDiagnostic(warning, request_id, cluster_id, node_id));
            }
            return diagnostics;
        }

        ViewRegistryDiagnostic MakeTransportDiagnostic(
            const ViewNodeClientCallDiagnostics &rpc)
        {
            ViewRegistryDiagnostic diagnostic;
            diagnostic.code =
                rpc.retryable ? ViewRegistryIssueCode::kNodeUnavailable
                              : ViewRegistryIssueCode::kUnknown;
            diagnostic.message = rpc.grpc_error_message.empty()
                                     ? "ViewNode RPC transport failed"
                                     : rpc.grpc_error_message;
            diagnostic.request_id = rpc.request_id;
            diagnostic.cluster_id = rpc.cluster_id;
            diagnostic.node_id = rpc.node_id;
            diagnostic.endpoint = rpc.target_endpoint;
            return diagnostic;
        }

        void FillTransportFailureSummary(const ViewNodeClientCallDiagnostics &rpc,
                                         ViewRegistryResponseSummary *summary)
        {
            if (summary == nullptr)
            {
                return;
            }
            summary->status = MapGrpcStatusCode(rpc.grpc_status_code);
            summary->message = rpc.grpc_error_message.empty()
                                   ? "ViewNode RPC transport failed"
                                   : rpc.grpc_error_message;
            summary->request_id = rpc.request_id;
            summary->cluster_id = rpc.cluster_id;
            summary->node_id = rpc.node_id;
            summary->retry_after_ms = 0;
        }

        // client adapter 只做 transport + proto 映射；leader hint 和 discovery
        // 结果仍然只是观测信息，不能在这里升级成 authority。
        void FillRegisterResponse(const view::RegisterNodeResponse &proto,
                                  RegisterNodeResult *result)
        {
            if (result == nullptr)
            {
                return;
            }
            FillResponseSummary(proto.summary(), &result->summary);
            result->created = proto.created();
            result->idempotent = proto.idempotent();
            result->conflict = proto.conflict();
            if (proto.has_snapshot())
            {
                result->snapshot = FromProtoSnapshot(proto.snapshot());
            }
            result->diagnostics = WarningsToDiagnostics(proto.warnings(),
                                                        result->summary.request_id,
                                                        result->summary.cluster_id,
                                                        result->summary.node_id);
        }

        void FillHeartbeatResponse(const view::HeartbeatNodeResponse &proto,
                                   HeartbeatNodeResult *result)
        {
            if (result == nullptr)
            {
                return;
            }
            FillResponseSummary(proto.summary(), &result->summary);
            result->accepted_sequence = proto.accepted_sequence();
            result->applied = proto.applied();
            result->idempotent = proto.idempotent();
            result->stale_ignored = proto.stale_ignored();
            if (proto.has_snapshot())
            {
                result->snapshot = FromProtoSnapshot(proto.snapshot());
            }
        }

        void FillDiscoverMetadataResponse(
            const view::DiscoverMetadataResponse &proto,
            DiscoverMetadataResult *result)
        {
            if (result == nullptr)
            {
                return;
            }
            FillResponseSummary(proto.summary(), &result->summary);
            result->observed_at_unix_ms = proto.observed_at_unix_ms();
            result->membership_epoch = proto.membership_epoch();
            result->metadata_nodes.reserve(
                static_cast<std::size_t>(proto.metadata_nodes_size()));
            for (const auto &snapshot : proto.metadata_nodes())
            {
                result->metadata_nodes.push_back(FromProtoSnapshot(snapshot));
            }
            if (proto.has_leader_hint())
            {
                result->leader_hint = FromProtoLeaderHint(proto.leader_hint());
            }
            result->diagnostics = WarningsToDiagnostics(proto.warnings(),
                                                        result->summary.request_id,
                                                        result->summary.cluster_id,
                                                        result->summary.node_id);
        }

        void FillDiscoverStorageResponse(
            const view::DiscoverStorageResponse &proto,
            DiscoverStorageResult *result)
        {
            if (result == nullptr)
            {
                return;
            }
            FillResponseSummary(proto.summary(), &result->summary);
            result->observed_at_unix_ms = proto.observed_at_unix_ms();
            result->storage_nodes.reserve(
                static_cast<std::size_t>(proto.storage_nodes_size()));
            for (const auto &snapshot : proto.storage_nodes())
            {
                result->storage_nodes.push_back(FromProtoSnapshot(snapshot));
            }
            result->diagnostics = WarningsToDiagnostics(proto.warnings(),
                                                        result->summary.request_id,
                                                        result->summary.cluster_id,
                                                        result->summary.node_id);
        }

        void FillClusterViewResponse(const view::GetClusterViewResponse &proto,
                                     GetClusterViewResult *result)
        {
            if (result == nullptr)
            {
                return;
            }
            FillResponseSummary(proto.summary(), &result->summary);
            result->snapshot.observed_at_unix_ms = proto.observed_at_unix_ms();
            result->snapshot.view_nodes.reserve(
                static_cast<std::size_t>(proto.view_nodes_size()));
            result->snapshot.metadata_nodes.reserve(
                static_cast<std::size_t>(proto.metadata_nodes_size()));
            result->snapshot.storage_nodes.reserve(
                static_cast<std::size_t>(proto.storage_nodes_size()));
            for (const auto &snapshot : proto.view_nodes())
            {
                result->snapshot.view_nodes.push_back(FromProtoSnapshot(snapshot));
            }
            for (const auto &snapshot : proto.metadata_nodes())
            {
                result->snapshot.metadata_nodes.push_back(
                    FromProtoSnapshot(snapshot));
            }
            for (const auto &snapshot : proto.storage_nodes())
            {
                result->snapshot.storage_nodes.push_back(
                    FromProtoSnapshot(snapshot));
            }
            if (proto.has_leader_hint())
            {
                result->snapshot.leader_hint =
                    FromProtoLeaderHint(proto.leader_hint());
            }
            result->snapshot.diagnostics =
                WarningsToDiagnostics(proto.warnings(),
                                      result->summary.request_id,
                                      result->summary.cluster_id,
                                      result->summary.node_id);
        }

        void FillProtoPeerSyncSnapshot(const ViewPeerSyncSnapshot &snapshot,
                                       view::ViewPeerSyncSnapshot *proto)
        {
            if (proto == nullptr)
            {
                return;
            }

            proto->set_cluster_id(snapshot.cluster_id);
            proto->set_generated_at_unix_ms(snapshot.generated_at_unix_ms);
            for (const auto &view_node : snapshot.view_nodes)
            {
                FillProtoSnapshot(view_node, proto->add_view_nodes());
            }
            for (const auto &metadata_node : snapshot.metadata_nodes)
            {
                FillProtoSnapshot(metadata_node, proto->add_metadata_nodes());
            }
            for (const auto &storage_node : snapshot.storage_nodes)
            {
                FillProtoSnapshot(storage_node, proto->add_storage_nodes());
            }
            if (snapshot.leader_hint.has_value())
            {
                FillProtoLeaderHint(*snapshot.leader_hint,
                                    proto->mutable_leader_hint());
            }
        }

        ViewPeerSyncSnapshot FromProtoPeerSyncSnapshot(
            const view::ViewPeerSyncSnapshot &proto)
        {
            ViewPeerSyncSnapshot snapshot;
            snapshot.cluster_id = proto.cluster_id();
            snapshot.generated_at_unix_ms = proto.generated_at_unix_ms();
            snapshot.view_nodes.reserve(
                static_cast<std::size_t>(proto.view_nodes_size()));
            snapshot.metadata_nodes.reserve(
                static_cast<std::size_t>(proto.metadata_nodes_size()));
            snapshot.storage_nodes.reserve(
                static_cast<std::size_t>(proto.storage_nodes_size()));
            for (const auto &view_node : proto.view_nodes())
            {
                snapshot.view_nodes.push_back(FromProtoSnapshot(view_node));
            }
            for (const auto &metadata_node : proto.metadata_nodes())
            {
                snapshot.metadata_nodes.push_back(
                    FromProtoSnapshot(metadata_node));
            }
            for (const auto &storage_node : proto.storage_nodes())
            {
                snapshot.storage_nodes.push_back(
                    FromProtoSnapshot(storage_node));
            }
            if (proto.has_leader_hint())
            {
                snapshot.leader_hint = FromProtoLeaderHint(proto.leader_hint());
            }
            return snapshot;
        }

        void FillPullPeerViewSnapshotResponse(
            const view::PullPeerViewSnapshotResponse &proto,
            PullPeerViewSnapshotResult *result)
        {
            if (result == nullptr)
            {
                return;
            }
            FillResponseSummary(proto.summary(), &result->summary);
            if (proto.has_snapshot())
            {
                result->snapshot = FromProtoPeerSyncSnapshot(proto.snapshot());
            }
            result->diagnostics = WarningsToDiagnostics(proto.warnings(),
                                                        result->summary.request_id,
                                                        result->summary.cluster_id,
                                                        result->summary.node_id);
        }

        void FillPushPeerViewSnapshotResponse(
            const view::PushPeerViewSnapshotResponse &proto,
            PushPeerViewSnapshotResult *result)
        {
            if (result == nullptr)
            {
                return;
            }
            FillResponseSummary(proto.summary(), &result->summary);
            result->received_node_count = proto.received_node_count();
            result->accepted_node_count = proto.accepted_node_count();
            result->applied_node_count = proto.applied_node_count();
            result->stale_ignored_node_count =
                proto.stale_ignored_node_count();
            result->conflict_node_count = proto.conflict_node_count();
            result->diagnostics = WarningsToDiagnostics(proto.warnings(),
                                                        result->summary.request_id,
                                                        result->summary.cluster_id,
                                                        result->summary.node_id);
        }
    } // namespace

    ViewNodeClient::ViewNodeClient(
        std::unique_ptr<view::ViewNodeService::StubInterface> stub,
        std::string target_endpoint,
        ViewNodeClientConfig config)
        : stub_(std::move(stub))
        , target_endpoint_(std::move(target_endpoint))
        , config_(config)
    {
        if (stub_ == nullptr)
        {
            throw std::invalid_argument(
                "ViewNodeClient requires a non-null stub");
        }
    }

    ViewNodeClient::ViewNodeClient(std::shared_ptr<grpc::Channel> channel,
                                   std::string target_endpoint,
                                   ViewNodeClientConfig config)
        : ViewNodeClient(view::ViewNodeService::NewStub(std::move(channel)),
                         std::move(target_endpoint),
                         config)
    {
    }

    ViewNodeClientRegisterNodeResult ViewNodeClient::RegisterNode(
        const RegisterNodeRequest &request,
        ViewNodeClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.register_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        ViewNodeClientRegisterNodeResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.registration.cluster_id,
                                             request.registration.node_id,
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        grpc::ClientContext context;
        ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

        view::RegisterNodeRequest proto_request;
        proto_request.set_request_id(request.request_id);
        FillProtoRegistration(request.registration,
                              proto_request.mutable_registration());

        view::RegisterNodeResponse proto_response;
        const grpc::Status grpc_status =
            stub_->RegisterNode(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
            FillTransportFailureSummary(call_result.rpc, &call_result.result.summary);
            call_result.result.diagnostics.push_back(
                MakeTransportDiagnostic(call_result.rpc));
            return call_result;
        }

        FillRegisterResponse(proto_response, &call_result.result);
        call_result.rpc.retryable =
            IsRetryableRegistryStatus(call_result.result.summary.status);
        return call_result;
    }

    ViewNodeClientHeartbeatNodeResult ViewNodeClient::HeartbeatNode(
        const HeartbeatNodeRequest &request,
        ViewNodeClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.heartbeat_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        ViewNodeClientHeartbeatNodeResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.cluster_id,
                                             request.node_id,
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        grpc::ClientContext context;
        ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

        view::HeartbeatNodeRequest proto_request;
        proto_request.set_request_id(request.request_id);
        proto_request.set_cluster_id(request.cluster_id);
        proto_request.set_node_id(request.node_id);
        proto_request.set_node_type(ToProtoNodeType(request.node_type));
        proto_request.set_sequence(request.sequence);
        proto_request.set_incarnation_id(request.incarnation_id);
        FillProtoRegistration(request.observation,
                              proto_request.mutable_observation());

        view::HeartbeatNodeResponse proto_response;
        const grpc::Status grpc_status =
            stub_->HeartbeatNode(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
            FillTransportFailureSummary(call_result.rpc, &call_result.result.summary);
            call_result.result.diagnostics.push_back(
                MakeTransportDiagnostic(call_result.rpc));
            return call_result;
        }

        FillHeartbeatResponse(proto_response, &call_result.result);
        call_result.rpc.retryable =
            IsRetryableRegistryStatus(call_result.result.summary.status);
        return call_result;
    }

    ViewNodeClientDiscoverMetadataResult ViewNodeClient::DiscoverMetadata(
        const DiscoverMetadataRequest &request,
        ViewNodeClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.discovery_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        ViewNodeClientDiscoverMetadataResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.cluster_id,
                                             {},
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        grpc::ClientContext context;
        ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

        view::DiscoverMetadataRequest proto_request;
        proto_request.set_request_id(request.request_id);
        proto_request.set_cluster_id(request.cluster_id);
        proto_request.set_prefer_leader(request.prefer_leader);
        proto_request.set_live_only(request.live_only);
        proto_request.set_limit(request.limit);

        view::DiscoverMetadataResponse proto_response;
        const grpc::Status grpc_status =
            stub_->DiscoverMetadata(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
            FillTransportFailureSummary(call_result.rpc, &call_result.result.summary);
            call_result.result.diagnostics.push_back(
                MakeTransportDiagnostic(call_result.rpc));
            return call_result;
        }

        FillDiscoverMetadataResponse(proto_response, &call_result.result);
        call_result.rpc.retryable =
            IsRetryableRegistryStatus(call_result.result.summary.status);
        return call_result;
    }

    ViewNodeClientDiscoverStorageResult ViewNodeClient::DiscoverStorage(
        const DiscoverStorageRequest &request,
        ViewNodeClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.discovery_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        ViewNodeClientDiscoverStorageResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.cluster_id,
                                             {},
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        grpc::ClientContext context;
        ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

        view::DiscoverStorageRequest proto_request;
        proto_request.set_request_id(request.request_id);
        proto_request.set_cluster_id(request.cluster_id);
        proto_request.set_live_only(request.live_only);
        proto_request.set_minimum_available_capacity_bytes(
            request.minimum_available_capacity_bytes);
        proto_request.set_zone(request.zone);
        proto_request.set_rack(request.rack);
        proto_request.set_limit(request.limit);
        proto_request.set_require_writable(request.require_writable);

        view::DiscoverStorageResponse proto_response;
        const grpc::Status grpc_status =
            stub_->DiscoverStorage(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
            FillTransportFailureSummary(call_result.rpc, &call_result.result.summary);
            call_result.result.diagnostics.push_back(
                MakeTransportDiagnostic(call_result.rpc));
            return call_result;
        }

        FillDiscoverStorageResponse(proto_response, &call_result.result);
        call_result.rpc.retryable =
            IsRetryableRegistryStatus(call_result.result.summary.status);
        return call_result;
    }

    ViewNodeClientGetClusterViewResult ViewNodeClient::GetClusterView(
        const GetClusterViewRequest &request,
        ViewNodeClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.cluster_view_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        ViewNodeClientGetClusterViewResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.cluster_id,
                                             {},
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        grpc::ClientContext context;
        ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

        view::GetClusterViewRequest proto_request;
        proto_request.set_request_id(request.request_id);
        proto_request.set_cluster_id(request.cluster_id);
        proto_request.set_include_dead_nodes(request.include_dead_nodes);
        proto_request.set_include_warnings(request.include_warnings);

        view::GetClusterViewResponse proto_response;
        const grpc::Status grpc_status =
            stub_->GetClusterView(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
            FillTransportFailureSummary(call_result.rpc, &call_result.result.summary);
            call_result.result.snapshot.diagnostics.push_back(
                MakeTransportDiagnostic(call_result.rpc));
            return call_result;
        }

        FillClusterViewResponse(proto_response, &call_result.result);
        call_result.rpc.retryable =
            IsRetryableRegistryStatus(call_result.result.summary.status);
        return call_result;
    }

    ViewNodeClientPullPeerViewSnapshotResult
    ViewNodeClient::PullPeerViewSnapshot(const PullPeerViewSnapshotRequest &request,
                                         ViewNodeClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.peer_sync_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        ViewNodeClientPullPeerViewSnapshotResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.cluster_id,
                                             {},
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        grpc::ClientContext context;
        ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

        view::PullPeerViewSnapshotRequest proto_request;
        proto_request.set_request_id(request.request_id);
        proto_request.set_cluster_id(request.cluster_id);
        proto_request.set_include_dead_nodes(request.include_dead_nodes);
        proto_request.set_include_warnings(request.include_warnings);

        view::PullPeerViewSnapshotResponse proto_response;
        const grpc::Status grpc_status =
            stub_->PullPeerViewSnapshot(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
            FillTransportFailureSummary(call_result.rpc, &call_result.result.summary);
            call_result.result.diagnostics.push_back(
                MakeTransportDiagnostic(call_result.rpc));
            return call_result;
        }

        FillPullPeerViewSnapshotResponse(proto_response, &call_result.result);
        call_result.rpc.retryable =
            IsRetryableRegistryStatus(call_result.result.summary.status);
        return call_result;
    }

    ViewNodeClientPushPeerViewSnapshotResult
    ViewNodeClient::PushPeerViewSnapshot(const PushPeerViewSnapshotRequest &request,
                                         ViewNodeClientCallOptions options)
    {
        const auto effective_timeout =
            ResolveTimeout(config_.peer_sync_timeout, options);
        const bool wait_for_ready =
            ResolveWaitForReady(config_.wait_for_ready, options);

        ViewNodeClientPushPeerViewSnapshotResult call_result;
        call_result.rpc = MakeRpcDiagnostics(request.request_id,
                                             request.cluster_id,
                                             {},
                                             target_endpoint_,
                                             effective_timeout,
                                             wait_for_ready);

        grpc::ClientContext context;
        ApplyRpcOptions(effective_timeout, wait_for_ready, &context);

        view::PushPeerViewSnapshotRequest proto_request;
        proto_request.set_request_id(request.request_id);
        proto_request.set_cluster_id(request.cluster_id);
        FillProtoPeerSyncSnapshot(request.snapshot,
                                  proto_request.mutable_snapshot());

        view::PushPeerViewSnapshotResponse proto_response;
        const grpc::Status grpc_status =
            stub_->PushPeerViewSnapshot(&context, proto_request, &proto_response);
        if (!grpc_status.ok())
        {
            FillRpcFailureDiagnostics(grpc_status, &call_result.rpc);
            FillTransportFailureSummary(call_result.rpc, &call_result.result.summary);
            call_result.result.diagnostics.push_back(
                MakeTransportDiagnostic(call_result.rpc));
            return call_result;
        }

        FillPushPeerViewSnapshotResponse(proto_response, &call_result.result);
        call_result.rpc.retryable =
            IsRetryableRegistryStatus(call_result.result.summary.status);
        return call_result;
    }

    std::string_view ViewNodeClient::target_endpoint() const
    {
        return target_endpoint_;
    }

    const ViewNodeClientConfig &ViewNodeClient::config() const
    {
        return config_;
    }

} // namespace viewdemo
