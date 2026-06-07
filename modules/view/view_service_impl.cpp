#include "view/view_service_impl.h"

#include <chrono>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace viewdemo
{
    namespace
    {
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
            }
        }

        template <typename Response>
        void SetInternalSummary(Response *response, std::string_view message)
        {
            if (response == nullptr)
            {
                return;
            }

            auto *summary = response->mutable_summary();
            summary->set_code(::view::VIEW_NODE_STATUS_CODE_INTERNAL_ERROR);
            summary->set_message(std::string(message));
        }

        template <typename Response>
        ::grpc::Status ValidateRpcState(Response *response,
                                        const ViewNodeRegistry *registry)
        {
            if (response == nullptr)
            {
                return ::grpc::Status(
                    ::grpc::StatusCode::INVALID_ARGUMENT,
                    "response must not be null");
            }
            if (registry == nullptr)
            {
                SetInternalSummary(response, "view registry is not configured");
                return ::grpc::Status(
                    ::grpc::StatusCode::FAILED_PRECONDITION,
                    "view registry is not configured");
            }
            return ::grpc::Status::OK;
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

        const auto state = ValidateRpcState(response, registry_.get());
        if (!state.ok())
        {
            return state;
        }

        try
        {
            // service adapter 只做 proto -> registry 的观测事实映射。
            const auto result = registry_->RegisterNode(RegisterNodeRequest{
                .request_id = request->request_id(),
                .registration = FromProtoRegistration(request->registration())});

            FillProtoSummary(result.summary, response->mutable_summary());
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
            AppendWarnings(result.diagnostics, response);
            return ::grpc::Status::OK;
        }
        catch (const std::exception &ex)
        {
            SetInternalSummary(response, ex.what());
            return MakeInternalStatus("RegisterNode", ex);
        }
        catch (...)
        {
            SetInternalSummary(response, "unknown internal error");
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

        const auto state = ValidateRpcState(response, registry_.get());
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
            SetInternalSummary(response, ex.what());
            return MakeInternalStatus("HeartbeatNode", ex);
        }
        catch (...)
        {
            SetInternalSummary(response, "unknown internal error");
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

        const auto state = ValidateRpcState(response, registry_.get());
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
            SetInternalSummary(response, ex.what());
            return MakeInternalStatus("DiscoverMetadata", ex);
        }
        catch (...)
        {
            SetInternalSummary(response, "unknown internal error");
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

        const auto state = ValidateRpcState(response, registry_.get());
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
            SetInternalSummary(response, ex.what());
            return MakeInternalStatus("DiscoverStorage", ex);
        }
        catch (...)
        {
            SetInternalSummary(response, "unknown internal error");
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

        const auto state = ValidateRpcState(response, registry_.get());
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
            return ::grpc::Status::OK;
        }
        catch (const std::exception &ex)
        {
            SetInternalSummary(response, ex.what());
            return MakeInternalStatus("GetClusterView", ex);
        }
        catch (...)
        {
            SetInternalSummary(response, "unknown internal error");
            return MakeUnknownInternalStatus("GetClusterView");
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
