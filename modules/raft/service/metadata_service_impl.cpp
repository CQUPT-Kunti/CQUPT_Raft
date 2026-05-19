#include "raft/service/metadata_service_impl.h"

#include <optional>
#include <string>
#include <utility>

#include "raft/common/metadata_command.h"
#include "raft/common/metadata_result.h"
#include "raft/node/raft_node.h"
#include "raft/state_machine/metadata_state_machine.h"

namespace raftdemo
{
  namespace
  {

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

    raft::MetadataRecordState ToProtoRecordState(const MetadataRecordState state)
    {
      switch (state)
      {
      case MetadataRecordState::kPending:
        return raft::METADATA_RECORD_STATE_PENDING;
      case MetadataRecordState::kCommitted:
        return raft::METADATA_RECORD_STATE_COMMITTED;
      case MetadataRecordState::kDeleted:
        return raft::METADATA_RECORD_STATE_DELETED;
      default:
        return raft::METADATA_RECORD_STATE_UNSPECIFIED;
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

    void FillManifest(const MetadataRecord &record, raft::MetadataManifest *manifest)
    {
      if (manifest == nullptr)
      {
        return;
      }

      manifest->set_object_size(record.object_size);
      manifest->set_chunk_size(record.chunk_size);
      manifest->set_chunk_count(record.chunk_count);
      manifest->set_checksum(record.checksum);
      manifest->clear_mock_locations();
      for (const auto &location : record.mock_locations)
      {
        manifest->add_mock_locations(location);
      }
    }

    void FillRecord(const MetadataRecord &record, raft::MetadataRecord *out)
    {
      if (out == nullptr)
      {
        return;
      }

      out->set_object_key(record.object_key);
      out->set_state(ToProtoRecordState(record.state));
      FillManifest(record, out->mutable_manifest());
      out->set_payload(record.payload);
      out->set_create_request_id(record.create_request_id);
      out->set_commit_request_id(record.commit_request_id.value_or(""));
      out->set_delete_request_id(record.delete_request_id.value_or(""));
      out->set_created_at_log_index(record.created_at_log_index);
      out->set_committed_at_log_index(record.committed_at_log_index.value_or(0));
      out->set_deleted_at_log_index(record.deleted_at_log_index.value_or(0));
      out->set_commit_info(record.commit_info);
      out->set_delete_info(record.delete_info);
    }

    void FillSummary(const NodeStatusSnapshot &status,
                     MetadataStatusCode code,
                     const MetadataResponseSummary &summary,
                     raft::MetadataResponseSummary *out)
    {
      if (out == nullptr)
      {
        return;
      }

      out->set_code(ToProtoStatusCode(code));
      out->set_message(summary.message);
      out->set_request_id(summary.request_id);
      out->set_object_key(summary.object_key);
      out->set_state(summary.result_state.has_value()
                         ? ToProtoRecordState(*summary.result_state)
                         : raft::METADATA_RECORD_STATE_UNSPECIFIED);
      out->set_term(summary.term.value_or(status.term));
      out->set_log_index(summary.log_index.value_or(0));
      FillLeaderHint(status, out->mutable_leader_hint());
    }

    void FillWriteSummary(const NodeStatusSnapshot &status,
                          const ProposeResult &result,
                          const std::string &request_id,
                          const std::string &object_key,
                          const std::optional<MetadataRecordState> result_state,
                          raft::MetadataResponseSummary *out)
    {
      const MetadataStatusCode code = ToMetadataStatusCode(result);
      MetadataResponseSummary summary;
      summary.request_id = request_id;
      summary.object_key = object_key;
      if (code == MetadataStatusCode::kOk || code == MetadataStatusCode::kIdempotentReplay)
      {
        summary.result_state = result_state;
      }
      summary.term = result.term;
      summary.log_index = result.log_index;
      summary.message = result.message;
      FillSummary(status, code, summary, out);
    }

    MetadataRecord MakeRecordFromCreateRequest(const raft::CreateMetadataRecordRequest &request)
    {
      MetadataRecord record;
      record.object_key = request.object_key();
      record.state = MetadataRecordState::kPending;
      record.object_size = request.manifest().object_size();
      record.chunk_size = request.manifest().chunk_size();
      record.chunk_count = request.manifest().chunk_count();
      record.checksum = request.manifest().checksum();
      record.payload = request.payload();
      record.create_request_id = request.request_id();
      for (const auto &location : request.manifest().mock_locations())
      {
        record.mock_locations.push_back(location);
      }
      return record;
    }

    MetadataCommand MakeCommitMetadataCommand(const raft::CommitMetadataRecordRequest &request)
    {
      MetadataCommand command;
      command.operation = MetadataOperation::kCommit;
      command.request_id = request.request_id();
      command.object_key = request.object_key();
      command.commit_info = request.commit_info();
      return command;
    }

    MetadataCommand MakeDeleteMetadataCommand(const raft::DeleteMetadataRecordRequest &request)
    {
      MetadataCommand command;
      command.operation = MetadataOperation::kDelete;
      command.request_id = request.request_id();
      command.object_key = request.object_key();
      command.delete_info = request.delete_info();
      return command;
    }

    bool EnsureLeaderForRead(const NodeStatusSnapshot &status,
                             const std::string &object_key,
                             raft::MetadataResponseSummary *summary)
    {
      if (status.role == "Leader")
      {
        return true;
      }

      if (summary != nullptr)
      {
        summary->set_code(raft::METADATA_STATUS_CODE_NOT_LEADER);
        summary->set_message("node is not the leader");
        summary->set_object_key(object_key);
        summary->set_term(status.term);
        FillLeaderHint(status, summary->mutable_leader_hint());
      }
      return false;
    }

  } // namespace

  MetadataServiceImpl::MetadataServiceImpl(RaftNode &node) : node_(node) {}

  grpc::ServerUnaryReactor *MetadataServiceImpl::CreateMetadataRecord(
      grpc::CallbackServerContext *context,
      const raft::CreateMetadataRecordRequest *request,
      raft::CreateMetadataRecordResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto status = node_.GetStatusSnapshot();

    MetadataCommand command = MakeCreateMetadataCommand(MakeRecordFromCreateRequest(*request));
    std::string validation_error;
    if (!ValidateMetadataCommand(command, &validation_error))
    {
      MetadataResponseSummary summary;
      summary.request_id = request->request_id();
      summary.object_key = request->object_key();
      summary.message = validation_error;
      FillSummary(status, MetadataStatusCode::kInvalidArgument, summary, response->mutable_summary());
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    const ProposeResult result = node_.ProposeMetadata(SerializeMetadataCommand(command));
    const auto latest_status = node_.GetStatusSnapshot();
    FillWriteSummary(latest_status, result, request->request_id(), request->object_key(),
                     MetadataRecordState::kPending, response->mutable_summary());

    if (ToMetadataStatusCode(result) == MetadataStatusCode::kOk ||
        ToMetadataStatusCode(result) == MetadataStatusCode::kIdempotentReplay)
    {
      MetadataRecord record = *command.record;
      record.created_at_log_index = result.log_index;
      FillRecord(record, response->mutable_record());
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::CommitMetadataRecord(
      grpc::CallbackServerContext *context,
      const raft::CommitMetadataRecordRequest *request,
      raft::CommitMetadataRecordResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto status = node_.GetStatusSnapshot();

    MetadataCommand command = MakeCommitMetadataCommand(*request);
    std::string validation_error;
    if (!ValidateMetadataCommand(command, &validation_error))
    {
      MetadataResponseSummary summary;
      summary.request_id = request->request_id();
      summary.object_key = request->object_key();
      summary.message = validation_error;
      FillSummary(status, MetadataStatusCode::kInvalidArgument, summary, response->mutable_summary());
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    const ProposeResult result = node_.ProposeMetadata(SerializeMetadataCommand(command));
    const auto latest_status = node_.GetStatusSnapshot();
    FillWriteSummary(latest_status, result, request->request_id(), request->object_key(),
                     MetadataRecordState::kCommitted, response->mutable_summary());

    if (ToMetadataStatusCode(result) == MetadataStatusCode::kOk ||
        ToMetadataStatusCode(result) == MetadataStatusCode::kIdempotentReplay)
    {
      const StrongConsistencyMetadataStateMachine *metadata_state_machine =
          node_.GetMetadataStateMachine();
      if (metadata_state_machine != nullptr)
      {
        const MetadataHeadResponse head =
            metadata_state_machine->HeadMetadataRecord({request->object_key()});
        if (head.record.has_value())
        {
          FillRecord(*head.record, response->mutable_record());
        }
      }
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::DeleteMetadataRecord(
      grpc::CallbackServerContext *context,
      const raft::DeleteMetadataRecordRequest *request,
      raft::DeleteMetadataRecordResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto status = node_.GetStatusSnapshot();

    MetadataCommand command = MakeDeleteMetadataCommand(*request);
    std::string validation_error;
    if (!ValidateMetadataCommand(command, &validation_error))
    {
      MetadataResponseSummary summary;
      summary.request_id = request->request_id();
      summary.object_key = request->object_key();
      summary.message = validation_error;
      FillSummary(status, MetadataStatusCode::kInvalidArgument, summary, response->mutable_summary());
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    const ProposeResult result = node_.ProposeMetadata(SerializeMetadataCommand(command));
    const auto latest_status = node_.GetStatusSnapshot();
    FillWriteSummary(latest_status, result, request->request_id(), request->object_key(),
                     MetadataRecordState::kDeleted, response->mutable_summary());

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::HeadMetadataRecord(
      grpc::CallbackServerContext *context,
      const raft::HeadMetadataRecordRequest *request,
      raft::HeadMetadataRecordResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto status = node_.GetStatusSnapshot();
    if (!EnsureLeaderForRead(status, request->object_key(), response->mutable_summary()))
    {
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    const StrongConsistencyMetadataStateMachine *metadata_state_machine =
        node_.GetMetadataStateMachine();
    if (metadata_state_machine == nullptr)
    {
      MetadataResponseSummary summary;
      summary.object_key = request->object_key();
      summary.message = "metadata state machine is not configured";
      FillSummary(status, MetadataStatusCode::kInternalError, summary, response->mutable_summary());
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    const MetadataHeadResponse head =
        metadata_state_machine->HeadMetadataRecord({request->object_key()});
    FillSummary(status, head.result.code, head.result.summary, response->mutable_summary());
    response->set_found(head.record.has_value());
    if (head.record.has_value())
    {
      FillRecord(*head.record, response->mutable_record());
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerUnaryReactor *MetadataServiceImpl::ListMetadataRecords(
      grpc::CallbackServerContext *context,
      const raft::ListMetadataRecordsRequest *request,
      raft::ListMetadataRecordsResponse *response)
  {
    auto *reactor = context->DefaultReactor();
    const auto status = node_.GetStatusSnapshot();
    if (!EnsureLeaderForRead(status, "", response->mutable_summary()))
    {
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    const StrongConsistencyMetadataStateMachine *metadata_state_machine =
        node_.GetMetadataStateMachine();
    if (metadata_state_machine == nullptr)
    {
      MetadataResponseSummary summary;
      summary.message = "metadata state machine is not configured";
      FillSummary(status, MetadataStatusCode::kInternalError, summary, response->mutable_summary());
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }

    MetadataListRequest list_request;
    list_request.prefix = request->prefix();
    if (request->limit() > 0)
    {
      list_request.limit = static_cast<std::size_t>(request->limit());
    }
    list_request.page_token = request->page_token();

    const MetadataListResponse list = metadata_state_machine->ListMetadataRecords(list_request);
    FillSummary(status, list.result.code, list.result.summary, response->mutable_summary());
    response->set_next_page_token(list.next_page_token);
    for (const auto &record : list.records)
    {
      FillRecord(record, response->add_records());
    }

    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

} // namespace raftdemo
