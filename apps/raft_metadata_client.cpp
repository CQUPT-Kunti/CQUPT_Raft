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
#include <string_view>
#include <utility>
#include <vector>

#include "metadata.grpc.pb.h"

namespace
{

  struct ParsedArgs
  {
    std::string server_address;
    std::string command;
    std::string request_id;
    std::string bucket;
    std::string object_key;
    std::string object_id;
    std::string prefix;
    std::string continuation_token;
    std::optional<std::uint32_t> limit;
    std::optional<std::uint64_t> client_time_ms;
    std::uint64_t version = 1;
    std::uint64_t size = 0;
    std::uint64_t chunk_size = 0;
    std::string etag;
    bool if_empty = true;
    int timeout_ms = 3000;
  };

  struct ClientRunResult
  {
    bool rpc_ok = false;
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

  const char *MetadataObjectStateToString(const raft::MetadataObjectState state)
  {
    switch (state)
    {
    case raft::METADATA_OBJECT_STATE_PENDING:
      return "PENDING";
    case raft::METADATA_OBJECT_STATE_COMMITTED:
      return "COMMITTED";
    case raft::METADATA_OBJECT_STATE_DELETED:
      return "DELETED";
    case raft::METADATA_OBJECT_STATE_UNSPECIFIED:
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
      const unsigned long long parsed = std::stoull(value, &pos, 10);
      if (pos != value.size())
      {
        throw std::invalid_argument("trailing characters");
      }
      return static_cast<std::uint64_t>(parsed);
    }
    catch (const std::exception &)
    {
      std::cerr << "invalid numeric value for " << name << ": " << value << '\n';
      std::exit(2);
    }
  }

  bool ParseBoolValue(const std::string &value, const char *name)
  {
    if (value == "true" || value == "1")
    {
      return true;
    }
    if (value == "false" || value == "0")
    {
      return false;
    }
    std::cerr << "invalid boolean value for " << name << ": " << value << '\n';
    std::exit(2);
  }

  std::uint64_t StableTimestamp(std::string_view request_id)
  {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : request_id)
    {
      hash ^= static_cast<std::uint64_t>(ch);
      hash *= 1099511628211ULL;
    }
    return 1700000000000ULL + (hash % 100000000000ULL);
  }

  std::uint64_t EffectiveClientTime(const ParsedArgs &args)
  {
    if (args.client_time_ms.has_value())
    {
      return *args.client_time_ms;
    }
    return StableTimestamp(args.request_id);
  }

  bool IsSupportedCommand(const std::string &command)
  {
    return command == "create-bucket" ||
           command == "delete-bucket" ||
           command == "create-object" ||
           command == "commit-object" ||
           command == "abort-object" ||
           command == "delete-object" ||
           command == "head-object" ||
           command == "list-objects" ||
           command == "verify-read-after-write";
  }

