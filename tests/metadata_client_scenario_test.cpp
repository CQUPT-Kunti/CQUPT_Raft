#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <google/protobuf/descriptor.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef RAFT_METADATA_CLIENT_PATH
#error "RAFT_METADATA_CLIENT_PATH must be defined"
#endif

#ifndef METADATA_NODE_APP_PATH
#error "METADATA_NODE_APP_PATH must be defined"
#endif

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "metadata.grpc.pb.h"
#include "view.grpc.pb.h"

#ifdef _WIN32
int raft_metadata_client_entry(int argc, char **argv);
#endif

namespace
{
  std::string ClientBinaryPath()
  {
    return RAFT_METADATA_CLIENT_PATH;
  }

  std::string MetadataNodeAppBinaryPath()
  {
    return METADATA_NODE_APP_PATH;
  }

  std::string QuoteArg(const std::string &value)
  {
    std::string quoted = "\"";
    for (const char ch : value)
    {
      if (ch == '"' || ch == '\\')
      {
        quoted.push_back('\\');
      }
      quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
  }

  struct ClientRunResult
  {
    int exit_code = -1;
    std::string output;
  };

  int NormalizeSystemExitCode(const int raw_code)
  {
#ifdef _WIN32
    return raw_code;
#else
    if (raw_code >= 0 && WIFEXITED(raw_code))
    {
      return WEXITSTATUS(raw_code);
    }
    return raw_code;
#endif
  }

#ifdef _WIN32
  ClientRunResult RunClientWindows(const std::vector<std::string> &args,
                                   const std::filesystem::path &output_path)
  {
    std::vector<std::string> argv_storage;
    argv_storage.reserve(args.size() + 1);
    argv_storage.push_back(ClientBinaryPath());
    argv_storage.insert(argv_storage.end(), args.begin(), args.end());

    std::vector<char *> argv;
    argv.reserve(argv_storage.size());
    for (auto &value : argv_storage)
    {
      argv.push_back(value.data());
    }

    testing::internal::CaptureStdout();
    testing::internal::CaptureStderr();

    int exit_code = -1;
    try
    {
      exit_code = raft_metadata_client_entry(
          static_cast<int>(argv.size()), argv.data());
    }
    catch (const std::exception &ex)
    {
      std::cerr << "raft_metadata_client_entry threw exception: " << ex.what() << '\n';
      exit_code = -1;
    }
    catch (...)
    {
      std::cerr << "raft_metadata_client_entry threw unknown exception\n";
      exit_code = -1;
    }

    std::string output = testing::internal::GetCapturedStdout();
    output += testing::internal::GetCapturedStderr();
    std::ofstream(output_path, std::ios::binary) << output;
    return {exit_code, std::move(output)};
  }
#endif

  ClientRunResult RunClient(const std::vector<std::string> &args,
                            const std::string &test_name)
  {
    const auto output_dir =
        std::filesystem::current_path() / "metadata_client_scenario_outputs";
    std::filesystem::create_directories(output_dir);

    const auto output_path =
        output_dir / (test_name + "_" +
                      std::to_string(static_cast<std::uint64_t>(
                          std::chrono::steady_clock::now().time_since_epoch().count())) +
                      ".log");

#ifdef _WIN32
    return RunClientWindows(args, output_path);
#else
    std::ostringstream command;
    command << QuoteArg(ClientBinaryPath());
    for (const auto &arg : args)
    {
      command << ' ' << QuoteArg(arg);
    }
    command << " > " << QuoteArg(output_path.string()) << " 2>&1";

    const int raw_exit = std::system(command.str().c_str());
    std::ifstream input(output_path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return {NormalizeSystemExitCode(raw_exit), buffer.str()};
#endif
  }

  ClientRunResult RunExternalBinary(const std::string &binary_path,
                                    const std::vector<std::string> &args,
                                    const std::string &output_prefix,
                                    const std::string &test_name)
  {
    const auto output_dir =
        std::filesystem::current_path() / output_prefix;
    std::filesystem::create_directories(output_dir);

    const auto output_path =
        output_dir / (test_name + "_" +
                      std::to_string(static_cast<std::uint64_t>(
                          std::chrono::steady_clock::now().time_since_epoch().count())) +
                      ".log");

    std::ostringstream command;
    command << QuoteArg(binary_path);
    for (const auto &arg : args)
    {
      command << ' ' << QuoteArg(arg);
    }
    command << " > " << QuoteArg(output_path.string()) << " 2>&1";

    const int raw_exit = std::system(command.str().c_str());
    std::ifstream input(output_path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return {NormalizeSystemExitCode(raw_exit), buffer.str()};
  }

  ClientRunResult RunMetadataNodeApp(const std::vector<std::string> &args,
                                     const std::string &test_name)
  {
    return RunExternalBinary(MetadataNodeAppBinaryPath(),
                             args,
                             "metadata_node_app_scenario_outputs",
                             test_name);
  }

  bool Contains(const std::string &text, const std::string &needle)
  {
    return text.find(needle) != std::string::npos;
  }

  std::string JsonStringLiteral(const std::string &value)
  {
    std::string escaped = "\"";
    for (const char ch : value)
    {
      switch (ch)
      {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(ch);
        break;
      }
    }
    escaped.push_back('"');
    return escaped;
  }

  std::filesystem::path MakeScenarioDirectory(const std::string &test_name)
  {
    const auto dir =
        std::filesystem::current_path() / "metadata_node_app_scenarios" /
        (test_name + "_" +
         std::to_string(static_cast<std::uint64_t>(
             std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);
    return dir;
  }

  std::filesystem::path WriteDynamicJoinClusterConfig(
      const std::filesystem::path &root,
      const std::string &cluster_id,
      const std::string &view_endpoint,
      const std::string &meta1_endpoint,
      const std::string &meta2_endpoint,
      const std::string &meta3_endpoint,
      const std::string &candidate_endpoint)
  {
    std::filesystem::create_directories(root / "nodes");

    const auto config_path = root / "cluster.json";
    std::ofstream out(config_path);
    out << "{\n"
        << "  \"cluster_id\": " << JsonStringLiteral(cluster_id) << ",\n"
        << "  \"base_dir\": " << JsonStringLiteral(root.string()) << ",\n"
        << "  \"view_nodes\": [\n"
        << "    {\n"
        << "      \"node_id\": \"view-join-1\",\n"
        << "      \"endpoint\": " << JsonStringLiteral(view_endpoint) << ",\n"
        << "      \"data_dir\": " << JsonStringLiteral((root / "nodes/view-join-1/data").string()) << "\n"
        << "    }\n"
        << "  ],\n"
        << "  \"metadata_nodes\": [\n"
        << "    {\n"
        << "      \"node_id\": \"meta-1\",\n"
        << "      \"raft_id\": 1,\n"
        << "      \"endpoint\": " << JsonStringLiteral(meta1_endpoint) << ",\n"
        << "      \"data_dir\": " << JsonStringLiteral((root / "nodes/meta-1/data").string()) << ",\n"
        << "      \"snapshot_dir\": " << JsonStringLiteral((root / "nodes/meta-1/snapshots").string()) << ",\n"
        << "      \"initial_role\": \"voter\"\n"
        << "    },\n"
        << "    {\n"
        << "      \"node_id\": \"meta-2\",\n"
        << "      \"raft_id\": 2,\n"
        << "      \"endpoint\": " << JsonStringLiteral(meta2_endpoint) << ",\n"
        << "      \"data_dir\": " << JsonStringLiteral((root / "nodes/meta-2/data").string()) << ",\n"
        << "      \"snapshot_dir\": " << JsonStringLiteral((root / "nodes/meta-2/snapshots").string()) << ",\n"
        << "      \"initial_role\": \"voter\"\n"
        << "    },\n"
        << "    {\n"
        << "      \"node_id\": \"meta-3\",\n"
        << "      \"raft_id\": 3,\n"
        << "      \"endpoint\": " << JsonStringLiteral(meta3_endpoint) << ",\n"
        << "      \"data_dir\": " << JsonStringLiteral((root / "nodes/meta-3/data").string()) << ",\n"
        << "      \"snapshot_dir\": " << JsonStringLiteral((root / "nodes/meta-3/snapshots").string()) << ",\n"
        << "      \"initial_role\": \"voter\"\n"
        << "    },\n"
        << "    {\n"
        << "      \"node_id\": \"meta-candidate-1\",\n"
        << "      \"raft_id\": 11,\n"
        << "      \"endpoint\": " << JsonStringLiteral(candidate_endpoint) << ",\n"
        << "      \"data_dir\": " << JsonStringLiteral((root / "nodes/meta-candidate-1/data").string()) << ",\n"
        << "      \"snapshot_dir\": " << JsonStringLiteral((root / "nodes/meta-candidate-1/snapshots").string()) << ",\n"
        << "      \"initial_role\": \"candidate\"\n"
        << "    }\n"
        << "  ],\n"
        << "  \"storage_nodes\": [\n"
        << "    {\n"
        << "      \"node_id\": \"store-dummy-1\",\n"
        << "      \"endpoint\": \"127.0.0.1:7999\",\n"
        << "      \"data_dir\": " << JsonStringLiteral((root / "nodes/store-dummy-1/data").string()) << ",\n"
        << "      \"capacity_bytes\": 1048576,\n"
        << "      \"failure_domain\": {\n"
        << "        \"zone\": \"zone-a\",\n"
        << "        \"rack\": \"rack-a1\"\n"
        << "      }\n"
        << "    }\n"
        << "  ],\n"
        << "  \"initial_raft_membership\": {\n"
        << "    \"membership_epoch\": 1,\n"
        << "    \"voter_raft_ids\": [1, 2, 3],\n"
        << "    \"learner_raft_ids\": []\n"
        << "  },\n"
        << "  \"chunk_policy\": {\n"
        << "    \"chunk_size_bytes\": 1024,\n"
        << "    \"replica_count\": 1,\n"
        << "    \"minimum_successful_writes\": 1,\n"
        << "    \"checksum_algorithm\": \"sha256\"\n"
        << "  },\n"
        << "  \"timeouts\": {\n"
        << "    \"discovery_rpc_timeout_ms\": 500,\n"
        << "    \"metadata_rpc_timeout_ms\": 500,\n"
        << "    \"storage_rpc_timeout_ms\": 500,\n"
        << "    \"heartbeat_interval_ms\": 200,\n"
        << "    \"registration_timeout_ms\": 500,\n"
        << "    \"commit_deadline_ms\": 500,\n"
        << "    \"liveness_stale_timeout_ms\": 2000,\n"
        << "    \"liveness_dead_timeout_ms\": 5000\n"
        << "  }\n"
        << "}\n";
    return config_path;
  }

  std::string ObjectIdentity(const std::string &bucket, const std::string &object_key)
  {
    return bucket + "\n" + object_key;
  }

  bool MetadataServiceHasMethod(const std::string &method_name)
  {
    const auto *file = raft::HeadObjectRequest::descriptor()->file();
    if (file == nullptr)
    {
      return false;
    }

    const auto *service = file->FindServiceByName("MetadataService");
    if (service == nullptr)
    {
      return false;
    }

    return service->FindMethodByName(method_name) != nullptr;
  }

  bool MessageHasField(const google::protobuf::Descriptor *descriptor,
                       const std::string &field_name)
  {
    return descriptor != nullptr &&
           descriptor->FindFieldByName(field_name) != nullptr;
  }

  template <typename Response>
  struct ReplayEntry
  {
    std::string fingerprint;
    Response response;
  };

  class FakeMetadataService final : public raft::MetadataService::Service
  {
  public:
    explicit FakeMetadataService(std::string leader_address)
        : leader_address_(std::move(leader_address))
    {
    }

    void SetLeaderAddress(std::string leader_address)
    {
      std::lock_guard<std::mutex> lock(mu_);
      leader_address_ = std::move(leader_address);
    }

    void ForceWriteResponse(const std::string &request_id,
                            const raft::MetadataStatusCode code,
                            std::string message)
    {
      std::lock_guard<std::mutex> lock(mu_);
      forced_write_responses_[request_id] =
          ForcedWriteResponse{code, std::move(message)};
    }

    void ForceHeadResponse(const std::string &bucket,
                           const std::string &object_key,
                           const raft::MetadataStatusCode code,
                           std::string message)
    {
      std::lock_guard<std::mutex> lock(mu_);
      forced_head_responses_[ObjectIdentity(bucket, object_key)] =
          ForcedReadResponse{code, std::move(message)};
    }

    void ForceListResponse(const std::string &bucket,
                           const std::string &prefix,
                           const raft::MetadataStatusCode code,
                           std::string message)
    {
      std::lock_guard<std::mutex> lock(mu_);
      forced_list_responses_[ObjectIdentity(bucket, prefix)] =
          ForcedReadResponse{code, std::move(message)};
    }

    struct Snapshot
    {
      bool bucket_exists = false;
      bool bucket_deleted = false;
      std::size_t object_count = 0;
      std::optional<raft::ObjectRecord> object;
      std::size_t create_bucket_calls = 0;
      std::size_t create_object_calls = 0;
      std::size_t commit_object_calls = 0;
      std::size_t delete_object_calls = 0;
    };

    Snapshot TakeSnapshot(const std::string &bucket,
                          const std::string &object_key) const
    {
      std::lock_guard<std::mutex> lock(mu_);
      Snapshot snapshot;
      snapshot.create_bucket_calls = create_bucket_calls_;
      snapshot.create_object_calls = create_object_calls_;
      snapshot.commit_object_calls = commit_object_calls_;
      snapshot.delete_object_calls = delete_object_calls_;
      snapshot.object_count = objects_.size();

      const auto bucket_it = buckets_.find(bucket);
      if (bucket_it != buckets_.end())
      {
        snapshot.bucket_exists = true;
        snapshot.bucket_deleted = bucket_it->second.deleted();
      }

      const auto object_it = objects_.find(ObjectIdentity(bucket, object_key));
      if (object_it != objects_.end())
      {
        snapshot.object = object_it->second;
      }

      return snapshot;
    }

    grpc::Status CreateBucket(grpc::ServerContext *,
                              const raft::CreateBucketRequest *request,
                              raft::CreateBucketResponse *response) override
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++create_bucket_calls_;

      if (MaybeServeForcedWriteResponse(
              request->request_id(), request->bucket(), "", "", response))
      {
        return grpc::Status::OK;
      }

      const std::string fingerprint = CreateBucketFingerprint(*request);
      auto replay = create_bucket_replays_.find(request->request_id());
      if (replay != create_bucket_replays_.end())
      {
        if (replay->second.fingerprint != fingerprint)
        {
          response->mutable_summary()->CopyFrom(
              MakeSummary(raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT,
                          "create bucket request_id conflict",
                          request->request_id(),
                          request->bucket(),
                          "",
                          "",
                          raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                          CurrentLogIndex()));
          return grpc::Status::OK;
        }
        *response = replay->second.response;
        response->mutable_summary()->set_code(raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY);
        response->mutable_summary()->set_message("create bucket replayed");
        return grpc::Status::OK;
      }

      if (request->request_id().empty() || request->bucket().empty())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_INVALID_ARGUMENT,
                        "request_id and bucket are required",
                        request->request_id(),
                        request->bucket(),
                        "",
                        "",
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }

      auto bucket_it = buckets_.find(request->bucket());
      if (bucket_it != buckets_.end() && !bucket_it->second.deleted())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_STATE_CONFLICT,
                        "bucket already exists",
                        request->request_id(),
                        request->bucket(),
                        "",
                        "",
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }

      raft::BucketRecord bucket_record;
      bucket_record.set_bucket(request->bucket());
      bucket_record.set_create_time(request->client_time_unix_ms());
      bucket_record.set_deleted(false);
      bucket_record.set_delete_time(0);
      buckets_[request->bucket()] = bucket_record;

      response->mutable_summary()->CopyFrom(
          MakeSummary(raft::METADATA_STATUS_CODE_OK,
                      "ok",
                      request->request_id(),
                      request->bucket(),
                      "",
                      "",
                      raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                      NextLogIndex()));
      response->mutable_bucket_record()->CopyFrom(bucket_record);
      create_bucket_replays_[request->request_id()] = {fingerprint, *response};
      return grpc::Status::OK;
    }

    grpc::Status DeleteBucket(grpc::ServerContext *,
                              const raft::DeleteBucketRequest *request,
                              raft::DeleteBucketResponse *response) override
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (MaybeServeForcedWriteResponse(
              request->request_id(), request->bucket(), "", "", response))
      {
        return grpc::Status::OK;
      }

      auto bucket_it = buckets_.find(request->bucket());
      if (bucket_it == buckets_.end())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                        "bucket not found",
                        request->request_id(),
                        request->bucket(),
                        "",
                        "",
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }
      bucket_it->second.set_deleted(true);
      bucket_it->second.set_delete_time(request->client_time_unix_ms());
      response->mutable_summary()->CopyFrom(
          MakeSummary(raft::METADATA_STATUS_CODE_OK,
                      "ok",
                      request->request_id(),
                      request->bucket(),
                      "",
                      "",
                      raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                      NextLogIndex()));
      response->mutable_bucket_record()->CopyFrom(bucket_it->second);
      return grpc::Status::OK;
    }

    grpc::Status CreateObject(grpc::ServerContext *,
                              const raft::CreateObjectRequest *request,
                              raft::CreateObjectResponse *response) override
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++create_object_calls_;

      if (MaybeServeForcedWriteResponse(request->request_id(),
                                        request->bucket(),
                                        request->object_key(),
                                        request->object_id(),
                                        response))
      {
        return grpc::Status::OK;
      }

      const std::string fingerprint = CreateObjectFingerprint(*request);
      auto replay = create_object_replays_.find(request->request_id());
      if (replay != create_object_replays_.end())
      {
        if (replay->second.fingerprint != fingerprint)
        {
          response->mutable_summary()->CopyFrom(
              MakeSummary(raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT,
                          "create object request_id conflict",
                          request->request_id(),
                          request->bucket(),
                          request->object_key(),
                          request->object_id(),
                          raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                          CurrentLogIndex()));
          return grpc::Status::OK;
        }
        *response = replay->second.response;
        response->mutable_summary()->set_code(raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY);
        response->mutable_summary()->set_message("create object replayed");
        return grpc::Status::OK;
      }

      const auto bucket_it = buckets_.find(request->bucket());
      if (bucket_it == buckets_.end() || bucket_it->second.deleted())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                        "bucket not found",
                        request->request_id(),
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }

