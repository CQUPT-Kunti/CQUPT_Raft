#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "raft/common/metadata_command.h"
#include "raft/metadata/metadata_query.h"
#include "raft/state_machine/metadata_state_machine.h"
#include "store/chunk/local_disk_chunk_store.h"
#include "store/node/storage_node_service.h"
#include "support/metadata_test_utils.h"
#include "support/store_test_utils.h"

namespace
{
    using namespace std::chrono_literals;

    struct FixtureBinaryPayload
    {
        std::string payload;
        std::filesystem::path source_path;
    };

    std::filesystem::path RepoRoot()
    {
        return std::filesystem::path(__FILE__).parent_path().parent_path().lexically_normal();
    }

    FixtureBinaryPayload LoadFixtureBinaryPayload()
    {
        const std::filesystem::path fixture_path =
            RepoRoot() / "tests" / "test_file" / "test_file.deb";
        if (!std::filesystem::exists(fixture_path))
        {
            throw std::runtime_error("missing binary fixture: " + fixture_path.string());
        }

        std::ifstream input(fixture_path, std::ios::binary);
        if (!input.is_open())
        {
            throw std::runtime_error("failed to open binary fixture: " +
                                     fixture_path.string());
        }

        return FixtureBinaryPayload{
            .payload = std::string(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()),
            .source_path = fixture_path};
    }

