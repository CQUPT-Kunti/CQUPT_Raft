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
        << "  raft_metadata_client <addr> commit-retry"
        << " --request-id <id> --object-key <key>"
        << " [--expected-create-request-id <id>] [--commit-info <text>]"
        << " [--max-retries <n>] [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> delete-retry"
        << " --request-id <id> --object-key <key>"
        << " [--delete-info <text>]"
        << " [--max-retries <n>] [--timeout-ms <ms>]\n";
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
          std::cerr << "missing value for " << name << '\n';
          std::exit(2);
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
        std::cerr << "unknown argument: " << flag << '\n';
        PrintUsage();
        std::exit(2);
      }
    }

    if (args.command != "create" &&
        args.command != "commit-retry" &&
        args.command != "delete-retry")
    {
      std::cerr << "unsupported command: " << args.command << '\n';
      PrintUsage();
      std::exit(2);
    }

    if (args.request_id.empty() || args.object_key.empty())
    {
      std::cerr << "--request-id and --object-key are required\n";
      std::exit(2);
    }
    if (args.command == "create")
    {
      if (args.object_size == 0 || args.chunk_size == 0)
      {
        std::cerr << "--object-size and --chunk-size must be > 0 for create\n";
        std::exit(2);
      }
      if (args.chunk_count.has_value() && *args.chunk_count == 0)
      {
        std::cerr << "--chunk-count must be > 0 when specified\n";
        std::exit(2);
      }
    }

    if (args.max_retries < 0)
    {
      std::cerr << "--max-retries must be >= 0\n";
      std::exit(2);
    }

    if (args.timeout_ms <= 0)
    {
      std::cerr << "--timeout-ms must be > 0\n";
      std::exit(2);
    }

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

  void PrintSummary(const char *stage,
                    int attempt,
                    const std::string &target_address,
                    const raft::MetadataResponseSummary &summary)
  {
    std::cout << "stage=" << stage
              << " attempt=" << attempt
              << " target_address=" << target_address
              << " code=" << MetadataStatusCodeToString(summary.code())
              << " status=" << MetadataStatusCodeToString(summary.code())
              << " message=\"" << summary.message() << "\""
              << " request_id=" << summary.request_id()
              << " object_key=" << summary.object_key()
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

  raft::CommitMetadataRecordResponse DoCommitAttempt(const ParsedArgs &args,
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

  raft::DeleteMetadataRecordResponse DoDeleteAttempt(const ParsedArgs &args,
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
      std::cerr << "stage=create"
                << " target_address=" << args.server_address
                << " grpc_code=" << rpc_status.error_code()
                << " grpc_message=\"" << rpc_status.error_message() << "\""
                << " request_id=" << args.request_id
                << " object_key=" << args.object_key
                << '\n';
      return 1;
    }

    const raft::MetadataResponseSummary &summary = response.summary();
    std::cout << "stage=create"
              << " target_address=" << args.server_address
              << " code=" << MetadataStatusCodeToString(summary.code())
              << " status=" << MetadataStatusCodeToString(summary.code())
              << " message=\"" << summary.message() << "\""
              << " request_id=" << summary.request_id()
              << " object_key=" << summary.object_key()
              << " state=" << MetadataRecordStateToString(summary.state())
              << " object_size=" << args.object_size
              << " chunk_size=" << args.chunk_size
              << " chunk_count=" << chunk_count
              << " checksum=" << checksum
              << " mock_locations=" << JoinItems(mock_locations)
              << " payload_kind=metadata-only"
              << " payload_bytes=" << args.payload.size()
              << " leader_id=" << summary.leader_hint().leader_id()
              << " leader_address=" << summary.leader_hint().leader_address()
              << " term=" << summary.term()
              << " log_index=" << summary.log_index()
              << '\n';
    return summary.code() == raft::METADATA_STATUS_CODE_OK ||
                   summary.code() == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY
               ? 0
               : 1;
  }

  int RunCommitRetry(const ParsedArgs &args)
  {
    std::string current_address = args.server_address;
    for (int attempt = 1; attempt <= args.max_retries + 1; ++attempt)
    {
      grpc::Status rpc_status;
      raft::CommitMetadataRecordResponse response =
          DoCommitAttempt(args, current_address, &rpc_status);

      if (!rpc_status.ok())
      {
        std::cerr << "stage=commit-retry"
                  << " attempt=" << attempt
                  << " target_address=" << current_address
                  << " grpc_code=" << rpc_status.error_code()
                  << " grpc_message=\"" << rpc_status.error_message() << "\""
                  << " request_id=" << args.request_id
                  << " object_key=" << args.object_key
                  << '\n';
        return 1;
      }

      const raft::MetadataResponseSummary &summary = response.summary();
      PrintSummary("commit-retry", attempt, current_address, summary);
      if (!NeedsRetry(summary.code()) || attempt > args.max_retries)
      {
        return summary.code() == raft::METADATA_STATUS_CODE_OK ||
                       summary.code() == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY
                   ? 0
                   : 1;
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
      raft::DeleteMetadataRecordResponse response =
          DoDeleteAttempt(args, current_address, &rpc_status);

      if (!rpc_status.ok())
      {
        std::cerr << "stage=delete-retry"
                  << " attempt=" << attempt
                  << " target_address=" << current_address
                  << " grpc_code=" << rpc_status.error_code()
                  << " grpc_message=\"" << rpc_status.error_message() << "\""
                  << " request_id=" << args.request_id
                  << " object_key=" << args.object_key
                  << '\n';
        return 1;
      }

      const raft::MetadataResponseSummary &summary = response.summary();
      PrintSummary("delete-retry", attempt, current_address, summary);
      if (!NeedsRetry(summary.code()) || attempt > args.max_retries)
      {
        return summary.code() == raft::METADATA_STATUS_CODE_OK ||
                       summary.code() == raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY
                   ? 0
                   : 1;
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
  if (args.command == "commit-retry")
  {
    return RunCommitRetry(args);
  }
  return RunDeleteRetry(args);
}