      if (request->request_id().empty() || request->object_key().empty() ||
          request->object_id().empty() || request->size() == 0)
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_INVALID_ARGUMENT,
                        "request_id object_key object_id and size are required",
                        request->request_id(),
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }

      const std::string key = ObjectIdentity(request->bucket(), request->object_key());
      auto existing = objects_.find(key);
      if (existing != objects_.end() &&
          existing->second.state() != raft::METADATA_OBJECT_STATE_DELETED)
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_STATE_CONFLICT,
                        "object already exists",
                        request->request_id(),
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        existing->second.state(),
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }

      raft::ObjectRecord object;
      object.set_bucket(request->bucket());
      object.set_object_key(request->object_key());
      object.set_object_id(request->object_id());
      object.set_version(request->version());
      object.set_size(request->size());
      object.set_etag(request->etag());
      object.set_state(raft::METADATA_OBJECT_STATE_PENDING);
      object.set_create_time(request->client_time_unix_ms());
      object.set_commit_time(0);
      object.set_delete_time(0);
      objects_[key] = object;

      response->mutable_summary()->CopyFrom(
          MakeSummary(raft::METADATA_STATUS_CODE_OK,
                      "ok",
                      request->request_id(),
                      request->bucket(),
                      request->object_key(),
                      request->object_id(),
                      raft::METADATA_OBJECT_STATE_PENDING,
                      NextLogIndex()));
      response->mutable_object()->CopyFrom(object);
      create_object_replays_[request->request_id()] = {fingerprint, *response};
      return grpc::Status::OK;
    }

    grpc::Status CommitObject(grpc::ServerContext *,
                              const raft::CommitObjectRequest *request,
                              raft::CommitObjectResponse *response) override
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++commit_object_calls_;

      if (MaybeServeForcedWriteResponse(request->request_id(),
                                        request->bucket(),
                                        request->object_key(),
                                        request->object_id(),
                                        response))
      {
        return grpc::Status::OK;
      }

      const std::string fingerprint = CommitObjectFingerprint(*request);
      auto replay = commit_object_replays_.find(request->request_id());
      if (replay != commit_object_replays_.end())
      {
        if (replay->second.fingerprint != fingerprint)
        {
          response->mutable_summary()->CopyFrom(
              MakeSummary(raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT,
                          "commit object request_id conflict",
                          request->request_id(),
                          request->bucket(),
                          request->object_key(),
                          request->object_id(),
                          raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                          CurrentLogIndex()));
          return grpc::Status::OK;
        }
        *response = replay->second.response;
        response->mutable_summary()->set_code(raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY);
        response->mutable_summary()->set_message("commit object replayed");
        return grpc::Status::OK;
      }

      const auto bucket_it = buckets_.find(request->bucket());
      if (bucket_it == buckets_.end() || bucket_it->second.deleted())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                        "bucket not found",
                        request->request_id(),
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }

      const std::string key = ObjectIdentity(request->bucket(), request->object_key());
      auto object_it = objects_.find(key);
      if (object_it == objects_.end())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                        "object not found",
                        request->request_id(),
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }
      if (object_it->second.object_id() != request->object_id())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_STATE_CONFLICT,
                        "object_id mismatch",
                        request->request_id(),
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        object_it->second.state(),
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }
      if (object_it->second.state() != raft::METADATA_OBJECT_STATE_PENDING)
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_STATE_CONFLICT,
                        "object is not pending",
                        request->request_id(),
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        object_it->second.state(),
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }

      object_it->second.set_state(raft::METADATA_OBJECT_STATE_COMMITTED);
      object_it->second.set_version(request->version());
      object_it->second.set_size(request->size());
      object_it->second.set_etag(request->etag());
      object_it->second.set_commit_time(request->client_time_unix_ms());
      object_it->second.clear_chunks();
      for (const auto &chunk : request->chunks())
      {
        object_it->second.add_chunks()->CopyFrom(chunk);
      }

      response->mutable_summary()->CopyFrom(
          MakeSummary(raft::METADATA_STATUS_CODE_OK,
                      "ok",
                      request->request_id(),
                      request->bucket(),
                      request->object_key(),
                      request->object_id(),
                      raft::METADATA_OBJECT_STATE_COMMITTED,
                      NextLogIndex()));
      response->mutable_object()->CopyFrom(object_it->second);
      commit_object_replays_[request->request_id()] = {fingerprint, *response};
      return grpc::Status::OK;
    }

    grpc::Status AbortObject(grpc::ServerContext *,
                             const raft::AbortObjectRequest *request,
                             raft::AbortObjectResponse *response) override
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (MaybeServeForcedWriteResponse(request->request_id(),
                                        request->bucket(),
                                        request->object_key(),
                                        request->object_id(),
                                        response))
      {
        return grpc::Status::OK;
      }

      const std::string key = ObjectIdentity(request->bucket(), request->object_key());
      auto object_it = objects_.find(key);
      if (object_it == objects_.end())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                        "object not found",
                        request->request_id(),
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }
      object_it->second.set_state(raft::METADATA_OBJECT_STATE_DELETED);
      object_it->second.set_delete_time(request->client_time_unix_ms());
      response->mutable_summary()->CopyFrom(
          MakeSummary(raft::METADATA_STATUS_CODE_OK,
                      "ok",
                      request->request_id(),
                      request->bucket(),
                      request->object_key(),
                      request->object_id(),
                      raft::METADATA_OBJECT_STATE_DELETED,
                      NextLogIndex()));
      response->mutable_object()->CopyFrom(object_it->second);
      return grpc::Status::OK;
    }

    grpc::Status DeleteObject(grpc::ServerContext *,
                              const raft::DeleteObjectRequest *request,
                              raft::DeleteObjectResponse *response) override
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++delete_object_calls_;

      if (MaybeServeForcedWriteResponse(request->request_id(),
                                        request->bucket(),
                                        request->object_key(),
                                        request->object_id(),
                                        response))
      {
        return grpc::Status::OK;
      }

      const std::string fingerprint = DeleteObjectFingerprint(*request);
      auto replay = delete_object_replays_.find(request->request_id());
      if (replay != delete_object_replays_.end())
      {
        if (replay->second.fingerprint != fingerprint)
        {
          response->mutable_summary()->CopyFrom(
              MakeSummary(raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT,
                          "delete object request_id conflict",
                          request->request_id(),
                          request->bucket(),
                          request->object_key(),
                          request->object_id(),
                          raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                          CurrentLogIndex()));
          return grpc::Status::OK;
        }
        *response = replay->second.response;
        response->mutable_summary()->set_code(raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY);
        response->mutable_summary()->set_message("delete object replayed");
        return grpc::Status::OK;
      }

      const std::string key = ObjectIdentity(request->bucket(), request->object_key());
      auto object_it = objects_.find(key);
      if (object_it == objects_.end())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                        "object not found",
                        request->request_id(),
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }
      if (object_it->second.state() != raft::METADATA_OBJECT_STATE_COMMITTED)
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_STATE_CONFLICT,
                        "object is not committed",
                        request->request_id(),
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        object_it->second.state(),
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }

      object_it->second.set_state(raft::METADATA_OBJECT_STATE_DELETED);
      object_it->second.set_delete_time(request->client_time_unix_ms());
      object_it->second.clear_chunks();
      response->mutable_summary()->CopyFrom(
          MakeSummary(raft::METADATA_STATUS_CODE_OK,
                      "ok",
                      request->request_id(),
                      request->bucket(),
                      request->object_key(),
                      request->object_id(),
                      raft::METADATA_OBJECT_STATE_DELETED,
                      NextLogIndex()));
      response->mutable_object()->CopyFrom(object_it->second);
      delete_object_replays_[request->request_id()] = {fingerprint, *response};
      return grpc::Status::OK;
    }

    grpc::Status HeadObject(grpc::ServerContext *,
                            const raft::HeadObjectRequest *request,
                            raft::HeadObjectResponse *response) override
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (MaybeServeForcedHeadResponse(request->bucket(), request->object_key(), response))
      {
        return grpc::Status::OK;
      }
      const auto bucket_it = buckets_.find(request->bucket());
      if (bucket_it == buckets_.end() || bucket_it->second.deleted())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                        "bucket not found",
                        "",
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        response->set_found(false);
        return grpc::Status::OK;
      }

      const auto it = objects_.find(ObjectIdentity(request->bucket(), request->object_key()));
      if (it == objects_.end() ||
          it->second.state() != raft::METADATA_OBJECT_STATE_COMMITTED)
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                        "object not found",
                        "",
                        request->bucket(),
                        request->object_key(),
                        request->object_id(),
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        response->set_found(false);
        return grpc::Status::OK;
      }

      response->mutable_summary()->CopyFrom(
          MakeSummary(raft::METADATA_STATUS_CODE_OK,
                      "ok",
                      "",
                      request->bucket(),
                      request->object_key(),
                      request->object_id(),
                      raft::METADATA_OBJECT_STATE_COMMITTED,
                      CurrentLogIndex()));
      response->set_found(true);
      response->mutable_object()->CopyFrom(it->second);
      return grpc::Status::OK;
    }

    grpc::Status ListObjects(grpc::ServerContext *,
                             const raft::ListObjectsRequest *request,
                             raft::ListObjectsResponse *response) override
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (MaybeServeForcedListResponse(request->bucket(), request->prefix(), response))
      {
        return grpc::Status::OK;
      }
      const auto bucket_it = buckets_.find(request->bucket());
      if (bucket_it == buckets_.end() || bucket_it->second.deleted())
      {
        response->mutable_summary()->CopyFrom(
            MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                        "bucket not found",
                        "",
                        request->bucket(),
                        request->prefix(),
                        "",
                        raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                        CurrentLogIndex()));
        return grpc::Status::OK;
      }

      std::vector<const raft::ObjectRecord *> visible;
      for (const auto &[key, object] : objects_)
      {
        static_cast<void>(key);
        if (object.bucket() != request->bucket() ||
            object.state() != raft::METADATA_OBJECT_STATE_COMMITTED)
        {
          continue;
        }
        if (!request->prefix().empty() &&
            object.object_key().rfind(request->prefix(), 0) != 0)
        {
          continue;
        }
        if (!request->continuation_token().empty() &&
            object.object_key() <= request->continuation_token())
        {
          continue;
        }
        visible.push_back(&object);
      }

      std::sort(visible.begin(), visible.end(),
                [](const raft::ObjectRecord *lhs, const raft::ObjectRecord *rhs)
                {
                  return lhs->object_key() < rhs->object_key();
                });

      std::string next_token;
      if (request->limit() != 0 &&
          visible.size() > static_cast<std::size_t>(request->limit()))
      {
        visible.resize(request->limit());
        next_token = visible.back()->object_key();
      }

      response->mutable_summary()->CopyFrom(
          MakeSummary(raft::METADATA_STATUS_CODE_OK,
                      "ok",
                      "",
                      request->bucket(),
                      request->prefix(),
                      "",
                      raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                      CurrentLogIndex()));
      for (const auto *object : visible)
      {
        response->add_objects()->CopyFrom(*object);
      }
      response->set_next_continuation_token(next_token);
      return grpc::Status::OK;
    }

  private:
    struct ForcedWriteResponse
    {
      raft::MetadataStatusCode code;
      std::string message;
    };

    struct ForcedReadResponse
    {
      raft::MetadataStatusCode code;
      std::string message;
    };

    template <typename Response>
    bool MaybeServeForcedWriteResponse(const std::string &request_id,
                                       const std::string &bucket,
                                       const std::string &object_key,
                                       const std::string &object_id,
                                       Response *response)
    {
      const auto it = forced_write_responses_.find(request_id);
      if (it == forced_write_responses_.end())
      {
        return false;
      }

      response->mutable_summary()->CopyFrom(
          MakeSummary(it->second.code,
                      it->second.message,
                      request_id,
                      bucket,
                      object_key,
                      object_id,
                      raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                      CurrentLogIndex()));
      return true;
    }

    bool MaybeServeForcedHeadResponse(const std::string &bucket,
                                      const std::string &object_key,
                                      raft::HeadObjectResponse *response)
    {
      const auto it = forced_head_responses_.find(ObjectIdentity(bucket, object_key));
      if (it == forced_head_responses_.end())
      {
        return false;
      }

      response->mutable_summary()->CopyFrom(
          MakeSummary(it->second.code,
                      it->second.message,
                      "",
                      bucket,
                      object_key,
                      "",
                      raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                      CurrentLogIndex()));
      response->set_found(false);
      return true;
    }

    bool MaybeServeForcedListResponse(const std::string &bucket,
                                      const std::string &prefix,
                                      raft::ListObjectsResponse *response)
    {
      const auto it = forced_list_responses_.find(ObjectIdentity(bucket, prefix));
      if (it == forced_list_responses_.end())
      {
        return false;
      }

      response->mutable_summary()->CopyFrom(
          MakeSummary(it->second.code,
                      it->second.message,
                      "",
                      bucket,
                      prefix,
                      "",
                      raft::METADATA_OBJECT_STATE_UNSPECIFIED,
                      CurrentLogIndex()));
      response->clear_objects();
      response->set_next_continuation_token("");
      return true;
    }

    static std::string CreateBucketFingerprint(const raft::CreateBucketRequest &request)
    {
      return request.request_id() + "|" + request.bucket() + "|" +
             std::to_string(request.client_time_unix_ms());
    }

    static std::string CreateObjectFingerprint(const raft::CreateObjectRequest &request)
    {
      return request.request_id() + "|" + request.bucket() + "|" +
             request.object_key() + "|" + request.object_id() + "|" +
             std::to_string(request.version()) + "|" +
             std::to_string(request.size()) + "|" + request.etag() + "|" +
             std::to_string(request.client_time_unix_ms());
    }

    static std::string CommitObjectFingerprint(const raft::CommitObjectRequest &request)
    {
      std::ostringstream oss;
      oss << request.request_id() << "|" << request.bucket() << "|"
          << request.object_key() << "|" << request.object_id() << "|"
          << request.version() << "|" << request.size() << "|"
          << request.etag() << "|" << request.client_time_unix_ms();
      for (const auto &chunk : request.chunks())
      {
        oss << "|" << chunk.chunk_id() << ":" << chunk.offset()
            << ":" << chunk.size() << ":" << chunk.checksum();
      }
      return oss.str();
    }

    static std::string DeleteObjectFingerprint(const raft::DeleteObjectRequest &request)
    {
      return request.request_id() + "|" + request.bucket() + "|" +
             request.object_key() + "|" + request.object_id() + "|" +
             std::to_string(request.version()) + "|" +
             std::to_string(request.client_time_unix_ms());
    }

    raft::MetadataResponseSummary MakeSummary(const raft::MetadataStatusCode code,
                                              const std::string &message,
                                              const std::string &request_id,
                                              const std::string &bucket,
                                              const std::string &object_key,
                                              const std::string &object_id,
                                              const raft::MetadataObjectState state,
                                              const std::uint64_t log_index) const
    {
      raft::MetadataResponseSummary summary;
      summary.set_code(code);
      summary.set_message(message);
      summary.set_request_id(request_id);
      summary.set_bucket(bucket);
      summary.set_object_key(object_key);
      summary.set_object_id(object_id);
      summary.set_state(state);
      summary.set_term(term_);
      summary.set_log_index(log_index);
      summary.mutable_leader_hint()->set_leader_id(1);
      summary.mutable_leader_hint()->set_leader_address(leader_address_);
      return summary;
    }

    std::uint64_t CurrentLogIndex() const
    {
      return next_log_index_ == 0 ? 0 : next_log_index_ - 1;
    }

    std::uint64_t NextLogIndex()
    {
      return next_log_index_++;
    }

    mutable std::mutex mu_;
    std::string leader_address_;
    std::uint64_t term_ = 7;
    std::uint64_t next_log_index_ = 1;
    std::map<std::string, raft::BucketRecord> buckets_;
    std::map<std::string, raft::ObjectRecord> objects_;
    std::map<std::string, ForcedWriteResponse> forced_write_responses_;
    std::map<std::string, ForcedReadResponse> forced_head_responses_;
    std::map<std::string, ForcedReadResponse> forced_list_responses_;
    std::map<std::string, ReplayEntry<raft::CreateBucketResponse>> create_bucket_replays_;
    std::map<std::string, ReplayEntry<raft::CreateObjectResponse>> create_object_replays_;
    std::map<std::string, ReplayEntry<raft::CommitObjectResponse>> commit_object_replays_;
    std::map<std::string, ReplayEntry<raft::DeleteObjectResponse>> delete_object_replays_;
    std::size_t create_bucket_calls_ = 0;
    std::size_t create_object_calls_ = 0;
    std::size_t commit_object_calls_ = 0;
    std::size_t delete_object_calls_ = 0;
  };

  class ScopedFakeMetadataServer
  {
  public:
    ScopedFakeMetadataServer()
        : service_("pending")
    {
      grpc::ServerBuilder builder;
      builder.AddListeningPort("127.0.0.1:0",
                               grpc::InsecureServerCredentials(),
                               &selected_port_);
      builder.RegisterService(&service_);
      server_ = builder.BuildAndStart();
      address_ = "127.0.0.1:" + std::to_string(selected_port_);
      service_.SetLeaderAddress(address_);
    }

    ~ScopedFakeMetadataServer()
    {
      if (server_ != nullptr)
      {
        server_->Shutdown();
      }
    }

    const std::string &address() const
    {
      return address_;
    }

    FakeMetadataService &service()
    {
      return service_;
    }

  private:
    int selected_port_ = 0;
    std::string address_;
    FakeMetadataService service_;
    std::unique_ptr<grpc::Server> server_;
  };

  class FakeJoinMetadataService final : public raft::MetadataService::Service
  {
  public:
    struct JoinReply
    {
      raft::MetadataStatusCode code = raft::METADATA_STATUS_CODE_OK;
      raft::JoinMetadataClusterDisposition disposition =
          raft::JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT;
      std::string message =
          "join validation accepted on leader; no committed membership change performed";
      bool include_leader_hint = false;
      int leader_hint_id = 0;
      std::string leader_hint_address;
      std::uint64_t membership_epoch = 3;
    };

    void SetJoinReply(JoinReply reply)
    {
      std::lock_guard<std::mutex> lock(mu_);
      reply_ = std::move(reply);
    }

    std::size_t join_call_count() const
    {
      std::lock_guard<std::mutex> lock(mu_);
      return join_call_count_;
    }

    std::optional<raft::JoinMetadataClusterRequest> last_join_request() const
    {
      std::lock_guard<std::mutex> lock(mu_);
      return last_join_request_;
    }

    grpc::Status JoinMetadataCluster(
        grpc::ServerContext *,
        const raft::JoinMetadataClusterRequest *request,
        raft::JoinMetadataClusterResponse *response) override
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++join_call_count_;
      last_join_request_ = *request;

      response->mutable_summary()->set_code(reply_.code);
      response->mutable_summary()->set_message(reply_.message);
      response->mutable_summary()->set_request_id(request->request_id());
      response->mutable_summary()->set_term(7);
      response->mutable_summary()->set_log_index(reply_.membership_epoch);
      if (reply_.include_leader_hint)
      {
        response->mutable_summary()->mutable_leader_hint()->set_leader_id(
            reply_.leader_hint_id);
        response->mutable_summary()->mutable_leader_hint()->set_leader_address(
            reply_.leader_hint_address);
      }
      else
      {
        response->mutable_summary()->clear_leader_hint();
      }
      response->set_disposition(reply_.disposition);
      response->set_requested_membership(
          raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
      response->set_committed_membership_changed(false);
      response->set_membership_epoch(reply_.membership_epoch);
      response->set_canonical_node_id(request->node_id());
      response->set_assigned_raft_id(request->candidate_raft_id());
      return grpc::Status::OK;
    }

  private:
    mutable std::mutex mu_;
    JoinReply reply_;
    std::size_t join_call_count_ = 0;
    std::optional<raft::JoinMetadataClusterRequest> last_join_request_;
  };

  class ScopedFakeJoinMetadataServer
  {
  public:
    ScopedFakeJoinMetadataServer()
    {
      grpc::ServerBuilder builder;
      builder.AddListeningPort("127.0.0.1:0",
                               grpc::InsecureServerCredentials(),
                               &selected_port_);
      builder.RegisterService(&service_);
      server_ = builder.BuildAndStart();
      address_ = "127.0.0.1:" + std::to_string(selected_port_);
    }

    ~ScopedFakeJoinMetadataServer()
    {
      if (server_ != nullptr)
      {
        server_->Shutdown();
      }
    }

    const std::string &address() const
    {
      return address_;
    }

    FakeJoinMetadataService &service()
    {
      return service_;
    }

  private:
    int selected_port_ = 0;
    std::string address_;
    FakeJoinMetadataService service_;
    std::unique_ptr<grpc::Server> server_;
  };

  class FakeViewNodeService final : public view::ViewNodeService::Service
  {
  public:
    struct MetadataCandidate
    {
      std::string node_id;
      std::string endpoint;
      view::MetadataMembershipObservedState membership_state =
          view::METADATA_MEMBERSHIP_OBSERVED_STATE_VOTER;
      view::MetadataRaftObservedRole raft_role =
          view::METADATA_RAFT_OBSERVED_ROLE_FOLLOWER;
    };

    struct DiscoverPlan
    {
      std::string view_node_id = "view-join-1";
      std::vector<MetadataCandidate> metadata_candidates;
      std::optional<view::MetadataLeaderHint> leader_hint;
      std::uint64_t observed_at_unix_ms = 1710000000123ULL;
      std::uint64_t membership_epoch = 1;
    };

    void SetPlan(DiscoverPlan plan)
    {
      std::lock_guard<std::mutex> lock(mu_);
      plan_ = std::move(plan);
    }

    std::size_t discover_call_count() const
    {
      std::lock_guard<std::mutex> lock(mu_);
      return discover_call_count_;
    }

    grpc::Status DiscoverMetadata(
        grpc::ServerContext *,
        const view::DiscoverMetadataRequest *request,
        view::DiscoverMetadataResponse *response) override
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++discover_call_count_;

      response->mutable_summary()->set_code(view::VIEW_NODE_STATUS_CODE_OK);
      response->mutable_summary()->set_message("ok");
      response->mutable_summary()->set_request_id(request->request_id());
      response->mutable_summary()->set_cluster_id(request->cluster_id());
      response->mutable_summary()->set_node_id(plan_.view_node_id);
      response->set_observed_at_unix_ms(plan_.observed_at_unix_ms);
      response->set_membership_epoch(plan_.membership_epoch);

      for (const auto &candidate : plan_.metadata_candidates)
      {
        auto *snapshot = response->add_metadata_nodes();
        snapshot->set_cluster_id(request->cluster_id());
        snapshot->set_node_id(candidate.node_id);
        snapshot->set_node_type(view::VIEW_NODE_TYPE_METADATA);
        snapshot->set_endpoint(candidate.endpoint);
        snapshot->set_control_plane_endpoint(candidate.endpoint);
        snapshot->set_registered_at_unix_ms(plan_.observed_at_unix_ms);
        snapshot->set_last_seen_unix_ms(plan_.observed_at_unix_ms);
        snapshot->set_last_sequence(1);
        snapshot->set_liveness(view::VIEW_NODE_LIVENESS_STATE_LIVE);
        snapshot->mutable_health()->set_health(view::VIEW_NODE_HEALTH_HEALTHY);
        snapshot->mutable_health()->set_disk_pressure(
            view::VIEW_NODE_DISK_PRESSURE_LOW);
        snapshot->mutable_metadata()->set_raft_id(
            candidate.node_id == "meta-1" ? 1
            : candidate.node_id == "meta-2" ? 2
                                          : 3);
        snapshot->mutable_metadata()->set_raft_role(candidate.raft_role);
        snapshot->mutable_metadata()->set_membership_state(
            candidate.membership_state);
        snapshot->mutable_metadata()->set_observed_term(9);
        snapshot->mutable_metadata()->set_commit_index(12);
        snapshot->mutable_metadata()->set_membership_epoch(plan_.membership_epoch);
      }

      if (plan_.leader_hint.has_value())
      {
        response->mutable_leader_hint()->CopyFrom(*plan_.leader_hint);
      }
      return grpc::Status::OK;
    }

  private:
    mutable std::mutex mu_;
    DiscoverPlan plan_;
    std::size_t discover_call_count_ = 0;
  };

  class ScopedFakeViewNodeServer
  {
  public:
    ScopedFakeViewNodeServer()
    {
      grpc::ServerBuilder builder;
      builder.AddListeningPort("127.0.0.1:0",
                               grpc::InsecureServerCredentials(),
                               &selected_port_);
      builder.RegisterService(&service_);
      server_ = builder.BuildAndStart();
      address_ = "127.0.0.1:" + std::to_string(selected_port_);
    }

    ~ScopedFakeViewNodeServer()
    {
      if (server_ != nullptr)
      {
        server_->Shutdown();
      }
    }

    const std::string &address() const
    {
      return address_;
    }

    FakeViewNodeService &service()
    {
      return service_;
    }

  private:
    int selected_port_ = 0;
    std::string address_;
    FakeViewNodeService service_;
    std::unique_ptr<grpc::Server> server_;
  };

  class MetadataClientScenarioTest : public ::testing::Test
  {
  protected:
    static constexpr const char *kBucket = "scenario-bucket";
    ScopedFakeMetadataServer server_;
  };

} // namespace