    std::size_t CountRegularFilesRecursively(const std::filesystem::path &root)
    {
        std::error_code ec;
        if (!std::filesystem::exists(root, ec))
        {
            return 0;
        }

        std::size_t count = 0;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (entry.is_regular_file())
            {
                ++count;
            }
        }
        return count;
    }

    storedemo::ChunkChecksum ComputeStoreChecksumOrThrow(const std::string_view payload)
    {
        storedemo::ChunkChecksum checksum;
        std::string error_detail;
        const auto status =
            storedemo::ComputeChunkChecksum(payload, &checksum, &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to compute store checksum: " + error_detail);
        }
        return checksum;
    }

    storedemo::ChunkIdentity MakeStoreIdentityOrThrow(const std::string_view object_id,
                                                      const std::uint64_t version,
                                                      const std::uint32_t chunk_index,
                                                      const std::uint64_t offset = 0)
    {
        storedemo::ChunkId chunk_id;
        std::string error_detail;
        const auto status = storedemo::MakeChunkId(object_id,
                                                   version,
                                                   chunk_index,
                                                   &chunk_id,
                                                   &error_detail);
        if (status != storedemo::StorageNodeStatusCode::kOk)
        {
            throw std::runtime_error("failed to build chunk id: " + error_detail);
        }

        storedemo::ChunkIdentity identity;
        identity.chunk_id = std::move(chunk_id);
        identity.object_id = std::string(object_id);
        identity.version = version;
        identity.chunk_index = chunk_index;
        identity.offset = offset;
        return identity;
    }

    storedemo::ReadChunkRequest MakeReadRequest(const storedemo::ChunkId &chunk_id,
                                                const std::string &request_id)
    {
        return storedemo::ReadChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id};
    }

    raftdemo::MetadataCommand MakeCreateObjectCommandWithSize(
        const std::string &bucket,
        const std::string &object_key,
        const std::string &object_id,
        const std::string &request_id,
        const std::uint64_t size,
        const std::string &etag,
        const std::uint64_t create_time = 1712000001)
    {
        raftdemo::MetadataCommand command;
        command.command_type = raftdemo::MetadataCommandType::kCreateObject;
        command.request_id = request_id;
        command.create_object = raftdemo::CreateObjectCommandPayload{
            raftdemo::ObjectRecord{bucket,
                                   object_key,
                                   object_id,
                                   1,
                                   size,
                                   etag,
                                   raftdemo::ObjectState::PENDING,
                                   {},
                                   create_time,
                                   std::nullopt,
                                   std::nullopt}};
        command.request_context = raftdemo::RequestRecord{
            request_id,
            raftdemo::MetadataRequestType::kCreateObject,
            bucket,
            object_key,
            "accepted",
            0,
            create_time,
            std::nullopt};
        return command;
    }

    void FillProtoChecksum(const storedemo::ChunkChecksum &checksum,
                           raft::StorageChunkChecksum *proto_checksum)
    {
        ASSERT_NE(proto_checksum, nullptr);
        switch (checksum.algorithm)
        {
        case storedemo::ChunkChecksumAlgorithm::kSha256:
            proto_checksum->set_algorithm(raft::STORAGE_CHECKSUM_ALGORITHM_SHA256);
            break;
        case storedemo::ChunkChecksumAlgorithm::kUnknown:
        default:
            proto_checksum->set_algorithm(raft::STORAGE_CHECKSUM_ALGORITHM_UNSPECIFIED);
            break;
        }
        proto_checksum->set_value(checksum.value);
        proto_checksum->set_size_bytes(checksum.size_bytes);
        proto_checksum->set_computed_at_unix_ms(checksum.computed_at);
    }

    raft::WriteChunkRequest MakeProtoWriteRequest(const storedemo::ChunkIdentity &identity,
                                                  const std::string &payload,
                                                  const std::string &request_id)
    {
        raft::WriteChunkRequest request;
        request.set_request_id(request_id);
        request.set_chunk_id(identity.chunk_id);
        request.set_object_id(identity.object_id);
        request.set_version(identity.version);
        request.set_chunk_index(identity.chunk_index);
        request.set_offset(identity.offset);
        request.set_expected_size(static_cast<std::uint64_t>(payload.size()));
        FillProtoChecksum(ComputeStoreChecksumOrThrow(payload),
                          request.mutable_expected_checksum());
        request.set_payload(payload);
        request.set_timeout_ms(1500);
        request.set_best_effort_cancel(true);
        request.set_durability(raft::WRITE_CHUNK_DURABILITY_PUBLISH);
        return request;
    }

    class RecordingChunkStore final : public storedemo::ChunkStore
    {
    public:
        storedemo::WriteChunkResponse WriteChunk(
            const storedemo::WriteChunkRequest &request) override
        {
            ++write_calls;
            last_write_request = request;
            if (write_handler)
            {
                return write_handler(request);
            }
            return default_write_response;
        }

        storedemo::ReadChunkResponse ReadChunk(
            const storedemo::ReadChunkRequest &) override
        {
            storedemo::ReadChunkResponse response;
            response.status = storedemo::StorageNodeStatusCode::kUnsupported;
            response.error_detail = "not used in RecordingChunkStore";
            return response;
        }

        storedemo::DeleteChunkResponse DeleteChunk(
            const storedemo::DeleteChunkRequest &) override
        {
            storedemo::DeleteChunkResponse response;
            response.status = storedemo::StorageNodeStatusCode::kUnsupported;
            response.error_detail = "not used in RecordingChunkStore";
            return response;
        }

        storedemo::StatChunkResponse StatChunk(
            const storedemo::StatChunkRequest &) override
        {
            storedemo::StatChunkResponse response;
            response.status = storedemo::StorageNodeStatusCode::kUnsupported;
            response.error_detail = "not used in RecordingChunkStore";
            return response;
        }

        storedemo::ListChunksResponse ListChunks(
            const storedemo::ListChunksRequest &) override
        {
            storedemo::ListChunksResponse response;
            response.status = storedemo::StorageNodeStatusCode::kUnsupported;
            response.error_detail = "not used in RecordingChunkStore";
            return response;
        }

        std::function<storedemo::WriteChunkResponse(const storedemo::WriteChunkRequest &)>
            write_handler;
        storedemo::WriteChunkRequest last_write_request;
        storedemo::WriteChunkResponse default_write_response;
        std::size_t write_calls{0};
    };

    class RunningStorageNodeService
    {
    public:
        explicit RunningStorageNodeService(std::shared_ptr<storedemo::StorageNodeService> service)
            : service_(std::move(service))
        {
            constexpr int kMaxMessageBytes = 32 * 1024 * 1024;
            grpc::ServerBuilder builder;
            builder.SetMaxReceiveMessageSize(kMaxMessageBytes);
            builder.SetMaxSendMessageSize(kMaxMessageBytes);
            builder.AddListeningPort("127.0.0.1:0",
                                     grpc::InsecureServerCredentials(),
                                     &selected_port_);
            builder.RegisterService(service_.get());
            server_ = builder.BuildAndStart();
            if (server_ == nullptr || selected_port_ <= 0)
            {
                throw std::runtime_error("failed to start in-process storage node service");
            }

            grpc::ChannelArguments channel_arguments;
            channel_arguments.SetMaxReceiveMessageSize(kMaxMessageBytes);
            channel_arguments.SetMaxSendMessageSize(kMaxMessageBytes);
            channel_ = grpc::CreateCustomChannel(
                "127.0.0.1:" + std::to_string(selected_port_),
                grpc::InsecureChannelCredentials(),
                channel_arguments);
            if (!channel_->WaitForConnected(std::chrono::system_clock::now() + 5s))
            {
                throw std::runtime_error("storage node test channel did not connect");
            }

            stub_ = raft::StorageNodeService::NewStub(channel_);
        }

        ~RunningStorageNodeService()
        {
            if (server_ != nullptr)
            {
                server_->Shutdown();
                server_->Wait();
            }
        }

        RunningStorageNodeService(const RunningStorageNodeService &) = delete;
        RunningStorageNodeService &operator=(const RunningStorageNodeService &) = delete;

        raft::WriteChunkResponse WriteChunk(const raft::WriteChunkRequest &request,
                                            grpc::Status *grpc_status = nullptr)
        {
            grpc::ClientContext context;
            context.set_deadline(std::chrono::system_clock::now() + 5s);

            raft::WriteChunkResponse response;
            grpc::Status status = stub_->WriteChunk(&context, request, &response);
            if (grpc_status != nullptr)
            {
                *grpc_status = status;
            }
            return response;
        }

    private:
        std::shared_ptr<storedemo::StorageNodeService> service_;
        std::unique_ptr<grpc::Server> server_;
        std::shared_ptr<grpc::Channel> channel_;
        std::unique_ptr<raft::StorageNodeService::Stub> stub_;
        int selected_port_{0};
    };

    class StorageNodeServiceTest : public ::testing::Test
    {
    protected:
        static storedemo::LocalDiskChunkStoreConfig MakeStoreConfig(
            const std::filesystem::path &data_dir,
            const std::size_t index)
        {
            return storedemo::LocalDiskChunkStoreConfig{
                .data_dir = data_dir,
                .node_id = storedemo::test::MakeStorageNodeIdFixture(index)};
        }
    };

    TEST_F(StorageNodeServiceTest, ConstructingWithoutChunkStoreThrows)
    {
        EXPECT_THROW(storedemo::StorageNodeService(nullptr), std::invalid_argument);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkMapsFieldsAndResponseFacts)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        const auto identity = MakeStoreIdentityOrThrow("obj-t031-recording", 7, 3, 4096);
        const auto payload = storedemo::test::MakeChunkPayload(192, "t031-recording");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        recording_store->write_handler =
            [identity, checksum](const storedemo::WriteChunkRequest &request)
        {
            storedemo::WriteChunkResponse response;
            response.status = storedemo::StorageNodeStatusCode::kAlreadyExists;
            response.error_detail = "already durable";
            response.retry_after_ms = 27;
            response.metadata.identity = identity;
            response.metadata.node_id = "service-node-t031";
            response.metadata.size = checksum.size_bytes;
            response.metadata.checksum = checksum;
            response.metadata.state = storedemo::ChunkState::kLive;
            response.durable = true;
            response.already_exists = true;
            return response;
        };

        auto service = std::make_shared<storedemo::StorageNodeService>(recording_store,
                                                                       "service-node-t031");
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.WriteChunk(
            MakeProtoWriteRequest(identity, payload, "write-recording-t031"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        ASSERT_EQ(recording_store->write_calls, 1U);
        EXPECT_EQ(recording_store->last_write_request.request_id, "write-recording-t031");
        EXPECT_EQ(recording_store->last_write_request.identity.chunk_id, identity.chunk_id);
        EXPECT_EQ(recording_store->last_write_request.identity.object_id, identity.object_id);
        EXPECT_EQ(recording_store->last_write_request.identity.version, identity.version);
        EXPECT_EQ(recording_store->last_write_request.identity.chunk_index,
                  identity.chunk_index);
        EXPECT_EQ(recording_store->last_write_request.identity.offset, identity.offset);
        ASSERT_TRUE(recording_store->last_write_request.expected_size.has_value());
        EXPECT_EQ(*recording_store->last_write_request.expected_size, payload.size());
        EXPECT_EQ(recording_store->last_write_request.expected_checksum.algorithm,
                  checksum.algorithm);
        EXPECT_EQ(recording_store->last_write_request.expected_checksum.value,
                  checksum.value);
        EXPECT_EQ(recording_store->last_write_request.payload, payload);

        EXPECT_EQ(response.summary().code(),
                  raft::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS);
        EXPECT_EQ(response.summary().message(), "already durable");
        EXPECT_EQ(response.summary().request_id(), "write-recording-t031");
        EXPECT_EQ(response.summary().node_id(), "service-node-t031");
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.summary().retry_after_ms(), 27U);
        EXPECT_TRUE(response.durable());
        EXPECT_TRUE(response.already_exists());
        EXPECT_EQ(response.size(), checksum.size_bytes);
        EXPECT_EQ(response.state(), raft::STORAGE_CHUNK_STATE_LIVE);
        EXPECT_EQ(response.checksum().algorithm(),
                  raft::STORAGE_CHECKSUM_ALGORITHM_SHA256);
        EXPECT_EQ(response.checksum().value(), checksum.value);
        EXPECT_EQ(response.checksum().size_bytes(), checksum.size_bytes);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkDurableSuccessKeepsMetadataUncommitted)
    {
        const auto fixture = LoadFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        raftdemo::MetadataStateMachine machine;
        std::uint64_t index = 1;
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        raftdemo::test::MakeCreateBucketCommand(
                            "bucket-t031-durable",
                            "create-bucket-t031-durable"))
                        .Ok);

        const auto identity = MakeStoreIdentityOrThrow("obj-t031-durable", 1, 0, 0);
        ASSERT_TRUE(raftdemo::test::ApplyMetadataCommand(
                        machine,
                        index++,
                        MakeCreateObjectCommandWithSize("bucket-t031-durable",
                                                        "uploads/t031-durable",
                                                        identity.object_id,
                                                        "create-object-t031-durable",
                                                        fixture.payload.size(),
                                                        "etag-t031-durable"))
                        .Ok);

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_durable");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 31));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        grpc::Status grpc_status;
        const auto response = server.WriteChunk(
            MakeProtoWriteRequest(identity, fixture.payload, "write-durable-t031"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), raft::STORAGE_NODE_STATUS_CODE_OK);
        EXPECT_TRUE(response.durable());
        EXPECT_FALSE(response.already_exists());
        EXPECT_EQ(response.summary().node_id(), store->config().node_id);
        EXPECT_EQ(response.summary().chunk_id(), identity.chunk_id);
        EXPECT_EQ(response.state(), raft::STORAGE_CHUNK_STATE_LIVE);

        const auto head = machine.HeadObject(
            {.bucket = "bucket-t031-durable", .object_key = "uploads/t031-durable"});
        EXPECT_EQ(head.result.code, raftdemo::MetadataStatusCode::kNotFound);
        EXPECT_FALSE(head.record.has_value());

        const auto list = machine.ListObjects(
            {.bucket = "bucket-t031-durable", .prefix = "uploads/"});
        EXPECT_EQ(list.result.code, raftdemo::MetadataStatusCode::kOk);
        EXPECT_TRUE(list.records.empty());

        const auto read_response =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-durable-t031"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, fixture.payload);
        EXPECT_EQ(CountRegularFilesRecursively(store->paths().live_root), 1U);
        EXPECT_EQ(CountRegularFilesRecursively(store->paths().staging_root), 0U);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkChecksumMismatchDoesNotWriteLiveChunk)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_checksum");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 32));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        const auto identity = MakeStoreIdentityOrThrow("obj-t031-checksum", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(128, "t031-checksum");
        auto request = MakeProtoWriteRequest(identity, payload, "write-checksum-t031");
        FillProtoChecksum(ComputeStoreChecksumOrThrow("different-checksum-payload"),
                          request.mutable_expected_checksum());

        grpc::Status grpc_status;
        const auto response = server.WriteChunk(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  raft::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH);
        EXPECT_FALSE(response.durable());
        EXPECT_FALSE(response.already_exists());

        const auto read_response =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-checksum-t031"));
        EXPECT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kNotFound);
        EXPECT_EQ(CountRegularFilesRecursively(store->paths().live_root), 0U);
        EXPECT_EQ(CountRegularFilesRecursively(store->paths().staging_root), 0U);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkSamePayloadRetryReturnsAlreadyExists)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_idempotent");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 33));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        const auto identity = MakeStoreIdentityOrThrow("obj-t031-idempotent", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(144, "t031-idempotent");
        const auto request = MakeProtoWriteRequest(identity, payload, "retry-safe-t031");

        grpc::Status grpc_status;
        const auto first = server.WriteChunk(request, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        ASSERT_EQ(first.summary().code(), raft::STORAGE_NODE_STATUS_CODE_OK);

        const auto retry = server.WriteChunk(request, &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_TRUE(retry.durable());
        EXPECT_TRUE(retry.already_exists());
        EXPECT_TRUE(retry.summary().code() == raft::STORAGE_NODE_STATUS_CODE_OK ||
                    retry.summary().code() ==
                        raft::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS);

        const auto read_response =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-idempotent-t031"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, payload);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkDifferentPayloadReturnsConflict)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_conflict");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 34));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        const auto identity = MakeStoreIdentityOrThrow("obj-t031-conflict", 1, 0, 0);
        const auto original_payload =
            storedemo::test::MakeChunkPayload(176, "t031-conflict-original");
        const auto conflicting_payload =
            storedemo::test::MakeChunkPayload(176, "t031-conflict-different");

        grpc::Status grpc_status;
        ASSERT_EQ(server.WriteChunk(
                      MakeProtoWriteRequest(identity,
                                            original_payload,
                                            "write-conflict-original-t031"),
                      &grpc_status)
                      .summary()
                      .code(),
                  raft::STORAGE_NODE_STATUS_CODE_OK);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();

        const auto conflict = server.WriteChunk(
            MakeProtoWriteRequest(identity,
                                  conflicting_payload,
                                  "write-conflict-different-t031"),
            &grpc_status);
        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(conflict.summary().code(), raft::STORAGE_NODE_STATUS_CODE_CONFLICT);
        EXPECT_FALSE(conflict.durable());

        const auto read_response =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-conflict-t031"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, original_payload);
    }

    TEST_F(StorageNodeServiceTest, WriteChunkInvalidRequestReturnsInvalidArgument)
    {
        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_service_invalid");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 35));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        raft::WriteChunkRequest request;
        request.set_object_id("obj-t031-invalid");
        request.set_version(1);
        request.set_chunk_index(0);
        request.set_payload("payload-without-request-id");
        request.set_expected_size(request.payload().size());

        grpc::Status grpc_status;
        const auto response = server.WriteChunk(request, &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(),
                  raft::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT);
        EXPECT_FALSE(response.durable());
    }

    TEST_F(StorageNodeServiceTest, WriteChunkOverloadedMapsStatusAndRetryAfter)
    {
        auto recording_store = std::make_shared<RecordingChunkStore>();
        recording_store->default_write_response.status =
            storedemo::StorageNodeStatusCode::kOverloaded;
        recording_store->default_write_response.error_detail = "executor queue is full";
        recording_store->default_write_response.retry_after_ms = 88;

        auto service = std::make_shared<storedemo::StorageNodeService>(recording_store,
                                                                       "service-node-t031");
        RunningStorageNodeService server(service);

        const auto identity = MakeStoreIdentityOrThrow("obj-t031-overloaded", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(96, "t031-overloaded");

        grpc::Status grpc_status;
        const auto response = server.WriteChunk(
            MakeProtoWriteRequest(identity, payload, "write-overloaded-t031"),
            &grpc_status);

        ASSERT_TRUE(grpc_status.ok()) << grpc_status.error_message();
        EXPECT_EQ(response.summary().code(), raft::STORAGE_NODE_STATUS_CODE_OVERLOADED);
        EXPECT_EQ(response.summary().message(), "executor queue is full");
        EXPECT_EQ(response.summary().retry_after_ms(), 88U);
        EXPECT_FALSE(response.durable());
        EXPECT_FALSE(response.already_exists());
    }
} // namespace