  void PrintUsage()
  {
    std::cerr
        << "Usage:\n"
        << "  raft_metadata_client <addr> create-bucket"
        << " --request-id <id> --bucket <bucket>"
        << " [--client-time-ms <ms>] [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> delete-bucket"
        << " --request-id <id> --bucket <bucket>"
        << " [--if-empty true|false] [--client-time-ms <ms>] [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> create-object"
        << " --request-id <id> --bucket <bucket> --object-key <key> --object-id <id>"
        << " --size <bytes> [--version <n>] [--etag <etag>]"
        << " [--client-time-ms <ms>] [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> commit-object"
        << " --request-id <id> --bucket <bucket> --object-key <key> --object-id <id>"
        << " --size <bytes> --chunk-size <bytes> [--version <n>] [--etag <etag>]"
        << " [--client-time-ms <ms>] [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> abort-object"
        << " --request-id <id> --bucket <bucket> --object-key <key> --object-id <id>"
        << " [--version <n>] [--client-time-ms <ms>] [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> delete-object"
        << " --request-id <id> --bucket <bucket> --object-key <key> --object-id <id>"
        << " [--version <n>] [--client-time-ms <ms>] [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> head-object"
        << " --bucket <bucket> --object-key <key>"
        << " [--object-id <id>] [--version <n>] [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> list-objects"
        << " --bucket <bucket> [--prefix <prefix>] [--limit <n>] [--continuation-token <token>]"
        << " [--timeout-ms <ms>]\n"
        << "  raft_metadata_client <addr> verify-read-after-write"
        << " --request-id <id> --bucket <bucket> --object-key <key> --object-id <id>"
        << " --size <bytes> --chunk-size <bytes> [--version <n>] [--etag <etag>]"
        << " [--timeout-ms <ms>]\n";
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

    if (args.command == "create-bucket" || args.command == "delete-bucket")
    {
      if (args.request_id.empty() || args.bucket.empty())
      {
        ExitUsageError(args.command + " requires --request-id and --bucket");
      }
      return;
    }

    if (args.command == "create-object" ||
        args.command == "commit-object" ||
        args.command == "abort-object" ||
        args.command == "delete-object" ||
        args.command == "verify-read-after-write")
    {
      if (args.request_id.empty() || args.bucket.empty() ||
          args.object_key.empty() || args.object_id.empty())
      {
        ExitUsageError(args.command + " requires --request-id --bucket --object-key --object-id");
      }
      if ((args.command == "create-object" || args.command == "commit-object" ||
           args.command == "verify-read-after-write") &&
          args.size == 0)
      {
        ExitUsageError(args.command + " requires --size > 0");
      }
      if ((args.command == "commit-object" || args.command == "verify-read-after-write") &&
          args.chunk_size == 0)
      {
        ExitUsageError(args.command + " requires --chunk-size > 0");
      }
      return;
    }

    if (args.command == "head-object")
    {
      if (args.bucket.empty() || args.object_key.empty())
      {
        ExitUsageError("head-object requires --bucket and --object-key");
      }
      return;
    }

    if (args.command == "list-objects")
    {
      if (args.bucket.empty())
      {
        ExitUsageError("list-objects requires --bucket");
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
      else if (flag == "--bucket")
      {
        args.bucket = require_value("--bucket");
      }
      else if (flag == "--object-key")
      {
        args.object_key = require_value("--object-key");
      }
      else if (flag == "--object-id")
      {
        args.object_id = require_value("--object-id");
      }
      else if (flag == "--prefix")
      {
        args.prefix = require_value("--prefix");
      }
      else if (flag == "--continuation-token")
      {
        args.continuation_token = require_value("--continuation-token");
      }
      else if (flag == "--limit")
      {
        args.limit = static_cast<std::uint32_t>(ParsePositiveInt(require_value("--limit"), "--limit"));
      }
      else if (flag == "--client-time-ms")
      {
        args.client_time_ms = ParsePositiveInt(require_value("--client-time-ms"), "--client-time-ms");
      }
      else if (flag == "--version")
      {
        args.version = ParsePositiveInt(require_value("--version"), "--version");
      }
      else if (flag == "--size")
      {
        args.size = ParsePositiveInt(require_value("--size"), "--size");
      }
      else if (flag == "--chunk-size")
      {
        args.chunk_size = ParsePositiveInt(require_value("--chunk-size"), "--chunk-size");
      }
      else if (flag == "--etag")
      {
        args.etag = require_value("--etag");
      }
      else if (flag == "--if-empty")
      {
        args.if_empty = ParseBoolValue(require_value("--if-empty"), "--if-empty");
      }
      else if (flag == "--timeout-ms")
      {
        args.timeout_ms = static_cast<int>(ParsePositiveInt(require_value("--timeout-ms"), "--timeout-ms"));
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

  std::uint64_t ComputeChunkCount(const std::uint64_t size, const std::uint64_t chunk_size)
  {
    return 1 + ((size - 1) / chunk_size);
  }

  std::string EffectiveEtag(const ParsedArgs &args)
  {
    if (!args.etag.empty())
    {
      return args.etag;
    }
    return "etag-" + args.object_id;
  }

  std::vector<raft::ChunkRef> BuildChunkRefs(const ParsedArgs &args)
  {
    std::vector<raft::ChunkRef> chunks;
    const std::uint64_t chunk_count = ComputeChunkCount(args.size, args.chunk_size);
    chunks.reserve(static_cast<std::size_t>(chunk_count));

    for (std::uint64_t i = 0; i < chunk_count; ++i)
    {
      const std::uint64_t offset = i * args.chunk_size;
      const std::uint64_t remaining = args.size - offset;
      raft::ChunkRef chunk;
      chunk.set_chunk_id(args.object_id + "-chunk-" + std::to_string(i));
      chunk.set_offset(offset);
      chunk.set_size(remaining < args.chunk_size ? remaining : args.chunk_size);
      chunk.add_replica_nodes("node-" + std::to_string((i % 3) + 1));
      chunk.add_replica_nodes("node-" + std::to_string(((i + 1) % 3) + 1));
      chunk.set_checksum("checksum-" + std::to_string(i));
      chunks.push_back(std::move(chunk));
    }

    return chunks;
  }

  void ConfigureContext(grpc::ClientContext *context, const int timeout_ms)
  {
    context->set_deadline(std::chrono::system_clock::now() +
                          std::chrono::milliseconds(timeout_ms));
  }

  void PrintSummary(const char *stage,
                    const std::string &target_address,
                    const raft::MetadataResponseSummary &summary)
  {
    std::cout << "stage=" << stage
              << " target_address=" << target_address
              << " code=" << MetadataStatusCodeToString(summary.code())
              << " status=" << MetadataStatusCodeToString(summary.code())
              << " message=\"" << summary.message() << "\""
              << " request_id=" << summary.request_id()
              << " bucket=" << summary.bucket()
              << " object_key=" << summary.object_key()
              << " object_id=" << summary.object_id()
              << " state=" << MetadataObjectStateToString(summary.state())
              << " leader_id=" << summary.leader_hint().leader_id()
              << " leader_address=" << summary.leader_hint().leader_address()
              << " term=" << summary.term()
              << " log_index=" << summary.log_index()
              << '\n';
  }

  void PrintRpcFailure(const char *stage,
                       const std::string &target_address,
                       const ParsedArgs &args,
                       const grpc::Status &rpc_status)
  {
    std::cerr << "stage=" << stage
              << " target_address=" << target_address
              << " grpc_code=" << rpc_status.error_code()
              << " grpc_message=\"" << rpc_status.error_message() << "\""
              << " request_id=" << args.request_id
              << " bucket=" << args.bucket
              << " object_key=" << args.object_key
              << '\n';
  }

  void PrintBucketRecord(const char *prefix, const raft::BucketRecord &record)
  {
    std::cout << prefix
              << " bucket=" << record.bucket()
              << " create_time=" << record.create_time()
              << " deleted=" << (record.deleted() ? "true" : "false")
              << " delete_time=" << record.delete_time()
              << '\n';
  }

  void PrintObjectRecord(const char *prefix, const raft::ObjectRecord &record)
  {
    std::cout << prefix
              << " bucket=" << record.bucket()
              << " object_key=" << record.object_key()
              << " object_id=" << record.object_id()
              << " version=" << record.version()
              << " size=" << record.size()
              << " etag=" << record.etag()
              << " state=" << MetadataObjectStateToString(record.state())
              << " create_time=" << record.create_time()
              << " commit_time=" << record.commit_time()
              << " delete_time=" << record.delete_time()
              << " chunks=" << record.chunks_size()
              << '\n';
    for (int i = 0; i < record.chunks_size(); ++i)
    {
      const auto &chunk = record.chunks(i);
      std::cout << prefix << ".chunk[" << i << "]"
                << " chunk_id=" << chunk.chunk_id()
                << " offset=" << chunk.offset()
                << " size=" << chunk.size()
                << " replicas=" << chunk.replica_nodes_size()
                << " checksum=" << chunk.checksum()
                << '\n';
    }
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

  bool ListContainsObject(const raft::ListObjectsResponse &response,
                          const std::string &object_key)
  {
    for (const auto &object : response.objects())
    {
      if (object.object_key() == object_key)
      {
        return true;
      }
    }
    return false;
  }

  raft::CreateBucketResponse DoCreateBucket(const ParsedArgs &args,
                                            grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::CreateBucketRequest request;
    request.set_request_id(args.request_id);
    request.set_bucket(args.bucket);
    request.set_client_time_unix_ms(EffectiveClientTime(args));

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::CreateBucketResponse response;
    *rpc_status = stub->CreateBucket(&context, request, &response);
    return response;
  }

  raft::DeleteBucketResponse DoDeleteBucket(const ParsedArgs &args,
                                            grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::DeleteBucketRequest request;
    request.set_request_id(args.request_id);
    request.set_bucket(args.bucket);
    request.set_if_empty(args.if_empty);
    request.set_client_time_unix_ms(EffectiveClientTime(args));

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::DeleteBucketResponse response;
    *rpc_status = stub->DeleteBucket(&context, request, &response);
    return response;
  }

  raft::CreateObjectResponse DoCreateObject(const ParsedArgs &args,
                                            grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::CreateObjectRequest request;
    request.set_request_id(args.request_id);
    request.set_bucket(args.bucket);
    request.set_object_key(args.object_key);
    request.set_object_id(args.object_id);
    request.set_version(args.version);
    request.set_size(args.size);
    request.set_etag(EffectiveEtag(args));
    request.set_client_time_unix_ms(EffectiveClientTime(args));

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::CreateObjectResponse response;
    *rpc_status = stub->CreateObject(&context, request, &response);
    return response;
  }

  raft::CommitObjectResponse DoCommitObject(const ParsedArgs &args,
                                            grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::CommitObjectRequest request;
    request.set_request_id(args.request_id);
    request.set_bucket(args.bucket);
    request.set_object_key(args.object_key);
    request.set_object_id(args.object_id);
    request.set_version(args.version);
    request.set_size(args.size);
    request.set_etag(EffectiveEtag(args));
    request.set_client_time_unix_ms(EffectiveClientTime(args));
    for (auto &chunk : BuildChunkRefs(args))
    {
      request.add_chunks()->Swap(&chunk);
    }

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::CommitObjectResponse response;
    *rpc_status = stub->CommitObject(&context, request, &response);
    return response;
  }

  raft::AbortObjectResponse DoAbortObject(const ParsedArgs &args,
                                          grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::AbortObjectRequest request;
    request.set_request_id(args.request_id);
    request.set_bucket(args.bucket);
    request.set_object_key(args.object_key);
    request.set_object_id(args.object_id);
    request.set_version(args.version);
    request.set_client_time_unix_ms(EffectiveClientTime(args));

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::AbortObjectResponse response;
    *rpc_status = stub->AbortObject(&context, request, &response);
    return response;
  }

  raft::DeleteObjectResponse DoDeleteObject(const ParsedArgs &args,
                                            grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::DeleteObjectRequest request;
    request.set_request_id(args.request_id);
    request.set_bucket(args.bucket);
    request.set_object_key(args.object_key);
    request.set_object_id(args.object_id);
    request.set_version(args.version);
    request.set_client_time_unix_ms(EffectiveClientTime(args));

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::DeleteObjectResponse response;
    *rpc_status = stub->DeleteObject(&context, request, &response);
    return response;
  }

  raft::HeadObjectResponse DoHeadObject(const ParsedArgs &args,
                                        grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::HeadObjectRequest request;
    request.set_bucket(args.bucket);
    request.set_object_key(args.object_key);
    if (!args.object_id.empty())
    {
      request.set_object_id(args.object_id);
    }
    request.set_version(args.version);

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::HeadObjectResponse response;
    *rpc_status = stub->HeadObject(&context, request, &response);
    return response;
  }

  raft::ListObjectsResponse DoListObjects(const ParsedArgs &args,
                                          grpc::Status *rpc_status)
  {
    auto stub = MakeStub(args.server_address);
    raft::ListObjectsRequest request;
    request.set_bucket(args.bucket);
    request.set_prefix(args.prefix);
    if (args.limit.has_value())
    {
      request.set_limit(*args.limit);
    }
    request.set_continuation_token(args.continuation_token);

    grpc::ClientContext context;
    ConfigureContext(&context, args.timeout_ms);

    raft::ListObjectsResponse response;
    *rpc_status = stub->ListObjects(&context, request, &response);
    return response;
  }

  void PrintVerificationCheck(const char *step,
                              const std::string &expected,
                              const std::string &actual,
                              const raft::MetadataResponseSummary &summary,
                              const bool pass)
  {
    std::cout << "verification_check"
              << " step=" << step
              << " result=" << (pass ? "PASS" : "FAIL")
              << " request_id=" << summary.request_id()
              << " bucket=" << summary.bucket()
              << " object_key=" << summary.object_key()
              << " object_id=" << summary.object_id()
              << " status=" << MetadataStatusCodeToString(summary.code())
              << " message=\"" << summary.message() << "\""
              << " expected=\"" << expected << "\""
              << " actual=\"" << actual << "\""
              << '\n';
  }

  int RunCreateBucket(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const auto response = DoCreateBucket(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("create-bucket", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("create-bucket", args.server_address, response.summary());
    PrintBucketRecord("bucket_record", response.bucket_record());
    return WriteStatusToExitCode(response.summary().code());
  }

  int RunDeleteBucket(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const auto response = DoDeleteBucket(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("delete-bucket", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("delete-bucket", args.server_address, response.summary());
    PrintBucketRecord("bucket_record", response.bucket_record());
    return WriteStatusToExitCode(response.summary().code());
  }

  int RunCreateObject(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const auto response = DoCreateObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("create-object", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("create-object", args.server_address, response.summary());
    PrintObjectRecord("object_record", response.object());
    return WriteStatusToExitCode(response.summary().code());
  }

  int RunCommitObject(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const auto response = DoCommitObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("commit-object", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("commit-object", args.server_address, response.summary());
    PrintObjectRecord("object_record", response.object());
    return WriteStatusToExitCode(response.summary().code());
  }

  int RunAbortObject(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const auto response = DoAbortObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("abort-object", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("abort-object", args.server_address, response.summary());
    PrintObjectRecord("object_record", response.object());
    return WriteStatusToExitCode(response.summary().code());
  }

  int RunDeleteObject(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const auto response = DoDeleteObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("delete-object", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("delete-object", args.server_address, response.summary());
    PrintObjectRecord("object_record", response.object());
    return WriteStatusToExitCode(response.summary().code());
  }

  int RunHeadObject(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const auto response = DoHeadObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("head-object", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("head-object", args.server_address, response.summary());
    std::cout << "head_result"
              << " bucket=" << args.bucket
              << " object_key=" << args.object_key
              << " found=" << (response.found() ? "true" : "false")
              << '\n';
    if (response.found())
    {
      PrintObjectRecord("head_object", response.object());
    }
    return ReadStatusToExitCode(response.summary().code());
  }

  int RunListObjects(const ParsedArgs &args)
  {
    grpc::Status rpc_status;
    const auto response = DoListObjects(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("list-objects", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("list-objects", args.server_address, response.summary());
    std::cout << "list_result"
              << " bucket=" << args.bucket
              << " prefix=" << args.prefix
              << " objects_count=" << response.objects_size()
              << " next_continuation_token=" << response.next_continuation_token()
              << '\n';
    for (int i = 0; i < response.objects_size(); ++i)
    {
      PrintObjectRecord(("list_object[" + std::to_string(i) + "]").c_str(), response.objects(i));
    }
    return ReadStatusToExitCode(response.summary().code());
  }

  int RunVerifyReadAfterWrite(const ParsedArgs &args)
  {
    ParsedArgs create_bucket_args = args;
    create_bucket_args.command = "create-bucket";
    create_bucket_args.request_id = args.request_id + "-bucket";
    create_bucket_args.object_key.clear();
    create_bucket_args.object_id.clear();

    grpc::Status rpc_status;
    const auto bucket_response = DoCreateBucket(create_bucket_args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("verify-create-bucket", args.server_address, create_bucket_args, rpc_status);
      return 1;
    }
    PrintSummary("verify-create-bucket", args.server_address, bucket_response.summary());
    if (WriteStatusToExitCode(bucket_response.summary().code()) != 0)
    {
      return 1;
    }

    const auto create_response = DoCreateObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("verify-create-object", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("verify-create-object", args.server_address, create_response.summary());
    if (WriteStatusToExitCode(create_response.summary().code()) != 0)
    {
      return 1;
    }

    const auto create_head = DoHeadObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("verify-head-after-create", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("verify-head-after-create", args.server_address, create_head.summary());
    const bool create_head_hidden =
        create_head.summary().code() == raft::METADATA_STATUS_CODE_NOT_FOUND && !create_head.found();
    PrintVerificationCheck("create-head-invisible",
                           "NOT_FOUND",
                           MetadataStatusCodeToString(create_head.summary().code()),
                           create_head.summary(),
                           create_head_hidden);

    const auto commit_response = DoCommitObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("verify-commit-object", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("verify-commit-object", args.server_address, commit_response.summary());
    if (WriteStatusToExitCode(commit_response.summary().code()) != 0)
    {
      return 1;
    }

    const auto commit_head = DoHeadObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("verify-head-after-commit", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("verify-head-after-commit", args.server_address, commit_head.summary());
    const bool commit_head_visible =
        commit_head.summary().code() == raft::METADATA_STATUS_CODE_OK &&
        commit_head.found() &&
        commit_head.object().state() == raft::METADATA_OBJECT_STATE_COMMITTED;
    PrintVerificationCheck("commit-head-visible",
                           "COMMITTED",
                           commit_head.found()
                               ? MetadataObjectStateToString(commit_head.object().state())
                               : "NOT_FOUND",
                           commit_head.summary(),
                           commit_head_visible);

    ParsedArgs list_args = args;
    list_args.command = "list-objects";
    list_args.prefix = args.object_key;
    const auto commit_list = DoListObjects(list_args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("verify-list-after-commit", args.server_address, list_args, rpc_status);
      return 1;
    }
    PrintSummary("verify-list-after-commit", args.server_address, commit_list.summary());
    const bool commit_list_visible =
        commit_list.summary().code() == raft::METADATA_STATUS_CODE_OK &&
        ListContainsObject(commit_list, args.object_key);
    PrintVerificationCheck("commit-list-visible",
                           "present",
                           commit_list_visible ? "present" : "missing",
                           commit_list.summary(),
                           commit_list_visible);

    const auto delete_response = DoDeleteObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("verify-delete-object", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("verify-delete-object", args.server_address, delete_response.summary());
    if (WriteStatusToExitCode(delete_response.summary().code()) != 0)
    {
      return 1;
    }

    const auto delete_head = DoHeadObject(args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("verify-head-after-delete", args.server_address, args, rpc_status);
      return 1;
    }
    PrintSummary("verify-head-after-delete", args.server_address, delete_head.summary());
    const bool delete_head_hidden =
        delete_head.summary().code() == raft::METADATA_STATUS_CODE_NOT_FOUND && !delete_head.found();
    PrintVerificationCheck("delete-head-invisible",
                           "NOT_FOUND",
                           MetadataStatusCodeToString(delete_head.summary().code()),
                           delete_head.summary(),
                           delete_head_hidden);

    const auto delete_list = DoListObjects(list_args, &rpc_status);
    if (!rpc_status.ok())
    {
      PrintRpcFailure("verify-list-after-delete", args.server_address, list_args, rpc_status);
      return 1;
    }
    PrintSummary("verify-list-after-delete", args.server_address, delete_list.summary());
    const bool delete_list_hidden =
        delete_list.summary().code() == raft::METADATA_STATUS_CODE_OK &&
        !ListContainsObject(delete_list, args.object_key);
    PrintVerificationCheck("delete-list-invisible",
                           "missing",
                           delete_list_hidden ? "missing" : "present",
                           delete_list.summary(),
                           delete_list_hidden);

    const bool pass =
        create_head_hidden && commit_head_visible && commit_list_visible &&
        delete_head_hidden && delete_list_hidden;
    std::cout << "verification_result"
              << " mode=read-after-write"
              << " result=" << (pass ? "PASS" : "FAIL")
              << " bucket=" << args.bucket
              << " object_key=" << args.object_key
              << " object_id=" << args.object_id
              << '\n';
    return pass ? 0 : 1;
  }

} // namespace

int main(int argc, char **argv)
{
  const ParsedArgs args = ParseArgs(argc, argv);

  if (args.command == "create-bucket")
  {
    return RunCreateBucket(args);
  }
  if (args.command == "delete-bucket")
  {
    return RunDeleteBucket(args);
  }
  if (args.command == "create-object")
  {
    return RunCreateObject(args);
  }
  if (args.command == "commit-object")
  {
    return RunCommitObject(args);
  }
  if (args.command == "abort-object")
  {
    return RunAbortObject(args);
  }
  if (args.command == "delete-object")
  {
    return RunDeleteObject(args);
  }
  if (args.command == "head-object")
  {
    return RunHeadObject(args);
  }
  if (args.command == "list-objects")
  {
    return RunListObjects(args);
  }
  if (args.command == "verify-read-after-write")
  {
    return RunVerifyReadAfterWrite(args);
  }

  PrintUsage();
  return 2;
}