TEST_F(MetadataClientScenarioTest, CreateObjectScenarioCreatesPendingMetadataObject)
{
  ASSERT_EQ(RunClient(
                {server_.address(), "create-bucket",
                 "--request-id", "req-bucket-1",
                 "--bucket", kBucket},
                "create_bucket")
                .exit_code,
            0);

  const ClientRunResult result = RunClient(
      {server_.address(), "create-object",
       "--request-id", "req-create-1",
       "--bucket", kBucket,
       "--object-key", "scenario/object-a",
       "--object-id", "obj-a",
       "--size", "16",
       "--etag", "etag-a"},
      "create_object");

  ASSERT_EQ(result.exit_code, 0) << result.output;
  EXPECT_TRUE(Contains(result.output, "stage=create-object")) << result.output;
  EXPECT_TRUE(Contains(result.output, "state=PENDING")) << result.output;

  const auto snapshot = server_.service().TakeSnapshot(kBucket, "scenario/object-a");
  ASSERT_TRUE(snapshot.bucket_exists);
  ASSERT_TRUE(snapshot.object.has_value());
  EXPECT_EQ(snapshot.object->state(), raft::METADATA_OBJECT_STATE_PENDING);
  EXPECT_EQ(snapshot.object->size(), 16U);
  EXPECT_EQ(snapshot.object->etag(), "etag-a");
}

