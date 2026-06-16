#include "raft/service/metadata_service_impl.h"

#include "metadata.pb.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/metadata/metadata_command_types.h"
#include "raft/node/raft_node.h"
#include "raft/state_machine/metadata_state_machine.h"

namespace raftdemo
{
  namespace
  {

    std::uint64_t FallbackClientTime(std::string_view request_id)
    {
      std::uint64_t hash = 1469598103934665603ULL;
      for (const unsigned char ch : request_id)
      {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
      }
      return 1700000000000ULL + (hash % 100000000000ULL);
    }

    std::uint64_t ResolveClientTime(const std::string &request_id,
                                    const std::uint64_t client_time_unix_ms)
    {
      if (client_time_unix_ms != 0)
      {
        return client_time_unix_ms;
      }
      return FallbackClientTime(request_id);
    }

    raft::MetadataStatusCode ToProtoStatusCode(const MetadataStatusCode code)
    {
      switch (code)
      {
      case MetadataStatusCode::kOk:
        return raft::METADATA_STATUS_CODE_OK;
      case MetadataStatusCode::kNotLeader:
        return raft::METADATA_STATUS_CODE_NOT_LEADER;
      case MetadataStatusCode::kInvalidArgument:
        return raft::METADATA_STATUS_CODE_INVALID_ARGUMENT;
      case MetadataStatusCode::kNotFound:
        return raft::METADATA_STATUS_CODE_NOT_FOUND;
      case MetadataStatusCode::kIdempotentReplay:
        return raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY;
      case MetadataStatusCode::kIdempotencyConflict:
        return raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT;
      case MetadataStatusCode::kStateConflict:
        return raft::METADATA_STATUS_CODE_STATE_CONFLICT;
      case MetadataStatusCode::kTimeout:
        return raft::METADATA_STATUS_CODE_TIMEOUT;
      case MetadataStatusCode::kInternalError:
      default:
        return raft::METADATA_STATUS_CODE_INTERNAL_ERROR;
      }
    }

    bool MessageStartsWith(const std::string &message, const std::string &prefix)
    {
      return message.rfind(prefix, 0) == 0;
    }

    raft::MetadataObjectState ToProtoObjectState(const ObjectState state)
    {
      switch (state)
      {
      case ObjectState::PENDING:
        return raft::METADATA_OBJECT_STATE_PENDING;
      case ObjectState::COMMITTED:
        return raft::METADATA_OBJECT_STATE_COMMITTED;
      case ObjectState::DELETED:
        return raft::METADATA_OBJECT_STATE_DELETED;
      default:
        return raft::METADATA_OBJECT_STATE_UNSPECIFIED;
      }
    }

    raft::MetadataStatusCode ToWriteProtoStatusCode(const ProposeResult &result)
    {
      switch (result.status)
      {
      case ProposeStatus::kOk:
        if (MessageStartsWith(result.message, "idempotent replay"))
        {
          return raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY;
        }
        return raft::METADATA_STATUS_CODE_OK;
      case ProposeStatus::kNotLeader:
        return raft::METADATA_STATUS_CODE_NOT_LEADER;
      case ProposeStatus::kInvalidCommand:
        return raft::METADATA_STATUS_CODE_INVALID_ARGUMENT;
      case ProposeStatus::kTimeout:
        return raft::METADATA_STATUS_CODE_TIMEOUT;
      case ProposeStatus::kOverloaded:
        return raft::METADATA_STATUS_CODE_OVERLOADED;
      case ProposeStatus::kNodeStopping:
        return raft::METADATA_STATUS_CODE_SERVICE_UNAVAILABLE;
      case ProposeStatus::kReplicationFailed:
        if (MessageStartsWith(result.message, "failed to replicate log entry to majority"))
        {
          return raft::METADATA_STATUS_CODE_SERVICE_UNAVAILABLE;
        }
        return raft::METADATA_STATUS_CODE_INTERNAL_ERROR;
      case ProposeStatus::kCommitFailed:
        return raft::METADATA_STATUS_CODE_SERVICE_UNAVAILABLE;
      case ProposeStatus::kApplyFailed:
        if (MessageStartsWith(result.message, "invalid metadata command:"))
        {
          return raft::METADATA_STATUS_CODE_INVALID_ARGUMENT;
        }
        if (MessageStartsWith(result.message, "not found:"))
        {
          return raft::METADATA_STATUS_CODE_NOT_FOUND;
        }
        if (MessageStartsWith(result.message, "state conflict:"))
        {
          return raft::METADATA_STATUS_CODE_STATE_CONFLICT;
        }
        if (MessageStartsWith(result.message, "idempotency conflict:"))
        {
          return raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT;
        }
        return raft::METADATA_STATUS_CODE_INTERNAL_ERROR;
      default:
        return raft::METADATA_STATUS_CODE_INTERNAL_ERROR;
      }
    }

    const char *CommittedMembershipRoleName(const CommittedMembershipRole role)
    {
      switch (role)
      {
      case CommittedMembershipRole::kVoter:
        return "voter";
      case CommittedMembershipRole::kLearner:
        return "learner";
      case CommittedMembershipRole::kNonMember:
        return "non_member";
      case CommittedMembershipRole::kUnknown:
      default:
        return "unknown";
      }
    }

    bool ShouldAttachWriteDiagnostics(const ProposeResult &result)
    {
      switch (result.status)
      {
      case ProposeStatus::kNotLeader:
      case ProposeStatus::kTimeout:
      case ProposeStatus::kNodeStopping:
      case ProposeStatus::kCommitFailed:
        return true;
      case ProposeStatus::kReplicationFailed:
        return MessageStartsWith(result.message, "failed to replicate log entry to majority");
      default:
        return false;
      }
    }

    int ResolveLeaderHintId(const NodeStatusSnapshot &status,
                            const std::optional<int> fallback_leader_id)
    {
      if (status.leader_id >= 0)
      {
        return status.leader_id;
      }
      if (fallback_leader_id.has_value())
      {
        return *fallback_leader_id;
      }
      return -1;
    }

    std::string BuildContextDiagnosticMessage(
        const std::string &base_message,
        const NodeStatusSnapshot &status,
        const std::optional<int> fallback_leader_id)
    {
      std::ostringstream oss;
      oss << base_message;
      const auto append_field = [&](const std::string &key, const auto &value)
      {
        if (base_message.find(key + "=") != std::string::npos)
        {
          return;
        }
        if (oss.tellp() > 0)
        {
          oss << "; ";
        }
        oss << key << "=" << value;
      };
      append_field("local_node_id", status.node_id);
      append_field("local_node_address", status.address);
      append_field("leader_hint_id", ResolveLeaderHintId(status, fallback_leader_id));
      append_field("leader_hint_address", status.leader_address);
      return oss.str();
    }

