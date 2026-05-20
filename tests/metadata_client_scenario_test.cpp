#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <limits>
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

#include "raft.grpc.pb.h"

#ifdef _WIN32
int raft_metadata_client_entry(int argc, char **argv);
#endif

namespace
{
    using namespace std::chrono_literals;

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
            std::cerr << "raft_metadata_client_entry threw exception: " << ex.what()
                      << '\n';
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

        return ClientRunResult{
            exit_code,
            std::move(output),
        };
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

        return ClientRunResult{
            NormalizeSystemExitCode(raw_exit),
            buffer.str(),
        };
#endif
    }

    bool Contains(const std::string &text, const std::string &needle)
    {
        return text.find(needle) != std::string::npos;
    }

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

        struct Snapshot
        {
            std::size_t record_count = 0;
            std::optional<raft::MetadataRecord> record;
            std::size_t create_calls = 0;
            std::size_t commit_calls = 0;
            std::size_t delete_calls = 0;
        };

        Snapshot TakeSnapshot(const std::string &object_key) const
        {
            std::lock_guard<std::mutex> lock(mu_);
            Snapshot snapshot;
            snapshot.record_count = records_.size();
            snapshot.create_calls = create_calls_;
            snapshot.commit_calls = commit_calls_;
            snapshot.delete_calls = delete_calls_;

            const auto it = records_.find(object_key);
            if (it != records_.end())
            {
                snapshot.record = it->second;
            }
            return snapshot;
        }

        grpc::Status CreateMetadataRecord(
            grpc::ServerContext *,
            const raft::CreateMetadataRecordRequest *request,
            raft::CreateMetadataRecordResponse *response) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++create_calls_;

            const std::string fingerprint = CreateFingerprint(*request);
            auto replay_it = create_replays_.find(request->request_id());
            if (replay_it != create_replays_.end())
            {
                if (replay_it->second.fingerprint != fingerprint)
                {
                    response->mutable_summary()->CopyFrom(
                        MakeSummary(raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT,
                                    "create request_id conflict",
                                    request->request_id(),
                                    request->object_key(),
                                    raft::METADATA_RECORD_STATE_UNSPECIFIED,
                                    CurrentLogIndex()));
                    return grpc::Status::OK;
                }

                *response = replay_it->second.response;
                response->mutable_summary()->set_code(
                    raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY);
                response->mutable_summary()->set_message("create replayed");
                return grpc::Status::OK;
            }

            std::string error;
            if (!ValidateCreateRequest(*request, &error))
            {
                response->mutable_summary()->CopyFrom(
                    MakeSummary(raft::METADATA_STATUS_CODE_INVALID_ARGUMENT,
                                error,
                                request->request_id(),
                                request->object_key(),
                                raft::METADATA_RECORD_STATE_UNSPECIFIED,
                                CurrentLogIndex()));
                return grpc::Status::OK;
            }

            raft::MetadataRecord record;
            record.set_object_key(request->object_key());
            record.set_state(raft::METADATA_RECORD_STATE_PENDING);
            record.mutable_manifest()->CopyFrom(request->manifest());
            record.set_payload(request->payload());
            record.set_create_request_id(request->request_id());
            record.set_created_at_log_index(NextLogIndex());

            records_[request->object_key()] = record;

            response->mutable_summary()->CopyFrom(
                MakeSummary(raft::METADATA_STATUS_CODE_OK,
                            "create applied",
                            request->request_id(),
                            request->object_key(),
                            raft::METADATA_RECORD_STATE_PENDING,
                            record.created_at_log_index()));
            response->mutable_record()->CopyFrom(record);

            create_replays_[request->request_id()] = {fingerprint, *response};
            return grpc::Status::OK;
        }

        grpc::Status CommitMetadataRecord(
            grpc::ServerContext *,
            const raft::CommitMetadataRecordRequest *request,
            raft::CommitMetadataRecordResponse *response) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++commit_calls_;

            const std::string fingerprint = CommitFingerprint(*request);
            auto replay_it = commit_replays_.find(request->request_id());
            if (replay_it != commit_replays_.end())
            {
                if (replay_it->second.fingerprint != fingerprint)
                {
                    response->mutable_summary()->CopyFrom(
                        MakeSummary(raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT,
                                    "commit request_id conflict",
                                    request->request_id(),
                                    request->object_key(),
                                    raft::METADATA_RECORD_STATE_UNSPECIFIED,
                                    CurrentLogIndex()));
                    return grpc::Status::OK;
                }

                *response = replay_it->second.response;
                response->mutable_summary()->set_code(
                    raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY);
                response->mutable_summary()->set_message("commit replayed");
                return grpc::Status::OK;
            }

            auto it = records_.find(request->object_key());
            if (it == records_.end())
            {
                response->mutable_summary()->CopyFrom(
                    MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                                "commit target not found",
                                request->request_id(),
                                request->object_key(),
                                raft::METADATA_RECORD_STATE_UNSPECIFIED,
                                CurrentLogIndex()));
                return grpc::Status::OK;
            }

            if (!request->expected_create_request_id().empty() &&
                request->expected_create_request_id() != it->second.create_request_id())
            {
                response->mutable_summary()->CopyFrom(
                    MakeSummary(raft::METADATA_STATUS_CODE_STATE_CONFLICT,
                                "unexpected create_request_id",
                                request->request_id(),
                                request->object_key(),
                                it->second.state(),
                                CurrentLogIndex()));
                return grpc::Status::OK;
            }

            if (it->second.state() == raft::METADATA_RECORD_STATE_DELETED)
            {
                response->mutable_summary()->CopyFrom(
                    MakeSummary(raft::METADATA_STATUS_CODE_STATE_CONFLICT,
                                "cannot commit deleted object",
                                request->request_id(),
                                request->object_key(),
                                it->second.state(),
                                CurrentLogIndex()));
                return grpc::Status::OK;
            }

            it->second.set_state(raft::METADATA_RECORD_STATE_COMMITTED);
            it->second.set_commit_request_id(request->request_id());
            it->second.set_commit_info(request->commit_info());
            it->second.set_committed_at_log_index(NextLogIndex());

            response->mutable_summary()->CopyFrom(
                MakeSummary(raft::METADATA_STATUS_CODE_OK,
                            "commit applied",
                            request->request_id(),
                            request->object_key(),
                            raft::METADATA_RECORD_STATE_COMMITTED,
                            it->second.committed_at_log_index()));
            response->mutable_record()->CopyFrom(it->second);

            commit_replays_[request->request_id()] = {fingerprint, *response};
            return grpc::Status::OK;
        }

        grpc::Status DeleteMetadataRecord(
            grpc::ServerContext *,
            const raft::DeleteMetadataRecordRequest *request,
            raft::DeleteMetadataRecordResponse *response) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            ++delete_calls_;

            const std::string fingerprint = DeleteFingerprint(*request);
            auto replay_it = delete_replays_.find(request->request_id());
            if (replay_it != delete_replays_.end())
            {
                if (replay_it->second.fingerprint != fingerprint)
                {
                    response->mutable_summary()->CopyFrom(
                        MakeSummary(raft::METADATA_STATUS_CODE_IDEMPOTENCY_CONFLICT,
                                    "delete request_id conflict",
                                    request->request_id(),
                                    request->object_key(),
                                    raft::METADATA_RECORD_STATE_UNSPECIFIED,
                                    CurrentLogIndex()));
                    return grpc::Status::OK;
                }

                *response = replay_it->second.response;
                response->mutable_summary()->set_code(
                    raft::METADATA_STATUS_CODE_IDEMPOTENT_REPLAY);
                response->mutable_summary()->set_message("delete replayed");
                return grpc::Status::OK;
            }

            auto it = records_.find(request->object_key());
            if (it == records_.end())
            {
                response->mutable_summary()->CopyFrom(
                    MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                                "delete target not found",
                                request->request_id(),
                                request->object_key(),
                                raft::METADATA_RECORD_STATE_UNSPECIFIED,
                                CurrentLogIndex()));
                return grpc::Status::OK;
            }

            if (it->second.state() != raft::METADATA_RECORD_STATE_COMMITTED)
            {
                response->mutable_summary()->CopyFrom(
                    MakeSummary(raft::METADATA_STATUS_CODE_STATE_CONFLICT,
                                "delete requires committed state",
                                request->request_id(),
                                request->object_key(),
                                it->second.state(),
                                CurrentLogIndex()));
                return grpc::Status::OK;
            }

            it->second.set_state(raft::METADATA_RECORD_STATE_DELETED);
            it->second.set_delete_request_id(request->request_id());
            it->second.set_delete_info(request->delete_info());
            it->second.set_deleted_at_log_index(NextLogIndex());

            response->mutable_summary()->CopyFrom(
                MakeSummary(raft::METADATA_STATUS_CODE_OK,
                            "delete applied",
                            request->request_id(),
                            request->object_key(),
                            raft::METADATA_RECORD_STATE_DELETED,
                            it->second.deleted_at_log_index()));

            delete_replays_[request->request_id()] = {fingerprint, *response};
            return grpc::Status::OK;
        }

        grpc::Status HeadMetadataRecord(
            grpc::ServerContext *,
            const raft::HeadMetadataRecordRequest *request,
            raft::HeadMetadataRecordResponse *response) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = records_.find(request->object_key());
            if (it == records_.end() ||
                it->second.state() != raft::METADATA_RECORD_STATE_COMMITTED)
            {
                response->mutable_summary()->CopyFrom(
                    MakeSummary(raft::METADATA_STATUS_CODE_NOT_FOUND,
                                "head target not visible",
                                "",
                                request->object_key(),
                                raft::METADATA_RECORD_STATE_UNSPECIFIED,
                                CurrentLogIndex()));
                response->set_found(false);
                return grpc::Status::OK;
            }

            response->mutable_summary()->CopyFrom(
                MakeSummary(raft::METADATA_STATUS_CODE_OK,
                            "head visible",
                            "",
                            request->object_key(),
                            it->second.state(),
                            CurrentLogIndex()));
            response->set_found(true);
            response->mutable_record()->CopyFrom(it->second);
            return grpc::Status::OK;
        }

        grpc::Status ListMetadataRecords(
            grpc::ServerContext *,
            const raft::ListMetadataRecordsRequest *request,
            raft::ListMetadataRecordsResponse *response) override
        {
            std::lock_guard<std::mutex> lock(mu_);
            response->mutable_summary()->CopyFrom(
                MakeSummary(raft::METADATA_STATUS_CODE_OK,
                            "list complete",
                            "",
                            request->prefix(),
                            raft::METADATA_RECORD_STATE_UNSPECIFIED,
                            CurrentLogIndex()));

            std::uint32_t emitted = 0;
            const std::uint32_t limit =
                request->limit() == 0 ? std::numeric_limits<std::uint32_t>::max()
                                      : request->limit();
            for (const auto &[key, record] : records_)
            {
                if (record.state() != raft::METADATA_RECORD_STATE_COMMITTED)
                {
                    continue;
                }
                if (!request->prefix().empty() && key.rfind(request->prefix(), 0) != 0)
                {
                    continue;
                }
                if (emitted >= limit)
                {
                    break;
                }
                response->add_records()->CopyFrom(record);
                ++emitted;
            }
            response->set_next_page_token("");
            return grpc::Status::OK;
        }

    private:
        template <typename ResponseT>
        struct ReplayEntry
        {
            std::string fingerprint;
            ResponseT response;
        };

        static std::string CreateFingerprint(const raft::CreateMetadataRecordRequest &request)
        {
            std::ostringstream oss;
            oss << request.object_key() << '|'
                << request.manifest().object_size() << '|'
                << request.manifest().chunk_size() << '|'
                << request.manifest().chunk_count() << '|'
                << request.manifest().checksum() << '|'
                << request.payload();
            for (const auto &location : request.manifest().mock_locations())
            {
                oss << '|' << location;
            }
            return oss.str();
        }

        static std::string CommitFingerprint(const raft::CommitMetadataRecordRequest &request)
        {
            return request.object_key() + "|" +
                   request.expected_create_request_id() + "|" +
                   request.commit_info();
        }

        static std::string DeleteFingerprint(const raft::DeleteMetadataRecordRequest &request)
        {
            return request.object_key() + "|" + request.delete_info();
        }

        bool ValidateCreateRequest(const raft::CreateMetadataRecordRequest &request,
                                   std::string *error) const
        {
            if (request.request_id().empty())
            {
                *error = "missing request_id";
                return false;
            }
            if (request.object_key().empty())
            {
                *error = "missing object_key";
                return false;
            }
            if (request.manifest().object_size() == 0 ||
                request.manifest().chunk_size() == 0 ||
                request.manifest().chunk_count() == 0)
            {
                *error = "invalid manifest size fields";
                return false;
            }
            if (request.manifest().checksum().empty())
            {
                *error = "missing checksum";
                return false;
            }
            if (request.manifest().mock_locations_size() == 0)
            {
                *error = "missing mock_locations";
                return false;
            }
            const std::uint64_t expected_chunk_count =
                1 + ((request.manifest().object_size() - 1) /
                     request.manifest().chunk_size());
            if (expected_chunk_count != request.manifest().chunk_count())
            {
                *error = "chunk_count mismatch";
                return false;
            }
            if (request.payload().size() > 4096)
            {
                *error = "payload exceeds limit";
                return false;
            }
            return true;
        }

        raft::MetadataResponseSummary MakeSummary(raft::MetadataStatusCode code,
                                                  const std::string &message,
                                                  const std::string &request_id,
                                                  const std::string &object_key,
                                                  raft::MetadataRecordState state,
                                                  std::uint64_t log_index) const
        {
            raft::MetadataResponseSummary summary;
            summary.set_code(code);
            summary.set_message(message);
            summary.set_request_id(request_id);
            summary.set_object_key(object_key);
            summary.set_state(state);
            summary.set_term(term_);
            summary.set_log_index(log_index);
            summary.mutable_leader_hint()->set_leader_id(leader_id_);
            summary.mutable_leader_hint()->set_leader_address(leader_address_);
            return summary;
        }

        std::uint64_t NextLogIndex()
        {
            return next_log_index_++;
        }

        std::uint64_t CurrentLogIndex() const
        {
            return next_log_index_ == 0 ? 0 : (next_log_index_ - 1);
        }

        mutable std::mutex mu_;
        std::string leader_address_;
        int leader_id_ = 1;
        std::uint64_t term_ = 7;
        std::uint64_t next_log_index_ = 1;
        std::map<std::string, raft::MetadataRecord> records_;
        std::map<std::string, ReplayEntry<raft::CreateMetadataRecordResponse>> create_replays_;
        std::map<std::string, ReplayEntry<raft::CommitMetadataRecordResponse>> commit_replays_;
        std::map<std::string, ReplayEntry<raft::DeleteMetadataRecordResponse>> delete_replays_;
        std::size_t create_calls_ = 0;
        std::size_t commit_calls_ = 0;
        std::size_t delete_calls_ = 0;
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
        ScopedFakeMetadataServer server_;
    };

} // namespace