TEST_F(MetadataClientScenarioTest, CreateCommitHeadListDeleteFlowSucceeds)
{
  ASSERT_EQ(RunClient(
                {server_.address(), "create-bucket",
                 "--request-id", "req-flow-bucket",
                 "--bucket", kBucket},
                "flow_create_bucket")
                .exit_code,
            0);

  ASSERT_EQ(RunClient(
                {server_.address(), "create-object",
                 "--request-id", "req-flow-create",
                 "--bucket", kBucket,
                 "--object-key", "scenario/object-flow",
                 "--object-id", "obj-flow",
                 "--size", "24"},
                "flow_create_object")
                .exit_code,
            0);

  ClientRunResult result = RunClient(
      {server_.address(), "commit-object",
       "--request-id", "req-flow-commit",
       "--bucket", kBucket,
       "--object-key", "scenario/object-flow",
       "--object-id", "obj-flow",
       "--size", "24",
       "--chunk-size", "8"},
      "flow_commit");
  ASSERT_EQ(result.exit_code, 0) << result.output;
  EXPECT_TRUE(Contains(result.output, "stage=commit-object")) << result.output;
  EXPECT_TRUE(Contains(result.output, "status=OK")) << result.output;

  result = RunClient(
      {server_.address(), "head-object",
       "--bucket", kBucket,
       "--object-key", "scenario/object-flow"},
      "flow_head");
  ASSERT_EQ(result.exit_code, 0) << result.output;
  EXPECT_TRUE(Contains(result.output, "found=true")) << result.output;
  EXPECT_TRUE(Contains(result.output, "state=COMMITTED")) << result.output;

  result = RunClient(
      {server_.address(), "list-objects",
       "--bucket", kBucket,
       "--prefix", "scenario/object-flow"},
      "flow_list");
  ASSERT_EQ(result.exit_code, 0) << result.output;
  EXPECT_TRUE(Contains(result.output, "objects_count=1")) << result.output;
  EXPECT_TRUE(Contains(result.output, "list_object[0] bucket=scenario-bucket")) << result.output;
  EXPECT_TRUE(Contains(result.output, "object_key=scenario/object-flow")) << result.output;

  result = RunClient(
      {server_.address(), "delete-object",
       "--request-id", "req-flow-delete",
       "--bucket", kBucket,
       "--object-key", "scenario/object-flow",
       "--object-id", "obj-flow"},
      "flow_delete");
  ASSERT_EQ(result.exit_code, 0) << result.output;
  EXPECT_TRUE(Contains(result.output, "stage=delete-object")) << result.output;

  result = RunClient(
      {server_.address(), "head-object",
       "--bucket", kBucket,
       "--object-key", "scenario/object-flow"},
      "flow_head_after_delete");
  ASSERT_NE(result.exit_code, 0);
  EXPECT_TRUE(Contains(result.output, "status=NOT_FOUND")) << result.output;
}