    std::string BuildDiagnosticMessage(
        const std::string &base_message,
        const NodeStatusSnapshot &status,
        const CommittedMembershipQuorumSummary &quorum_summary,
        const std::optional<int> fallback_leader_id,
        const bool emphasize_quorum_boundary)
    {
      std::ostringstream oss;
      oss << BuildContextDiagnosticMessage(base_message, status, fallback_leader_id)
          << "; committed_voter_count=" << quorum_summary.voter_count
          << "; committed_quorum_size=" << quorum_summary.quorum_size
          << "; local_committed_membership_role="
          << CommittedMembershipRoleName(quorum_summary.local_role)
          << "; committed_membership_index=" << quorum_summary.committed_log_index
          << "; committed_membership_term=" << quorum_summary.committed_term
          << "; committed_voter_ids=[";
      for (std::size_t index = 0; index < quorum_summary.voter_ids.size(); ++index)
      {
        if (index > 0)
        {
          oss << ",";
        }
        oss << quorum_summary.voter_ids[index];
      }
      oss << "]";
      if (emphasize_quorum_boundary)
      {
        // 只读诊断必须明确说明 quorum 来源于 committed membership，不能按 live 节点或
        // ViewNode 观测结果改变写路径共识规则。
        oss << "; quorum_rule=committed_membership_majority_only";
      }
      return oss.str();
    }

    const RuntimeMembershipEntry *FindRuntimeLearnerEntry(
        const RuntimeMembershipSummary &runtime_summary,
        const raft::JoinMetadataClusterRequest &request)
    {
      for (const auto &entry : runtime_summary.learner_entries)
      {
        if (entry.raft_id == request.candidate_raft_id())
        {
          return &entry;
        }
        if (!entry.canonical_node_id.empty() &&
            entry.canonical_node_id == request.node_id())
        {
          return &entry;
        }
      }
      return nullptr;
    }

    std::uint64_t HighestObservedLearnerProgressIndex(
        const RuntimeMembershipEntry &entry)
    {
      std::uint64_t highest = entry.match_index;
      highest = std::max(highest, entry.last_snapshot_index);
      highest = std::max(highest, entry.last_applied_index);
      highest = std::max(highest, entry.observed_last_log_index);
      return highest;
    }

    bool IsRuntimeLearnerReadyToPromote(
        const RuntimeMembershipSummary &runtime_summary,
        const RuntimeMembershipEntry &entry)
    {
      const std::uint64_t highest_observed =
          HighestObservedLearnerProgressIndex(entry);
      if (highest_observed == 0)
      {
        return false;
      }
      return highest_observed >= runtime_summary.committed_log_index;
    }

    std::size_t CountReadyRuntimeLearners(
        const RuntimeMembershipSummary &runtime_summary)
    {
      std::size_t ready_count = 0U;
      for (const auto &entry : runtime_summary.learner_entries)
      {
        if (IsRuntimeLearnerReadyToPromote(runtime_summary, entry))
        {
          ++ready_count;
        }
      }
      return ready_count;
    }

    std::string BuildJoinLearnerStatusMessage(
        const std::string &base_message,
        const RuntimeMembershipSummary &runtime_summary,
        const raft::JoinMetadataClusterRequest &request,
        const bool committed_membership_changed)
    {
      std::ostringstream oss;
      oss << base_message
          << "; runtime_voter_count=" << runtime_summary.voter_count
          << "; runtime_learner_count=" << runtime_summary.learner_count;

      const auto *entry = FindRuntimeLearnerEntry(runtime_summary, request);
      if (entry == nullptr)
      {
        if (committed_membership_changed &&
            std::find(runtime_summary.voter_ids.begin(),
                      runtime_summary.voter_ids.end(),
                      request.candidate_raft_id()) != runtime_summary.voter_ids.end())
        {
          oss << "; learner_status=promoted"
              << "; promotion_status=batch_promoted"
              << "; promotion_batch_size=2"
              << "; promotion_policy=odd_committed_voter_count_only";
        }
        return oss.str();
      }

      const std::size_t ready_learner_count =
          CountReadyRuntimeLearners(runtime_summary);
      const bool ready_to_promote =
          IsRuntimeLearnerReadyToPromote(runtime_summary, *entry);
      const bool waiting_for_pair =
          ready_to_promote && runtime_summary.voter_count > 0 &&
          ((runtime_summary.voter_count + 1U) % 2U == 0U);

      oss << "; learner_status="
          << (ready_to_promote ? "ready_to_promote" : "pending")
          << "; learner_match_index=" << entry->match_index
          << "; learner_next_index=" << entry->next_index
          << "; learner_ready_index=" << runtime_summary.committed_log_index
          << "; promotion_status=";
      if (waiting_for_pair)
      {
        oss << "waiting_for_pair"
            << "; promotion_block_reason=even_voter_count";
      }
      else if (ready_to_promote && ready_learner_count == 2U)
      {
        oss << "ready_pair";
      }
      else if (ready_to_promote)
      {
        oss << "ready_to_promote";
      }
      else
      {
        oss << "catching_up";
      }
      oss << "; promotion_policy=odd_committed_voter_count_only";
      return oss.str();
    }

    void DecorateJoinSummaryWithLearnerStatus(
        const RuntimeMembershipSummary &runtime_summary,
        const raft::JoinMetadataClusterRequest &request,
        const bool committed_membership_changed,
        raft::MetadataResponseSummary *summary)
    {
      if (summary == nullptr)
      {
        return;
      }
      summary->set_message(BuildJoinLearnerStatusMessage(summary->message(),
                                                         runtime_summary,
                                                         request,
                                                         committed_membership_changed));
    }

    void FillLeaderHint(const NodeStatusSnapshot &status,
                        const std::optional<int> fallback_leader_id,
                        raft::MetadataLeaderHint *leader_hint)
    {
      if (leader_hint == nullptr)
      {
        return;
      }
      leader_hint->set_leader_id(ResolveLeaderHintId(status, fallback_leader_id));
      leader_hint->set_leader_address(status.leader_address);
    }

    void DecorateSummaryWithContext(
        const NodeStatusSnapshot &status,
        const std::optional<int> fallback_leader_id,
        raft::MetadataResponseSummary *summary)
    {
      if (summary == nullptr)
      {
        return;
      }
      // proto 当前没有 node_id 字段；这里统一把本地服务节点身份补进诊断 message。
      summary->set_message(
          BuildContextDiagnosticMessage(summary->message(), status, fallback_leader_id));
      FillLeaderHint(status, fallback_leader_id, summary->mutable_leader_hint());
    }

    void DecorateSummaryWithDiagnostics(
        const NodeStatusSnapshot &status,
        const CommittedMembershipQuorumSummary &quorum_summary,
        const std::optional<int> fallback_leader_id,
        const bool emphasize_quorum_boundary,
        raft::MetadataResponseSummary *summary)
    {
      if (summary == nullptr)
      {
        return;
      }
      summary->set_message(BuildDiagnosticMessage(summary->message(),
                                                  status,
                                                  quorum_summary,
                                                  fallback_leader_id,
                                                  emphasize_quorum_boundary));
      FillLeaderHint(status, fallback_leader_id, summary->mutable_leader_hint());
    }

    void FillBucketRecord(const BucketRecord &record, raft::BucketRecord *out)
    {
      if (out == nullptr)
      {
        return;
      }
      out->set_bucket(record.bucket);
      out->set_create_time(record.create_time);
      out->set_deleted(record.deleted);
      out->set_delete_time(record.delete_time.value_or(0));
    }

