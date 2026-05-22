#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

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

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "metadata.grpc.pb.h"

#ifdef _WIN32
int raft_metadata_client_entry(int argc, char **argv);
#endif

namespace
{
  std::string ClientBinaryPath()
  {
    return RAFT_METADATA_CLIENT_PATH;
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

  bool Contains(const std::string &text, const std::string &needle)
  {
    return text.find(needle) != std::string::npos;
  }

  std::string ObjectIdentity(const std::string &bucket, const std::string &object_key)
  {
    return bucket + "\n" + object_key;
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