TEST_F(MetadataClientScenarioTest, VerifyReadAfterWriteModeReportsPass)
{
  const ClientRunResult result = RunClient(
      {server_.address(), "verify-read-after-write",
       "--request-id", "req-verify",
       "--bucket", kBucket,
       "--object-key", "scenario/object-verify",
       "--object-id", "obj-verify",
       "--size", "16",
       "--chunk-size", "8"},
      "verify_raw");

  ASSERT_EQ(result.exit_code, 0) << result.output;
  EXPECT_TRUE(Contains(result.output, "verification_check step=create-head-invisible result=PASS"))
      << result.output;
  EXPECT_TRUE(Contains(result.output, "verification_check step=commit-head-visible result=PASS"))
      << result.output;
  EXPECT_TRUE(Contains(result.output, "verification_check step=delete-list-invisible result=PASS"))
      << result.output;
  EXPECT_TRUE(Contains(result.output, "verification_result mode=read-after-write result=PASS"))
      << result.output;
}

TEST_F(MetadataClientScenarioTest, DuplicateRequestIdDoesNotCreateDuplicateVisibleObject)
{
  ASSERT_EQ(RunClient(
                {server_.address(), "create-bucket",
                 "--request-id", "req-dup-bucket",
                 "--bucket", kBucket},
                "dup_bucket")
                .exit_code,
            0);

  ClientRunResult result = RunClient(
      {server_.address(), "create-object",
       "--request-id", "req-dup-create",
       "--bucket", kBucket,
       "--object-key", "scenario/object-dup",
       "--object-id", "obj-dup",
       "--size", "16"},
      "dup_create_first");
  ASSERT_EQ(result.exit_code, 0) << result.output;

  result = RunClient(
      {server_.address(), "create-object",
       "--request-id", "req-dup-create",
       "--bucket", kBucket,
       "--object-key", "scenario/object-dup",
       "--object-id", "obj-dup",
       "--size", "16"},
      "dup_create_second");
  ASSERT_EQ(result.exit_code, 0) << result.output;
  EXPECT_TRUE(Contains(result.output, "status=IDEMPOTENT_REPLAY")) << result.output;

  result = RunClient(
      {server_.address(), "commit-object",
       "--request-id", "req-dup-commit",
       "--bucket", kBucket,
       "--object-key", "scenario/object-dup",
       "--object-id", "obj-dup",
       "--size", "16",
       "--chunk-size", "8"},
      "dup_commit");
  ASSERT_EQ(result.exit_code, 0) << result.output;

  result = RunClient(
      {server_.address(), "list-objects",
       "--bucket", kBucket,
       "--prefix", "scenario/object-dup"},
      "dup_list");
  ASSERT_EQ(result.exit_code, 0) << result.output;
  EXPECT_TRUE(Contains(result.output, "objects_count=1")) << result.output;

  const auto snapshot = server_.service().TakeSnapshot(kBucket, "scenario/object-dup");
  ASSERT_TRUE(snapshot.object.has_value());
  EXPECT_EQ(snapshot.object->state(), raft::METADATA_OBJECT_STATE_COMMITTED);
  EXPECT_EQ(snapshot.object_count, 1U);
}