    void FillChunkRef(const ChunkRef &ref, raft::ChunkRef *out)
    {
      if (out == nullptr)
      {
        return;
      }
      out->set_chunk_id(ref.chunk_id);
      out->set_offset(ref.offset);
      out->set_size(ref.size);
      out->clear_replica_nodes();
      for (const auto &node : ref.replica_nodes)
      {
        out->add_replica_nodes(node);
      }
      out->set_checksum(ref.checksum);
    }

    void FillObjectRecord(const ObjectRecord &record, raft::ObjectRecord *out)
    {
      if (out == nullptr)
      {
        return;
      }
      out->set_bucket(record.bucket);
      out->set_object_key(record.object_key);
      out->set_object_id(record.object_id);
      out->set_version(record.version);
      out->set_size(record.size);
      out->set_etag(record.etag);
      out->set_state(ToProtoObjectState(record.state));
      out->clear_chunks();
      for (const auto &chunk : record.chunks)
      {
        FillChunkRef(chunk, out->add_chunks());
      }
      out->set_create_time(record.create_time);
      out->set_commit_time(record.commit_time.value_or(0));
      out->set_delete_time(record.delete_time.value_or(0));
    }

    void FillSummary(const NodeStatusSnapshot &status,
                     raft::MetadataStatusCode code,
                     const std::string &message,
                     const std::string &request_id,
                     const std::string &bucket,
                     const std::string &object_key,
                     const std::string &object_id,
                     const std::optional<ObjectState> state,
                     const std::optional<std::uint64_t> log_index,
                     const std::optional<std::uint64_t> term,
                     raft::MetadataResponseSummary *out)
    {
      if (out == nullptr)
      {
        return;
      }
      out->set_code(code);
      out->set_message(message);
      out->set_request_id(request_id);
      out->set_bucket(bucket);
      out->set_object_key(object_key);
      out->set_object_id(object_id);
      out->set_state(state.has_value() ? ToProtoObjectState(*state)
                                       : raft::METADATA_OBJECT_STATE_UNSPECIFIED);
      out->set_log_index(log_index.value_or(0));
      out->set_term(term.value_or(status.term));
      FillLeaderHint(status, std::nullopt, out->mutable_leader_hint());
    }

    void FillSummary(const NodeStatusSnapshot &status,
                     MetadataStatusCode code,
                     const std::string &message,
                     const std::string &request_id,
                     const std::string &bucket,
                     const std::string &object_key,
                     const std::string &object_id,
                     const std::optional<ObjectState> state,
                     const std::optional<std::uint64_t> log_index,
                     const std::optional<std::uint64_t> term,
                     raft::MetadataResponseSummary *out)
    {
      FillSummary(status,
                  ToProtoStatusCode(code),
                  message,
                  request_id,
                  bucket,
                  object_key,
                  object_id,
                  state,
                  log_index,
                  term,
                  out);
    }

    void FillWriteSummary(const NodeStatusSnapshot &status,
                          const CommittedMembershipQuorumSummary &quorum_summary,
                          const ProposeResult &result,
                          const std::string &request_id,
                          const std::string &bucket,
                          const std::string &object_key,
                          const std::string &object_id,
                          const std::optional<ObjectState> state,
                          raft::MetadataResponseSummary *out)
    {
      FillSummary(status,
                  ToWriteProtoStatusCode(result),
                  result.message,
                  request_id,
                  bucket,
                  object_key,
                  object_id,
                  state,
                  result.log_index,
                  result.term,
                  out);
      DecorateSummaryWithContext(status,
                                 result.leader_id >= 0
                                     ? std::optional<int>(result.leader_id)
                                     : std::nullopt,
                                 out);
      if (ShouldAttachWriteDiagnostics(result))
      {
        DecorateSummaryWithDiagnostics(status,
                                       quorum_summary,
                                       result.leader_id >= 0
                                           ? std::optional<int>(result.leader_id)
                                           : std::nullopt,
                                       true,
                                       out);
      }
    }