TEST_F(MetadataClientScenarioTest, CreateScenarioBuildsMetadataOnlyManifest)
{
    const ClientRunResult result = RunClient(
        {
            server_.address(),
            "create",
            "--request-id", "req-create-1",
            "--object-key", "scenario/object-a",
            "--object-size", "16",
            "--chunk-size", "8",
            "--payload", "metadata-only",
            "--mock-location", "node-missing-a/chunk-0",
            "--mock-location", "fake/path/chunk-1",
        },
        "create_manifest");

    ASSERT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(Contains(result.output, "stage=create")) << result.output;
    EXPECT_TRUE(Contains(result.output, "payload_kind=metadata-only")) << result.output;
    EXPECT_TRUE(Contains(result.output,
                         "mock_locations=node-missing-a/chunk-0,fake/path/chunk-1"))
        << result.output;

    const auto snapshot = server_.service().TakeSnapshot("scenario/object-a");
    ASSERT_TRUE(snapshot.record.has_value());
    EXPECT_EQ(snapshot.record->state(), raft::METADATA_RECORD_STATE_PENDING);
    EXPECT_EQ(snapshot.record->manifest().chunk_count(), 2U);
    EXPECT_EQ(snapshot.record->manifest().mock_locations_size(), 2);
}

TEST_F(MetadataClientScenarioTest, CreateCommitHeadListDeleteFlowSucceeds)
{
    ASSERT_EQ(RunClient(
                  {
                      server_.address(),
                      "create",
                      "--request-id", "req-flow-create",
                      "--object-key", "scenario/object-flow",
                      "--object-size", "24",
                      "--chunk-size", "8",
                      "--payload", "payload-flow",
                  },
                  "flow_create")
                  .exit_code,
              0);

    ClientRunResult result = RunClient(
        {
            server_.address(),
            "commit",
            "--request-id", "req-flow-commit",
            "--object-key", "scenario/object-flow",
            "--expected-create-request-id", "req-flow-create",
            "--commit-info", "commit-flow",
        },
        "flow_commit");
    ASSERT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(Contains(result.output, "stage=commit")) << result.output;
    EXPECT_TRUE(Contains(result.output, "status=OK")) << result.output;

    result = RunClient(
        {
            server_.address(),
            "head",
            "--object-key", "scenario/object-flow",
        },
        "flow_head");
    ASSERT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(Contains(result.output, "found=true")) << result.output;
    EXPECT_TRUE(Contains(result.output, "state=COMMITTED")) << result.output;

    result = RunClient(
        {
            server_.address(),
            "list",
            "--prefix", "scenario/object-flow",
        },
        "flow_list");
    ASSERT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(Contains(result.output, "records_count=1")) << result.output;
    EXPECT_TRUE(Contains(result.output, "list_record[0] object_key=scenario/object-flow"))
        << result.output;

    result = RunClient(
        {
            server_.address(),
            "delete",
            "--request-id", "req-flow-delete",
            "--object-key", "scenario/object-flow",
            "--delete-info", "delete-flow",
        },
        "flow_delete");
    ASSERT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(Contains(result.output, "stage=delete")) << result.output;

    result = RunClient(
        {
            server_.address(),
            "head",
            "--object-key", "scenario/object-flow",
        },
        "flow_head_after_delete");
    ASSERT_NE(result.exit_code, 0);
    EXPECT_TRUE(Contains(result.output, "status=NOT_FOUND")) << result.output;
}

