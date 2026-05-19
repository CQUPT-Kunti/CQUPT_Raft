#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "raft.grpc.pb.h"
#include "raft.pb.h"

namespace
{

  struct ParsedArgs
  {
    std::string server_address;
    std::string command;
    std::string request_id;
    std::string object_key;
    std::string prefix;
    std::optional<std::uint32_t> limit;
    std::string page_token;
    std::uint64_t object_size = 0;
    std::uint64_t chunk_size = 0;
    std::optional<std::uint64_t> chunk_count;
    std::string checksum;
    std::vector<std::string> mock_locations;
    std::string payload;
    std::string expected_create_request_id;
    std::string commit_info;
    std::string delete_info;
    int max_retries = 1;
    int timeout_ms = 3000;
  };

  const char *MetadataStatusCodeToString(const raft::MetadataStatusCode code)
  {
    switch (code)
    {
    case raft::METADATA_STATUS_CODE_OK:
      return "OK";
    case raft::METADATA_STATUS_CODE_NOT_LEADER:
      return "NOT_LEADER";
    case raft::METADATA_STATUS_CODE_INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
    case raft::METADATA_STATUS_CODE_NOT_FOUND:
      return "NOT_FOUND";
    case raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY:
      return "IDEMPOTENT_REPLAY";
    case raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT:
      return "IDEMPOTENCY_CONFLICT";
    case raft::METADATA_STATUS_CODE_STATE_CONFLICT:
      return "STATE_CONFLICT";
    case raft::METADATA_STATUS_CODE_TIMEOUT:
      return "TIMEOUT";
    case raft::METADATA_STATUS_CODE_INTERNAL_ERROR:
      return "INTERNAL_ERROR";
    case raft::METADATA_STATUS_CODE_UNSPECIFIED:
    default:
      return "UNSPECIFIED";
    }
  }

  const char *MetadataRecordStateToString(const raft::MetadataRecordState state)
  {
    switch (state)
    {
    case raft::METADATA_RECORD_STATE_PENDING:
      return "PENDING";
    case raft::METADATA_RECORD_STATE_COMMITTED:
      return "COMMITTED";
    case raft::METADATA_RECORD_STATE_DELETED:
      return "DELETED";
    case raft::METADATA_RECORD_STATE_UNSPECIFIED:
    default:
      return "UNSPECIFIED";
    }
  }

  bool NeedsRetry(const raft::MetadataStatusCode code)
  {
    return code == raft::METADATA_STATUS_CODE_NOT_LEADER ||
           code == raft::METADATA_STATUS_CODE_TIMEOUT;
  }

  std::uint64_t ParsePositiveInt(const std::string &value, const char *name)
  {
    try
    {
      std::size_t pos = 0;
      const unsigned long parsed = std::stoul(value, &pos, 10);
      if (pos != value.size())
      {
        throw std::invalid_argument("trailing characters");
      }
      return parsed;
    }
    catch (const std::exception &)
    {
      std::cerr << "invalid numeric value for " << name << ": " << value << '\n';
      std::exit(2);
    }
  }

  bool IsSupportedCommand(const std::string &command)
  {
    return command == "create" ||
           command == "commit" ||
           command == "delete" ||
           command == "head" ||
           command == "list" ||
           command == "commit-retry" ||
           command == "delete-retry";
  }

  void PrintUsage()
  {
    std::cerr
        << "Usage:\n"
        << "  raft_metadata_client <addr> create"
        << " --request-id <id> --object-key <key>"
        << " --object-size <bytes> --chunk-size <bytes>"
        << " [--chunk-count <n>] [--checksum <mock-checksum>]"
        << " [--mock-location <value>]..."
        << " [--payload <metadata-only-payload>]"
        << " [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> commit"
        << " --request-id <id> --object-key <key>"
        << " [--expected-create-request-id <id>] [--commit-info <text>]"
        << " [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> delete"
        << " --request-id <id> --object-key <key>"
        << " [--delete-info <text>]"
        << " [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> head"
        << " --object-key <key>"
        << " [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> list"
        << " [--prefix <prefix>] [--limit <n>] [--page-token <token>]"
        << " [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> commit-retry"
        << " --request-id <id> --object-key <key>"
        << " [--expected-create-request-id <id>] [--commit-info <text>]"
        << " [--max-retries <n>] [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> delete-retry"
        << " --request-id <id> --object-key <key>"
        << " [--delete-info <text>]"
        << " [--max-retries <n>] [--timeout-ms <ms>]\n";
  }