TEST_F(MetadataClientScenarioTest, ClientShowsRetryableAdmissionStatuses)
{
  server_.service().ForceWriteResponse(
      "req-not-leader", raft::METADATA_STATUS_CODE_NOT_LEADER,
      "node is not the leader");
  server_.service().ForceWriteResponse(
      "req-timeout", raft::METADATA_STATUS_CODE_TIMEOUT,
      "timed out waiting for metadata proposal completion");
  server_.service().ForceWriteResponse(
      "req-overloaded", raft::METADATA_STATUS_CODE_OVERLOADED,
      "metadata proposal admission rejected: in-flight limit reached");
  server_.service().ForceWriteResponse(
      "req-stopped", raft::METADATA_STATUS_CODE_SERVICE_UNAVAILABLE,
      "node is stopping");

  for (const auto &[request_id, expected_status] :
       std::vector<std::pair<std::string, std::string>>{
           {"req-not-leader", "NOT_LEADER"},
           {"req-timeout", "TIMEOUT"},
           {"req-overloaded", "OVERLOADED"},
           {"req-stopped", "SERVICE_UNAVAILABLE"}})
  {
    const ClientRunResult result = RunClient(
        {server_.address(), "create-bucket",
         "--request-id", request_id,
         "--bucket", kBucket},
        "retryable_" + request_id);

    ASSERT_NE(result.exit_code, 0);
    EXPECT_TRUE(Contains(result.output, "status=" + expected_status)) << result.output;
    EXPECT_TRUE(Contains(result.output, "retryable=true")) << result.output;
    EXPECT_TRUE(Contains(result.output, "request_id=" + request_id)) << result.output;
  }
}

