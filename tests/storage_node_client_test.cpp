#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <grpcpp/grpcpp.h>

#include "store/chunk/local_disk_chunk_store.h"
#include "store/node/storage_node_client.h"
#include "store/node/storage_node_service.h"
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

    storedemo::WriteChunkRequest MakeWriteRequest(const storedemo::ChunkIdentity &identity,
                                                  const std::string &payload,
                                                  const std::string &request_id)
    {
        return storedemo::WriteChunkRequest{
            .request_id = request_id,
            .identity = identity,
            .expected_size = static_cast<std::uint64_t>(payload.size()),
            .expected_checksum = ComputeStoreChecksumOrThrow(payload),
            .payload = payload};
    }

    storedemo::ReadChunkRequest MakeReadRequest(const storedemo::ChunkId &chunk_id,
                                                const std::string &request_id)
    {
        return storedemo::ReadChunkRequest{
            .request_id = request_id,
            .chunk_id = chunk_id};
    }

    void FillProtoChecksum(const storedemo::ChunkChecksum &checksum,
                           storage::StorageChunkChecksum *proto_checksum)
    {
        ASSERT_NE(proto_checksum, nullptr);
        switch (checksum.algorithm)
        {
        case storedemo::ChunkChecksumAlgorithm::kSha256:
            proto_checksum->set_algorithm(storage::STORAGE_CHECKSUM_ALGORITHM_SHA256);
            break;
        case storedemo::ChunkChecksumAlgorithm::kUnknown:
        default:
            proto_checksum->set_algorithm(storage::STORAGE_CHECKSUM_ALGORITHM_UNSPECIFIED);
            break;
        }
        proto_checksum->set_value(checksum.value);
        proto_checksum->set_size_bytes(checksum.size_bytes);
        proto_checksum->set_computed_at_unix_ms(checksum.computed_at);
    }

    storage::WriteChunkResponse MakeProtoWriteResponse(
        const storedemo::StorageNodeStatusCode status,
        const storedemo::ChunkIdentity &identity,
        const std::string &node_id,
        const storedemo::ChunkChecksum &checksum,
        const std::uint64_t size,
        const storedemo::ChunkState state,
        const bool durable,
        const bool already_exists,
        const std::string &message = {},
        const std::uint64_t retry_after_ms = 0)
    {
        storage::WriteChunkResponse response;

        storage::StorageNodeStatusCode proto_status =
            storage::STORAGE_NODE_STATUS_CODE_UNSPECIFIED;
        switch (status)
        {
        case storedemo::StorageNodeStatusCode::kOk:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_OK;
            break;
        case storedemo::StorageNodeStatusCode::kAlreadyExists:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_ALREADY_EXISTS;
            break;
        case storedemo::StorageNodeStatusCode::kConflict:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_CONFLICT;
            break;
        case storedemo::StorageNodeStatusCode::kChecksumMismatch:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_CHECKSUM_MISMATCH;
            break;
        case storedemo::StorageNodeStatusCode::kOverloaded:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_OVERLOADED;
            break;
        case storedemo::StorageNodeStatusCode::kInvalidArgument:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_INVALID_ARGUMENT;
            break;
        default:
            proto_status = storage::STORAGE_NODE_STATUS_CODE_IO_ERROR;
            break;
        }

        response.mutable_summary()->set_code(proto_status);
        response.mutable_summary()->set_message(message);
        response.mutable_summary()->set_request_id("proto-request-id");
        response.mutable_summary()->set_node_id(node_id);
        response.mutable_summary()->set_chunk_id(identity.chunk_id);
        response.mutable_summary()->set_retry_after_ms(retry_after_ms);
        response.set_size(size);
        FillProtoChecksum(checksum, response.mutable_checksum());

        switch (state)
        {
        case storedemo::ChunkState::kStaging:
            response.set_state(storage::STORAGE_CHUNK_STATE_STAGING);
            break;
        case storedemo::ChunkState::kLive:
            response.set_state(storage::STORAGE_CHUNK_STATE_LIVE);
            break;
        case storedemo::ChunkState::kDeleting:
            response.set_state(storage::STORAGE_CHUNK_STATE_DELETING);
            break;
        case storedemo::ChunkState::kDeleted:
            response.set_state(storage::STORAGE_CHUNK_STATE_DELETED);
            break;
        case storedemo::ChunkState::kQuarantined:
            response.set_state(storage::STORAGE_CHUNK_STATE_QUARANTINED);
            break;
        case storedemo::ChunkState::kCorrupted:
            response.set_state(storage::STORAGE_CHUNK_STATE_CORRUPTED);
            break;
        case storedemo::ChunkState::kMissing:
        default:
            response.set_state(storage::STORAGE_CHUNK_STATE_MISSING);
            break;
        }

        response.set_durable(durable);
        response.set_already_exists(already_exists);
        return response;
    }

    class FakeStorageNodeStub final : public storage::StorageNodeService::StubInterface
    {
    public:
        grpc::Status WriteChunk(grpc::ClientContext *context,
                                const storage::WriteChunkRequest &request,
                                storage::WriteChunkResponse *response) override
        {
            ++write_calls;
            last_request = request;
            call_observed_at = std::chrono::system_clock::now();
            observed_deadline = context->deadline();

            if (write_handler)
            {
                return write_handler(context, request, response);
            }

            return grpc::Status::OK;
        }

        grpc::Status ReadChunk(grpc::ClientContext *,
                               const storage::ReadChunkRequest &,
                               storage::ReadChunkResponse *) override
        {
            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "ReadChunk is not implemented in T042 tests");
        }

        std::function<grpc::Status(grpc::ClientContext *,
                                   const storage::WriteChunkRequest &,
                                   storage::WriteChunkResponse *)>
            write_handler;
        storage::WriteChunkRequest last_request;
        std::size_t write_calls{0};
        std::chrono::system_clock::time_point call_observed_at{};
        std::chrono::system_clock::time_point observed_deadline{};

    private:
        grpc::ClientAsyncResponseReaderInterface<storage::WriteChunkResponse> *
        AsyncWriteChunkRaw(grpc::ClientContext *,
                           const storage::WriteChunkRequest &,
                           grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::WriteChunkResponse> *
        PrepareAsyncWriteChunkRaw(grpc::ClientContext *,
                                  const storage::WriteChunkRequest &,
                                  grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::ReadChunkResponse> *
        AsyncReadChunkRaw(grpc::ClientContext *,
                          const storage::ReadChunkRequest &,
                          grpc::CompletionQueue *) override
        {
            return nullptr;
        }

        grpc::ClientAsyncResponseReaderInterface<storage::ReadChunkResponse> *
        PrepareAsyncReadChunkRaw(grpc::ClientContext *,
                                 const storage::ReadChunkRequest &,
                                 grpc::CompletionQueue *) override
        {
            return nullptr;
        }
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

        [[nodiscard]] std::shared_ptr<grpc::Channel> channel() const
        {
            return channel_;
        }

    private:
        std::shared_ptr<storedemo::StorageNodeService> service_;
        std::unique_ptr<grpc::Server> server_;
        std::shared_ptr<grpc::Channel> channel_;
        int selected_port_{0};
    };

    class StorageNodeClientTest : public ::testing::Test
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

    TEST_F(StorageNodeClientTest, ConstructingWithoutStubThrows)
    {
        EXPECT_THROW(storedemo::StorageNodeClient(
                         std::unique_ptr<storage::StorageNodeService::StubInterface>()),
                     std::invalid_argument);
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsRequestFieldsAndSuccessResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-success", 9, 2, 2048);
        const auto payload = storedemo::test::MakeChunkPayload(180, "t032-success");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(storedemo::StorageNodeStatusCode::kOk,
                                               identity,
                                               "client-node-t032",
                                               checksum,
                                               checksum.size_bytes,
                                               storedemo::ChunkState::kLive,
                                               true,
                                               false);
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};

        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-success-t032"),
            {.context = {.timeout_ms = 1200, .best_effort_cancel = true}});

        ASSERT_EQ(stub_ptr->write_calls, 1U);
        EXPECT_EQ(stub_ptr->last_request.request_id(), "write-success-t032");
        EXPECT_EQ(stub_ptr->last_request.chunk_id(), identity.chunk_id);
        EXPECT_EQ(stub_ptr->last_request.object_id(), identity.object_id);
        EXPECT_EQ(stub_ptr->last_request.version(), identity.version);
        EXPECT_EQ(stub_ptr->last_request.chunk_index(), identity.chunk_index);
        EXPECT_EQ(stub_ptr->last_request.offset(), identity.offset);
        EXPECT_EQ(stub_ptr->last_request.expected_size(),
                  static_cast<std::uint64_t>(payload.size()));
        EXPECT_EQ(stub_ptr->last_request.expected_checksum().value(), checksum.value);
        EXPECT_EQ(stub_ptr->last_request.payload(), payload);
        EXPECT_EQ(stub_ptr->last_request.timeout_ms(), 1200U);
        EXPECT_TRUE(stub_ptr->last_request.best_effort_cancel());
        EXPECT_EQ(stub_ptr->last_request.durability(),
                  storage::WRITE_CHUNK_DURABILITY_PUBLISH);

        const auto deadline_delta = stub_ptr->observed_deadline - stub_ptr->call_observed_at;
        EXPECT_GT(deadline_delta, 0ms);
        EXPECT_LE(deadline_delta, 1500ms);

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(response.durable);
        EXPECT_FALSE(response.already_exists);
        EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);
        EXPECT_EQ(response.metadata.identity.object_id, identity.object_id);
        EXPECT_EQ(response.metadata.identity.version, identity.version);
        EXPECT_EQ(response.metadata.identity.chunk_index, identity.chunk_index);
        EXPECT_EQ(response.metadata.identity.offset, identity.offset);
        EXPECT_EQ(response.metadata.node_id, "client-node-t032");
        EXPECT_EQ(response.metadata.size, checksum.size_bytes);
        EXPECT_EQ(response.metadata.checksum.value, checksum.value);
        EXPECT_EQ(response.metadata.state, storedemo::ChunkState::kLive);
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsAlreadyExistsResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity =
            MakeStoreIdentityOrThrow("obj-t032-already-exists", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(64, "t032-already-exists");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(
                storedemo::StorageNodeStatusCode::kAlreadyExists,
                identity,
                "client-node-t032",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kLive,
                true,
                true,
                "already durable");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.WriteChunk(MakeWriteRequest(identity, payload, "write-already-t032"));

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kAlreadyExists);
        EXPECT_TRUE(response.durable);
        EXPECT_TRUE(response.already_exists);
        EXPECT_EQ(response.error_detail, "already durable");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsConflictResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-conflict", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(72, "t032-conflict");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(storedemo::StorageNodeStatusCode::kConflict,
                                               identity,
                                               "client-node-t032",
                                               checksum,
                                               checksum.size_bytes,
                                               storedemo::ChunkState::kLive,
                                               false,
                                               false,
                                               "content conflict");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.WriteChunk(MakeWriteRequest(identity, payload, "write-conflict-t032"));

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kConflict);
        EXPECT_FALSE(response.durable);
        EXPECT_EQ(response.error_detail, "content conflict");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsChecksumMismatchResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-checksum", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(80, "t032-checksum");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(
                storedemo::StorageNodeStatusCode::kChecksumMismatch,
                identity,
                "client-node-t032",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kMissing,
                false,
                false,
                "checksum mismatch");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.WriteChunk(MakeWriteRequest(identity, payload, "write-checksum-t032"));

        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kChecksumMismatch);
        EXPECT_FALSE(response.durable);
        EXPECT_EQ(response.error_detail, "checksum mismatch");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsOverloadedResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-overloaded", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(96, "t032-overloaded");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(storedemo::StorageNodeStatusCode::kOverloaded,
                                               identity,
                                               "client-node-t032",
                                               checksum,
                                               checksum.size_bytes,
                                               storedemo::ChunkState::kMissing,
                                               false,
                                               false,
                                               "queue full",
                                               55);
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response =
            client.WriteChunk(MakeWriteRequest(identity, payload, "write-overloaded-t032"));

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOverloaded);
        EXPECT_EQ(response.retry_after_ms, 55U);
        EXPECT_EQ(response.error_detail, "queue full");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsGrpcDeadlineExceededToTimeout)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        stub_ptr->write_handler =
            [](grpc::ClientContext *,
               const storage::WriteChunkRequest &,
               storage::WriteChunkResponse *)
        {
            return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                "rpc deadline exceeded");
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-timeout", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "t032-timeout");

        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-timeout-t032"),
            {.context = {.timeout_ms = 50}});

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kTimeout);
        EXPECT_EQ(response.error_detail, "rpc deadline exceeded");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsGrpcCancelledToCancelled)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        stub_ptr->write_handler =
            [](grpc::ClientContext *,
               const storage::WriteChunkRequest &,
               storage::WriteChunkResponse *)
        {
            return grpc::Status(grpc::StatusCode::CANCELLED,
                                "cancelled by caller");
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-cancelled", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "t032-cancelled");

        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-cancelled-t032"),
            {.context = {.timeout_ms = 300, .best_effort_cancel = true}});

        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kCancelled);
        EXPECT_EQ(response.error_detail, "cancelled by caller");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsGrpcUnavailableToNodeUnavailable)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        stub_ptr->write_handler =
            [](grpc::ClientContext *,
               const storage::WriteChunkRequest &,
               storage::WriteChunkResponse *)
        {
            return grpc::Status(grpc::StatusCode::UNAVAILABLE, "remote unavailable");
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-unavailable", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "t032-unavailable");

        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-unavailable-t032"));

        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kNodeUnavailable);
        EXPECT_EQ(response.error_detail, "remote unavailable");
    }

    TEST_F(StorageNodeClientTest, WriteChunkMapsInvalidArgumentResponse)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity =
            MakeStoreIdentityOrThrow("obj-t032-invalid-argument", 1, 0, 0);
        const auto payload =
            storedemo::test::MakeChunkPayload(48, "t032-invalid-argument");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(
                storedemo::StorageNodeStatusCode::kInvalidArgument,
                identity,
                "client-node-t032",
                checksum,
                checksum.size_bytes,
                storedemo::ChunkState::kMissing,
                false,
                false,
                "request invalid");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr)};
        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-invalid-argument-t032"));

        EXPECT_EQ(response.status,
                  storedemo::StorageNodeStatusCode::kInvalidArgument);
        EXPECT_EQ(response.error_detail, "request invalid");
    }

    TEST_F(StorageNodeClientTest, WriteChunkRetriesRetryableUnavailableWithinBudget)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-retry", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "t032-retry");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum, attempts = 0](grpc::ClientContext *,
                                               const storage::WriteChunkRequest &,
                                               storage::WriteChunkResponse *response) mutable
        {
            ++attempts;
            if (attempts == 1)
            {
                return grpc::Status(grpc::StatusCode::UNAVAILABLE, "first unavailable");
            }

            *response = MakeProtoWriteResponse(storedemo::StorageNodeStatusCode::kOk,
                                               identity,
                                               "client-node-t032",
                                               checksum,
                                               checksum.size_bytes,
                                               storedemo::ChunkState::kLive,
                                               true,
                                               false);
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr),
            {.max_write_retries = 1}};
        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-retry-t032"));

        EXPECT_EQ(stub_ptr->write_calls, 2U);
        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk);
        EXPECT_TRUE(response.durable);
    }

    TEST_F(StorageNodeClientTest, WriteChunkDoesNotRetryNonRetryableConflict)
    {
        auto *stub_ptr = new FakeStorageNodeStub();
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-no-retry", 1, 0, 0);
        const auto payload = storedemo::test::MakeChunkPayload(48, "t032-no-retry");
        const auto checksum = ComputeStoreChecksumOrThrow(payload);

        stub_ptr->write_handler =
            [identity, checksum](grpc::ClientContext *,
                                 const storage::WriteChunkRequest &,
                                 storage::WriteChunkResponse *response)
        {
            *response = MakeProtoWriteResponse(storedemo::StorageNodeStatusCode::kConflict,
                                               identity,
                                               "client-node-t032",
                                               checksum,
                                               checksum.size_bytes,
                                               storedemo::ChunkState::kLive,
                                               false,
                                               false,
                                               "conflict");
            return grpc::Status::OK;
        };

        storedemo::StorageNodeClient client{
            std::unique_ptr<storage::StorageNodeService::StubInterface>(stub_ptr),
            {.max_write_retries = 3}};
        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, payload, "write-no-retry-t032"));

        EXPECT_EQ(stub_ptr->write_calls, 1U);
        EXPECT_EQ(response.status, storedemo::StorageNodeStatusCode::kConflict);
    }

    TEST_F(StorageNodeClientTest, WriteChunkBinaryPayloadUsesFixtureThroughRealService)
    {
        const auto fixture = LoadFixtureBinaryPayload();
        ASSERT_EQ(fixture.source_path.filename(), "test_file.deb");

        storedemo::test::ScopedStoreTestDir temp_dir("storage_node_client_binary");
        auto store = std::make_shared<storedemo::LocalDiskChunkStore>(
            MakeStoreConfig(temp_dir.root(), 32));
        ASSERT_EQ(store->Initialize().status, storedemo::StorageNodeStatusCode::kOk);

        auto service = std::make_shared<storedemo::StorageNodeService>(
            store,
            store->config().node_id);
        RunningStorageNodeService server(service);

        storedemo::StorageNodeClient client{server.channel()};
        const auto identity = MakeStoreIdentityOrThrow("obj-t032-binary", 1, 0, 0);
        const auto response = client.WriteChunk(
            MakeWriteRequest(identity, fixture.payload, "write-binary-t032"),
            {.context = {.timeout_ms = 1500, .best_effort_cancel = true}});

        ASSERT_EQ(response.status, storedemo::StorageNodeStatusCode::kOk)
            << response.error_detail;
        EXPECT_TRUE(response.durable);
        EXPECT_FALSE(response.already_exists);
        EXPECT_EQ(response.metadata.node_id, store->config().node_id);
        EXPECT_EQ(response.metadata.identity.chunk_id, identity.chunk_id);

        const auto read_response =
            store->ReadChunk(MakeReadRequest(identity.chunk_id, "read-binary-t032"));
        ASSERT_EQ(read_response.status, storedemo::StorageNodeStatusCode::kOk)
            << read_response.error_detail;
        EXPECT_EQ(read_response.payload, fixture.payload);
    }
} // namespace