  [[noreturn]] void ExitUsageError(const std::string &message)
  {
    if (!message.empty())
    {
      std::cerr << message << '\n';
    }
    PrintUsage();
    std::exit(2);
  }

  void ValidateArgs(const ParsedArgs &args)
  {
    if (!IsSupportedCommand(args.command))
    {
      ExitUsageError("unsupported command: " + args.command);
    }

    if (args.timeout_ms <= 0)
    {
      ExitUsageError("--timeout-ms must be > 0");
    }

    if (args.max_retries < 0)
    {
      ExitUsageError("--max-retries must be >= 0");
    }

    if (args.command == "create")
    {
      if (args.request_id.empty() || args.object_key.empty())
      {
        ExitUsageError("create requires --request-id and --object-key");
      }
      if (args.object_size == 0 || args.chunk_size == 0)
      {
        ExitUsageError("create requires --object-size > 0 and --chunk-size > 0");
      }
      if (args.chunk_count.has_value() && *args.chunk_count == 0)
      {
        ExitUsageError("--chunk-count must be > 0 when specified");
      }
      return;
    }

    if (args.command == "commit" ||
        args.command == "delete" ||
        args.command == "commit-retry" ||
        args.command == "delete-retry")
    {
      if (args.request_id.empty() || args.object_key.empty())
      {
        ExitUsageError(args.command + " requires --request-id and --object-key");
      }
      return;
    }

    if (args.command == "head")
    {
      if (args.object_key.empty())
      {
        ExitUsageError("head requires --object-key");
      }
      return;
    }
  }

  ParsedArgs ParseArgs(int argc, char **argv)
  {
    if (argc < 3)
    {
      PrintUsage();
      std::exit(2);
    }

    ParsedArgs args;
    args.server_address = argv[1];
    args.command = argv[2];

    for (int i = 3; i < argc; ++i)
    {
      const std::string flag = argv[i];
      auto require_value = [&](const char *name) -> std::string {
        if (i + 1 >= argc)
        {
          ExitUsageError(std::string("missing value for ") + name);
        }
        return argv[++i];
      };

      if (flag == "--request-id")
      {
        args.request_id = require_value("--request-id");
      }
      else if (flag == "--object-key")
      {
        args.object_key = require_value("--object-key");
      }
      else if (flag == "--prefix")
      {
        args.prefix = require_value("--prefix");
      }
      else if (flag == "--limit")
      {
        args.limit =
            static_cast<std::uint32_t>(ParsePositiveInt(require_value("--limit"), "--limit"));
      }
      else if (flag == "--page-token")
      {
        args.page_token = require_value("--page-token");
      }
      else if (flag == "--expected-create-request-id")
      {
        args.expected_create_request_id = require_value("--expected-create-request-id");
      }
      else if (flag == "--object-size")
      {
        args.object_size = ParsePositiveInt(require_value("--object-size"), "--object-size");
      }
      else if (flag == "--chunk-size")
      {
        args.chunk_size = ParsePositiveInt(require_value("--chunk-size"), "--chunk-size");
      }
      else if (flag == "--chunk-count")
      {
        args.chunk_count = ParsePositiveInt(require_value("--chunk-count"), "--chunk-count");
      }
      else if (flag == "--checksum")
      {
        args.checksum = require_value("--checksum");
      }
      else if (flag == "--mock-location")
      {
        args.mock_locations.push_back(require_value("--mock-location"));
      }
      else if (flag == "--payload")
      {
        args.payload = require_value("--payload");
      }
      else if (flag == "--commit-info")
      {
        args.commit_info = require_value("--commit-info");
      }
      else if (flag == "--delete-info")
      {
        args.delete_info = require_value("--delete-info");
      }
      else if (flag == "--max-retries")
      {
        args.max_retries =
            static_cast<int>(ParsePositiveInt(require_value("--max-retries"), "--max-retries"));
      }
      else if (flag == "--timeout-ms")
      {
        args.timeout_ms =
            static_cast<int>(ParsePositiveInt(require_value("--timeout-ms"), "--timeout-ms"));
      }
      else
      {
        ExitUsageError("unknown argument: " + flag);
      }
    }

    ValidateArgs(args);
    return args;
  }