TEST_F(MetadataClientScenarioTest, VerifyReadAfterWriteModeReportsPass)
{
    const ClientRunResult result = RunClient(
        {
            server_.address(),
            "verify-read-after-write",
            "--request-id", "req-verify",
            "--object-key", "scenario/object-verify",
            "--object-size", "16",
            "--chunk-size", "8",
            "--payload", "verify-payload",
            "--mock-location", "node-a/chunk-0",
            "--mock-location", "node-b/chunk-1",
            "--commit-info", "commit-verify",
            "--delete-info", "delete-verify",
        },
        "verify_raw");

    ASSERT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(Contains(result.output, "verification_check step=create-head-invisible result=PASS"))
        << result.output;
    EXPECT_TRUE(Contains(result.output, "verification_check step=commit-head-visible result=PASS"))
        << result.output;
    EXPECT_TRUE(Contains(result.output, "verification_check step=delete-list-invisible result=PASS"))
        << result.output;
    EXPECT_TRUE(Contains(result.output, "verification_result mode=read-after-write")) << result.output;
    EXPECT_TRUE(Contains(result.output, "result=PASS")) << result.output;
}

TEST_F(MetadataClientScenarioTest, DuplicateRequestIdDoesNotCreateDuplicateVisibleRecord)
{
    ClientRunResult result = RunClient(
        {
            server_.address(),
            "create",
            "--request-id", "req-dup-create",
            "--object-key", "scenario/object-dup",
            "--object-size", "16",
            "--chunk-size", "8",
            "--payload", "dup-payload",
        },
        "dup_create_first");
    ASSERT_EQ(result.exit_code, 0) << result.output;

    result = RunClient(
        {
            server_.address(),
            "create",
            "--request-id", "req-dup-create",
            "--object-key", "scenario/object-dup",
            "--object-size", "16",
            "--chunk-size", "8",
            "--payload", "dup-payload",
        },
        "dup_create_second");
    ASSERT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(Contains(result.output, "status=IDEMPOTENT_REPLAY")) << result.output;

    result = RunClient(
        {
            server_.address(),
            "commit",
            "--request-id", "req-dup-commit",
            "--object-key", "scenario/object-dup",
            "--expected-create-request-id", "req-dup-create",
        },
        "dup_commit");
    ASSERT_EQ(result.exit_code, 0) << result.output;

    result = RunClient(
        {
            server_.address(),
            "list",
            "--prefix", "scenario/object-dup",
        },
        "dup_list");
    ASSERT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(Contains(result.output, "records_count=1")) << result.output;

    const auto snapshot = server_.service().TakeSnapshot("scenario/object-dup");
    ASSERT_TRUE(snapshot.record.has_value());
    EXPECT_EQ(snapshot.record->state(), raft::METADATA_RECORD_STATE_COMMITTED);
    EXPECT_EQ(snapshot.record_count, 1U);
}