    template <typename Response>
    grpc::ServerUnaryReactor *FinishReadError(grpc::CallbackServerContext *context,
                                              const NodeStatusSnapshot &status,
                                              const raft::MetadataStatusCode code,
                                              const std::string &message,
                                              const std::string &bucket,
                                              const std::string &object_key,
                                              const std::string &object_id,
                                              Response *response)
    {
      auto *reactor = context->DefaultReactor();
      FillSummary(status,
                  code,
                  message,
                  "",
                  bucket,
                  object_key,
                  object_id,
                  std::nullopt,
                  std::nullopt,
                  status.term,
                  response->mutable_summary());
      DecorateSummaryWithContext(status,
                                 status.leader_id >= 0
                                     ? std::optional<int>(status.leader_id)
                                     : std::nullopt,
                                 response->mutable_summary());
      if constexpr (std::is_same_v<Response, raft::HeadObjectResponse>)
      {
        response->set_found(false);
      }
      if constexpr (std::is_same_v<Response, raft::ListObjectsResponse>)
      {
        response->clear_objects();
        response->set_next_continuation_token("");
      }
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    bool IsDeadlineExpired(grpc::CallbackServerContext *context)
    {
      return context != nullptr &&
             context->deadline() <= std::chrono::system_clock::now();
    }

    template <typename Response>
    grpc::ServerUnaryReactor *FinishReadAdmissionErrorWithDiagnostics(
        grpc::CallbackServerContext *context,
        const NodeStatusSnapshot &status,
        const CommittedMembershipQuorumSummary &quorum_summary,
        const raft::MetadataStatusCode code,
        const std::string &message,
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        Response *response)
    {
      auto *reactor = FinishReadError(context,
                                      status,
                                      code,
                                      message,
                                      bucket,
                                      object_key,
                                      object_id,
                                      response);
      DecorateSummaryWithDiagnostics(status,
                                     quorum_summary,
                                     status.leader_id >= 0
                                         ? std::optional<int>(status.leader_id)
                                         : std::nullopt,
                                     true,
                                     response->mutable_summary());
      return reactor;
    }

    template <typename Response>
    grpc::ServerUnaryReactor *FinishReadAdmissionIfRejected(
        const RaftNode &node,
        grpc::CallbackServerContext *context,
        const NodeStatusSnapshot &status,
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        Response *response)
    {
      const CommittedMembershipQuorumSummary quorum_summary =
          node.GetCommittedMembershipQuorumSummary();
      if (IsDeadlineExpired(context))
      {
        return FinishReadAdmissionErrorWithDiagnostics(context,
                                                       status,
                                                       quorum_summary,
                                                       raft::METADATA_STATUS_CODE_TIMEOUT,
                                                       "read deadline already expired before admission",
                                                       bucket,
                                                       object_key,
                                                       object_id,
                                                       response);
      }

      if (!node.IsRunning())
      {
        return FinishReadAdmissionErrorWithDiagnostics(context,
                                                       status,
                                                       quorum_summary,
                                                       raft::METADATA_STATUS_CODE_SERVICE_UNAVAILABLE,
                                                       "node is stopping",
                                                       bucket,
                                                       object_key,
                                                       object_id,
                                                       response);
      }

      if (status.role != "Leader")
      {
        return FinishReadAdmissionErrorWithDiagnostics(context,
                                                       status,
                                                       quorum_summary,
                                                       raft::METADATA_STATUS_CODE_NOT_LEADER,
                                                       "node is not the leader",
                                                       bucket,
                                                       object_key,
                                                       object_id,
                                                       response);
      }

      return nullptr;
    }

    bool ValidateHeadObjectRequest(const raft::HeadObjectRequest &request,
                                   std::string *reason)
    {
      if (request.bucket().empty())
      {
        if (reason != nullptr)
        {
          *reason = "bucket is required";
        }
        return false;
      }
      if (request.object_key().empty())
      {
        if (reason != nullptr)
        {
          *reason = "object_key is required";
        }
        return false;
      }
      return true;
    }

    bool ValidateListObjectsRequest(const raft::ListObjectsRequest &request,
                                    std::string *reason)
    {
      if (request.bucket().empty())
      {
        if (reason != nullptr)
        {
          *reason = "bucket is required";
        }
        return false;
      }
      return true;
    }

    RequestRecord MakeRequestContext(const std::string &request_id,
                                     const MetadataRequestType request_type,
                                     const std::string &bucket,
                                     const std::string &object_key,
                                     const std::uint64_t client_time_unix_ms)
    {
      return RequestRecord{
          request_id,
          request_type,
          bucket,
          object_key,
          "accepted",
          0,
          client_time_unix_ms,
          client_time_unix_ms};
    }

    MetadataCommand MakeCreateBucketCommand(const raft::CreateBucketRequest &request)
    {
      const std::uint64_t client_time =
          ResolveClientTime(request.request_id(), request.client_time_unix_ms());
      MetadataCommand command;
      command.command_type = MetadataCommandType::kCreateBucket;
      command.request_id = request.request_id();
      command.create_bucket = CreateBucketCommandPayload{
          BucketRecord{request.bucket(), client_time, false, std::nullopt}};
      command.request_context = MakeRequestContext(request.request_id(),
                                                   MetadataRequestType::kCreateBucket,
                                                   request.bucket(),
                                                   "",
                                                   client_time);
      return command;
    }

    MetadataCommand MakeDeleteBucketCommand(const raft::DeleteBucketRequest &request)
    {
      const std::uint64_t client_time =
          ResolveClientTime(request.request_id(), request.client_time_unix_ms());
      MetadataCommand command;
      command.command_type = MetadataCommandType::kDeleteBucket;
      command.request_id = request.request_id();
      command.delete_bucket = DeleteBucketCommandPayload{
          request.bucket(),
          request.if_empty()};
      command.request_context = MakeRequestContext(request.request_id(),
                                                   MetadataRequestType::kDeleteBucket,
                                                   request.bucket(),
                                                   "",
                                                   client_time);
      return command;
    }

    MetadataCommand MakeCreateObjectCommand(const raft::CreateObjectRequest &request)
    {
      const std::uint64_t client_time =
          ResolveClientTime(request.request_id(), request.client_time_unix_ms());
      MetadataCommand command;
      command.command_type = MetadataCommandType::kCreateObject;
      command.request_id = request.request_id();
      command.create_object = CreateObjectCommandPayload{
          ObjectRecord{
              request.bucket(),
              request.object_key(),
              request.object_id(),
              request.version(),
              request.size(),
              request.etag(),
              ObjectState::PENDING,
              {},
              client_time,
              std::nullopt,
              std::nullopt}};
      command.request_context = MakeRequestContext(request.request_id(),
                                                   MetadataRequestType::kCreateObject,
                                                   request.bucket(),
                                                   request.object_key(),
                                                   client_time);
      return command;
    }

    std::vector<ChunkRef> MakeChunkRefs(
        const google::protobuf::RepeatedPtrField<raft::ChunkRef> &chunks)
    {
      std::vector<ChunkRef> refs;
      refs.reserve(static_cast<std::size_t>(chunks.size()));
      for (const auto &chunk : chunks)
      {
        std::vector<std::string> replicas;
        replicas.reserve(static_cast<std::size_t>(chunk.replica_nodes_size()));
        for (const auto &node : chunk.replica_nodes())
        {
          replicas.push_back(node);
        }
        refs.push_back(ChunkRef{
            chunk.chunk_id(),
            chunk.offset(),
            chunk.size(),
            std::move(replicas),
            chunk.checksum()});
      }
      return refs;
    }

    MetadataCommand MakeCommitObjectCommand(const raft::CommitObjectRequest &request)
    {
      const std::uint64_t client_time =
          ResolveClientTime(request.request_id(), request.client_time_unix_ms());
      MetadataCommand command;
      command.command_type = MetadataCommandType::kCommitObject;
      command.request_id = request.request_id();
      command.commit_object = CommitObjectCommandPayload{
          request.bucket(),
          request.object_key(),
          request.object_id(),
          request.version(),
          request.size(),
          request.etag(),
          MakeChunkRefs(request.chunks()),
          client_time};
      command.request_context = MakeRequestContext(request.request_id(),
                                                   MetadataRequestType::kCommitObject,
                                                   request.bucket(),
                                                   request.object_key(),
                                                   client_time);
      return command;
    }

    MetadataCommand MakeAbortObjectCommand(const raft::AbortObjectRequest &request)
    {
      const std::uint64_t client_time =
          ResolveClientTime(request.request_id(), request.client_time_unix_ms());
      MetadataCommand command;
      command.command_type = MetadataCommandType::kAbortObject;
      command.request_id = request.request_id();
      command.abort_object = AbortObjectCommandPayload{
          request.bucket(),
          request.object_key(),
          request.object_id(),
          request.version()};
      command.request_context = MakeRequestContext(request.request_id(),
                                                   MetadataRequestType::kAbortObject,
                                                   request.bucket(),
                                                   request.object_key(),
                                                   client_time);
      return command;
    }

    MetadataCommand MakeDeleteObjectCommand(const raft::DeleteObjectRequest &request)
    {
      const std::uint64_t client_time =
          ResolveClientTime(request.request_id(), request.client_time_unix_ms());
      MetadataCommand command;
      command.command_type = MetadataCommandType::kDeleteObject;
      command.request_id = request.request_id();
      command.delete_object = DeleteObjectCommandPayload{
          request.bucket(),
          request.object_key(),
          request.object_id(),
          request.version(),
          client_time};
      command.request_context = MakeRequestContext(request.request_id(),
                                                   MetadataRequestType::kDeleteObject,
                                                   request.bucket(),
                                                   request.object_key(),
                                                   client_time);
      return command;
    }

    template <typename Response>
    grpc::ServerUnaryReactor *FinishValidationError(grpc::CallbackServerContext *context,
                                                    const NodeStatusSnapshot &status,
                                                    const std::string &message,
                                                    const std::string &request_id,
                                                    const std::string &bucket,
                                                    const std::string &object_key,
                                                    const std::string &object_id,
                                                    Response *response)
    {
      auto *reactor = context->DefaultReactor();
      FillSummary(status,
                  MetadataStatusCode::kInvalidArgument,
                  message,
                  request_id,
                  bucket,
                  object_key,
                  object_id,
                  std::nullopt,
                  std::nullopt,
                  status.term,
                  response->mutable_summary());
      DecorateSummaryWithContext(status,
                                 status.leader_id >= 0
                                     ? std::optional<int>(status.leader_id)
                                     : std::nullopt,
                                 response->mutable_summary());
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    bool IsJoinCandidateStateHintValid(
        const raft::JoinMetadataCandidateStateHint state_hint)
    {
      return state_hint == raft::JOIN_METADATA_CANDIDATE_STATE_HINT_JOINING ||
             state_hint == raft::JOIN_METADATA_CANDIDATE_STATE_HINT_CANDIDATE;
    }

    bool HasCommittedVoterRaftId(const CommittedMembershipQuorumSummary &quorum_summary,
                                 const std::int32_t candidate_raft_id)
    {
      return std::find(quorum_summary.voter_ids.begin(),
                       quorum_summary.voter_ids.end(),
                       candidate_raft_id) != quorum_summary.voter_ids.end();
    }

    std::optional<std::string> ValidateJoinMetadataClusterRequest(
        const raft::JoinMetadataClusterRequest &request,
        const CommittedMembershipQuorumSummary &quorum_summary)
    {
      if (request.request_id().empty())
      {
        return "request_id is required";
      }
      if (request.cluster_id().empty())
      {
        return "cluster_id is required";
      }
      if (request.node_id().empty())
      {
        return "node_id is required";
      }
      if (request.candidate_raft_id() <= 0)
      {
        return "candidate_raft_id must be positive";
      }
      if (request.candidate_client_address().empty())
      {
        return "candidate_client_address is required";
      }
      if (request.candidate_raft_address().empty())
      {
        return "candidate_raft_address is required";
      }
      if (request.candidate_incarnation_id().empty())
      {
        return "candidate_incarnation_id is required";
      }
      if (request.candidate_sequence() == 0)
      {
        return "candidate_sequence must be non-zero";
      }
      if (request.persistent_generation() == 0)
      {
        return "persistent_generation must be non-zero";
      }
      if (request.data_dir_fingerprint().empty())
      {
        return "data_dir_fingerprint is required";
      }
      if (!IsJoinCandidateStateHintValid(request.local_state_hint()))
      {
        return "local_state_hint must be joining or candidate";
      }
      if (HasCommittedVoterRaftId(quorum_summary, request.candidate_raft_id()))
      {
        return "candidate_raft_id already exists in committed voter set";
      }
      return std::nullopt;
    }

    std::string DecorateJoinMetadataAuthorityBoundary(
        const raft::JoinMetadataClusterRequest &request,
        std::string message)
    {
      const std::string boundary =
          "viewnode_observation=discovery_only; "
          "join_authority=metadata_leader_committed_membership_only; "
          "requested_membership=learner_not_voter";
      if (message.find("viewnode_observation=discovery_only") !=
          std::string::npos)
      {
        return message;
      }

      std::ostringstream oss;
      oss << message;
      if (!request.observed_view_node_id().empty())
      {
        oss << "; observed_view_node_id=" << request.observed_view_node_id();
      }
      if (!request.observed_metadata_endpoint().empty())
      {
        oss << "; observed_metadata_endpoint="
            << request.observed_metadata_endpoint();
      }
      if (request.observed_time_unix_ms() != 0)
      {
        oss << "; observed_time_unix_ms=" << request.observed_time_unix_ms();
      }
      if (oss.tellp() > 0)
      {
        oss << "; ";
      }
      oss << boundary;
      return oss.str();
    }

    AddLearnerProposalRequest BuildAddLearnerProposalRequest(
        const raft::JoinMetadataClusterRequest &request)
    {
      AddLearnerProposalRequest proposal_request;
      proposal_request.cluster_id = request.cluster_id();
      proposal_request.node_id = request.node_id();
      proposal_request.candidate_raft_id = request.candidate_raft_id();
      proposal_request.candidate_client_address =
          request.candidate_client_address();
      proposal_request.candidate_raft_address =
          request.candidate_raft_address();
      proposal_request.candidate_incarnation_id =
          request.candidate_incarnation_id();
      proposal_request.candidate_sequence = request.candidate_sequence();
      proposal_request.persistent_generation =
          request.persistent_generation();
      proposal_request.data_dir_fingerprint = request.data_dir_fingerprint();
      return proposal_request;
    }

    bool ShouldRouteJoinDuplicateToBatchPromotion(
        const AddLearnerProposalResult &proposal_result,
        const RuntimeMembershipSummary &runtime_summary,
        const raft::JoinMetadataClusterRequest &request)
    {
      if (proposal_result.status != AddLearnerProposalStatus::kDuplicate)
      {
        return false;
      }

      const auto *entry = FindRuntimeLearnerEntry(runtime_summary, request);
      if (entry == nullptr)
      {
        return false;
      }
      if (!IsRuntimeLearnerReadyToPromote(runtime_summary, *entry))
      {
        return false;
      }
      return CountReadyRuntimeLearners(runtime_summary) == 2U;
    }

    raft::MetadataStatusCode ToJoinMetadataStatusCode(
        const AddLearnerProposalResult &result)
    {
      switch (result.status)
      {
      case AddLearnerProposalStatus::kAcceptedPendingCommit:
        return raft::METADATA_STATUS_CODE_OK;
      case AddLearnerProposalStatus::kDuplicate:
        return raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY;
      case AddLearnerProposalStatus::kPendingMembershipChange:
      case AddLearnerProposalStatus::kRejected:
        return raft::METADATA_STATUS_CODE_STATE_CONFLICT;
      case AddLearnerProposalStatus::kNotLeader:
        return raft::METADATA_STATUS_CODE_NOT_LEADER;
      case AddLearnerProposalStatus::kNodeStopping:
        return raft::METADATA_STATUS_CODE_SERVICE_UNAVAILABLE;
      case AddLearnerProposalStatus::kInvalidArgument:
        return raft::METADATA_STATUS_CODE_INVALID_ARGUMENT;
      }
      return raft::METADATA_STATUS_CODE_INTERNAL_ERROR;
    }

    raft::JoinMetadataClusterDisposition ToJoinMetadataDisposition(
        const AddLearnerProposalResult &result)
    {
      switch (result.status)
      {
      case AddLearnerProposalStatus::kAcceptedPendingCommit:
        return raft::JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT;
      case AddLearnerProposalStatus::kDuplicate:
        return raft::JOIN_METADATA_CLUSTER_DISPOSITION_DUPLICATE;
      case AddLearnerProposalStatus::kPendingMembershipChange:
        return raft::JOIN_METADATA_CLUSTER_DISPOSITION_PENDING_MEMBERSHIP_CHANGE;
      case AddLearnerProposalStatus::kNotLeader:
        return raft::JOIN_METADATA_CLUSTER_DISPOSITION_NOT_LEADER;
      case AddLearnerProposalStatus::kInvalidArgument:
        return raft::JOIN_METADATA_CLUSTER_DISPOSITION_INVALID_CANDIDATE;
      case AddLearnerProposalStatus::kRejected:
      case AddLearnerProposalStatus::kNodeStopping:
      default:
        return raft::JOIN_METADATA_CLUSTER_DISPOSITION_REJECTED;
      }
    }

    grpc::ServerUnaryReactor *FinishJoinMetadataClusterResponse(
        grpc::CallbackServerContext *context,
        const NodeStatusSnapshot &status,
        const CommittedMembershipQuorumSummary &quorum_summary,
        const RuntimeMembershipSummary &runtime_summary,
        const raft::JoinMetadataClusterRequest &request,
        const raft::MetadataStatusCode code,
        const raft::JoinMetadataClusterDisposition disposition,
        const std::string &message,
        const std::uint64_t membership_epoch,
        const bool committed_membership_changed,
        raft::JoinMetadataClusterResponse *response)
    {
      auto *reactor = context->DefaultReactor();
      FillSummary(status,
                  code,
                  DecorateJoinMetadataAuthorityBoundary(request, message),
                  request.request_id(),
                  "",
                  "",
                  "",
                  std::nullopt,
                  std::nullopt,
                  status.term,
                  response->mutable_summary());
      DecorateSummaryWithDiagnostics(status,
                                     quorum_summary,
                                     status.leader_id >= 0
                                         ? std::optional<int>(status.leader_id)
                                         : std::nullopt,
                                     true,
                                     response->mutable_summary());
      DecorateJoinSummaryWithLearnerStatus(runtime_summary,
                                           request,
                                           committed_membership_changed,
                                           response->mutable_summary());
      response->set_disposition(disposition);
      response->set_requested_membership(
          raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
      response->set_committed_membership_changed(committed_membership_changed);
      response->set_membership_epoch(membership_epoch);
      response->set_canonical_node_id(request.node_id());
      if (request.candidate_raft_id() > 0)
      {
        response->set_assigned_raft_id(request.candidate_raft_id());
      }
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

  } // namespace

  MetadataServiceImpl::MetadataServiceImpl(RaftNode &node)
      : node_(node)
  {
  }

  MetadataServiceImpl::~MetadataServiceImpl() = default;

  grpc::ServerUnaryReactor *MetadataServiceImpl::CreateBucket(
      grpc::CallbackServerContext *context,
      const raft::CreateBucketRequest *request,
      raft::CreateBucketResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const NodeStatusSnapshot status = node_.GetStatusSnapshot();
    MetadataCommand command = MakeCreateBucketCommand(*request);
    std::string validation_error;
    if (!ValidateMetadataCommand(command, &validation_error))
    {
      return FinishValidationError(context,
                                   status,
                                   validation_error,
                                   request->request_id(),
                                   request->bucket(),
                                   "",
                                   "",
                                   response);
    }

    const ProposeResult result = node_.ProposeMetadata(SerializeMetadataCommand(command));
    const NodeStatusSnapshot latest_status = node_.GetStatusSnapshot();
    const CommittedMembershipQuorumSummary quorum_summary =
        node_.GetCommittedMembershipQuorumSummary();
    FillWriteSummary(latest_status,
                     quorum_summary,
                     result,
                     request->request_id(),
                     request->bucket(),
                     "",
                     "",
                     std::nullopt,
                     response->mutable_summary());

    if (const raft::MetadataStatusCode code = ToWriteProtoStatusCode(result);
        code == raft::METADATA_STATUS_CODE_OK ||
        code == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
    {
      if (const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
          metadata_state_machine != nullptr)
      {
        if (const auto bucket = metadata_state_machine->FindBucket(request->bucket());
            bucket.has_value())
        {
          FillBucketRecord(*bucket, response->mutable_bucket_record());
        }
      }
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::DeleteBucket(
      grpc::CallbackServerContext *context,
      const raft::DeleteBucketRequest *request,
      raft::DeleteBucketResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const NodeStatusSnapshot status = node_.GetStatusSnapshot();
    MetadataCommand command = MakeDeleteBucketCommand(*request);
    std::string validation_error;
    if (!ValidateMetadataCommand(command, &validation_error))
    {
      return FinishValidationError(context,
                                   status,
                                   validation_error,
                                   request->request_id(),
                                   request->bucket(),
                                   "",
                                   "",
                                   response);
    }

    const ProposeResult result = node_.ProposeMetadata(SerializeMetadataCommand(command));
    const NodeStatusSnapshot latest_status = node_.GetStatusSnapshot();
    const CommittedMembershipQuorumSummary quorum_summary =
        node_.GetCommittedMembershipQuorumSummary();
    FillWriteSummary(latest_status,
                     quorum_summary,
                     result,
                     request->request_id(),
                     request->bucket(),
                     "",
                     "",
                     std::nullopt,
                     response->mutable_summary());

    if (const raft::MetadataStatusCode code = ToWriteProtoStatusCode(result);
        code == raft::METADATA_STATUS_CODE_OK ||
        code == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
    {
      if (const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
          metadata_state_machine != nullptr)
      {
        if (const auto bucket = metadata_state_machine->FindBucket(request->bucket());
            bucket.has_value())
        {
          FillBucketRecord(*bucket, response->mutable_bucket_record());
        }
      }
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::CreateObject(
      grpc::CallbackServerContext *context,
      const raft::CreateObjectRequest *request,
      raft::CreateObjectResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const NodeStatusSnapshot status = node_.GetStatusSnapshot();
    MetadataCommand command = MakeCreateObjectCommand(*request);
    std::string validation_error;
    if (!ValidateMetadataCommand(command, &validation_error))
    {
      return FinishValidationError(context,
                                   status,
                                   validation_error,
                                   request->request_id(),
                                   request->bucket(),
                                   request->object_key(),
                                   request->object_id(),
                                   response);
    }

    // 受控前置校验：bucket 缺失时必须直接拒绝，不能让无效 CreateObject
    // 提案进入已提交日志并污染 committed metadata apply 边界。
    const bool local_node_is_leader =
        status.role == "Leader" && status.leader_id == status.node_id;
    if (local_node_is_leader)
    {
      if (const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
          metadata_state_machine != nullptr)
      {
        const auto bucket = metadata_state_machine->FindBucket(request->bucket());
        if (!bucket.has_value() || bucket->deleted)
        {
          FillSummary(status,
                      raft::METADATA_STATUS_CODE_NOT_FOUND,
                      "not found: bucket does not exist",
                      request->request_id(),
                      request->bucket(),
                      request->object_key(),
                      request->object_id(),
                      ObjectState::PENDING,
                      std::nullopt,
                      status.term,
                      response->mutable_summary());
          DecorateSummaryWithContext(status,
                                     status.leader_id >= 0
                                         ? std::optional<int>(status.leader_id)
                                         : std::nullopt,
                                     response->mutable_summary());
          reactor->Finish(grpc::Status::OK);
          return reactor;
        }

        const auto existing_object =
            metadata_state_machine->FindObject(request->bucket(), request->object_key());
        if (existing_object.has_value() && existing_object->state != ObjectState::DELETED)
        {
          FillSummary(status,
                      raft::METADATA_STATUS_CODE_STATE_CONFLICT,
                      "state conflict: object already exists",
                      request->request_id(),
                      request->bucket(),
                      request->object_key(),
                      request->object_id(),
                      ObjectState::PENDING,
                      std::nullopt,
                      status.term,
                      response->mutable_summary());
          DecorateSummaryWithContext(status,
                                     status.leader_id >= 0
                                         ? std::optional<int>(status.leader_id)
                                         : std::nullopt,
                                     response->mutable_summary());
          reactor->Finish(grpc::Status::OK);
          return reactor;
        }

        // CreateWritePlan 请求允许 version=0，由 MetadataService 在 leader
        // 本地基于 committed metadata 边界分配稳定版本。
        if (command.create_object->object_record.version == 0)
        {
          std::uint64_t resolved_version = 1;
          if (existing_object.has_value() && existing_object->version != 0)
          {
            resolved_version = existing_object->version + 1;
          }
          command.create_object->object_record.version = resolved_version;
        }
      }
    }

    const ProposeResult result = node_.ProposeMetadata(SerializeMetadataCommand(command));
    const NodeStatusSnapshot latest_status = node_.GetStatusSnapshot();
    const CommittedMembershipQuorumSummary quorum_summary =
        node_.GetCommittedMembershipQuorumSummary();
    FillWriteSummary(latest_status,
                     quorum_summary,
                     result,
                     request->request_id(),
                     request->bucket(),
                     request->object_key(),
                     request->object_id(),
                     ObjectState::PENDING,
                     response->mutable_summary());

    if (const raft::MetadataStatusCode code = ToWriteProtoStatusCode(result);
        code == raft::METADATA_STATUS_CODE_OK ||
        code == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
    {
      if (const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
          metadata_state_machine != nullptr)
      {
        if (const auto record =
                metadata_state_machine->FindObject(request->bucket(), request->object_key());
            record.has_value())
        {
          FillObjectRecord(*record, response->mutable_object());
        }
      }
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::CommitObject(
      grpc::CallbackServerContext *context,
      const raft::CommitObjectRequest *request,
      raft::CommitObjectResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const NodeStatusSnapshot status = node_.GetStatusSnapshot();
    MetadataCommand command = MakeCommitObjectCommand(*request);
    std::string validation_error;
    if (!ValidateMetadataCommand(command, &validation_error))
    {
      return FinishValidationError(context,
                                   status,
                                   validation_error,
                                   request->request_id(),
                                   request->bucket(),
                                   request->object_key(),
                                   request->object_id(),
                                   response);
    }

    const ProposeResult result = node_.ProposeMetadata(SerializeMetadataCommand(command));
    const NodeStatusSnapshot latest_status = node_.GetStatusSnapshot();
    const CommittedMembershipQuorumSummary quorum_summary =
        node_.GetCommittedMembershipQuorumSummary();
    FillWriteSummary(latest_status,
                     quorum_summary,
                     result,
                     request->request_id(),
                     request->bucket(),
                     request->object_key(),
                     request->object_id(),
                     ObjectState::COMMITTED,
                     response->mutable_summary());

    if (const raft::MetadataStatusCode code = ToWriteProtoStatusCode(result);
        code == raft::METADATA_STATUS_CODE_OK ||
        code == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
    {
      if (const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
          metadata_state_machine != nullptr)
      {
        if (const auto record =
                metadata_state_machine->FindObject(request->bucket(), request->object_key());
            record.has_value())
        {
          FillObjectRecord(*record, response->mutable_object());
        }
      }
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::AbortObject(
      grpc::CallbackServerContext *context,
      const raft::AbortObjectRequest *request,
      raft::AbortObjectResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const NodeStatusSnapshot status = node_.GetStatusSnapshot();
    MetadataCommand command = MakeAbortObjectCommand(*request);
    std::string validation_error;
    if (!ValidateMetadataCommand(command, &validation_error))
    {
      return FinishValidationError(context,
                                   status,
                                   validation_error,
                                   request->request_id(),
                                   request->bucket(),
                                   request->object_key(),
                                   request->object_id(),
                                   response);
    }

    const ProposeResult result = node_.ProposeMetadata(SerializeMetadataCommand(command));
    const NodeStatusSnapshot latest_status = node_.GetStatusSnapshot();
    const CommittedMembershipQuorumSummary quorum_summary =
        node_.GetCommittedMembershipQuorumSummary();
    FillWriteSummary(latest_status,
                     quorum_summary,
                     result,
                     request->request_id(),
                     request->bucket(),
                     request->object_key(),
                     request->object_id(),
                     ObjectState::DELETED,
                     response->mutable_summary());

    if (const raft::MetadataStatusCode code = ToWriteProtoStatusCode(result);
        code == raft::METADATA_STATUS_CODE_OK ||
        code == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
    {
      if (const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
          metadata_state_machine != nullptr)
      {
        if (const auto record =
                metadata_state_machine->FindObject(request->bucket(), request->object_key());
            record.has_value())
        {
          FillObjectRecord(*record, response->mutable_object());
        }
      }
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::DeleteObject(
      grpc::CallbackServerContext *context,
      const raft::DeleteObjectRequest *request,
      raft::DeleteObjectResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const NodeStatusSnapshot status = node_.GetStatusSnapshot();
    MetadataCommand command = MakeDeleteObjectCommand(*request);
    std::string validation_error;
    if (!ValidateMetadataCommand(command, &validation_error))
    {
      return FinishValidationError(context,
                                   status,
                                   validation_error,
                                   request->request_id(),
                                   request->bucket(),
                                   request->object_key(),
                                   request->object_id(),
                                   response);
    }

    const ProposeResult result = node_.ProposeMetadata(SerializeMetadataCommand(command));
    const NodeStatusSnapshot latest_status = node_.GetStatusSnapshot();
    const CommittedMembershipQuorumSummary quorum_summary =
        node_.GetCommittedMembershipQuorumSummary();
    FillWriteSummary(latest_status,
                     quorum_summary,
                     result,
                     request->request_id(),
                     request->bucket(),
                     request->object_key(),
                     request->object_id(),
                     ObjectState::DELETED,
                     response->mutable_summary());

    if (const raft::MetadataStatusCode code = ToWriteProtoStatusCode(result);
        code == raft::METADATA_STATUS_CODE_OK ||
        code == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY)
    {
      if (const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
          metadata_state_machine != nullptr)
      {
        if (const auto record =
                metadata_state_machine->FindObject(request->bucket(), request->object_key());
            record.has_value())
        {
          FillObjectRecord(*record, response->mutable_object());
        }
      }
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::HeadObject(
      grpc::CallbackServerContext *context,
      const raft::HeadObjectRequest *request,
      raft::HeadObjectResponse *response)
  {
    const NodeStatusSnapshot status = node_.GetStatusSnapshot();
    if (auto *reactor = FinishReadAdmissionIfRejected(
            node_,
            context,
            status,
            request->bucket(),
            request->object_key(),
            request->object_id(),
            response);
        reactor != nullptr)
    {
      return reactor;
    }

    std::string validation_error;
    if (!ValidateHeadObjectRequest(*request, &validation_error))
    {
      return FinishReadError(context,
                             status,
                             raft::METADATA_STATUS_CODE_INVALID_ARGUMENT,
                             validation_error,
                             request->bucket(),
                             request->object_key(),
                             request->object_id(),
                             response);
    }

    const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
    if (metadata_state_machine == nullptr)
    {
      return FinishReadError(context,
                             status,
                             raft::METADATA_STATUS_CODE_INTERNAL_ERROR,
                             "metadata state machine is not configured",
                             request->bucket(),
                             request->object_key(),
                             request->object_id(),
                             response);
    }

    HeadObjectQuery query;
    query.bucket = request->bucket();
    query.object_key = request->object_key();
    if (!request->object_id().empty())
    {
      query.object_id = request->object_id();
    }
    if (request->version() != 0)
    {
      query.version = request->version();
    }
    const MetadataHeadObjectResponse head = metadata_state_machine->HeadObject(query);
    FillSummary(status,
                head.result.code,
                head.result.summary.message,
                head.result.summary.request_id,
                request->bucket(),
                request->object_key(),
                request->object_id(),
                head.record.has_value() ? std::optional<ObjectState>(head.record->state)
                                        : std::nullopt,
                head.result.summary.log_index,
                head.result.summary.term,
                response->mutable_summary());
    DecorateSummaryWithContext(status,
                               status.leader_id >= 0
                                   ? std::optional<int>(status.leader_id)
                                   : std::nullopt,
                               response->mutable_summary());
    response->set_found(head.record.has_value());
    if (head.record.has_value())
    {
      FillObjectRecord(*head.record, response->mutable_object());
    }

    auto *reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::ListObjects(
      grpc::CallbackServerContext *context,
      const raft::ListObjectsRequest *request,
      raft::ListObjectsResponse *response)
  {
    const NodeStatusSnapshot status = node_.GetStatusSnapshot();
    if (auto *reactor = FinishReadAdmissionIfRejected(
            node_,
            context,
            status,
            request->bucket(),
            request->prefix(),
            "",
            response);
        reactor != nullptr)
    {
      return reactor;
    }

    std::string validation_error;
    if (!ValidateListObjectsRequest(*request, &validation_error))
    {
      return FinishReadError(context,
                             status,
                             raft::METADATA_STATUS_CODE_INVALID_ARGUMENT,
                             validation_error,
                             request->bucket(),
                             request->prefix(),
                             "",
                             response);
    }

    const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
    if (metadata_state_machine == nullptr)
    {
      return FinishReadError(context,
                             status,
                             raft::METADATA_STATUS_CODE_INTERNAL_ERROR,
                             "metadata state machine is not configured",
                             request->bucket(),
                             request->prefix(),
                             "",
                             response);
    }

    ListObjectsQuery query;
    query.bucket = request->bucket();
    query.prefix = request->prefix();
    if (request->limit() != 0)
    {
      query.limit = static_cast<std::size_t>(request->limit());
    }
    query.continuation_token = request->continuation_token();

    const MetadataListObjectsResponse list = metadata_state_machine->ListObjects(query);
    FillSummary(status,
                list.result.code,
                list.result.summary.message,
                list.result.summary.request_id,
                request->bucket(),
                request->prefix(),
                "",
                std::nullopt,
                list.result.summary.log_index,
                list.result.summary.term,
                response->mutable_summary());
    DecorateSummaryWithContext(status,
                               status.leader_id >= 0
                                   ? std::optional<int>(status.leader_id)
                                   : std::nullopt,
                               response->mutable_summary());
    response->clear_objects();
    for (const auto &record : list.records)
    {
      FillObjectRecord(record, response->add_objects());
    }
    response->set_next_continuation_token(list.next_page_token);

    auto *reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::JoinMetadataCluster(
      grpc::CallbackServerContext *context,
      const raft::JoinMetadataClusterRequest *request,
      raft::JoinMetadataClusterResponse *response)
  {
    const NodeStatusSnapshot initial_status = node_.GetStatusSnapshot();
    const CommittedMembershipQuorumSummary initial_quorum_summary =
        node_.GetCommittedMembershipQuorumSummary();
    const RuntimeMembershipSummary initial_runtime_summary =
        node_.GetRuntimeMembershipSummary();
    const auto finish_response =
        [&](const raft::MetadataStatusCode code,
            const raft::JoinMetadataClusterDisposition disposition,
            const std::string &message,
            const std::uint64_t membership_epoch,
            const bool committed_membership_changed) {
          return FinishJoinMetadataClusterResponse(
              context,
              initial_status,
              initial_quorum_summary,
              initial_runtime_summary,
              *request,
              code,
              disposition,
              message,
              membership_epoch,
              committed_membership_changed,
              response);
        };

    if (IsDeadlineExpired(context))
    {
      return finish_response(
          raft::METADATA_STATUS_CODE_TIMEOUT,
          raft::JOIN_METADATA_CLUSTER_DISPOSITION_REJECTED,
          "join validation deadline already expired before admission",
          initial_quorum_summary.committed_log_index,
          false);
    }

    if (!node_.IsRunning())
    {
      return finish_response(raft::METADATA_STATUS_CODE_SERVICE_UNAVAILABLE,
                             raft::JOIN_METADATA_CLUSTER_DISPOSITION_REJECTED,
                             "node is stopping",
                             initial_quorum_summary.committed_log_index,
                             false);
    }

    if (initial_status.role != "Leader")
    {
      return finish_response(raft::METADATA_STATUS_CODE_NOT_LEADER,
                             raft::JOIN_METADATA_CLUSTER_DISPOSITION_NOT_LEADER,
                             "join authority belongs to metadata leader",
                             initial_quorum_summary.committed_log_index,
                             false);
    }

    if (const auto validation_error =
            ValidateJoinMetadataClusterRequest(*request, initial_quorum_summary);
        validation_error.has_value())
    {
      return finish_response(raft::METADATA_STATUS_CODE_INVALID_ARGUMENT,
                             raft::JOIN_METADATA_CLUSTER_DISPOSITION_INVALID_CANDIDATE,
                             *validation_error,
                             initial_quorum_summary.committed_log_index,
                             false);
    }

    const AddLearnerProposalRequest proposal_request =
        BuildAddLearnerProposalRequest(*request);
    AddLearnerProposalResult proposal_result =
        node_.ProposeAddLearner(proposal_request);
    RuntimeMembershipSummary latest_runtime_summary =
        node_.GetRuntimeMembershipSummary();
    if (ShouldRouteJoinDuplicateToBatchPromotion(proposal_result,
                                                 latest_runtime_summary,
                                                 *request))
    {
      proposal_result = node_.PromoteReadyLearnerBatch(proposal_request);
      latest_runtime_summary = node_.GetRuntimeMembershipSummary();
    }

    const NodeStatusSnapshot latest_status = node_.GetStatusSnapshot();
    const CommittedMembershipQuorumSummary latest_quorum_summary =
        node_.GetCommittedMembershipQuorumSummary();
    return FinishJoinMetadataClusterResponse(context,
                                             latest_status,
                                             latest_quorum_summary,
                                             latest_runtime_summary,
                                             *request,
                                             ToJoinMetadataStatusCode(proposal_result),
                                             ToJoinMetadataDisposition(proposal_result),
                                             proposal_result.message,
                                             proposal_result.membership_epoch,
                                             proposal_result.committed_membership_changed,
                                             response);
  }

} // namespace raftdemo