  std::unique_ptr<raft::MetadataService::Stub> MakeStub(const std::string &address)
  {
    auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
    return raft::MetadataService::NewStub(channel);
  }

  std::uint64_t ComputeChunkCount(std::uint64_t object_size, std::uint64_t chunk_size)
  {
    return 1 + ((object_size - 1) / chunk_size);
  }

  std::string MakeMockChecksum(const ParsedArgs &args, std::uint64_t chunk_count)
  {
    std::ostringstream oss;
    oss << "sha256:mock:"
        << args.object_key << ':'
        << args.object_size << ':'
        << args.chunk_size << ':'
        << chunk_count;
    return oss.str();
  }

  std::vector<std::string> MakeMockLocations(const ParsedArgs &args, std::uint64_t chunk_count)
  {
    if (!args.mock_locations.empty())
    {
      return args.mock_locations;
    }

    std::vector<std::string> locations;
    locations.reserve(static_cast<std::size_t>(chunk_count));
    for (std::uint64_t i = 0; i < chunk_count; ++i)
    {
      locations.push_back("mock-node-" + std::to_string((i % 3) + 1) +
                          "/chunk-" + std::to_string(i));
    }
    return locations;
  }

  std::string JoinItems(const std::vector<std::string> &items)
  {
    std::ostringstream oss;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
      if (i != 0)
      {
        oss << ',';
      }
      oss << items[i];
    }
    return oss.str();
  }

  std::string SummaryRequestId(const ParsedArgs &args,
                               const raft::MetadataResponseSummary &summary)
  {
    return summary.request_id().empty() ? args.request_id : summary.request_id();
  }

  std::string SummaryObjectKey(const ParsedArgs &args,
                               const raft::MetadataResponseSummary &summary)
  {
    return summary.object_key().empty() ? args.object_key : summary.object_key();
  }

  void PrintSummary(const char *stage,
                    const ParsedArgs &args,
                    const std::string &target_address,
                    const raft::MetadataResponseSummary &summary,
                    std::optional<int> attempt = std::nullopt)
  {
    std::cout << "stage=" << stage;
    if (attempt.has_value())
    {
      std::cout << " attempt=" << *attempt;
    }
    std::cout << " target_address=" << target_address
              << " code=" << MetadataStatusCodeToString(summary.code())
              << " status=" << MetadataStatusCodeToString(summary.code())
              << " message=\"" << summary.message() << "\""
              << " request_id=" << SummaryRequestId(args, summary)
              << " object_key=" << SummaryObjectKey(args, summary)
              << " state=" << MetadataRecordStateToString(summary.state())
              << " leader_id=" << summary.leader_hint().leader_id()
              << " leader_address=" << summary.leader_hint().leader_address()
              << " term=" << summary.term()
              << " log_index=" << summary.log_index()
              << '\n';
  }

  void PrintRetryDecision(int next_attempt,
                          const std::string &request_id,
                          const std::string &current_address,
                          const std::string &next_address,
                          const raft::MetadataResponseSummary &summary)
  {
    std::cout << "retry_decision"
              << " next_attempt=" << next_attempt
              << " request_id=" << request_id
              << " retry_reason=" << MetadataStatusCodeToString(summary.code())
              << " current_address=" << current_address
              << " next_address=" << next_address
              << " leader_id=" << summary.leader_hint().leader_id()
              << " leader_address=" << summary.leader_hint().leader_address()
              << '\n';
  }

  std::string ChooseNextAddress(const std::string &current_address,
                                const raft::MetadataResponseSummary &summary)
  {
    if (!summary.leader_hint().leader_address().empty())
    {
      return summary.leader_hint().leader_address();
    }
    return current_address;
  }

  void ConfigureContext(grpc::ClientContext *context, int timeout_ms)
  {
    context->set_deadline(std::chrono::system_clock::now() +
                          std::chrono::milliseconds(timeout_ms));
  }

  void PrintRpcFailure(const char *stage,
                       const ParsedArgs &args,
                       const std::string &target_address,
                       const grpc::Status &rpc_status,
                       std::optional<int> attempt = std::nullopt)
  {
    std::cerr << "stage=" << stage;
    if (attempt.has_value())
    {
      std::cerr << " attempt=" << *attempt;
    }
    std::cerr << " target_address=" << target_address
              << " grpc_code=" << rpc_status.error_code()
              << " grpc_message=\"" << rpc_status.error_message() << "\""
              << " request_id=" << args.request_id
              << " object_key=" << args.object_key
              << " prefix=" << args.prefix
              << '\n';
  }

  void PrintRecordDetails(const char *prefix, const raft::MetadataRecord &record)
  {
    std::vector<std::string> mock_locations;
    mock_locations.reserve(static_cast<std::size_t>(record.manifest().mock_locations_size()));
    for (const auto &location : record.manifest().mock_locations())
    {
      mock_locations.push_back(location);
    }

    std::cout << prefix
              << " object_key=" << record.object_key()
              << " state=" << MetadataRecordStateToString(record.state())
              << " object_size=" << record.manifest().object_size()
              << " chunk_size=" << record.manifest().chunk_size()
              << " chunk_count=" << record.manifest().chunk_count()
              << " checksum=" << record.manifest().checksum()
              << " mock_locations=" << JoinItems(mock_locations)
              << " create_request_id=" << record.create_request_id()
              << " commit_request_id=" << record.commit_request_id()
              << " delete_request_id=" << record.delete_request_id()
              << " created_at_log_index=" << record.created_at_log_index()
              << " committed_at_log_index=" << record.committed_at_log_index()
              << " deleted_at_log_index=" << record.deleted_at_log_index()
              << " commit_info=\"" << record.commit_info() << "\""
              << " delete_info=\"" << record.delete_info() << "\""
              << " payload_kind=metadata-only"
              << " payload_bytes=" << record.payload().size()
              << '\n';
  }

  int WriteStatusToExitCode(const raft::MetadataStatusCode code)
  {
    return code == raft::METADATA_STATUS_CODE_OK ||
                   code == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY
               ? 0
               : 1;
  }

  int ReadStatusToExitCode(const raft::MetadataStatusCode code)
  {
    return code == raft::METADATA_STATUS_CODE_OK ? 0 : 1;
  }

  raft::CreateMetadataRecordResponse DoCreate(const ParsedArgs &args,
                                              std::uint64_t chunk_count,
                                              const std::string &checksum,
                                              const std::vector<std::string> &mock_locations,
                                              grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::CreateMetadataRecordRequest request;
    request.set_request_id(args.request_id);
    request.set_object_key(args.object_key);
    request.mutable_manifest()->set_object_size(args.object_size);
    request.mutable_manifest()->set_chunk_size(args.chunk_size);
    request.mutable_manifest()->set_chunk_count(chunk_count);
    request.mutable_manifest()->set_checksum(checksum);
    for (const auto &location : mock_locations)
    {
      request.mutable_manifest()->add_mock_locations(location);
    }
    request.set_payload(args.payload);

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::CreateMetadataRecordResponse response;
    *rpc_status = stub->CreateMetadataRecord(&context, request, &response);
    return response;
  }

  raft::CommitMetadataRecordResponse DoCommit(const ParsedArgs &args,
                                              const std::string &address,
                                              grpc::Status *rpc_status)
  {
    auto stub = MakeStub(address);
    raft::CommitMetadataRecordRequest request;
    request.set_request_id(args.request_id);
    request.set_object_key(args.object_key);
    request.set_expected_create_request_id(args.expected_create_request_id);
    request.set_commit_info(args.commit_info);

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::CommitMetadataRecordResponse response;
    *rpc_status = stub->CommitMetadataRecord(&context, request, &response);
    return response;
  }

  raft::DeleteMetadataRecordResponse DoDelete(const ParsedArgs &args,
                                              const std::string &address,
                                              grpc::Status *rpc_status)
  {
    auto stub = MakeStub(address);
    raft::DeleteMetadataRecordRequest request;
    request.set_request_id(args.request_id);
    request.set_object_key(args.object_key);
    request.set_delete_info(args.delete_info);

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::DeleteMetadataRecordResponse response;
    *rpc_status = stub->DeleteMetadataRecord(&context, request, &response);
    return response;
  }

  raft::HeadMetadataRecordResponse DoHead(const ParsedArgs &args, grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::HeadMetadataRecordRequest request;
    request.set_object_key(args.object_key);

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::HeadMetadataRecordResponse response;
    *rpc_status = stub->HeadMetadataRecord(&context, request, &response);
    return response;
  }

  raft::ListMetadataRecordsResponse DoList(const ParsedArgs &args, grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::ListMetadataRecordsRequest request;
    request.set_prefix(args.prefix);
    if (args.limit.has_value())
    {
      request.set_limit(*args.limit);
    }
    request.set_page_token(args.page_token);

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::ListMetadataRecordsResponse response;
    *rpc_status = stub->ListMetadataRecords(&context, request, &response);
    return response;
  }

  int RunCreate(const ParsedArgs &args)
  {
    const std::uint64_t chunk_count =
        args.chunk_count.value_or(ComputeChunkCount(args.object_size, args.chunk_size));
    const std::string checksum =
        args.checksum.empty() ? MakeMockChecksum(args, chunk_count) : args.checksum;
    const std::vector<std::string> mock_locations = MakeMockLocations(args, chunk_count);

    grpc::Status rpc_status;
    const raft::CreateMetadataRecordResponse response =
        DoCreate(args, chunk_count, checksum, mock_locations, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("create", args, args.server_address, rpc_status);
      return 1;
    }

    const raft::MetadataResponseSummary &summary = response.summary();
    PrintSummary("create", args, args.server_address, summary);
    std::cout << "create_manifest"
              << " request_id=" << SummaryRequestId(args, summary)
              << " object_key=" << SummaryObjectKey(args, summary)
              << " object_size=" << args.object_size
              << " chunk_size=" << args.chunk_size
              << " chunk_count=" << chunk_count
              << " checksum=" << checksum
              << " mock_locations=" << JoinItems(mock_locations)
              << " payload_kind=metadata-only"
              << " payload_bytes=" << args.payload.size()
              << '\n';
    return WriteStatusToExitCode(summary.code());
  }

  int RunCommit(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const raft::CommitMetadataRecordResponse response =
        DoCommit(args, args.server_address, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("commit", args, args.server_address, rpc_status);
      return 1;
    }

    const raft::MetadataResponseSummary &summary = response.summary();
    PrintSummary("commit", args, args.server_address, summary);
    return WriteStatusToExitCode(summary.code());
  }

  int RunDelete(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const raft::DeleteMetadataRecordResponse response =
        DoDelete(args, args.server_address, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("delete", args, args.server_address, rpc_status);
      return 1;
    }

    const raft::MetadataResponseSummary &summary = response.summary();
    PrintSummary("delete", args, args.server_address, summary);
    return WriteStatusToExitCode(summary.code());
  }

  int RunHead(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const raft::HeadMetadataRecordResponse response = DoHead(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("head", args, args.server_address, rpc_status);
      return 1;
    }

    const raft::MetadataResponseSummary &summary = response.summary();
    PrintSummary("head", args, args.server_address, summary);
    std::cout << "head_result"
              << " object_key=" << SummaryObjectKey(args, summary)
              << " found=" << (response.found() ? "true" : "false")
              << '\n';
    if (response.found())
    {
      PrintRecordDetails("head_record", response.record());
    }
    return ReadStatusToExitCode(summary.code());
  }

  int RunList(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const raft::ListMetadataRecordsResponse response = DoList(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("list", args, args.server_address, rpc_status);
      return 1;
    }

    const raft::MetadataResponseSummary &summary = response.summary();
    PrintSummary("list", args, args.server_address, summary);
    std::cout << "list_result"
              << " prefix=" << args.prefix
              << " records_count=" << response.records_size()
              << " next_page_token=" << response.next_page_token()
              << '\n';
    for (int i = 0; i < response.records_size(); ++i)
    {
      PrintRecordDetails(("list_record[" + std::to_string(i) + "]").c_str(), response.records(i));
    }
    return ReadStatusToExitCode(summary.code());
  }

  int RunCommitRetry(const ParsedArgs &args)
  {
    std::string current_address = args.server_address;
    for (int attempt = 1; attempt <= args.max_retries + 1; ++attempt)
    {
      grpc::Status rpc_status;
      const raft::CommitMetadataRecordResponse response =
          DoCommit(args, current_address, &rpc_status);

      if (!rpc_status.ok())
      {
        PrintRpcFailure("commit-retry", args, current_address, rpc_status, attempt);
        return 1;
      }

      const raft::MetadataResponseSummary &summary = response.summary();
      PrintSummary("commit-retry", args, current_address, summary, attempt);
      if (!NeedsRetry(summary.code()) || attempt > args.max_retries)
      {
        return WriteStatusToExitCode(summary.code());
      }

      const std::string next_address = ChooseNextAddress(current_address, summary);
      PrintRetryDecision(attempt + 1, args.request_id, current_address, next_address, summary);
      current_address = next_address;
    }

    return 1;
  }

  int RunDeleteRetry(const ParsedArgs &args)
  {
    std::string current_address = args.server_address;
    for (int attempt = 1; attempt <= args.max_retries + 1; ++attempt)
    {
      grpc::Status rpc_status;
      const raft::DeleteMetadataRecordResponse response =
          DoDelete(args, current_address, &rpc_status);

      if (!rpc_status.ok())
      {
        PrintRpcFailure("delete-retry", args, current_address, rpc_status, attempt);
        return 1;
      }

      const raft::MetadataResponseSummary &summary = response.summary();
      PrintSummary("delete-retry", args, current_address, summary, attempt);
      if (!NeedsRetry(summary.code()) || attempt > args.max_retries)
      {
        return WriteStatusToExitCode(summary.code());
      }

      const std::string next_address = ChooseNextAddress(current_address, summary);
      PrintRetryDecision(attempt + 1, args.request_id, current_address, next_address, summary);
      current_address = next_address;
    }

    return 1;
  }

} // namespace

int main(int argc, char **argv)
{
  const ParsedArgs args = ParseArgs(argc, argv);
  if (args.command == "create")
  {
    return RunCreate(args);
  }
  if (args.command == "commit")
  {
    return RunCommit(args);
  }
  if (args.command == "delete")
  {
    return RunDelete(args);
  }
  if (args.command == "head")
  {
    return RunHead(args);
  }
  if (args.command == "list")
  {
    return RunList(args);
  }
  if (args.command == "commit-retry")
  {
    return RunCommitRetry(args);
  }
  return RunDeleteRetry(args);
}