TEST_F(MetadataClientScenarioTest, PayloadBoundaryAndMockLocationsBehaviorAreExposed)
{
    ClientRunResult result = RunClient(
        {
            server_.address(),
            "create",
            "--request-id", "req-payload-ok",
            "--object-key", "scenario/object-payload-ok",
            "--object-size", "16",
            "--chunk-size", "8",
            "--payload", std::string(4096, 'x'),
            "--mock-location", "definitely-missing-node/chunk-0",
            "--mock-location", "nowhere/chunk-1",
        },
        "payload_ok");
    ASSERT_EQ(result.exit_code, 0) << result.output;
    EXPECT_TRUE(Contains(result.output, "payload_bytes=4096")) << result.output;
    EXPECT_TRUE(Contains(result.output,
                         "mock_locations=definitely-missing-node/chunk-0,nowhere/chunk-1"))
        << result.output;

    result = RunClient(
        {
            server_.address(),
            "create",
            "--request-id", "req-payload-too-large",
            "--object-key", "scenario/object-payload-bad",
            "--object-size", "16",
            "--chunk-size", "8",
            "--payload", std::string(4097, 'y'),
            "--mock-location", "node-x/chunk-0",
        },
        "payload_too_large");
    ASSERT_NE(result.exit_code, 0);
    EXPECT_TRUE(Contains(result.output, "status=INVALID_ARGUMENT")) << result.output;
    EXPECT_TRUE(Contains(result.output, "payload exceeds limit")) << result.output;
}