TEST_F(MetadataClientScenarioTest,
       JoinMetadataClusterContractIsExposedByMetadataServiceProto)
{
  EXPECT_TRUE(MetadataServiceHasMethod("JoinMetadataCluster"));
  EXPECT_FALSE(MetadataServiceHasMethod("AddLearner"));

  const auto *request_descriptor = raft::JoinMetadataClusterRequest::descriptor();
  ASSERT_NE(request_descriptor, nullptr);
  EXPECT_TRUE(MessageHasField(request_descriptor, "request_id"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "cluster_id"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "node_id"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "candidate_raft_id"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "candidate_client_address"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "candidate_raft_address"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "candidate_incarnation_id"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "candidate_sequence"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "persistent_generation"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "data_dir_fingerprint"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "local_state_hint"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "observed_view_node_id"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "observed_time_unix_ms"));
  EXPECT_TRUE(MessageHasField(request_descriptor, "observed_metadata_endpoint"));

  const auto *response_descriptor = raft::JoinMetadataClusterResponse::descriptor();
  ASSERT_NE(response_descriptor, nullptr);
  EXPECT_TRUE(MessageHasField(response_descriptor, "summary"));
  EXPECT_TRUE(MessageHasField(response_descriptor, "disposition"));
  EXPECT_TRUE(MessageHasField(response_descriptor, "requested_membership"));
  EXPECT_TRUE(MessageHasField(response_descriptor, "committed_membership_changed"));
  EXPECT_TRUE(MessageHasField(response_descriptor, "membership_epoch"));
  EXPECT_TRUE(MessageHasField(response_descriptor, "canonical_node_id"));
  EXPECT_TRUE(MessageHasField(response_descriptor, "assigned_raft_id"));

  raft::JoinMetadataClusterRequest request;
  request.set_request_id("req-join-contract");
  request.set_cluster_id("cluster-alpha");
  request.set_node_id("meta-candidate-1");
  request.set_candidate_raft_id(401);
  request.set_candidate_client_address("127.0.0.1:7501");
  request.set_candidate_raft_address("127.0.0.1:7601");
  request.set_candidate_incarnation_id("meta-candidate-1:boot:123");
  request.set_candidate_sequence(7);
  request.set_persistent_generation(1);
  request.set_data_dir_fingerprint("fp-meta-candidate-1");
  request.set_local_state_hint(
      raft::JOIN_METADATA_CANDIDATE_STATE_HINT_CANDIDATE);
  request.set_observed_view_node_id("view-1");
  request.set_observed_time_unix_ms(1710000000123ULL);
  request.set_observed_metadata_endpoint("127.0.0.1:7501");

  raft::JoinMetadataClusterResponse response;
  response.mutable_summary()->set_code(raft::METADATA_STATUS_CODE_NOT_LEADER);
  response.mutable_summary()->set_message("join authority belongs to metadata leader");
  response.mutable_summary()->set_request_id(request.request_id());
  response.mutable_summary()->mutable_leader_hint()->set_leader_id(2);
  response.mutable_summary()->mutable_leader_hint()->set_leader_address(
      "127.0.0.1:7412");
  response.set_disposition(
      raft::JOIN_METADATA_CLUSTER_DISPOSITION_NOT_LEADER);
  response.set_requested_membership(
      raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
  response.set_committed_membership_changed(false);
  response.set_membership_epoch(3);
  response.set_canonical_node_id(request.node_id());
  response.set_assigned_raft_id(request.candidate_raft_id());

  EXPECT_EQ(response.summary().code(), raft::METADATA_STATUS_CODE_NOT_LEADER);
  EXPECT_EQ(response.disposition(),
            raft::JOIN_METADATA_CLUSTER_DISPOSITION_NOT_LEADER);
  EXPECT_EQ(response.requested_membership(),
            raft::JOIN_METADATA_TARGET_MEMBERSHIP_LEARNER);
  EXPECT_FALSE(response.committed_membership_changed());
  EXPECT_EQ(response.summary().leader_hint().leader_address(),
            "127.0.0.1:7412");
  EXPECT_NE(response.disposition(),
            raft::JOIN_METADATA_CLUSTER_DISPOSITION_ACCEPTED_PENDING_COMMIT);
}

TEST_F(MetadataClientScenarioTest,
       UnsupportedJoinMetadataClusterCliDoesNotBypassLeaderAuthority)
{
  const ClientRunResult result = RunClient(
      {server_.address(), "join-metadata-cluster",
       "--request-id", "req-join-leader-validation",
       "--bucket", kBucket},
      "join_metadata_cluster_unsupported");

  ASSERT_EQ(result.exit_code, 2) << result.output;
  EXPECT_TRUE(Contains(result.output,
                       "unsupported command: join-metadata-cluster"))
      << result.output;
  EXPECT_TRUE(Contains(result.output, "Usage:")) << result.output;
}

TEST_F(MetadataClientScenarioTest,
       FutureJoinLeaderValidationMustReturnNotLeaderAndLeaderHintForFollowerAuthority)
{
  server_.service().SetLeaderAddress("127.0.0.1:7412");
  server_.service().ForceWriteResponse(
      "req-join-authority-not-leader",
      raft::METADATA_STATUS_CODE_NOT_LEADER,
      "join authority belongs to metadata leader");

  const ClientRunResult result = RunClient(
      {server_.address(), "create-bucket",
       "--request-id", "req-join-authority-not-leader",
       "--bucket", "join-authority-probe"},
      "join_authority_not_leader");

  ASSERT_NE(result.exit_code, 0);
  EXPECT_TRUE(Contains(result.output, "status=NOT_LEADER")) << result.output;
  EXPECT_TRUE(Contains(result.output, "retryable=true")) << result.output;
  EXPECT_TRUE(Contains(result.output, "leader_address=127.0.0.1:7412"))
      << result.output;
  EXPECT_TRUE(Contains(result.output,
                       "message=\"join authority belongs to metadata leader\""))
      << result.output;
}

TEST_F(MetadataClientScenarioTest, ClientShowsIdempotencyConflictAsNonRetryable)
{
  ASSERT_EQ(RunClient(
                {server_.address(), "create-bucket",
                 "--request-id", "req-conflict-bucket",
                 "--bucket", kBucket},
                "conflict_bucket")
                .exit_code,
            0);

  ASSERT_EQ(RunClient(
                {server_.address(), "create-object",
                 "--request-id", "req-conflict-create",
                 "--bucket", kBucket,
                 "--object-key", "scenario/object-conflict-a",
                 "--object-id", "obj-conflict-a",
                 "--size", "16"},
                "conflict_create_first")
                .exit_code,
            0);

  const ClientRunResult result = RunClient(
      {server_.address(), "create-object",
       "--request-id", "req-conflict-create",
       "--bucket", kBucket,
       "--object-key", "scenario/object-conflict-b",
       "--object-id", "obj-conflict-b",
       "--size", "32"},
      "conflict_create_second");

  ASSERT_NE(result.exit_code, 0);
  EXPECT_TRUE(Contains(result.output, "status=IDEMPOTENCY_CONFLICT")) << result.output;
  EXPECT_TRUE(Contains(result.output, "retryable=false")) << result.output;
}

TEST_F(MetadataClientScenarioTest, ReadCommandsShowRetryableAdmissionStatuses)
{
  server_.service().ForceHeadResponse(
      "head-not-leader-bucket", "object/not-leader",
      raft::METADATA_STATUS_CODE_NOT_LEADER, "node is not the leader");
  server_.service().ForceHeadResponse(
      "head-timeout-bucket", "object/timeout",
      raft::METADATA_STATUS_CODE_TIMEOUT,
      "read deadline already expired before admission");
  server_.service().ForceListResponse(
      "list-stopped-bucket", "prefix/stopped",
      raft::METADATA_STATUS_CODE_SERVICE_UNAVAILABLE, "node is stopping");

  ClientRunResult result = RunClient(
      {server_.address(), "head-object",
       "--bucket", "head-not-leader-bucket",
       "--object-key", "object/not-leader"},
      "read_head_not_leader");
  ASSERT_NE(result.exit_code, 0);
  EXPECT_TRUE(Contains(result.output, "status=NOT_LEADER")) << result.output;
  EXPECT_TRUE(Contains(result.output, "retryable=true")) << result.output;

  result = RunClient(
      {server_.address(), "head-object",
       "--bucket", "head-timeout-bucket",
       "--object-key", "object/timeout"},
      "read_head_timeout");
  ASSERT_NE(result.exit_code, 0);
  EXPECT_TRUE(Contains(result.output, "status=TIMEOUT")) << result.output;
  EXPECT_TRUE(Contains(result.output, "retryable=true")) << result.output;

  result = RunClient(
      {server_.address(), "list-objects",
       "--bucket", "list-stopped-bucket",
       "--prefix", "prefix/stopped"},
      "read_list_service_unavailable");
  ASSERT_NE(result.exit_code, 0);
  EXPECT_TRUE(Contains(result.output, "status=SERVICE_UNAVAILABLE")) << result.output;
  EXPECT_TRUE(Contains(result.output, "retryable=true")) << result.output;
}

TEST_F(MetadataClientScenarioTest, ChunkLayoutAndCustomEtagAreExposed)
{
  ASSERT_EQ(RunClient(
                {server_.address(), "create-bucket",
                 "--request-id", "req-layout-bucket",
                 "--bucket", kBucket},
                "layout_bucket")
                .exit_code,
            0);
  ASSERT_EQ(RunClient(
                {server_.address(), "create-object",
                 "--request-id", "req-layout-create",
                 "--bucket", kBucket,
                 "--object-key", "scenario/object-layout",
                 "--object-id", "obj-layout",
                 "--size", "18",
                 "--etag", "custom-etag"},
                "layout_create")
                .exit_code,
            0);

  const ClientRunResult result = RunClient(
      {server_.address(), "commit-object",
       "--request-id", "req-layout-commit",
       "--bucket", kBucket,
       "--object-key", "scenario/object-layout",
       "--object-id", "obj-layout",
       "--size", "18",
       "--chunk-size", "8",
       "--etag", "custom-etag"},
      "layout_commit");

  ASSERT_EQ(result.exit_code, 0) << result.output;
  EXPECT_TRUE(Contains(result.output, "etag=custom-etag")) << result.output;
  EXPECT_TRUE(Contains(result.output, "chunks=3")) << result.output;
  EXPECT_TRUE(Contains(result.output, "object_record.chunk[0]")) << result.output;
  EXPECT_TRUE(Contains(result.output, "object_record.chunk[2]")) << result.output;
}

TEST_F(MetadataClientScenarioTest,
       MetadataNodeCandidateUsesViewLeaderHintBeforeFollowerFallback)
{
  ScopedFakeJoinMetadataServer follower;
  ScopedFakeJoinMetadataServer leader;
  ScopedFakeJoinMetadataServer spare;
  ScopedFakeViewNodeServer view_server;

  follower.service().SetJoinReply(FakeJoinMetadataService::JoinReply{
      .code = raft::METADATA_STATUS_CODE_NOT_LEADER,
      .disposition = raft::JOIN_METADATA_CLUSTER_DISPOSITION_NOT_LEADER,
      .message = "join authority belongs to metadata leader",
      .include_leader_hint = true,
      .leader_hint_id = 2,
      .leader_hint_address = leader.address(),
      .membership_epoch = 3,
  });
  leader.service().SetJoinReply(FakeJoinMetadataService::JoinReply{});
  spare.service().SetJoinReply(FakeJoinMetadataService::JoinReply{
      .code = raft::METADATA_STATUS_CODE_STATE_CONFLICT,
      .disposition = raft::JOIN_METADATA_CLUSTER_DISPOSITION_PENDING_MEMBERSHIP_CHANGE,
      .message = "pending membership change already exists",
      .membership_epoch = 3,
  });

  view::MetadataLeaderHint leader_hint;
  leader_hint.set_node_id("meta-2");
  leader_hint.set_raft_id(2);
  leader_hint.set_endpoint(leader.address());
  leader_hint.set_observed_term(9);
  leader_hint.set_observed_at_unix_ms(1710000000123ULL);
  view_server.service().SetPlan(FakeViewNodeService::DiscoverPlan{
      .view_node_id = "view-join-1",
      .metadata_candidates = {
          FakeViewNodeService::MetadataCandidate{.node_id = "meta-1",
                                                 .endpoint = follower.address()},
          FakeViewNodeService::MetadataCandidate{.node_id = "meta-2",
                                                 .endpoint = leader.address()},
      },
      .leader_hint = leader_hint,
      .observed_at_unix_ms = 1710000000123ULL,
      .membership_epoch = 1,
  });

  const auto scenario_dir = MakeScenarioDirectory("t062_view_hint");
  const auto config_path = WriteDynamicJoinClusterConfig(
      scenario_dir,
      "cluster-t062-hint",
      view_server.address(),
      follower.address(),
      leader.address(),
      spare.address(),
      "127.0.0.1:7811");

  const ClientRunResult result = RunMetadataNodeApp(
      {"--config", config_path.string(),
       "--node_id", "meta-candidate-1"},
      "t062_view_hint");

  ASSERT_EQ(result.exit_code, 5) << result.output;
  EXPECT_TRUE(Contains(result.output, "candidate mode join validation reached"))
      << result.output;
  EXPECT_TRUE(Contains(result.output, "discovery_source=view_candidates"))
      << result.output;
  EXPECT_EQ(view_server.service().discover_call_count(), 1U);
  EXPECT_EQ(follower.service().join_call_count(), 0U);
  EXPECT_EQ(leader.service().join_call_count(), 1U);

  const auto leader_request = leader.service().last_join_request();
  ASSERT_TRUE(leader_request.has_value());
  EXPECT_EQ(leader_request->observed_view_node_id(), "view-join-1");
  EXPECT_EQ(leader_request->observed_metadata_endpoint(), leader.address());
}

TEST_F(MetadataClientScenarioTest,
       MetadataNodeCandidateFallsBackToNextDiscoveredMetadataNodeWithoutLeaderHint)
{
  ScopedFakeJoinMetadataServer follower;
  ScopedFakeJoinMetadataServer leader;
  ScopedFakeJoinMetadataServer spare;
  ScopedFakeViewNodeServer view_server;

  follower.service().SetJoinReply(FakeJoinMetadataService::JoinReply{
      .code = raft::METADATA_STATUS_CODE_NOT_LEADER,
      .disposition = raft::JOIN_METADATA_CLUSTER_DISPOSITION_NOT_LEADER,
      .message = "join authority belongs to metadata leader",
      .membership_epoch = 3,
  });
  leader.service().SetJoinReply(FakeJoinMetadataService::JoinReply{});
  spare.service().SetJoinReply(FakeJoinMetadataService::JoinReply{
      .code = raft::METADATA_STATUS_CODE_STATE_CONFLICT,
      .disposition = raft::JOIN_METADATA_CLUSTER_DISPOSITION_PENDING_MEMBERSHIP_CHANGE,
      .message = "pending membership change already exists",
      .membership_epoch = 3,
  });

  view_server.service().SetPlan(FakeViewNodeService::DiscoverPlan{
      .view_node_id = "view-join-1",
      .metadata_candidates = {
          FakeViewNodeService::MetadataCandidate{.node_id = "meta-1",
                                                 .endpoint = follower.address()},
          FakeViewNodeService::MetadataCandidate{.node_id = "meta-2",
                                                 .endpoint = leader.address()},
      },
      .leader_hint = std::nullopt,
      .observed_at_unix_ms = 1710000001123ULL,
      .membership_epoch = 1,
  });

  const auto scenario_dir = MakeScenarioDirectory("t062_discovered_fallback");
  const auto config_path = WriteDynamicJoinClusterConfig(
      scenario_dir,
      "cluster-t062-fallback",
      view_server.address(),
      follower.address(),
      leader.address(),
      spare.address(),
      "127.0.0.1:7812");

  const ClientRunResult result = RunMetadataNodeApp(
      {"--config", config_path.string(),
       "--node_id", "meta-candidate-1"},
      "t062_discovered_fallback");

  ASSERT_EQ(result.exit_code, 5) << result.output;
  EXPECT_TRUE(Contains(result.output, "candidate mode join validation reached"))
      << result.output;
  EXPECT_TRUE(Contains(result.output, "discovery_source=view_candidates"))
      << result.output;
  EXPECT_EQ(view_server.service().discover_call_count(), 1U);
  EXPECT_EQ(follower.service().join_call_count(), 1U);
  EXPECT_EQ(leader.service().join_call_count(), 1U);

  const auto leader_request = leader.service().last_join_request();
  ASSERT_TRUE(leader_request.has_value());
  EXPECT_EQ(leader_request->observed_view_node_id(), "view-join-1");
  EXPECT_EQ(leader_request->observed_metadata_endpoint(), leader.address());
}

TEST_F(MetadataClientScenarioTest,
       MetadataNodeCandidateReportsClearFailureWhenAllDiscoveredMetadataCandidatesFail)
{
  ScopedFakeJoinMetadataServer first;
  ScopedFakeJoinMetadataServer second;
  ScopedFakeJoinMetadataServer spare;
  ScopedFakeViewNodeServer view_server;

  const auto not_leader_reply = FakeJoinMetadataService::JoinReply{
      .code = raft::METADATA_STATUS_CODE_NOT_LEADER,
      .disposition = raft::JOIN_METADATA_CLUSTER_DISPOSITION_NOT_LEADER,
      .message = "join authority belongs to metadata leader",
      .membership_epoch = 3,
  };
  first.service().SetJoinReply(not_leader_reply);
  second.service().SetJoinReply(not_leader_reply);
  spare.service().SetJoinReply(not_leader_reply);

  view_server.service().SetPlan(FakeViewNodeService::DiscoverPlan{
      .view_node_id = "view-join-1",
      .metadata_candidates = {
          FakeViewNodeService::MetadataCandidate{.node_id = "meta-1",
                                                 .endpoint = first.address()},
          FakeViewNodeService::MetadataCandidate{.node_id = "meta-2",
                                                 .endpoint = second.address()},
      },
      .leader_hint = std::nullopt,
      .observed_at_unix_ms = 1710000002123ULL,
      .membership_epoch = 1,
  });

  const auto scenario_dir = MakeScenarioDirectory("t062_all_fail");
  const auto config_path = WriteDynamicJoinClusterConfig(
      scenario_dir,
      "cluster-t062-fail",
      view_server.address(),
      first.address(),
      second.address(),
      spare.address(),
      "127.0.0.1:7813");

  const ClientRunResult result = RunMetadataNodeApp(
      {"--config", config_path.string(),
       "--node_id", "meta-candidate-1"},
      "t062_all_fail");

  ASSERT_EQ(result.exit_code, 6) << result.output;
  EXPECT_TRUE(Contains(result.output,
                       "dynamic join failed: no metadata leader accepted join validation"))
      << result.output;
  EXPECT_TRUE(Contains(result.output, "discovery_source=view_candidates"))
      << result.output;
  EXPECT_TRUE(Contains(result.output, "attempts=[")) << result.output;
  EXPECT_TRUE(Contains(result.output, first.address())) << result.output;
  EXPECT_TRUE(Contains(result.output, second.address())) << result.output;
  EXPECT_EQ(first.service().join_call_count(), 1U);
  EXPECT_EQ(second.service().join_call_count(), 1U);
}
