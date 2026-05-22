#include "raft/service/metadata_service_impl.h"

#include <cstdint>
#include <optional>
#include <string>
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

    MetadataStatusCode ToMetadataStatusCode(const ProposeResult &result)
    {
      switch (result.status)
      {
      case ProposeStatus::kOk:
        if (result.message == "idempotent replay")
        {
          return MetadataStatusCode::kIdempotentReplay;
        }
        return MetadataStatusCode::kOk;
      case ProposeStatus::kNotLeader:
        return MetadataStatusCode::kNotLeader;
      case ProposeStatus::kInvalidCommand:
        return MetadataStatusCode::kInvalidArgument;
      case ProposeStatus::kTimeout:
        return MetadataStatusCode::kTimeout;
      case ProposeStatus::kApplyFailed:
        if (result.message.rfind("invalid metadata command:", 0) == 0)
        {
          return MetadataStatusCode::kInvalidArgument;
        }
        if (result.message.rfind("not found:", 0) == 0)
        {
          return MetadataStatusCode::kNotFound;
        }
        if (result.message.rfind("state conflict:", 0) == 0)
        {
          return MetadataStatusCode::kStateConflict;
        }
        if (result.message.rfind("idempotency conflict:", 0) == 0)
        {
          return MetadataStatusCode::kIdempotencyConflict;
        }
        return MetadataStatusCode::kInternalError;
      case ProposeStatus::kNodeStopping:
      case ProposeStatus::kReplicationFailed:
      case ProposeStatus::kCommitFailed:
      default:
        return MetadataStatusCode::kInternalError;
      }
    }

    void FillLeaderHint(const NodeStatusSnapshot &status,
                        raft::MetadataLeaderHint *leader_hint)
    {
      if (leader_hint == nullptr)
      {
        return;
      }
      leader_hint->set_leader_id(status.leader_id);
      leader_hint->set_leader_address(status.leader_address);
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
      if (out == nullptr)
      {
        return;
      }
      out->set_code(ToProtoStatusCode(code));
      out->set_message(message);
      out->set_request_id(request_id);
      out->set_bucket(bucket);
      out->set_object_key(object_key);
      out->set_object_id(object_id);
      out->set_state(state.has_value() ? ToProtoObjectState(*state)
                                       : raft::METADATA_OBJECT_STATE_UNSPECIFIED);
      out->set_log_index(log_index.value_or(0));
      out->set_term(term.value_or(status.term));
      FillLeaderHint(status, out->mutable_leader_hint());
    }

    void FillWriteSummary(const NodeStatusSnapshot &status,
                          const ProposeResult &result,
                          const std::string &request_id,
                          const std::string &bucket,
                          const std::string &object_key,
                          const std::string &object_id,
                          const std::optional<ObjectState> state,
                          raft::MetadataResponseSummary *out)
    {
      FillSummary(status,
                  ToMetadataStatusCode(result),
                  result.message,
                  request_id,
                  bucket,
                  object_key,
                  object_id,
                  state,
                  result.log_index,
                  result.term,
                  out);
    }

    bool EnsureLeaderForRead(const NodeStatusSnapshot &status,
                             const std::string &bucket,
                             const std::string &object_key,
                             const std::string &object_id,
                             raft::MetadataResponseSummary *summary)
    {
      if (status.role == "Leader")
      {
        return true;
      }

      FillSummary(status,
                  MetadataStatusCode::kNotLeader,
                  "node is not the leader",
                  "",
                  bucket,
                  object_key,
                  object_id,
                  std::nullopt,
                  std::nullopt,
                  status.term,
                  summary);
      return false;
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
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

  } // namespace

  MetadataServiceImpl::MetadataServiceImpl(RaftNode &node) : node_(node) {}

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
    FillWriteSummary(latest_status,
                     result,
                     request->request_id(),
                     request->bucket(),
                     "",
                     "",
                     std::nullopt,
                     response->mutable_summary());

    if (const MetadataStatusCode code = ToMetadataStatusCode(result);
        code == MetadataStatusCode::kOk || code == MetadataStatusCode::kIdempotentReplay)
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
    FillWriteSummary(latest_status,
                     result,
                     request->request_id(),
                     request->bucket(),
                     "",
                     "",
                     std::nullopt,
                     response->mutable_summary());

    if (const MetadataStatusCode code = ToMetadataStatusCode(result);
        code == MetadataStatusCode::kOk || code == MetadataStatusCode::kIdempotentReplay)
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

    const ProposeResult result = node_.ProposeMetadata(SerializeMetadataCommand(command));
    const NodeStatusSnapshot latest_status = node_.GetStatusSnapshot();
    FillWriteSummary(latest_status,
                     result,
                     request->request_id(),
                     request->bucket(),
                     request->object_key(),
                     request->object_id(),
                     ObjectState::PENDING,
                     response->mutable_summary());

    if (const MetadataStatusCode code = ToMetadataStatusCode(result);
        code == MetadataStatusCode::kOk || code == MetadataStatusCode::kIdempotentReplay)
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
    FillWriteSummary(latest_status,
                     result,
                     request->request_id(),
                     request->bucket(),
                     request->object_key(),
                     request->object_id(),
                     ObjectState::COMMITTED,
                     response->mutable_summary());

    if (const MetadataStatusCode code = ToMetadataStatusCode(result);
        code == MetadataStatusCode::kOk || code == MetadataStatusCode::kIdempotentReplay)
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
    FillWriteSummary(latest_status,
                     result,
                     request->request_id(),
                     request->bucket(),
                     request->object_key(),
                     request->object_id(),
                     ObjectState::DELETED,
                     response->mutable_summary());

    if (const MetadataStatusCode code = ToMetadataStatusCode(result);
        code == MetadataStatusCode::kOk || code == MetadataStatusCode::kIdempotentReplay)
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
    FillWriteSummary(latest_status,
                     result,
                     request->request_id(),
                     request->bucket(),
                     request->object_key(),
                     request->object_id(),
                     ObjectState::DELETED,
                     response->mutable_summary());

    if (const MetadataStatusCode code = ToMetadataStatusCode(result);
        code == MetadataStatusCode::kOk || code == MetadataStatusCode::kIdempotentReplay)
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
    auto *reactor = context->DefaultReactor();
    const NodeStatusSnapshot status = node_.GetStatusSnapshot();
    if (!EnsureLeaderForRead(status,
                             request->bucket(),
                             request->object_key(),
                             request->object_id(),
                             response->mutable_summary()))
    {
      response->set_found(false);
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
    if (metadata_state_machine == nullptr)
    {
      FillSummary(status,
                  MetadataStatusCode::kInternalError,
                  "metadata state machine is not configured",
                  "",
                  request->bucket(),
                  request->object_key(),
                  request->object_id(),
                  std::nullopt,
                  std::nullopt,
                  status.term,
                  response->mutable_summary());
      response->set_found(false);
      reactor->Finish(grpc::Status::OK);
      return reactor;
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
    response->set_found(head.record.has_value());
    if (head.record.has_value())
    {
      FillObjectRecord(*head.record, response->mutable_object());
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::ListObjects(
      grpc::CallbackServerContext *context,
      const raft::ListObjectsRequest *request,
      raft::ListObjectsResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const NodeStatusSnapshot status = node_.GetStatusSnapshot();
    if (!EnsureLeaderForRead(status,
                             request->bucket(),
                             request->prefix(),
                             "",
                             response->mutable_summary()))
    {
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    const MetadataStateMachine *metadata_state_machine = node_.GetMetadataStateMachineV2();
    if (metadata_state_machine == nullptr)
    {
      FillSummary(status,
                  MetadataStatusCode::kInternalError,
                  "metadata state machine is not configured",
                  "",
                  request->bucket(),
                  request->prefix(),
                  "",
                  std::nullopt,
                  std::nullopt,
                  status.term,
                  response->mutable_summary());
      reactor->Finish(grpc::Status::OK);
      return reactor;
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
    response->clear_objects();
    for (const auto &record : list.records)
    {
      FillObjectRecord(record, response->add_objects());
    }
    response->set_next_continuation_token(list.next_page_token);

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

} // namespace raftdemo
